"""generate_bank.py — 离线生成 SFANC 硬选库 (N 条固定滤波器) + 分类 CNN 打标签.

与 SFANC-Window / MIMO-SFANC 同款, **无逐场景实机标定**:
  N 段代表性噪声 → 逐段离线 FxLMS (经实测 Pri/Sec 路径) 收敛 → N 条全频段成品滤波器
  → 写 data/wc_bank.bin (GFNC 16B 头 + N×(S×L) float32, 与 C 端 scene_bank.c 一致).

无语义场景 — 槽 k 就是"第 k 条滤波器", 不叫 road/children/... .

用法:
  A) 生成滤波器 (N 段噪声 → N 槽):
     python export/generate_bank.py --filters f1.wav f2.wav f3.wav -o data/wc_bank.bin
     --filters 的文件 = 你自己准备的噪声录音 (想消什么噪声就喂什么), 脚本不生成、仓库不附带;
     每段噪声经离线 FxLMS 收敛出一条全频段成品滤波器, 写一个库槽。可先用
     "Noise Examples/" 下的真实噪声样本试跑 (文件名带空格要加引号)。
  B) 打分打标签 (给分类 CNN 训通用 N 类): 每段 1s 噪声对 N 条候选滤波器算残差功率,
     argmin 当标签 (MIMO-SFANC generate_dataset_v2 法):
     python export/generate_bank.py --labels --wav-dir <含wav目录> -o data/wc_bank.bin
  A+B 一起 (先生成再打分):
     python export/generate_bank.py --filters f1.wav ... --labels --wav-dir <dir> -o data/wc_bank.bin

  与 SFANC-Window 同款: 滤波器只需 N 段代表噪声 (每条是优化拟合, 非监督学习);
  大语料只给分类 CNN 判别器 (train_real_bank_cnn.py) 用。

符号约定 (对齐 C 端): C 部署 residual = d + Sec(Wc⊗x) (fxnlms_mimo.c err_out).
离线 FxNLMS (FxNLMS_MIMO) 收敛 Wc 满足 Sec(Wc⊗x) ≈ +d (err = d - y),
故存槽时取反 — 使 Sec(Wc_slot⊗x) ≈ -d, 与 real-time 标定保存的 bank 同符号.

绝对增益: 库槽 = 离线收敛 Wc 原样写入 (含完整振幅, 不 RMS 归一化). 部署前用
  `main.exe GFANC_OPEN_LOOP` 验证 NR_true — 若整库幅度整体偏差, 用 --gain-scale 缩放.

重计算 (每段 ~train-sec 的 MIMO FxLMS 训练 + 打分) 建议留给用户自己跑.
"""
import os, sys, struct, argparse, json
from pathlib import Path
import numpy as np
import torch
import torchaudio
from tqdm import tqdm

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..')))
_PY_PROJ = _PROJECT_ROOT / 'SceneZone_Scene'
sys.path.insert(0, str(_PY_PROJ))

from training.control_filters.path_loader import load_multichannel_paths_with_variable_names
from training.control_filters.Disturbance_generation import disturbance_generation_batch_gpu
from training.control_filters.FxNLMS_algorithm import FxNLMS_MIMO

# ═══════════════════════════════════════════════════════════════
# 常量 — 与 C 端 / 训练管线对齐
# ═══════════════════════════════════════════════════════════════
FS         = 16000
LEN_CTRL   = 1024            # 控制滤波器长度 (C: L=1024)
TRAIN_SEC  = 60.0            # 每段滤波器的离线训练时长 (秒, 重复噪声至该长度)
MU         = 0.05            # FxNLMS 步长 (float64 + sum norm, 同 Pre_training)
PRI_FILE   = 'primary_path.npy'
SEC_FILE   = 'secondary_path.npy'

BANK_MAGIC   = 0x434E4647    # 磁盘字节 "GFNC" (LE) — 同 C SCENE_BANK_MAGIC
BANK_VERSION = 1
BANK_HEADER_SIZE = 16        # magic + version + n_slots + slot_len


