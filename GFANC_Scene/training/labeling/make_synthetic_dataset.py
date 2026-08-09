r"""
合成噪声数据集构造 + LMS 标注 — 修复 CNN 跨噪声区分度不足 (欠训练根因).

背景 (2026-08-09 实测):
  输入谱(20-1500Hz 带通子带能量)跨噪声类型可分 75.0%, 真实训练标签(gain_*)可分
  76.7%, 但 CNN 增益输出仅可分 35.8% → CNN 欠拟合, 没学会提取输入光谱信息.
  当前 4 类真实噪声(道路/儿童/施工/铁路)全是低频主导、光谱接近, 模型输出同一
  低频处方即可低损失 → 不学光谱→增益映射.
  合成数据用多样谱形(窄带/宽带/谱倾斜/谐波)覆盖 20-1500Hz 全子带空间, 逼 CNN
  必须用输入 → 修复欠拟合.

标签 = LMS 拟合最优增益 (与 label_real_noise.py 完全同机制): 合成 WAV 过真实
  声学路径(Pri/Sec) → FxNLMS 自适应拟合子带增益 gain_0..29. 因合成谱形多样,
  标签天然多样, 一并解决"标签退化"潜在问题.

用法:
    # 只生成 WAV (CPU, 快速) + 谱形覆盖 probe
    python training/labeling/make_synthetic_dataset.py --gen-only --n-train 500

    # 完整: 生成 + LMS 标注 (GPU)
    python training/labeling/make_synthetic_dataset.py --n-train 60000 --n-val 7500 --n-test 7500

    # 只标注已生成 WAV (改 LMS 参数后重标)
    python training/labeling/make_synthetic_dataset.py --label-only

    # 只跑谱形覆盖 probe (检查点1: 合成谱形是否覆盖全子带空间)
    python training/labeling/make_synthetic_dataset.py --probe-only --n-probe 300

输出:
    D:\Dataset\Synthetic_Dataset\{Training,Validate,Testing}_data\*.wav  (synth_*)
    D:\Dataset\Synthetic_Dataset\Index_synth_{split}_data.csv            (File_path/category/gain_*)
"""
import os, sys, argparse, json
import numpy as np
import scipy.signal as signal
import scipy.io as sio
import torch
import torchaudio
from pathlib import Path
from tqdm import tqdm

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

from gfanc._paths import MODELS_DIR
from training.control_filters.path_loader import load_multichannel_paths_with_variable_names
from training.control_filters.Disturbance_generation import disturbance_generation_batch_gpu
from training.labeling.Adaptive_control_filter_generator_batch import (
    adaptive_control_filter_batch_mimo, train_adaptive_gain_batch_mimo)

# ═══════════════════════════════════════════════════════════════
# 配置
# ═══════════════════════════════════════════════════════════════
FS          = 16000
CHUNK_SEC   = 1.0
CHUNK       = int(CHUNK_SEC * FS)          # 16000
BAND_LO, BAND_HI = 20, 1500                # 与部署带通严格一致 (bandpass_fir.bin)
DEFAULT_OUT = r'D:\Dataset\Synthetic_Dataset'

# 生成谱形族比例 (权重 → 多样性与真实感平衡)
FAMILY_WEIGHTS = {
    'synth_nb':    0.40,   # 窄带: 教子带选择性 (当前模型最缺)
    'synth_bb':    0.30,   # 宽带: 教宽带映射
    'synth_tilt':  0.15,   # 1/f^α 谱倾斜: 贴近真实(路噪/风机)
    'synth_tonal': 0.15,   # 基频+谐波: 施工/铁路强谐波成分
}

# LMS 标注参数 — 与 label_real_noise.py 一致
LMS_MU      = 0.001
LMS_REPET   = 3
BATCH_SIZE  = 128
FX_NOISE_DB = -30
RANDOM_SEED = 42
DEVICE      = 'cuda' if torch.cuda.is_available() else 'cpu'

