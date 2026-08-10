#!/usr/bin/env python3
"""P2 LayerCAM 离线诊断 — 部署 CNN (m5_scene 直接权重) 决策归因.

纯离线诊断脚本, 不进实时链. 用途:
  1. 复现 C 运行时 CNN 前向 — 从 data/*.bin 载权 (与 gfanc_realtime 逐位一致),
     输出 30 维 tanh 增益 + top 频带 (对齐实时日志 "top=2(21%) 17(15%)...").
  2. LayerCAM — 目标频带增益对最深层卷积特征图的梯度加权求和 → 时间热力图,
     定位 CNN "看" 的时域窗口.
  3. 输入空间显著性 — 目标增益对输入样本的梯度 → FFT → 关键频率,
     验证 CNN 决策键在真实音调频率 (250Hz 纯音应 ≈250Hz) 而非杂散频带.
  4. 多窗口抖动诊断 — 长录音逐秒切片, 各频带增益均值/方差 → 找出抖动频带
     (纯音下跨秒翻转根因, P0-2 平滑的对象).

用法:
  python tools/layercam_diagnose.py [wav] [--band N]
默认 wav=tone250.wav, band=argmax |增益| (可选 --band 指定 0..29).
"""
import os, sys, struct, zlib, argparse
from pathlib import Path
import numpy as np

# Windows 控制台 UTF-8 (同 C 运行时 SetConsoleOutputCP(CP_UTF8))
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')

import torch
import scipy.io.wavfile as wavfile
from scipy import signal as sig

SCRIPT_DIR = Path(__file__).resolve().parent
DATA_DIR   = SCRIPT_DIR.parent / 'data'
PY_PROJ    = SCRIPT_DIR.parent / 'GFANC_Scene'
sys.path.insert(0, str(PY_PROJ))

FS   = 16000          # ANC 处理采样率 (与 C 运行时一致)
IN_LEN = 16000        # CNN 输入长度 (1s)

# ────────────────────────────────────────────────────────────────────
# 1. .bin 加载 (v2: 16B 头 GFNC+ver+n_floats+crc32; 旧格式无头直接 float32)
# ────────────────────────────────────────────────────────────────────
def load_bin(path):
    raw = np.fromfile(str(path), dtype=np.uint8)
    if raw.size >= 16 and raw[:4].tobytes() == b'GFNC':
        _, _, n, crc = struct.unpack('<4sIII', raw[:16].tobytes())
        payload = raw[16:16 + 4 * n]
        if (zlib.crc32(payload.tobytes()) & 0xFFFFFFFF) != crc:
            print(f'  [WARN] CRC mismatch: {path.name}')
        return np.frombuffer(payload, dtype=np.float32).copy()
    return np.frombuffer(raw, dtype=np.float32).copy()