def _load_paths():
    Pri, Sec = load_multichannel_paths_with_variable_names(
        folder=str(_PY_PROJ / 'Primary and Secondary Path'), subfolder='',
        Pri_path_file_name=PRI_FILE, Sec_path_file_name=SEC_FILE)
    return Pri, Sec          # (E,R,L), (E,S,L)


def _load_mono_16k(path, n_sec=None):
    """加载 WAV → 单声道 16k float32; 可选截取/循环到 n_sec 秒."""
    sig, sr = torchaudio.load(path)
    if sig.shape[0] > 1:
        sig = sig.mean(dim=0, keepdim=True)
    if sr != FS:
        sig = torchaudio.functional.resample(sig, sr, FS)
    w = sig.squeeze().cpu().numpy().astype(np.float32)
    if n_sec is not None:
        n_want = int(n_sec * FS)
        if len(w) < n_want:
            w = np.tile(w, int(np.ceil(n_want / len(w))))[:n_want]
        else:
            w = w[:n_want]
    return w


def _gen_disturbance_fx(wave, Pri, Sec, fs=FS, repet=0):
    """wave: 1D float32 → Dis (E,T) / Fx (E,S,T) (CPU float64)."""
    w_t = torch.from_numpy(np.asarray(wave, dtype=np.float32))
    Dis, Fx, T_out = disturbance_generation_batch_gpu(
        [w_t], Pri, Sec, fs=fs, Repet=repet)
    return (Dis[0].cpu().numpy().astype(np.float64),
            Fx[0].cpu().numpy().astype(np.float64))


# ── A. 单段噪声 → 一条全频段成品滤波器 ─────────────────────────
def train_filter_for_noise(wave, Pri, Sec, E, S,
                           len_ctrl=LEN_CTRL, train_sec=TRAIN_SEC, mu=MU):
    """离线 MIMO FxLMS: 噪声过实测路径 → 收敛 Wc (S,Len).

    wave: 1D 16k float32 numpy (已裁/循环到 train_sec). 归一化
    (同 Pre_training_broadband_and_decompose.py): Fx/dis 同除 fx_rms —
    sum-norm 下缩放互抵, Wc 幅度不变 (只稳定步长). 返回负号库槽 (C 部署符号).
    """
    assert isinstance(wave, np.ndarray), 'wave 需为 1D numpy (先 _load_mono_16k)'
    Dis, Fx = _gen_disturbance_fx(wave, Pri, Sec)

    fx_rms = float(np.sqrt(np.mean(Fx ** 2)))
    if fx_rms < 1e-12:
        raise RuntimeError(f'Fx RMS≈0, 噪声无声?')
    Fx = Fx / fx_rms
    Dis = Dis / fx_rms

    ctrl = FxNLMS_MIMO(Len=len_ctrl, E=E, S=S, mu=mu,
                       power_norm='sum', dtype=np.float64)
    ctrl.train(Fx, Dis, show_progress=False)
    Wc = ctrl.get_coefficients().astype(np.float64)   # (S, Len)
    return -Wc.astype(np.float32)                     # C 部署符号


# ── B. 整库落盘 (GFNC 格式, 同 C scene_bank_write_all) ─────────
def write_bank(path, filters, S, L):
    """filters: list of (S,L) float32 → data/wc_bank.bin (N 槽)."""
    n = len(filters)
    slot_len = S * L
    data = np.stack([np.asarray(f, dtype=np.float32) for f in filters])
    data = data.reshape(n, slot_len)
    with open(path, 'wb') as f:
        f.write(struct.pack('<IIII', BANK_MAGIC, BANK_VERSION, n, slot_len))
        f.write(data.tobytes())
    return n, slot_len


def write_bank_info(path, filters_paths, n, slot_len, gain_scale, note=''):
    info = {
        'n_slots': n, 'slot_len': slot_len,
        'S': 2, 'L': LEN_CTRL, 'fs': FS,
        'format': 'GFNC 16B 头 + n_slots × (S*L) float32 (同 C scene_bank.c)',
        'source_wavs': [str(p) for p in filters_paths],
        'gain_scale': float(gain_scale),
        'note': note,
    }
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(info, f, indent=2, ensure_ascii=False)