# 子滤波器基 (与部署一致: broadband)
_SUB_FILTER_FILE = MODELS_DIR / 'MIMO_Pretrained_Control_filters_broadband.mat'
# ═══════════════════════════════════════════════════════════════

_rng = None
_bp20_1500 = None   # 20-1500Hz 带通 FIR (缓存)


def _bandpass20_1500():
    """20-1500Hz FIR (1024tap, hamming) — 与部署 bandpass_fir.bin 同 passband."""
    global _bp20_1500
    if _bp20_1500 is None:
        _bp20_1500 = signal.firwin(1024, [BAND_LO, BAND_HI], pass_zero='bandpass',
                                   window='hamming', fs=FS)
    return _bp20_1500


# ═══════════════════════════════════════════════════════════════
# 谱形生成族 — 全部限制在 [BAND_LO, BAND_HI] 内, 输出 1s 单声道 float32 [-1,1]
# ═══════════════════════════════════════════════════════════════

def _bandlimited(f_star, f_end, n=CHUNK):
    """带限白噪声 [f_star, f_end] (f_end ≤ BAND_HI). MIMO BandlimitedNoise 适配."""
    assert BAND_LO <= f_star < f_end <= BAND_HI, f'bad band [{f_star},{f_end}]'
    len_f = 1024
    b = signal.firwin(len_f, [f_star, f_end], pass_zero='bandpass',
                      window='hamming', fs=FS)
    x = _rng.randn(n + len_f - 1)
    y = signal.lfilter(b, 1, x)
    out = y[len_f - 1:len_f - 1 + n]
    std = np.std(out)
    return out / (std + 1e-12)


def gen_narrowband():
    """窄带: 中心频率 ∈U[40,1480] (覆盖全部署带, 含 1.3-1.5kHz), 带宽 ∈U[20,300]."""
    f_star = _rng.uniform(BAND_LO + 20, BAND_HI - 20)
    bw = _rng.uniform(20, min(300, BAND_HI - f_star - 1))
    return _bandlimited(f_star, f_star + bw)


def gen_broadband():
    """宽带: 带宽 ∈U[300,1300], 起始位置随机覆盖全带."""
    bw = _rng.uniform(300, min(1300, BAND_HI - BAND_LO - 20))
    f_star = _rng.uniform(BAND_LO + 10, BAND_HI - bw - 10)
    return _bandlimited(f_star, f_star + bw)


def gen_tilt():
    """1/f^α 谱倾斜 (α∈[0,2]) → 20-1500Hz 带通. 贴近真实噪声平滑谱."""
    alpha = _rng.uniform(0.0, 2.0)
    x = _rng.randn(CHUNK)
    X = np.fft.rfft(x)
    freq = np.fft.rfftfreq(CHUNK, 1.0 / FS)
    shape = np.ones_like(freq)
    mask = freq > 1.0
    shape[mask] = freq[mask] ** (-alpha / 2.0)                 # 幅值 1/f^(α/2)
    X *= shape
    y = np.fft.irfft(X, CHUNK)
    y = signal.lfilter(_bandpass20_1500(), 1, y)
    std = np.std(y)
    return y / (std + 1e-12)


def gen_tonal():
    """1-3 个基频 f0∈U[40,600] + 2-4 次谐波 + 窄带噪声底 (-20dB)."""
    n_f0 = _rng.randint(1, 4)
    t = np.arange(CHUNK) / FS
    y = np.zeros(CHUNK)
    for _ in range(n_f0):
        f0 = _rng.uniform(40, 600)
        for h in range(1, 5):
            amp = (1.0 / h) * _rng.uniform(0.7, 1.3)
            phase = _rng.uniform(0, 2 * np.pi)
            y += amp * np.sin(2 * np.pi * f0 * h * t + phase)
    y = y / (np.std(y) + 1e-12)
    nb = _bandlimited(40, BAND_HI - 40) * 10 ** (-1.0)          # 噪声底 -20dB
    y = signal.lfilter(_bandpass20_1500(), 1, y + nb)
    std = np.std(y)
    return y / (std + 1e-12)