# ────────────────────────────────────────────────────────────────────
# 2. torch 模型 = m5_scene(K=30) (与 C cnn_m5_forward.c 逐层一致)
#    从 data/*.bin 载权 → 保证 = 运行时部署权重, 不依赖 ckpt 路径.
# ────────────────────────────────────────────────────────────────────
def load_model():
    from gfanc.Network import m5_scene
    # K 从 linear_weight 文件大小推导 (同 C: n_w = K*CH)
    n_w = load_bin(DATA_DIR / 'cnn_linear_weight.bin').size
    K = n_w // 64
    model = m5_scene(K=K, dropout=0.3)
    model.eval()

    sd = {}
    def t(name):
        """.bin → float32 torch tensor."""
        return torch.from_numpy(load_bin(DATA_DIR / f'{name}.bin'))
    # stem: conv_block = (Conv1d, BN, ReLU)
    sd['conv_block.0.weight'] = t('cnn_stem_conv_weight')
    sd['conv_block.0.bias']   = t('cnn_stem_conv_bias')
    sd['conv_block.1.weight'] = t('cnn_stem_bn_gamma')
    sd['conv_block.1.bias']   = t('cnn_stem_bn_beta')
    sd['conv_block.1.running_mean'] = t('cnn_stem_bn_mean')
    sd['conv_block.1.running_var']  = t('cnn_stem_bn_var')
    # 4 个 ResBlock: res0→res_blocks.0.0, res1→res_blocks.0.1,
    #                res2→res_blocks.1.0, res3→res_blocks.1.1
    res_idx = {'res0': 'res_blocks.0.0', 'res1': 'res_blocks.0.1',
               'res2': 'res_blocks.1.0', 'res3': 'res_blocks.1.1'}
    for tag, prefix in res_idx.items():
        sd[f'{prefix}.res.0.weight'] = t(f'cnn_{tag}_conv1_weight')
        sd[f'{prefix}.res.0.bias']   = t(f'cnn_{tag}_conv1_bias')
        sd[f'{prefix}.res.1.weight'] = t(f'cnn_{tag}_bn1_gamma')
        sd[f'{prefix}.res.1.bias']   = t(f'cnn_{tag}_bn1_beta')
        sd[f'{prefix}.res.1.running_mean'] = t(f'cnn_{tag}_bn1_mean')
        sd[f'{prefix}.res.1.running_var']  = t(f'cnn_{tag}_bn1_var')
        sd[f'{prefix}.res.3.weight'] = t(f'cnn_{tag}_conv2_weight')
        sd[f'{prefix}.res.3.bias']   = t(f'cnn_{tag}_conv2_bias')
        sd[f'{prefix}.res.4.weight'] = t(f'cnn_{tag}_bn2_gamma')
        sd[f'{prefix}.res.4.bias']   = t(f'cnn_{tag}_bn2_beta')
        sd[f'{prefix}.res.4.running_mean'] = t(f'cnn_{tag}_bn2_mean')
        sd[f'{prefix}.res.4.running_var']  = t(f'cnn_{tag}_bn2_var')
    # linear
    sd['linear.weight'] = t('cnn_linear_weight')
    sd['linear.bias']   = t('cnn_linear_bias')

    # .bin 为扁平存储 (C 侧按 (oc,ic,k) 索引) → 按 torch 参数形状 reshape
    ref = model.state_dict()
    for k, v in sd.items():
        pshape = ref[k].shape
        if v.shape != pshape:
            sd[k] = v.reshape(pshape)
    missing, unexpected = model.load_state_dict(sd, strict=False)
    if unexpected:
        print(f'  [WARN] 未使用的权重键: {unexpected}')
    if missing:
        print(f'  [WARN] 缺失权重键 (通常为 num_batches_tracked, 可忽略): {missing}')
    return model, K

# ────────────────────────────────────────────────────────────────────
# 3. 输入预处理 — 复现实时链: ref → 带通(bandpass_fir.bin) → minmax 归一化
#    (scene_controller.c: denom=mx-mn, cnn_in=audio/denom)
# ────────────────────────────────────────────────────────────────────
def prep_1s(audio):
    """audio: float32 1s@16k. 返回 CNN 输入 (1,1,16000). 弱信号→全零 (同 C)."""
    mx, mn = float(audio.max()), float(audio.min())
    denom = mx - mn
    if denom <= 0.01:
        return np.zeros((1, 1, IN_LEN), dtype=np.float32), denom
    return (audio / denom).astype(np.float32).reshape(1, 1, IN_LEN), denom

def bandpass(audio):
    """带通 20-1500Hz (bandpass_fir.bin, 1024tap) — 同 main_realtime ref_cnn."""
    b = load_bin(DATA_DIR / 'bandpass_fir.bin')
    return sig.lfilter(b, [1.0], audio)

def load_windows(path):
    """读 wav → float32, 重采样到 16k, 带通, 切成逐秒窗口列表."""
    fs, x = wavfile.read(path)
    if x.ndim > 1:                        # 多声道取第 0 声道 (ref mic)
        x = x[:, 0]
    x = x.astype(np.float32) / 32768.0
    if fs != FS:
        n = int(round(len(x) * FS / fs))
        x = sig.resample_poly(x, FS, fs)
        print(f'  [INFO] 重采样 {fs}Hz → {FS}Hz')
    x = bandpass(x)
    n_win = len(x) // IN_LEN
    wins = [x[i * IN_LEN:(i + 1) * IN_LEN] for i in range(n_win)]
    print(f'  [INFO] {Path(path).name}: {len(x)/FS:.1f}s → {n_win} 个 1s 窗口')
    return wins

# ────────────────────────────────────────────────────────────────────
# 4. 诊断主流程
# ────────────────────────────────────────────────────────────────────
def top_fft_freqs(sig1d, k=3, fs=FS, min_f=20.0):
    """FFT 峰值频率 (排除 DC 和 <min_f 低频)."""
    n = len(sig1d)
    spec = np.abs(np.fft.rfft(sig1d))
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    spec[freqs < min_f] = 0.0
    idx = np.argsort(spec)[::-1][:k]
    return [(float(freqs[i]), float(spec[i])) for i in idx if spec[i] > 0]

