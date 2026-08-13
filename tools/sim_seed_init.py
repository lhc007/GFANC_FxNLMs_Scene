#!/usr/bin/env python3
"""验证: 实机日志 ~30s 慢收敛根因 = CNN init Wc 错位, 而非 step 太小.

种子 Wc = 部署 CNN 对纯音增益 → 直接权重构造 (scene_ctrl_construct_wc 数学):
  Wc[s,l] = -Σ_c gains[s·C+c]·sub[(c·S+s)·L+l] → RMS 标定到 wc_rms_target(0.01)
跑 sim_step_sweep.run (FB OFF, 合成 Ŝ, step 4.89e-8 = 实机日志配置),
对照日志轨迹: 7s err≈0.072, 18s err≈0.0426, anti 平台≈0.38.

用法: python tools/sim_seed_init.py [250] [500]
"""
import sys
from pathlib import Path
import numpy as np
import torch

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
sys.path.insert(0, str(SCRIPT_DIR.parent / 'GFANC_Scene'))
from layercam_diagnose import load_model, prep_1s, bandpass
import sim_step_sweep as sim

FS = 16000
S, C, L = 2, 15, 1024
WC_TARGET = 0.01            # gfanc_types.h 默认 wc_rms_target (GFANC_WC_TARGET 未设)

def load_sub_filters():
    import struct
    raw = np.fromfile(str(SCRIPT_DIR.parent / 'data' / 'sub_filters.bin'), dtype=np.uint8)
    if raw[:4].tobytes() == b'GFNC':
        n = struct.unpack('<I', raw[8:12].tobytes())[0]
        sf = np.frombuffer(raw[16:16 + 4 * n], dtype=np.float32).copy()
    else:
        sf = np.fromfile(str(SCRIPT_DIR.parent / 'data' / 'sub_filters.bin'), dtype=np.float32).copy()
    return sf[:S * C * L].reshape(S * C, L)    # (c*S+s, L), 同 scene_ctrl_construct_wc 布局

def cnn_wc_init(freq, model, sub):
    """部署 CNN 对 1s 纯音 → 增益 → Wc (RMS 0.01, 取反). 同实机 INIT 路径."""
    t = np.arange(FS) / FS
    xb = bandpass((0.2 * np.sin(2 * np.pi * freq * t)).astype(np.float32))
    xp, denom = prep_1s(xb)
    if denom <= 0.01:
        print(f'  [WARN] {freq}Hz 输入 denom={denom:.4f} 过弱, CNN 保持零增益')
        return np.zeros((S, L))
    with torch.no_grad():
        g = np.tanh(model(torch.from_numpy(xp)).numpy()[0])   # (30,), s*C+c
    wc = np.zeros((S, L))
    for s in range(S):
        for l in range(L):
            wc[s, l] = sum(g[s * C + c] * sub[c * S + s, l] for c in range(C))
    rms = np.sqrt(np.mean(wc ** 2))
    wc = -wc * (WC_TARGET / rms) if rms > 1e-6 else wc
    return wc

def main():
    freqs = [float(a) for a in sys.argv[1:]] or [250.0, 500.0]
    model, K = load_model()
    sub = load_sub_filters()
    print(f'部署模型 K={K}, wc_rms_target={WC_TARGET}, step=4.89e-8 (实机日志配置), FB OFF')
    log_ref = {250.0: [(7, 0.072), (18, 0.0426)], 500.0: []}
    for f in freqs:
        wc0 = cnn_wc_init(f, model, sub)
        rms0 = np.sqrt(np.mean(wc0 ** 2))
        # 增益指向 (对照 sweep_deployed_model: 250Hz → 906/188Hz 带, 错位)
        top = np.argsort(np.abs(wc0.reshape(-1)))[::-1][:3]
        print(f'\n══ {f:.0f}Hz init: Wc RMS={rms0:.4f} (目标0.01) top taps (s,l)={top} ══')
        er, ar = sim.run(f, 1e-7, False, 'synth', T_sec=60, wc_init=wc0)
        print(f"{'t(s)':>5} | {'err':>7} | {'NR(dB)':>7} | {'anti':>6} | 日志对照")
        print('-' * 56)
        for t in range(2, 61, 2):
            e = er[t - 1]
            db = 20 * np.log10((e + 1e-12) / 0.083)
            lg = ''
            for lt, le in log_ref.get(f, []):
                if t == lt:
                    lg = f' ← 日志 t={lt}s err≈{le}'
            print(f'{t:>5} | {e:7.4f} | {db:7.1f} | {ar[t-1]:6.3f}{lg}')
        print(f'  60s 平台: err={er[-1]:.4f} ({20*np.log10(er[-1]/0.083):.1f}dB) anti={ar[-1]:.3f} '
              f'[日志 anti≈0.38]')

if __name__ == '__main__':
    main()