_GEN_FAMILIES = {
    'synth_nb':    gen_narrowband,
    'synth_bb':    gen_broadband,
    'synth_tilt':  gen_tilt,
    'synth_tonal': gen_tonal,
}


def pick_family():
    fams = list(_GEN_FAMILIES)
    w = np.array([FAMILY_WEIGHTS[f] for f in fams], dtype=float)
    w = w / w.sum()
    return _rng.choice(fams, p=w)


# ═══════════════════════════════════════════════════════════════
# 生成
# ═══════════════════════════════════════════════════════════════

def generate_split(out_dir, n, prefix):
    """生成 n 个合成噪声到 out_dir. 返回 [(filename, family)]. """
    os.makedirs(out_dir, exist_ok=True)
    recs = []
    for i in tqdm(range(n), desc=f'生成 {prefix}', unit='个', leave=False):
        fam = pick_family()
        sig = _GEN_FAMILIES[fam]().astype(np.float32)
        fname = f'{fam}_{prefix}_{i:06d}.wav'
        torchaudio.save(os.path.join(out_dir, fname),
                        torch.from_numpy(sig).unsqueeze(0), FS)
        recs.append((fname, fam))
    return recs


def generate_all(out_root, n_train, n_val, n_test):
    """按族比例分层: 先按 total 生成, 再随机拆 train/val/test."""
    n_total = n_train + n_val + n_test
    fams = list(_GEN_FAMILIES)
    w = np.array([FAMILY_WEIGHTS[f] for f in fams], dtype=float)
    w = w / w.sum()
    # 逐族生成 (保证每族比例精确) → 打乱 → 拆
    all_recs = []
    for fam in fams:
        n_fam = int(round(n_total * w[fams.index(fam)]))
        os.makedirs(os.path.join(out_root, 'Training_data'), exist_ok=True)
        # 直接按族写到临时子目录, 再归并打乱
        tmp = os.path.join(out_root, '_gen_tmp', fam)
        os.makedirs(tmp, exist_ok=True)
        for i in tqdm(range(n_fam), desc=f'生成 {fam}', unit='个', leave=False):
            sig = _GEN_FAMILIES[fam]().astype(np.float32)
            fname = f'{fam}_{i:06d}.wav'
            torchaudio.save(os.path.join(tmp, fname),
                            torch.from_numpy(sig).unsqueeze(0), FS)
            all_recs.append(fname)
    _rng.shuffle(all_recs)
    # 拆 train/val/test
    cnt = {'Training_data': n_train, 'Validate_data': n_val, 'Testing_data': n_test}
    idx = 0
    out_recs = {}
    for split, n in cnt.items():
        split_dir = os.path.join(out_root, split)
        os.makedirs(split_dir, exist_ok=True)
        recs = []
        for fname in all_recs[idx:idx + n]:
            fam = fname.split('_')[0] + '_' + fname.split('_')[1]
            newname = f'{fam}_{split}_{len(recs):06d}.wav'
            os.replace(os.path.join(out_root, '_gen_tmp', fam, fname),
                       os.path.join(split_dir, newname))
            recs.append((newname, fam))
            idx += 1
        out_recs[split] = recs
    # 清理临时
    import shutil
    shutil.rmtree(os.path.join(out_root, '_gen_tmp'), ignore_errors=True)
    return out_recs


# ═══════════════════════════════════════════════════════════════
# LMS 标注 (与 label_real_noise.py 同机制)
# ═══════════════════════════════════════════════════════════════

def _load_paths():
    Pri_path, Sec_path = load_multichannel_paths_with_variable_names(
        folder=str(_PROJECT_ROOT / 'Primary and Secondary Path'), subfolder='',
        Pri_path_file_name='primary_path.npy', Sec_path_file_name='secondary_path.npy')
    return Pri_path, Sec_path


def _load_sub_filters():
    sub = sio.loadmat(str(_SUB_FILTER_FILE))['Wc_v']   # (C=15, S=2, L)
    return torch.from_numpy(sub).type(torch.float)


