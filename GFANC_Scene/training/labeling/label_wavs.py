r"""
统一 LMS 打标签 — 对已有 1s 片段标 gain_0..29（真实/合成共用一份核心循环）.

背景:
  合成数据由 generate_synthetic.py 生成, 真实数据由 cut_real_noise.py 切割.
  两者都需把 1s WAV 过实测声学路径 (Pri/Sec) → FxNLMS 拟合最优子带增益 gain_0..29.
  本脚本是唯一一份打标签实现 (与 MIMO 旧 band_* 场景标签语义不同, 不能混用).

用法:
    # 合成数据打标签 (generate_synthetic.py 之后)
    python training/labeling/label_wavs.py --wav-dir D:\Dataset\Synthetic_Dataset --tag synth

    # 真实数据打标签 (cut_real_noise.py 之后)
    python training/labeling/label_wavs.py --wav-dir D:\Dataset\Real_world_Dataset --tag real

    # 抑制首批 LMS 收敛诊断 (重标大样本时减少输出)
    python training/labeling/label_wavs.py --wav-dir <dir> --tag synth --no-lms-diag

输出:
    {wav-dir}\Index_{tag}_{Training,Validate,Testing}_data.csv
    {wav-dir}\Gains_{tag}_{Training,Validate,Testing}_data.npy   (N × C*S=30)
    列 = File_path / category / gain_0..gain_29 — 下游 noise_dataset.py 读取契约.

LMS 参数 (与部署一致):
    LMS_MU=0.001, LMS_REPET=3, Fx 注入 FX_NOISE_DB 低噪防增益坍缩.
    子滤波器基固定 broadband (USE_LOG_SPACING=False 的部署基; logspacing 会标错增益).
"""
import os, sys, argparse
import numpy as np
import scipy.io as sio
import torch
import torchaudio
import pandas as pd
from pathlib import Path
from tqdm import tqdm

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

from gfanc._paths import MODELS_DIR
from training.control_filters.path_loader import load_multichannel_paths_with_variable_names
from training.control_filters.Disturbance_generation import disturbance_generation_batch_gpu
from training.labeling.Adaptive_control_filter_generator_batch import (
    adaptive_control_filter_batch_mimo, train_adaptive_gain_batch_mimo)

# ══════════════════════════════════════════════════
# 配置
# ══════════════════════════════════════════════════
FS          = 16000
CHUNK_SEC   = 1.0
CHUNK       = int(CHUNK_SEC * FS)          # 16000

# LMS 标注参数 (与训练/部署一致)
LMS_MU      = 0.001     # LMS 步长
LMS_REPET   = 3         # 噪声重复次数 (1→1s迭代, 3→3s, 提高增益收敛)
BATCH_SIZE  = 128       # GPU 批大小
FX_NOISE_DB = -30       # Fx 注入噪声 SNR (dB, 防止增益坍缩到零)
DEVICE      = 'cuda' if torch.cuda.is_available() else 'cpu'

# 子滤波器基 (必须与部署基一致: broadband)
# 部署 (export_bin.py → data/sub_filters.bin → C 运行时) 和 MIMO_GFANC 参考均用 broadband。
# logspacing 与 broadband 内容不同 (最大差 0.33), 用 logspacing 标出的增益在部署
# broadband 基下不是最优权重 → 必须用 broadband 标。本脚本固定 broadband (等价
# USE_LOG_SPACING=False), 无需配置。
_SUB_FILTER_FILE = MODELS_DIR / 'MIMO_Pretrained_Control_filters_broadband.mat'

# 真实数据类别前缀 (real 模式用于识别 category 列; 合成模式固定 'synthetic')
CATEGORY_CONFIG = ['road', 'children', 'construction', 'railway']

SPLIT_DIRS = ['Training_data', 'Validate_data', 'Testing_data']


def _load_paths():
    Pri_path, Sec_path = load_multichannel_paths_with_variable_names(
        folder=str(_PROJECT_ROOT / 'Primary and Secondary Path'), subfolder='',
        Pri_path_file_name='primary_path.npy', Sec_path_file_name='secondary_path.npy')
    return Pri_path, Sec_path


def _load_sub_filters():
    sub = sio.loadmat(str(_SUB_FILTER_FILE))['Wc_v']   # (C=15, S=2, L)
    return torch.from_numpy(sub).type(torch.float)


def gather_split_files(out_root):
    """扫描 {Training,Validate,Testing}_data 下全部 *.wav. 返回 {split: [fname]}."""
    out = {}
    for split in SPLIT_DIRS:
        d = os.path.join(out_root, split)
        if os.path.isdir(d):
            out[split] = sorted([f for f in os.listdir(d) if f.endswith('.wav')])
        else:
            out[split] = []
    return out


def category_of(fname, categories):
    """真实模式: 按文件名前缀识别类别; 无匹配回退 'unknown'."""
    for cat in categories:
        if fname.startswith(cat + '_'):
            return cat
    return 'unknown'