def layer_cam(model, x_t, band):
    """LayerCAM: 目标 band 增益对 pool_blocks[1] 特征图 (64×31) 的梯度加权求和."""
    A = {}
    def hook(mod, inp, out):
        A['map'] = out
    h = model.pool_blocks[1].register_forward_hook(hook)
    out = model(x_t)
    target = out[0, band]
    grads = torch.autograd.grad(target, A['map'], retain_graph=True)[0]  # (1,64,31)
    h.remove()
    cam = (grads.clamp(min=0) * A['map']).sum(dim=1)[0]                  # (31,)
    cam = cam.detach().cpu().numpy()
    if cam.max() > 0:
        cam = cam / cam.max()
    return cam, target.item()

def input_saliency(model, x_t, band):
    """目标增益对输入样本的梯度 → 16000 点显著性 (决策"听"的波形).
    注意: FFT 含 maxpool 分块结构影响 (梯度在池窗口内块状), 频率归因以
    freq_occlusion 为准, 本函数作参考. """
    x_t = x_t.detach().requires_grad_(True)
    out = model(x_t)
    out[0, band].backward()
    return x_t.grad[0, 0].detach().cpu().numpy()

def freq_occlusion(model, x_np, band, n_bands=15, f_lo=20.0, f_hi=1500.0):
    """频率遮挡归因 — 把输入按对数间隔挖掉一个频率子带, 重跑 CNN, 测 gain 变化.
    |Δ| 最大的子带 = 驱动该频带增益决策的频率区. 无梯度伪影, 直接可解释.
    用系统自身的 C=15 子带划分 (20-1500Hz 对数间隔, 与 sub_filters 同框架)."""
    x = x_np[0, 0]
    X = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(len(x), 1.0 / FS)
    with torch.no_grad():
        base = model(torch.from_numpy(x_np))[0, band].item()
    edges = np.logspace(np.log10(f_lo), np.log10(f_hi), n_bands + 1)
    deltas = np.zeros(n_bands)
    for k in range(n_bands):
        Xm = X.copy()
        Xm[(freqs >= edges[k]) & (freqs <= edges[k + 1])] = 0.0
        xm = np.fft.irfft(Xm, n=len(x)).astype(np.float32).reshape(1, 1, IN_LEN)
        with torch.no_grad():
            gm = model(torch.from_numpy(xm))[0, band].item()
        deltas[k] = base - gm
    return edges, deltas, base

def analyze_window(model, x_np, band):
    """单个 1s 窗口: 增益 + LayerCAM 热力图 + 输入显著性频率."""
    x_t = torch.from_numpy(x_np)
    with torch.no_grad():
        logits = model(x_t)[0].numpy()
        gains = np.tanh(logits)
    cam, target_val = layer_cam(model, x_t, band)
    sal = input_saliency(model, x_t, band)
    sal_freqs = top_fft_freqs(sal)
    return logits, gains, cam, sal_freqs