def label_wavs(wav_files, wav_dir, out_csv, Pri_path, Sec_path, sub_T):
    """对 wav_dir 下全部 1s WAV 做 LMS 标注, 写 CSV. 返回样本数."""
    n = len(wav_files)
    if n == 0:
        print(f'  {out_csv}: 无文件, 跳过')
        return 0
    C, S, _ = sub_T.shape
    SC = C * S
    chunk_len = CHUNK
    rows = []
    gains_arr = np.zeros((n, SC), dtype=np.float32)
    print(f'\n  LMS 标注 {n} 片段 → {os.path.basename(out_csv)}')

    # 批次加载 + LMS
    for bs in tqdm(range(0, n, BATCH_SIZE), desc='  标注', unit='批'):
        be = min(bs + BATCH_SIZE, n)
        ba = be - bs
        batch_chunks = []
        for i in range(bs, be):
            sig, _ = torchaudio.load(os.path.join(wav_dir, wav_files[i]))
            batch_chunks.append(sig.squeeze()[:chunk_len])
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

        for j in range(ba):
            r = {'File_path': wav_files[bs + j], 'category': 'synthetic'}
            for b in range(SC):
                r[f'gain_{b}'] = float(gains_batch[j].flatten()[b])
            rows.append(r)
            gains_arr[bs + j] = gains_batch[j].flatten()

    import pandas as pd
    df = pd.DataFrame(rows)
    df.to_csv(out_csv, index=False)
    np.save(os.path.join(os.path.dirname(out_csv),
                         os.path.basename(out_csv).replace('Index_', 'Gains_')
                         .replace('.csv', '.npy')), gains_arr)
    print(f'  已写: {out_csv} ({n} 片段)')
    return n


# ═══════════════════════════════════════════════════════════════
# Probe — 检查点1: 合成谱形是否覆盖全子带空间
# ═══════════════════════════════════════════════════════════════

def probe(n_probe=300, out_root=None):
    """生成 n_probe 个样本, 投影到 15 子带, 报告多样性/覆盖率.
    与输入谱探针同口径: 子带能量向量归一化和=1, 类型间 cos 应远低于真实噪声(0.90)."""
    print(f'\n=== Probe: 合成谱形覆盖 (n={n_probe}) ===')
    sub = sio.loadmat(str(_SUB_FILTER_FILE))['Wc_v']        # (15, 2, L)
    subs = torch.from_numpy(sub[:, 0, :].copy()).numpy()    # 扬声器0 → (15, L)
    L = subs.shape[1]
    subs = subs / (np.sqrt((subs ** 2).sum(axis=1, keepdims=True)) + 1e-12)

    vecs, fams = [], []
    for _ in tqdm(range(n_probe), desc='生成+投影', leave=False):
        fam = pick_family()
        sig = _GEN_FAMILIES[fam]().astype(np.float32)
        e = []
        for k in range(subs.shape[0]):
            y = signal.fftconvolve(sig, subs[k], mode='valid')
            e.append(float(np.sqrt((y ** 2).mean()) + 1e-12))
        e = np.array(e); e = e / e.sum()
        vecs.append(e); fams.append(fam)
    V = np.array(vecs)
    F = np.array(fams)

    def cos_pair(i, j):
        return float(V[i] @ V[j] / (np.linalg.norm(V[i]) * np.linalg.norm(V[j]) + 1e-12))

    # 随机抽样 pairwise cos (多样性: 越低越多样; 对照: 真实噪声≈0.90-0.94)
    np.random.RandomState(0)
    cs = []
    for _ in range(2000):
        i, j = np.random.choice(n_probe, 2, replace=False)
        cs.append(cos_pair(int(i), int(j)))
    cs = np.array(cs)
    print(f'  逐对 cos: mean={cs.mean():.3f} p5={np.percentile(cs,5):.3f} p50={np.percentile(cs,50):.3f}')
    print(f'  (对照: 真实噪声带通子带能量 类型间 cos mean≈0.90, 类型内 pairwise≈0.94)')

    # 覆盖率: 每子带被"显著激活"(能量占比>10%样本) 的比例
    print('  每子带活跃比例 (能量>该样本总能量 10%):')
    active = (V > 0.10).mean(axis=0)
    for k in range(15):
        bar = '#' * int(active[k] * 50)
        print(f'    band {k:2d}: {active[k]*100:4.0f}% {bar}')
    # band 14 质心 3.9kHz, 20-1500 内能量仅 2% — 部署死带 (输入带通 20-1500), 不参与覆盖率
    n_active = (active[:14] > 0.20).sum()
    print(f'  覆盖率 = {100*n_active/14:.0f}% (band 0-13 活跃>20% 的 {n_active}/14; '
          f'band 14 为部署死带 3.9kHz, 预期 0)')

    # 逐族均值子带分布
    print('  逐族均值子带能量分布 (前8带):')
    for fam in _GEN_FAMILIES:
        v = V[F == fam]
        if len(v) == 0: continue
        m = v.mean(axis=0)
        print(f'    {fam:12s}: ' + ' '.join(f'{m[i]:.2f}' for i in range(15)))


