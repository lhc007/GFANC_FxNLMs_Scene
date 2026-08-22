"""generate_synthetic_noise.py — 合成噪声生成器 (SFANC-Window 同款, 按频带定类).

背景 (SFANC-Window, Luo-Zhengding 2024 MSSP):
  CNN 分类器用合成噪声训练 — "filtering white noise through various bandpass
  filters with randomly chosen center frequencies and bandwidths". 类 = 频带:
  每类一个频率范围, 训练样本 = 白噪过该类范围内随机带通, 标签 = 类 k.
  部署时 CNN 把当前噪声按谱形分到某个频带类 → 选该类对应库槽滤波器.

本脚本为 SFANC 硬选库生成两样东西:
  1) 每类 1 段代表性宽带噪声  band_k.wav  → 喂 generate_bank.py --filters
     (库槽 k 用频带 k 的宽带噪声离线 FxLMS 生成, 类 k ↔ 槽 k 语义对齐).
  2) 每类 N 段 1s 训练样本      cls_k/*.wav  + bank_labels_{train,valid}.csv
     → 直接喂 train_real_bank_cnn.py (列 File_path, filter_idx, 无语义类名).

用法:
  python export/generate_synthetic_noise.py
      --n-classes 4 --clips-per-class 2000

输出:
  data/synth_noise/band_0.wav ... band_{N-1}.wav   (代表性宽带, 每类 10s)
  data/synth_noise/cls_k/0000.wav ...              (1s 训练样本)
  data/bank_labels_train.csv / bank_labels_valid.csv  (train_real_bank_cnn.py 默认读)

重计算 (离线 FxLMS 生成库槽 + CNN 训练) 留给用户自己跑, 本脚本只合成噪声.
"""
import os, sys, json, csv, argparse
from pathlib import Path
import numpy as np
import scipy.signal as sps
import scipy.io.wavfile as wavfile

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

FS       = 16000
BAND_LOW = 50.0    # 与 gen_bandpass_fir.py --f-low 50 对齐 (ANC 关注频段)
BAND_HIGH = 1500.0 # 与 --f-high 1500 对齐
ORDER    = 4       # butter 带通阶数
RMS_TARGET = 0.05  # 合成噪声目标 RMS (minmax 归一化会再重标定, 这里只求不过载)


def log_bands(n, lo=BAND_LOW, hi=BAND_HIGH):
    """对数分频: N 个频带覆盖 [lo, hi] (低频更密, 贴合 ANC 低频为主)."""
    r = (hi / lo) ** (1.0 / n)
    return [(lo * r ** k, lo * r ** (k + 1)) for k in range(n)]


def bandpass_noise(lo, hi, sec, rng):
    """白噪 → butter 带通 [lo,hi] → 归一化到 RMS_TARGET. 返回 float32 1D."""
    n = int(sec * FS)
    x = rng.randn(n)
    sos = sps.butter(ORDER, [lo, hi], btype='bandpass', fs=FS, output='sos')
    y = sps.sosfilt(sos, x)
    rms = np.sqrt(np.mean(y ** 2))
    if rms < 1e-12:
        y = y * 0.0
    else:
        y = y * (RMS_TARGET / rms)
    return y.astype(np.float32)