def main():
    ap = argparse.ArgumentParser(description='LayerCAM CNN 决策归因诊断 (离线)')
    ap.add_argument('wav', nargs='?', default=str(SCRIPT_DIR.parent / 'tone250.wav'))
    ap.add_argument('--band', type=int, default=None,
                    help='目标频带 0..29 (默认 argmax |增益|)')
    args = ap.parse_args()

    print('══ P2 LayerCAM 诊断 ══')
    model, K = load_model()
    print(f'  模型: m5_scene direct_weight K={K} (从 data/*.bin 载权, 与运行时一致)')

    wins = load_windows(args.wav)
    if not wins:
        print('  [ERROR] wav 不足 1s, 无法分析')
        return 1

    # 解析目标频带: 未指定 → 首窗口 argmax |增益|
    if args.band is None:
        x0, d0 = prep_1s(wins[0])
        with torch.no_grad():
            logits0 = model(torch.from_numpy(x0))[0].numpy()
        band = int(np.argmax(np.abs(np.tanh(logits0))))
        print(f'  目标频带 (argmax |增益|): gain[{band}]')
    else:
        band = args.band

    # ── 全窗口增益统计 (抖动诊断) ──
    print('\n── 各窗口 30 维增益 (均值 |g|, 方差 — 找抖动频带) ──')
    all_gains = []
    all_cams, all_sals, all_oc = [], [], []
    for i, w in enumerate(wins):
        x_np, denom = prep_1s(w)
        if denom <= 0.01:
            all_gains.append(np.zeros(K)); continue
        logits, gains, cam, sal_freqs = analyze_window(model, x_np, band)
        all_gains.append(gains)
        all_cams.append(cam)
        all_sals.append(sal_freqs)
        _, oc_deltas, _ = freq_occlusion(model, x_np, band)
        all_oc.append(int(np.argmax(np.abs(oc_deltas))))

    G = np.array(all_gains)                        # (n_win, 30)
    gmean = np.abs(G).mean(axis=0)
    gstd  = G.std(axis=0)
    order = np.argsort(gmean)[::-1]
    print('  band |g|mean  |g|std   std/mean (抖动)')
    for b in order:
        m, s = gmean[b], gstd[b]
        jit = s / m if m > 1e-6 else 0.0
        flag = '  <-- 抖动' if jit > 0.5 else ''
        print(f'  {b:2d}    {m:.3f}   {s:.3f}    {jit:.2f}{flag}')
    # 抖动频带 = 高 |g| 且高相对方差 (频带跨秒翻转)
    jit_bands = [b for b in order if gmean[b] > 0.2 and (gstd[b] / gmean[b]) > 0.4]
    if jit_bands:
        print(f'  抖动频带 (|g|>0.2 且 std/mean>0.4): {jit_bands}')
    else:
        print('  未发现显著抖动频带')

    # ── 目标频带决策归因 (用首个有效窗口) ──
    print('\n── 决策归因 (窗口 #1) ──')
    g1 = all_gains[0]
    print(f'  目标频带: gain[{band}] = {g1[band]:+.3f} (tanh)')
    top_idx = np.argsort(np.abs(g1))[::-1][:5]
    print(f'  top-5 频带: ' + ' '.join(f'{i}({g1[i]*100:+.0f}%)' for i in top_idx))

    # 输入主导频率 (ground truth)
    in_freqs = top_fft_freqs(wins[0])
    print(f'  输入主导频率: {[f"{f:.0f}Hz" for f, _ in in_freqs]}')
    # 频率遮挡归因 (无梯度伪影, 主判据)
    x0n, d0n = prep_1s(wins[0])
    edges, deltas, base = freq_occlusion(model, x0n, band)
    oc_order = np.argsort(np.abs(deltas))[::-1]
    print(f'  频率遮挡归因 (挖掉子带 → gain[{band}] 变化 |Δ|):')
    for k in oc_order[:4]:
        f_c = 0.5 * (edges[k] + edges[k + 1])
        print(f'    band {k:2d} {f_c:6.0f}Hz  Δ={deltas[k]:+.3f}')
    oc_top_k = int(np.argmax(np.abs(deltas)))
    f_occ = 0.5 * (edges[oc_top_k] + edges[oc_top_k + 1])
    tone_in_occ_band = edges[oc_top_k] <= in_freqs[0][0] <= edges[oc_top_k + 1]
    print(f'  主导子带 {f_occ:.0f}Hz {"包含输入主导频率 ✅ 决策键在真实音调" if tone_in_occ_band else "不含输入主导频率 ⚠️ 决策键在其它频带"}')
    # CNN 决策关键频率 (输入显著性 FFT, 含 pool 结构影响 — 参考)
    print(f'  敏感滤波器 FFT (参考, 含 maxpool 结构影响): '
          f'{[f"{f:.0f}Hz" for f, _ in all_sals[0]]}')

    # LayerCAM 时间热力图 (31 bin ≈ 32ms/格)
    print('  LayerCAM 时间热力图 (每格≈32ms, 峰值=CNN最关注时窗):')
    n_bin = len(all_cams[0])
    bar = ''.join('█' if all_cams[0][i] > 0.8 else '▅' if all_cams[0][i] > 0.5 else
                  '▂' if all_cams[0][i] > 0.2 else '·' for i in range(n_bin))
    print(f'    {bar}')

    # ── 跨窗口决策稳定性 (遮挡主导子带是否漂移) ──
    print('\n── 跨窗口遮挡主导子带 (10s 逐秒, 主判据) ──')
    for i, b in enumerate(all_oc):
        f_c = 0.5 * (edges[b] + edges[b + 1])
        print(f'  win{i:02d}: {f_c:.0f}Hz')
    stable = len(set(all_oc)) == 1
    print(f'  决策频率 {"稳定" if stable else "漂移"} (基准 {in_freqs[0][0]:.0f}Hz)')

    return 0

if __name__ == '__main__':
    sys.exit(main())