# ═══════════════════════════════════════════════════════════════
# 主入口
# ═══════════════════════════════════════════════════════════════

def main():
    global _rng
    ap = argparse.ArgumentParser(description='合成噪声数据集 + LMS 标注')
    ap.add_argument('--n-train', type=int, default=60000)
    ap.add_argument('--n-val',   type=int, default=7500)
    ap.add_argument('--n-test',  type=int, default=7500)
    ap.add_argument('--out',     default=DEFAULT_OUT, help='输出根目录')
    ap.add_argument('--seed',    type=int, default=RANDOM_SEED)
    ap.add_argument('--gen-only',  action='store_true', help='只生成 WAV, 不 LMS 标注')
    ap.add_argument('--label-only', action='store_true', help='只对已生成 WAV 做 LMS 标注')
    ap.add_argument('--probe',    action='store_true', help='生成后跑谱形覆盖 probe')
    ap.add_argument('--probe-only', action='store_true', help='只跑 probe (不写数据集)')
    ap.add_argument('--n-probe',  type=int, default=300)
    args = ap.parse_args()

    _rng = np.random.RandomState(args.seed)
    torch.manual_seed(args.seed)

    if args.probe_only:
        probe(args.n_probe)
        return

    out_root = args.out
    print('=' * 60)
    print(f'  合成噪声数据集构造')
    print(f'  输出: {out_root}')
    print(f'  设备: {DEVICE}')
    print('=' * 60)

    if not args.label_only:
        # 生成
        recs = generate_all(out_root, args.n_train, args.n_val, args.n_test)
        print('\n  生成完成:')
        for split, r in recs.items():
            from collections import Counter
            c = Counter(x[1] for x in r)
            print(f'    {split}: {len(r)} 个 ({dict(c)})')

    if args.gen_only:
        print('\n  --gen-only: 跳过 LMS 标注. 下一步: python make_synthetic_dataset.py --label-only')
        return

    # LMS 标注
    Pri_path, Sec_path = _load_paths()
    sub_T = _load_sub_filters()
    print(f'\n  子滤波器: {sub_T.shape} (C,S,L), 路径: {_SUB_FILTER_FILE}')
    total = 0
    for split in ['Training_data', 'Validate_data', 'Testing_data']:
        wav_dir = os.path.join(out_root, split)
        wavs = sorted([f for f in os.listdir(wav_dir) if f.endswith('.wav')])
        if not wavs:
            print(f'  {split}: 无 WAV, 跳过'); continue
        out_csv = os.path.join(out_root, f'Index_synth_{split.replace("_data","")}_data.csv')
        total += label_wavs(wavs, wav_dir, out_csv, Pri_path, Sec_path, sub_T)
    print(f'\n  完成: 共标注 {total} 个合成样本')
    print('  下一步: 训练 (预训练用合成 → 微调用真实)')


if __name__ == '__main__':
    main()
