"""
谱可分性前置验证 (Phase 3 前置, 计划风险 #1 缓解) — 4 类真实噪声分类可行性检查.

在训练 4 类分类 CNN 之前, 用 verify_discrimination.py 的 nearest-mean 协议
度量 4 类真实场景 (road/children/construction/railway) 的输入谱可分性.
若可分 → 训练 4 类分类器; 否则并类到 N=2-3.

输入谱特征与 C 端 CNN 输入管道严格对齐:
  1s WAV → bandpass 50-1500Hz (bandpass_fir.mat) → 特征:
    (a) 15 子带能量 — MIMO_Pretrained_Control_filters_broadband.mat 子滤波器投影
        (归一化到单位范数, 与 verify_discrimination.py 同口径), 归 1 到和为 1
    (b) 64-bin 对数谱 (密谱) — 排除 15 子带投影本身是瓶颈的可能

指标:
  nearest_mean_acc — 逐窗口 → 最近类型质心 (cos), verify_discrimination.py 同协议
  gap_metric       — 类型内 cos − 类型间 cos (区分度间隙)
  per_class        — 逐类准确率 (暴露"某一类不可分"的弱类)
  MLP 5-fold CV    — 非线性分类可行性 (nearest-mean 是悲观下限, CNN ≥ MLP)
  MLP 混淆矩阵    — 诊断哪两类互混 (预期 construction↔railway)

判定 (计划阈值 nearest-mean ≥70%):
  [PASS] nearest-mean ≥70%           → 训练 4 类
  [PASS] MLP ≥70% 但 nearest-mean <70 → 谱可分 (nearest-mean 悲观下限), 训练 4 类
  [FAIL] 两者都 <70%                  → 并类到 N=2-3

用法:
    python training/network/verify_discrimination_bank.py [--samples-per-class 150]
"""
import sys, os, argparse
from pathlib import Path

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

import numpy as np
import pandas as pd
import scipy.signal as signal
import scipy.io as sio
import torch
import torchaudio

from noise_dataset import make_bandpass_tensor, prepare_batch as _pbatch
from gfanc._paths import MODELS_DIR

SAMPLE_LEN = 16000
_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_fir.mat'
_SUB_FILTER = MODELS_DIR / 'MIMO_Pretrained_Control_filters_broadband.mat'

# 训练集索引 + 音频根
REAL_CSV   = r'D:\Dataset\Real_world_Dataset\Index_real_Training_data.csv'
TRAIN_WAV  = r'D:\Dataset\Real_world_Dataset\Training_data'

# 4 类有序定义 = 库槽序 (与 scene_definitions_bank.json / 训练硬标签 / C 类名表 三处一致)
CLASSES = ['road', 'children', 'construction', 'railway']


def bandpass_window(win, bp_w, bp_pad):
    """窗口 → 带通 50-1500Hz 单声道 numpy (与 C 端 CNN 输入一致)."""
    x = torch.from_numpy(win).float().unsqueeze(0).unsqueeze(0).to(bp_w.device)
    return _pbatch(x, bp_w, bp_pad).squeeze().cpu().numpy()


def feature_subband(bp, subs):
    """带通窗口 → 15 子带能量 (归1). subs 已归一化到单位范数. """
    e = []
    for k in range(subs.shape[0]):
        y = signal.fftconvolve(bp, subs[k], mode='valid')
        e.append(float(np.sqrt((y ** 2).mean()) + 1e-12))
    e = np.array(e)
    return e / (e.sum() + 1e-12)


def feature_logspec(bp, freqs, bands):
    """带通窗口 → 64-bin 对数谱能量 (归1). 密谱交叉验证用. """
    _, psd = signal.periodogram(bp, fs=16000)
    idx = np.searchsorted(freqs, bands)
    e = np.array([np.sqrt(np.trapezoid(psd[idx[i]:idx[i + 1]] + 1e-12))
                  for i in range(len(bands) - 1)])
    return e / (e.sum() + 1e-12)


def nearest_mean_acc(X, y):
    """最近均值分类 (cos). 返回 (acc, per_type)."""
    types = np.unique(y)
    cents = {}
    for t in types:
        c = X[y == t].mean(axis=0)
        cents[t] = c / (np.linalg.norm(c) + 1e-12)
    Cs = np.array([cents[t] for t in types])
    Xn = X / (np.linalg.norm(X, axis=1, keepdims=True) + 1e-12)
    sims = Xn @ Cs.T
    pred = types[sims.argmax(axis=1)]
    acc = float((pred == y).mean())
    per = {t: float((pred[y == t] == t).mean()) for t in types}
    return acc, per


def gap_metric(X, y, n_pairs=4000):
    """区分度间隙 = 类型内 cos − 类型间 cos."""
    rng = np.random.RandomState(0)
    Xn = X / (np.linalg.norm(X, axis=1, keepdims=True) + 1e-12)
    intra, inter = [], []
    N = len(y)
    for _ in range(n_pairs):
        i, j = rng.randint(0, N, 2)
        c = float(Xn[i] @ Xn[j])
        (intra if y[i] == y[j] else inter).append(c)
    return float(np.mean(intra) - np.mean(inter)), np.mean(intra), np.mean(inter)


