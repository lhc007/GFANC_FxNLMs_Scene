#!/usr/bin/env python3
"""对部署模型 (data/*.bin 载权, 与实机一致) 做纯音扫频.
验证增益向量是否对准音调频带, 决策关键频率是否为真实音调.
输出: 每频段 top-6 |增益| + 对应子带中心频率 + 频率遮挡归因.

用法: python tools/sweep_deployed_model.py
"""
import sys
from pathlib import Path
import numpy as np
import torch

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
sys.path.insert(0, str(SCRIPT_DIR.parent / 'GFANC_Scene'))
from layercam_diagnose import load_model, prep_1s, bandpass, freq_occlusion, top_fft_freqs

FS = 16000
DUR = 2.0
AMP = 0.2

# 各子带中心频率 — 从 data/sub_filters.bin 每子带 FFT 峰值推 (与实机同文件)
def band_centers():
    import struct, zlib
    raw = np.fromfile(str(SCRIPT_DIR.parent / 'data' / 'sub_filters.bin'), dtype=np.uint8)
    if raw[:4].tobytes() == b'GFNC':
        n = struct.unpack('<I', raw[8:12].tobytes())[0]
        payload = raw[16:16 + 4 * n]
        sf = np.frombuffer(payload, dtype=np.float32).copy()
    else:
        sf = np.fromfile(str(SCRIPT_DIR.parent / 'data' / 'sub_filters.bin'), dtype=np.float32).copy()
    K = 30; L = 1024
    sf = sf[:K * L].reshape(K, L)
    centers = []
    for k in range(K):
        spec = np.abs(np.fft.rfft(sf[k]))
        freqs = np.fft.rfftfreq(L, 1.0 / FS)
        spec[freqs < 20] = 0
        centers.append(freqs[np.argmax(spec)])
    return np.array(centers)

def tone(freq):
    t = np.arange(0, DUR * FS) / FS
    return (AMP * np.sin(2 * np.pi * freq * t)).astype(np.float32)

def main():
    model, K = load_model()
    print(f'部署模型 m5_scene K={K} (data/*.bin, 与实机一致)')
    centers = band_centers()
    print('子带中心频率 (Hz):')
    print('  ' + ' '.join(f'{c:.0f}' for c in centers))

    for f_in in [100, 250, 500, 1000, 1500]:
        x = tone(f_in)
        xb = bandpass(x)
        gs = []
        for i in range(int(len(xb) // FS)):
            xp, denom = prep_1s(xb[i * FS:(i + 1) * FS])
            if denom <= 0.01:
                continue
            with torch.no_grad():
                gs.append(np.tanh(model(torch.from_numpy(xp)).numpy()[0]))
        if not gs:
            print(f'\n{f_in}Hz: 无有效窗口'); continue
        g = np.mean(gs, axis=0)
        order = np.argsort(np.abs(g))[::-1]
        print(f'\n══ {f_in}Hz 输入 ══')
        top = order[:6]
        print('  top-6 增益: ' + ' '.join(
            f'b{i}(g={g[i]:+.2f},峰{centers[i]:.0f}Hz)' for i in top))
        # 音调带命中: 是否有 top 增益落在 f_in±200Hz 内
        hit = [i for i in top if abs(centers[i] - f_in) < 200]
        print(f'  top-6 中落在音调带(±200Hz)的子带: {hit if hit else "无 ⚠️"}')
        # 频率遮挡: 哪个输入频带驱动 top 频带增益
        xp, denom = prep_1s(xb[:FS])
        edges, deltas, base = freq_occlusion(model, xp, top[0], n_bands=15)
        oc = np.argsort(np.abs(deltas))[::-1][:3]
        occ = [f'{0.5*(edges[k]+edges[k+1]):.0f}Hz(Δ{deltas[k]:+.3f})' for k in oc]
        print(f'  top 频带 b{top[0]} 决策驱动频率(遮挡): {", ".join(occ)}')
        in_f = top_fft_freqs(xb[:FS], k=1)[0][0]
        print(f'  输入主导频率: {in_f:.0f}Hz')

if __name__ == '__main__':
    main()