# ── C. 打分打标签 (分类 CNN 训练数据) ─────────────────────────
def _conv_fft_signal(x, h):
    """沿最后一维 FFT 卷积 y[n] = Σ_k h[k]·x[n-k] (same-length 截断).
    x: (E, T), h: (L,) → (E, T). n_fft 由长度推导 (线性卷积, 无循环混叠)."""
    n_fft = x.shape[-1] + h.shape[-1] - 1
    X = torch.fft.rfft(x, n=n_fft)
    H = torch.fft.rfft(h, n=n_fft)
    return torch.fft.irfft(X * H, n=n_fft)[..., :x.shape[-1]]


def score_slot_for_clip(wave, filters, Pri, Sec, fs=FS, trim_samples=LEN_CTRL):
    """对一段 1s 噪声, 每条候选滤波器 (C 符号) 算稳态残差功率 → argmin 索引.

    residual_k = Σ_e Σ_n>=trim || d_e[n] + Sec(Wc_k⊗x)[e,n] ||²
    (Dis/Fx 从 GPU 拉回 CPU — disturbance_generation_batch_gpu 返回设备张量)
    """
    w_t = torch.from_numpy(np.asarray(wave, dtype=np.float32))
    Dis, Fx, T_out = disturbance_generation_batch_gpu([w_t], Pri, Sec, fs=fs, Repet=0)
    # float64 全精度残差 (float32 FFT 在两条滤波器残差接近时可能翻转 argmin)
    d = Dis[0].cpu().double()                        # (E, T)
    fx = Fx[0].cpu().double()                        # (E, S, T)
    E, _, T = fx.shape
    T = min(T, d.shape[-1])
    N = len(filters)

    resid = torch.zeros(N, dtype=torch.float64)
    for k, Wc in enumerate(filters):
        Wc = torch.from_numpy(np.asarray(Wc, dtype=np.float64))
        y = torch.zeros(E, T, dtype=torch.float64)
        for s in range(Wc.shape[0]):
            y += _conv_fft_signal(fx[:, s, :T], Wc[s])
        e = d[:, :T] + y                             # C 符号: residual = d + anti
        resid[k] = e[:, trim_samples:].pow(2).sum().item()
    return int(torch.argmin(resid).item()), resid.tolist()


def score_corpus(wav_dir, filters, Pri, Sec, out_csvs, fs=FS, valid_frac=0.15, seed=0):
    """扫描 wav_dir 全部 *.wav (取前 1s), 逐段打 best-filter 标签. 写 train/valid CSV."""
    files = sorted(p for p in Path(wav_dir).rglob('*.wav'))
    if not files:
        raise SystemExit(f'ERROR: {wav_dir} 下无 *.wav')
    rng = np.random.RandomState(seed)
    perm = rng.permutation(len(files))
    n_valid = int(len(files) * valid_frac)
    val_idx = set(perm[:n_valid].tolist())

    rows = []
    for i, p in enumerate(tqdm(files, desc='打分', unit='段')):
        wave = _load_mono_16k(str(p), n_sec=1.0)
        if len(wave) < FS:
            continue
        label, resid = score_slot_for_clip(wave, filters, Pri, Sec, fs=fs)
        rows.append((str(p), label))

    n = len(rows)
    K = max(r[1] for r in rows) + 1
    train_rows = [r for i, r in enumerate(rows) if i not in val_idx]
    valid_rows = [r for i, r in enumerate(rows) if i in val_idx]

    import pandas as pd
    for csv_path, sub in ((out_csvs[0], train_rows), (out_csvs[1], valid_rows)):
        df = pd.DataFrame(sub, columns=['File_path', 'filter_idx'])
        df.to_csv(csv_path, index=False)
        print(f'  已写 {csv_path}: {len(sub)} 段, K={K}')
    # 分布统计
    import collections
    cnt = collections.Counter(r[1] for r in rows)
    print(f'  标签分布 (filter_idx: 样本数): {dict(sorted(cnt.items()))}')
    return K