def mlp_cv(X, labels, n_splits=5):
    """MLP 5-fold CV → (mean, std, preds_all, y_all)."""
    from sklearn.model_selection import StratifiedKFold
    from sklearn.neural_network import MLPClassifier
    from sklearn.preprocessing import StandardScaler
    skf = StratifiedKFold(n_splits=n_splits, shuffle=True, random_state=0)
    cvs, preds_all, y_all = [], [], []
    for tr, te in skf.split(X, labels):
        sc = StandardScaler().fit(X[tr])
        mlp = MLPClassifier(hidden_layer_sizes=(32,), max_iter=500, random_state=0)
        mlp.fit(sc.transform(X[tr]), labels[tr])
        p = mlp.predict(sc.transform(X[te]))
        cvs.append(mlp.score(sc.transform(X[te]), labels[te]))
        preds_all.append(p); y_all.append(labels[te])
    return (float(np.mean(cvs)), float(np.std(cvs)),
            np.concatenate(preds_all), np.concatenate(y_all))


def main():
    ap = argparse.ArgumentParser(description='Phase 3 前置: 4 类真实噪声谱可分性')
    ap.add_argument('--samples-per-class', type=int, default=150,
                    help='每类采样窗口数 (默认 150, 共 600)')
    args = ap.parse_args()

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print('=' * 60)
    print('  谱可分性前置验证 — 4 类真实噪声')
    print(f'  类序: {CLASSES}')
    print(f'  设备: {device}')
    print('=' * 60)

    bp_w, bp_pad = make_bandpass_tensor(_BP_PATH, device)
    sub = sio.loadmat(str(_SUB_FILTER))['Wc_v']            # (C=15, S=2, L)
    subs = sub[:, 0, :].copy()                             # 扬声器0 → (15, L)
    subs = subs / (np.linalg.norm(subs, axis=1, keepdims=True) + 1e-12)

    # 64-bin logspec 频率网格 (50-1500Hz)
    freqs = signal.periodogram(np.zeros(SAMPLE_LEN), fs=16000)[0]
    nb = 64
    bands = np.linspace(50, 1500, nb + 1)

    df = pd.read_csv(REAL_CSV)
    rng = np.random.RandomState(0)
    X15, X64, y = [], [], []
    for cname in CLASSES:
        paths = df[df['category'] == cname]['File_path'].values
        sel = rng.choice(paths, args.samples_per_class, replace=False)
        n_ok = 0
        for rel in sel:
            try:
                w, sr = torchaudio.load(os.path.join(TRAIN_WAV, rel))
            except Exception:
                continue
            w = w.mean(0).numpy()                          # 混单声道 (与数据集一致)
            if w.shape[0] < SAMPLE_LEN:
                w = np.pad(w, (0, SAMPLE_LEN - w.shape[0]))
            else:
                w = w[:SAMPLE_LEN]
            bp = bandpass_window(w, bp_w, bp_pad)
            X15.append(feature_subband(bp, subs))
            X64.append(feature_logspec(bp, freqs, bands))
            y.append(cname)
            n_ok += 1
        print(f'  {cname:12s}: {n_ok}/{args.samples_per_class} 窗口')
    X15, X64, y = map(np.array, (X15, X64, np.array(y)))
    labels = np.array([CLASSES.index(c) for c in y])
    print(f'\n  窗口集: {len(y)}, {len(np.unique(y))} 类')

    # ── 15 子带特征 (C 端 CNN 同口径) ────────────────────────
    print('  ' + '-' * 46)
    print('  特征 (a): 15 子带能量 (verify_discrimination.py 同口径)')
    acc, per = nearest_mean_acc(X15, y)
    gap, intra, inter = gap_metric(X15, y)
    print(f'    nearest-mean: {acc*100:5.1f}%  gap={gap:+.3f} '
          f'(内{intra:.3f} 间{inter:.3f})')
    print(f'    逐类: ' + ' '.join(f'{k}={v*100:.0f}%' for k, v in per.items()))
    m15, s15, p15, y15 = mlp_cv(X15, labels)
    print(f'    MLP 5-fold CV: {m15*100:5.1f}% ± {s15*100:.1f}%')
    print('    混淆 (行=真, 列=预测):')
    print('         ' + ' '.join(f'{c[:5]:>6s}' for c in CLASSES))
    for i, c in enumerate(CLASSES):
        rowp = p15[y15 == i]
        print(f'    {c[:5]:>6s}' + ''.join(f'{np.mean(rowp==j)*100:5.0f}%'
              for j in range(len(CLASSES))))

    # ── 64-bin 密谱特征 (排除 15 子带投影瓶颈) ───────────────
    print('  ' + '-' * 46)
    print('  特征 (b): 64-bin 对数谱 (密谱交叉验证)')
    acc64, _ = nearest_mean_acc(X64, y)
    m64, s64, _, _ = mlp_cv(X64, labels)
    print(f'    nearest-mean: {acc64*100:5.1f}%   MLP 5-fold CV: {m64*100:5.1f}% ± {s64*100:.1f}%')
    print('  ' + '-' * 46)

    # ── 判定 ──────────────────────────────────────────────────
    thresh = 0.70
    print(f'\n  判定 (计划阈值 nearest-mean >= {thresh*100:.0f}%):')
    nm = max(acc, acc64)
    ml = max(m15, m64)
    if nm >= thresh:
        print(f'  [PASS] nearest-mean {nm*100:.1f}% >= {thresh*100:.0f}% → 训练 4 类分类 CNN')
    elif ml >= thresh:
        print(f'  [PASS] nearest-mean {nm*100:.1f}% < {thresh*100:.0f}%, 但 MLP {ml*100:.1f}% '
              f'>= {thresh*100:.0f}% → 谱可分 (nearest-mean 悲观下限), 训练 4 类')
    else:
        print(f'  [FAIL] nearest-mean {nm*100:.1f}% 且 MLP {ml*100:.1f}% 均 < {thresh*100:.0f}% '
              f'→ 谱不可分, 需并类到 N=2-3')


if __name__ == '__main__':
    main()