def lms_label_files(files, wav_dir, out_csv, Pri_path, Sec_path, sub_T,
                    category_fn=None, lms_diag=False):
    """对 wav_dir 下全部 1s WAV 做 LMS 标注, 写 CSV + Gains npy. 返回样本数.
    category_fn 给出时逐文件取类别 (real 模式), 否则 category='synthetic'."""
    n = len(files)
    if n == 0:
        print(f'  {out_csv}: 无文件, 跳过')
        return 0
    C, S, _ = sub_T.shape
    SC = C * S
    rows = []
    gains_arr = np.zeros((n, SC), dtype=np.float32)
    print(f'\n  LMS 标注 {n} 片段 → {os.path.basename(out_csv)}')

    diag_done = [False]  # 首批 LMS 收敛诊断只打印一次
    for bs in tqdm(range(0, n, BATCH_SIZE), desc='  标注', unit='批'):
        be = min(bs + BATCH_SIZE, n)
        ba = be - bs
        batch_chunks = []
        for i in range(bs, be):
            sig, _ = torchaudio.load(os.path.join(wav_dir, files[i]))
            batch_chunks.append(sig.squeeze()[:CHUNK])
        Dis_batch, Fx_batch, _ = disturbance_generation_batch_gpu(
            batch_chunks, Pri_path, Sec_path, fs=FS, Repet=LMS_REPET)
        # Fx 注入低噪 — 防增益坍缩到零 (GFANC-generative 已验证)
        fx_power = Fx_batch.norm(p=2, dim=(1, 2), keepdim=True) / \
                   (Fx_batch.shape[1] * Fx_batch.shape[2]) ** 0.5
        noise_power = fx_power * (10 ** (FX_NOISE_DB / 20))
        Fx_batch = Fx_batch + torch.randn_like(Fx_batch) * noise_power
        gen = adaptive_control_filter_batch_mimo(sub_T, Batch_size=ba, muw=LMS_MU, device=DEVICE)
        err_traj = train_adaptive_gain_batch_mimo(gen, Fx_batch, Dis_batch, device=DEVICE)
        gains_batch = gen.get_coeffiecients_().cpu().numpy()   # (ba, SC)

        # 首批: LMS 收敛诊断 (1s 迭代是否足够 — 误差应明显衰减)
        if lms_diag and not diag_done[0]:
            diag_done[0] = True
            e_abs = np.abs(err_traj)  # (T, E)
            head = e_abs[:1600].mean(); tail = e_abs[-1600:].mean()
            decay_db = 20 * np.log10(max(tail, 1e-15) / max(head, 1e-15))
            status = '✓ 收敛良好' if decay_db < -3 else ('△ 收敛中' if decay_db < 0 else '✗ 未收敛, 考虑增大 LMS_MU 或 Repet')
            print(f'\n  [LMS收敛诊断] 首批误差: 前0.1s RMS={head:.4f} → 末0.1s RMS={tail:.4f} '
                  f'({decay_db:+.1f}dB) {status}\n')

        for j in range(ba):
            r = {'File_path': files[bs + j],
                 'category': category_fn(files[bs + j]) if category_fn else 'synthetic'}
            for b in range(SC):
                r[f'gain_{b}'] = float(gains_batch[j].flatten()[b])
            rows.append(r)
            gains_arr[bs + j] = gains_batch[j].flatten()

    df = pd.DataFrame(rows)
    df.to_csv(out_csv, index=False)
    np.save(os.path.join(os.path.dirname(out_csv),
                         os.path.basename(out_csv).replace('Index_', 'Gains_')
                         .replace('.csv', '.npy')), gains_arr)
    print(f'  已写: {out_csv} ({n} 片段)')
    return n


def main():
    ap = argparse.ArgumentParser(description='统一 LMS 打标签 (真实/合成)')
    ap.add_argument('--wav-dir', required=True,
                    help='数据根目录 (下含 Training/Validate/Testing_data)')
    ap.add_argument('--tag', required=True, choices=['real', 'synth'],
                    help='real=真实噪声(category=类别前缀); synth=合成(category=synthetic)')
    ap.add_argument('--no-lms-diag', action='store_true',
                    help='抑制首批 LMS 收敛诊断')
    args = ap.parse_args()

    out_root = args.wav_dir
    print('=' * 60)
    print(f'  LMS 打标签: tag={args.tag}')
    print(f'  数据根目录: {out_root}')
    print(f'  设备: {DEVICE}')
    print('=' * 60)

    Pri_path, Sec_path = _load_paths()
    sub_T = _load_sub_filters()
    print(f'  子滤波器: {tuple(sub_T.shape)} (C,S,L), 基: {_SUB_FILTER_FILE.name}')

    category_fn = (lambda f: category_of(f, CATEGORY_CONFIG)) if args.tag == 'real' else None

    split_files = gather_split_files(out_root)
    total = 0
    for split in SPLIT_DIRS:
        files = split_files[split]
        if not files:
            print(f'  {split}: 无 WAV, 跳过')
            continue
        out_csv = os.path.join(out_root, f'Index_{args.tag}_{split}.csv')
        total += lms_label_files(files, os.path.join(out_root, split), out_csv,
                                 Pri_path, Sec_path, sub_T,
                                 category_fn=category_fn, lms_diag=not args.no_lms_diag)
    print(f'\n  完成: 共标注 {total} 个样本')
    if args.tag == 'synth':
        print('  下一步训练: python training/network/Train_validate.py')
    else:
        print('  下一步 (可选): python training/labeling/recluster_real.py')


if __name__ == '__main__':
    main()