# ═══════════════════════════════════════════════════════════════
# main
# ═══════════════════════════════════════════════════════════════
def main():
    ap = argparse.ArgumentParser(
        description='离线生成 SFANC 硬选库 (N 条固定滤波器) + 分类 CNN 打标签')
    ap.add_argument('--filters', nargs='+', default=None,
                    help='N 段代表性噪声 WAV, 每段 → 一条滤波器 (库槽 k)')
    ap.add_argument('--labels', action='store_true',
                    help='对 --wav-dir 下 1s 噪声打分, 写分类 CNN 训练标签')
    ap.add_argument('--wav-dir', default=None, help='打分语料目录 (任意噪声, 无需语义类别)')
    ap.add_argument('-o', '--out', default=str(_PROJECT_ROOT / 'data' / 'wc_bank.bin'),
                    help='库输出路径 (默认 data/wc_bank.bin)')
    ap.add_argument('--info', default=None,
                    help='库信息 json (默认 <out 同目录>/wc_bank_info.json)')
    ap.add_argument('--gain-scale', type=float, default=1.0,
                    help='整库振幅缩放 (部署验证 NR_true 后按需调)')
    ap.add_argument('--train-sec', type=float, default=TRAIN_SEC,
                    help='每段滤波器离线训练时长 (秒, 默认 60)')
    ap.add_argument('--valid-frac', type=float, default=0.15,
                    help='打分切分的验证集比例')
    args = ap.parse_args()

    if not args.filters and not args.labels:
        raise SystemExit('ERROR: 至少给 --filters 或 --labels')

    Pri, Sec = _load_paths()
    E, R, L_pri = Pri.shape
    E_s, S, L_sec = Sec.shape
    assert E == E_s, 'Pri/Sec 误差通道数不一致'
    print('=' * 60)
    print(f'  离线 SFANC 硬选库生成 (无场景语义)')
    print(f'  路径: Pri({E},{R},{L_pri}) Sec({E},{S},{L_sec})  设备: '
          f'{torch.cuda.get_device_name(0) if torch.cuda.is_available() else "cpu"}')
    print('=' * 60)

    filters = []
    filter_paths = []
    if args.filters:
        for k, p in enumerate(args.filters):
            print(f'\n[滤波器 {k}/{len(args.filters)}] {p}')
            wave = _load_mono_16k(p, n_sec=args.train_sec)
            Wc = train_filter_for_noise(wave, Pri, Sec, E, S, train_sec=args.train_sec)
            filters.append(Wc)
            filter_paths.append(p)
            print(f'  Wc RMS = {np.sqrt(np.mean(Wc**2)):.6f} (S={S}, L={Wc.shape[1]})')
        if args.gain_scale != 1.0:
            filters = [f * args.gain_scale for f in filters]
        n, slot_len = write_bank(args.out, filters, S, LEN_CTRL)
        info_path = args.info or str(Path(args.out).parent / 'wc_bank_info.json')
        write_bank_info(info_path, filter_paths, n, slot_len, args.gain_scale,
                        note='离线 FxLMS 收敛成品 (绝对增益, 符号 = C 部署)。'
                             '部署前用 main.exe GFANC_OPEN_LOOP 验 NR_true。')
        print(f'\n  库已写: {args.out} (N={n} 槽, slot_len={slot_len})')
        print(f'  信息: {info_path}')

    if args.labels:
        if not filters:
            raise SystemExit('ERROR: --labels 需先有库 — 同一命令给 --filters, '
                             '或去掉 --filters 前先跑一次生成')
        if not args.wav_dir:
            raise SystemExit('ERROR: --labels 需要 --wav-dir (打分语料)')
        out_dir = Path(args.out).parent
        K = score_corpus(args.wav_dir, filters, Pri, Sec,
                         [str(out_dir / 'bank_labels_train.csv'),
                          str(out_dir / 'bank_labels_valid.csv')],
                         valid_frac=args.valid_frac)
        print(f'\n  分类 CNN 训练标签就绪 (K={K})。下一步:')
        print(f'    python SceneZone_Scene/training/network/train_real_bank_cnn.py')


if __name__ == '__main__':
    main()
