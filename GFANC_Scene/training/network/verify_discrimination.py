"""
判别力验证 (Checkpoint 3) — 复现 2026-08-09 诊断协议, 测 CNN 是否学会用输入.

协议与原始诊断 (.dbg/analyze.py) 完全一致:
  窗口集   = 7 个基准录音的逐秒窗口, 两个 road 合并为一类 → 6 类
              (road_noise-15 / road_noise_0-34 / Helicopter / Street /
               Trolley / Furniture / mixed_7types_56s)
  特征     = CNN 输出 (30 维 tanh 增益, 输入管道与 C 端一致: 带通50-1500 → minmax)
             输入谱   (15 子带能量, 带通后投影子滤波器)
             真实标签 (真实训练集 CSV gain_*, 4 类 — 训练不改变它, 是固定天花板)
  指标     = 最近均值分类准确率 (逐窗口 → 最近类型质心, 余弦) + 区分度间隙
             (类型内 cos − 类型间 cos)

基线 (2026-08-09, C 运行时实测): 输入谱 75.0% / 真实标签 76.7% / CNN 输出 35.8%.
目标: 两阶段训练后 CNN 输出 → ≥70% (追平输入谱量级).

用法:
    # 基线对照 (应复现 ≈35.8%)
    python training/network/verify_discrimination.py \
        --model models/MIMO_M5_DirectWeight_Real_baseline_35pct.pth

    # 训练后验证
    python training/network/verify_discrimination.py \
        --model models/MIMO_M5_DirectWeight_Real.pth
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

from gfanc._paths import MODELS_DIR
from noise_dataset import make_bandpass_tensor, prepare_batch as _pbatch
from gfanc.Network import m5_scene

N_SPEAKERS, N_BANDS, SC = 2, 15, 30
SAMPLE_LEN = 16000
_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_fir.mat'
_SUB_FILTER = MODELS_DIR / 'MIMO_Pretrained_Control_filters_broadband.mat'
_NOISE_DIR = _PROJECT_ROOT.parent / 'Noise Examples'   # 基准录音在仓库根, 不在 GFANC_Scene/

# 基准录音 → 类型 tag (road 两文件合并为同一类, 与原始分析一致)
TYPE_FILES = {
    'road':    ['road_noise-15.wav', 'road_noise_0-34.wav'],
    'helo':    ['Helicopter.wav'],
    'street':  ['Street.wav'],
    'trolley': ['Trolley.wav'],
    'furn':    ['Furniture.wav'],
    'mixed':   ['mixed_7types_56s.wav'],
}
REAL_CSV = r'D:\Dataset\Real_world_Dataset\Index_real_Training_data.csv'


# ═══════════════════════════════════════════════════════════════
# 指标 (与 .dbg/analyze.py 一致)
# ═══════════════════════════════════════════════════════════════

def nearest_mean_acc(X, y):
    """最近均值分类: 每窗口 → 与自身同类型的质心 cos 最高者. 返回 (acc, per_type)."""
    types = np.unique(y)
    cents = {}
    for t in types:
        c = X[y == t].mean(axis=0)
        cents[t] = c / (np.linalg.norm(c) + 1e-12)
    Cs = np.array([cents[t] for t in types])            # (T, D) 质心
    Xn = X / (np.linalg.norm(X, axis=1, keepdims=True) + 1e-12)
    sims = Xn @ Cs.T                                     # (N, T)
    pred = types[sims.argmax(axis=1)]
    acc = float((pred == y).mean())
    per = {t: float((pred[y == t] == t).mean()) for t in types}
    return acc, per


def gap_metric(X, y, n_pairs=4000):
    """区分度间隙 = 类型内均值 cos − 类型间均值 cos (随机采样对)."""
    rng = np.random.RandomState(0)
    Xn = X / (np.linalg.norm(X, axis=1, keepdims=True) + 1e-12)
    intra, inter = [], []
    N = len(y)
    for _ in range(n_pairs):
        i, j = rng.randint(0, N, 2)
        c = float(Xn[i] @ Xn[j])
        (intra if y[i] == y[j] else inter).append(c)
    return float(np.mean(intra) - np.mean(inter)), np.mean(intra), np.mean(inter)


# ═══════════════════════════════════════════════════════════════
# 特征提取
# ═══════════════════════════════════════════════════════════════

def per_second_windows(sig_16k):
    """44100→16000 重采样后取全部完整 1s 窗口 [k*16000:(k+1)*16000)."""
    n = sig_16k.shape[0]
    return [sig_16k[k * SAMPLE_LEN:(k + 1) * SAMPLE_LEN]
            for k in range(n // SAMPLE_LEN)]


def feature_input_spectrum(win, subs, bp_w, bp_pad):
    """窗口 → 带通 → 15 子带能量 (归1). 与 generate_synthetic.py probe 同口径."""
    x = torch.from_numpy(win).float().unsqueeze(0).unsqueeze(0).to(bp_w.device)
    x = _pbatch(x, bp_w, bp_pad).squeeze().cpu().numpy()   # 带通 50-1500 (与 CNN 输入一致)
    e = []
    for k in range(subs.shape[0]):
        y = signal.fftconvolve(x, subs[k], mode='valid')
        e.append(float(np.sqrt((y ** 2).mean()) + 1e-12))
    e = np.array(e)
    return e / (e.sum() + 1e-12)


def feature_cnn_output(win, model, bp_w, bp_pad, device):
    """窗口 → 带通 → minmax → CNN → tanh → 30 维增益 (与 C 端 scene_ctrl_process 一致)."""
    x = torch.from_numpy(win).float().unsqueeze(0).unsqueeze(0).to(device)
    x = _pbatch(x, bp_w, bp_pad)
    with torch.no_grad():
        pred = torch.tanh(model(x)).squeeze(0).cpu().numpy()
    return pred


def wc_only_nr(win, gains, Pri, Sec, sub_full, device, fs=16000, repet=3):
    """仅 CNN Wc 的稳态降噪 (无 FxLMS 自适应), 含 C 运行时 RMS 标定.

    与 scene_controller.c 的 scene_ctrl_construct_wc 对齐:
      Wc = Σ_g gains·sub → wc_rms → Wc *= stub_rms / wc_rms
    (stub_rms = 全 1 增益等权求和的 RMS; 符号取反是 C 端约定, Python 侧已对).
    Dis/Fx 走真实声学路径 (Repet=3), 末 1s 稳态窗计 NR.
    """
    from training.control_filters.Disturbance_generation import disturbance_generation_batch_gpu
    Dis, Fx, T = disturbance_generation_batch_gpu(
        [torch.from_numpy(win)], Pri, Sec, fs=fs, Repet=repet)
    Dis, Fx = Dis[0].cpu().numpy(), Fx[0].cpu().numpy()
    Wc = np.einsum('sc,csl->sl', gains, sub_full)          # (S, L)
    # RMS 标定 (C 端 wc_rms_target = stub_rms, 2026-08-10 实测: 无此步裸 Wc 低估 ~3.3 dB)
    S, L = Wc.shape
    stub = sub_full.sum(axis=0)                            # (S,L) 全1增益等权求和
    stub_rms = float(np.sqrt((stub ** 2).sum() / (S * L)))
    wc_rms = float(np.sqrt((Wc ** 2).sum() / (S * L)))
    if wc_rms > 1e-6:
        Wc = Wc * (stub_rms / wc_rms)
    E = Dis.shape[0]
    anti = np.zeros_like(Dis)
    for e in range(E):
        for s in range(N_SPEAKERS):
            anti[e] += np.convolve(Wc[s], Fx[e, s])[:T]
    n0 = T - fs
    pd = np.mean(Dis[:, n0:] ** 2)
    pe = np.mean((Dis - anti)[:, n0:] ** 2)
    return 10 * np.log10(pd / (pe + 1e-20))


def main():
    ap = argparse.ArgumentParser(description='Checkpoint 3: CNN 输出判别力 + 仅 CNN Wc 的真实基准降噪')
    ap.add_argument('--model', required=True)
    ap.add_argument('--no-nr', action='store_true',
                    help='跳过 Wc-only NR 实测 (只测判别力, 不生成 Dis/Fx)')
    args = ap.parse_args()

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print('=' * 60)
    print('  判别力验证 (Checkpoint 3) — 最近均值分类 + cos 间隙')
    print(f'  模型: {args.model}')
    print(f'  设备: {device}')
    print('=' * 60)

    # 模型 + 带通 + 子滤波器
    model = m5_scene(K=SC, dropout=0.3)
    model.load_state_dict(torch.load(args.model, map_location='cpu', weights_only=True))
    model = model.to(device).eval()
    bp_w, bp_pad = make_bandpass_tensor(_BP_PATH, device)
    sub = sio.loadmat(str(_SUB_FILTER))['Wc_v']         # (15, 2, L)
    subs = sub[:, 0, :].copy()                          # 扬声器0 → (15, L)
    # 与原始探针 (spectrum_probe.py) 一致: 每个子滤波器归一化到单位范数 (等带灵敏度)
    subs = subs / (np.linalg.norm(subs, axis=1, keepdims=True) + 1e-12)

    # ── 基准录音窗口集 (6 类) ─────────────────────────────
    X_in, X_out, y = [], [], []
    n_win = 0
    for t, files in TYPE_FILES.items():
        for fn in files:
            w, sr = torchaudio.load(str(_NOISE_DIR / fn))
            w = torchaudio.functional.resample(w, sr, 16000).squeeze().numpy()
            wins = per_second_windows(w)
            for win in wins:
                X_in.append(feature_input_spectrum(win, subs, bp_w, bp_pad))
                X_out.append(feature_cnn_output(win, model, bp_w, bp_pad, device))
                y.append(t)
                n_win += 1
    X_in, X_out, y = map(np.array, (X_in, X_out, np.array(y)))
    print(f'\n  基准窗口集: {n_win} 个逐秒窗口, {len(np.unique(y))} 类')
    print('  ' + '-' * 46)
    for name, X in [('输入谱 (15 子带)', X_in), ('CNN 输出 (30 维)', X_out)]:
        acc, per = nearest_mean_acc(X, y)
        gap, intra, inter = gap_metric(X, y)
        print(f'  {name:18s}: 最近均值 {acc*100:5.1f}%  gap={gap:+.3f} '
              f'(内{intra:.3f} 间{inter:.3f})')
        print(f'    逐类: ' + ' '.join(f'{k}={v*100:.0f}%' for k, v in per.items()))
        # mixed_7types 是 7 种噪声逐秒拼接, 作为单类最近均值天然不可分 (38% 的窗口),
        # 排除它给出非 mixed 类的干净指标 — 供跨模型对比 (系统伪影, 所有特征同偏)
        m = y != 'mixed'
        acc5, _ = nearest_mean_acc(X[m], y[m])
        print(f'    排除 mixed ({int(m.sum())} 窗, {len(np.unique(y[m]))} 类): 最近均值 {acc5*100:5.1f}%')
    print('  ' + '-' * 46)

    # ── 真实训练标签 (4 类, 训练不改变它 — 固定天花板) ────
    df = pd.read_csv(REAL_CSV)
    gcols = [c for c in df.columns if c.startswith('gain_')]
    gains = df[gcols].values.astype(np.float32)
    yy = df['category'].values
    m = np.abs(gains).max(axis=1, keepdims=True)
    m = np.where(m < 1e-12, 1.0, m)
    labels = gains / m
    # 与原始 148 窗口同量级: 每类采样 ~200 窗口 (平衡, 免类型量级偏差)
    rng = np.random.RandomState(0)
    idx = []
    for c in np.unique(yy):
        cidx = np.where(yy == c)[0]
        idx.extend(rng.choice(cidx, 200, replace=False))
    idx = np.array(sorted(idx))
    lab_acc, _ = nearest_mean_acc(labels[idx], yy[idx])
    lab_gap, lab_i, lab_o = gap_metric(labels[idx], yy[idx])
    print(f'  真实训练标签 (4类×200): 最近均值 {lab_acc*100:5.1f}%  gap={lab_gap:+.3f} '
          f'(内{lab_i:.3f} 间{lab_o:.3f})  [固定天花板]')
    print('  ' + '-' * 46)
    print('  基线 (2026-08-09 C 运行时): 输入谱 75.0% | 真实标签 76.7% | CNN 输出 35.8%')
    print('  目标: 两阶段训练后 CNN 输出判别力 ≥ 70%')

    # ── 仅 CNN Wc 的真实基准稳态降噪 (目标: 5-8 dB, 无 FxLMS) ──
    if not args.no_nr:
        print('\n  ── 仅 CNN Wc 的真实基准稳态降噪 ──')
        print('  (首 1s 窗口, Repet=3, Wc 固定卷积无自适应; 与 2026-08-09 实测同口径)')
        print('  ' + '-' * 46)
        from training.control_filters.path_loader import load_multichannel_paths_with_variable_names
        Pri, Sec = load_multichannel_paths_with_variable_names(
            folder=str(_PROJECT_ROOT / 'Primary and Secondary Path'), subfolder='',
            Pri_path_file_name='primary_path.npy', Sec_path_file_name='secondary_path.npy')
        sub_full = sio.loadmat(str(_SUB_FILTER))['Wc_v'].astype(np.float32)   # (C, S, L)
        nrs = []
        for t, files in TYPE_FILES.items():
            for fn in files:
                w, sr = torchaudio.load(str(_NOISE_DIR / fn))
                w = torchaudio.functional.resample(w, sr, 16000).squeeze().numpy()
                win = w[:SAMPLE_LEN]
                g = feature_cnn_output(win, model, bp_w, bp_pad, device).reshape(N_SPEAKERS, N_BANDS)
                n = wc_only_nr(win, g, Pri, Sec, sub_full, device)
                nrs.append(n)
                print(f'  {fn:28s}  {n:+.1f} dB')
        print('  ' + '-' * 46)
        print(f'  本模型 (含 C 端 RMS 标定): {min(nrs):+.1f}~{max(nrs):+.1f} dB')
        print(f'  参考: 标签(LMS)界 5.2~8.1 | 全1宽带 4.1~7.5 | 目标 5-8 dB')
        print(f'  2026-08-10 实测: v2 5.0~7.9 达成目标; 裸 Wc(无标定) 会低估约 3.3 dB')


if __name__ == '__main__':
    main()
