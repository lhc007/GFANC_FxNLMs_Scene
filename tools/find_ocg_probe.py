#!/usr/bin/env python3
"""找 OCG 真切换压力测试用的探针信号 (临时诊断 v2).

结论 v1: 所有真实噪声文件带通 20-1500Hz 后与 250Hz 纯音方向 cos>0.98,
          全部同簇 → 换噪声文件触发不了切换.
本版: 合成不同频率的纯音 + 分频段噪声, 找 cos<ocg_tau(0.8) 的信号.
      (纯音频率内容在带通内显著不同 → CNN 增益方向应不同.)

用法: python tools/find_ocg_probe.py
"""
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import numpy as np
import torch
import scipy.signal as sig

from layercam_diagnose import load_model, bandpass, prep_1s

FS = 16000
DUR = 2.0          # 每信号 2s (2 个窗口平均)
AMP = 0.2

def probe_signal(model, x):
    """x: float32 1s@16k (原始, 未带通). 返回平均 30 维增益方向."""
    xb = bandpass(x)
    n_win = len(xb) // FS
    gs = []
    for i in range(n_win):
        w = xb[i * FS:(i + 1) * FS]
        xp, denom = prep_1s(w)
        if denom <= 0.01:
            continue
        with torch.no_grad():
            g = np.tanh(model(torch.from_numpy(xp)).numpy()[0])
        gs.append(g)
    if not gs:
        return None
    d = np.mean(gs, axis=0)
    n = np.linalg.norm(d)
    return d / n if n > 1e-8 else None

def synth_tone(freq):
    t = np.arange(0, DUR * FS) / FS
    return (AMP * np.sin(2 * np.pi * freq * t)).astype(np.float32)

def synth_white():
    rng = np.random.default_rng(42)
    return (AMP * rng.standard_normal(int(DUR * FS))).astype(np.float32)

def synth_bandnoise(lo, hi, order=4):
    rng = np.random.default_rng(42)
    x = rng.standard_normal(int(DUR * FS))
    nyq = FS / 2
    sos = sig.butter(order, [lo / nyq, hi / nyq], btype='band', output='sos')
    x = sig.sosfilt(sos, x)
    # 归一化到合理响度
    x = AMP * x / (np.abs(x).max() + 1e-9)
    return x.astype(np.float32)

def main():
    model, K = load_model()
    print(f'  CNN K={K} (data/*.bin), ocg_tau=0.8\n')

    # 基准: 250Hz 纯音
    base = probe_signal(model, synth_tone(250))
    print(f'  基准 250Hz 纯音方向: top bands = {np.argsort(np.abs(base))[::-1][:4]}')

    print(f'\n  {"信号":26s} cos(与250Hz)  触发新簇?')
    rows = []
    for f in [250, 375, 500, 750, 1000, 1250, 1500]:
        d = probe_signal(model, synth_tone(f))
        cos = float(np.dot(base, d))
        rows.append((cos, f'纯音 {f}Hz', cos < 0.8))
        print(f'  {"纯音 " + str(f) + "Hz":26s} cos={cos:+.3f}   {"是" if cos < 0.8 else "否"}')
    for name, x in [('白噪声(带通内)', synth_white()),
                    ('低频噪 20-300Hz', synth_bandnoise(20, 300)),
                    ('中频噪 300-800Hz', synth_bandnoise(300, 800)),
                    ('高频噪 800-1500Hz', synth_bandnoise(800, 1500))]:
        d = probe_signal(model, x)
        cos = float(np.dot(base, d))
        rows.append((cos, name, cos < 0.8))
        print(f'  {name:26s} cos={cos:+.3f}   {"是" if cos < 0.8 else "否"}')

    trig = [r for r in rows if r[2]]
    print(f'\n  能触发新簇的探针 (cos<0.8):')
    for cos, name, _ in sorted(trig)[:3]:
        print(f'    {name:26s} cos={cos:+.3f}')

    # 详细: 各频率纯音的真实 30 维增益 (取首窗口), 看 CNN 是否频率盲
    print(f'\n  ── 单窗口 30 维增益 (tanh, 每扬声器 15 子带, 低频→高频) ──')
    for f in [250, 500, 1000, 1500]:
        xb = bandpass(synth_tone(f))
        xp, _ = prep_1s(xb[:FS])
        with torch.no_grad():
            g = np.tanh(model(torch.from_numpy(xp)).numpy()[0])
        spk0 = g[:15]; spk1 = g[15:]
        print(f'  {f}Hz  spk0 top3={np.argsort(np.abs(spk0))[::-1][:3]} '
              f'spk1 top3={np.argsort(np.abs(spk1))[::-1][:3]}')
        print(f'       spk0={np.round(spk0, 2)}')
        print(f'       spk1={np.round(spk1, 2)}')

if __name__ == '__main__':
    main()