def main():
    ap = argparse.ArgumentParser(
        description='合成噪声生成器 (SFANC-Window 同款): 每类代表性宽带 + N 段训练样本')
    ap.add_argument('--n-classes', type=int, default=4,
                    help='类数 N (== 库槽数 == CNN K, 默认 4)')
    ap.add_argument('--clips-per-class', type=int, default=2000,
                    help='每类训练样本数 (默认 2000; 每段 1s 16k int16 ≈ 32KB, '
                         '4 类 2000/类 ≈ 256MB 磁盘)')
    ap.add_argument('--represent-sec', type=float, default=10.0,
                    help='每类代表性宽带时长 (秒, 喂 generate_bank.py --filters, 会被循环到 train_sec)')
    ap.add_argument('--band-low', type=float, default=BAND_LOW, help='整体频带下限 Hz')
    ap.add_argument('--band-high', type=float, default=BAND_HIGH, help='整体频带上限 Hz')
    ap.add_argument('--valid-frac', type=float, default=0.15, help='验证集比例')
    ap.add_argument('--seed', type=int, default=0, help='随机种子 (可复现)')
    ap.add_argument('--out-dir', default=None,
                    help='样本输出目录 (默认 <项目根>/data/synth_noise)')
    ap.add_argument('--labels-dir', default=None,
                    help='标签 CSV 输出目录 (默认 <项目根>/data, train_real_bank_cnn.py 默认读)')
    args = ap.parse_args()

    script_dir = Path(os.path.dirname(os.path.abspath(__file__)))
    root = script_dir.parent
    out_dir = Path(args.out_dir) if args.out_dir else root / 'data' / 'synth_noise'
    labels_dir = Path(args.labels_dir) if args.labels_dir else root / 'data'
    out_dir.mkdir(parents=True, exist_ok=True)

    N = args.n_classes
    if not (2 <= N <= 30):
        raise SystemExit(f'ERROR: --n-classes 需在 2..30 (CNN_M5_OUT_MAX), 当前 {N}')
    bands = log_bands(N, args.band_low, args.band_high)
    rng = np.random.RandomState(args.seed)

    print('=' * 60)
    print(f'  合成噪声生成 (SFANC-Window 同款, 按频带定类)')
    print(f'  {N} 类, 对数分频覆盖 [{args.band_low:.0f}, {args.band_high:.0f}] Hz, '
          f'{args.clips_per_class} 段/类, seed={args.seed}')
    print('=' * 60)
    for k, (lo, hi) in enumerate(bands):
        print(f'  类 {k}: [{lo:.0f}, {hi:.0f}] Hz')

    rows = []          # (File_path, filter_idx)
    for k, (lo, hi) in enumerate(bands):
        # 1) 代表性宽带 (类 k 的整段频带) — 给 generate_bank.py --filters
        rep = bandpass_noise(lo, hi, args.represent_sec, rng)
        rep_path = out_dir / f'band_{k}.wav'
        wavfile.write(str(rep_path), FS, (rep * 32767).astype(np.int16))
        print(f'\n[类 {k}] 代表性宽带 → {rep_path} ({args.represent_sec:.0f}s)')

        # 2) 训练样本: 白噪 → 类频带内随机带通 → 1s
        cls_dir = out_dir / f'cls_{k}'
        cls_dir.mkdir(parents=True, exist_ok=True)
        for i in range(args.clips_per_class):
            # 随机子带 [c_lo, c_hi] ⊂ [lo, hi], 宽度 ≥ 10% 带宽
            min_bw = 0.1 * (hi - lo)
            c_lo = rng.uniform(lo, hi - min_bw)
            c_hi = rng.uniform(c_lo + min_bw, hi)
            clip = bandpass_noise(c_lo, c_hi, 1.0, rng)
            p = cls_dir / f'{i:04d}.wav'
            wavfile.write(str(p), FS, (clip * 32767).astype(np.int16))
            rows.append((str(p), k))
    print(f'\n  合成完成: {N} 类 × {args.clips_per_class} 段 = {len(rows)} 段')

    # 训练/验证切分: 每类前 valid_frac 段进验证集 (类均衡)
    n_val = int(args.clips_per_class * args.valid_frac)
    train_rows, valid_rows = [], []
    for k in range(N):
        cls_rows = [r for r in rows if r[1] == k]
        valid_rows += cls_rows[:n_val]
        train_rows += cls_rows[n_val:]
    tr_path = labels_dir / 'bank_labels_train.csv'
    va_path = labels_dir / 'bank_labels_valid.csv'
    for path, sub in ((tr_path, train_rows), (va_path, valid_rows)):
        with open(path, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['File_path', 'filter_idx'])
            w.writerows(sub)
        print(f'  已写 {path}: {len(sub)} 段')
    print(f'  K = {N} (标签 0..{N-1})')

    info = {
        'n_classes': N, 'clips_per_class': args.clips_per_class,
        'bands_hz': [list(map(float, b)) for b in bands],
        'band_low': args.band_low, 'band_high': args.band_high,
        'represent_sec': args.represent_sec,
        'fs': FS, 'order': ORDER, 'rms_target': RMS_TARGET,
        'seed': args.seed,
        'method': 'SFANC-Window: white noise -> random bandpass within class band',
    }
    info_path = out_dir / 'synth_noise_info.json'
    with open(info_path, 'w', encoding='utf-8') as f:
        json.dump(info, f, indent=2, ensure_ascii=False)
    print(f'  信息: {info_path}')

    # 下一步命令链
    rep_wavs = ' '.join(f'"{out_dir / f"band_{k}.wav"}"' for k in range(N))
    print('\n  下一步 (按顺序, 重计算由你跑):')
    print(f'    1. 生成库槽 (类 k ↔ 槽 k, 顺序即槽序):')
    print(f'       python export/generate_bank.py --filters {rep_wavs} -o data/wc_bank.bin')
    print(f'    2. 训分类 CNN (读上面的标签 CSV):')
    print('       python SceneZone_Scene/training/network/train_real_bank_cnn.py')
    print('    3. 导出 C 权重 + 刷新批次指纹:')
    print('       python export/export_bin.py')
    print('\n  部署时类 k 的噪声 → CNN 分到类 k → 选槽 k (与 SFANC-Window 同构).')


if __name__ == '__main__':
    main()
