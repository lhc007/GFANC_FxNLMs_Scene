#!/usr/bin/env python3
"""验证 v2 (两阶段: 合成预训+真实微调) 的方向区分度, 对比当前部署模型.

假设: 当前部署模型 (从零真实, 幅度增强重训) 频率盲 (方向单调);
       v2 (合成多样谱形预训练) 应更频率敏感 → 不同纯音方向 cos 更低.
同 find_ocg_probe.py 的指标: cos(方向) < 0.8 会触发 OCG 新簇.

用法: python tools/check_v2_direction.py
"""
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(ROOT / 'SceneZone_Scene'))
sys.path.insert(0, str(SCRIPT_DIR))

import numpy as np
import torch

from layercam_diagnose import bandpass, prep_1s

FS = 16000
DUR = 2.0
AMP = 0.2

def load_from_pth(path, strict=False):
    ckpt = torch.load(path, map_location='cpu')
    if isinstance(ckpt, dict) and 'state_dict' in ckpt:
        ckpt = ckpt['state_dict']
    elif isinstance(ckpt, dict) and 'model' in ckpt and isinstance(ckpt['model'], dict):
        ckpt = ckpt['model']
    # K 从 linear.weight 推导
    K = ckpt['linear.weight'].shape[0]
    from gfanc.Network import m5_scene
    model = m5_scene(K=K, dropout=0.3)
    missing, unexpected = model.load_state_dict(ckpt, strict=False)
    model.eval()
    return model, K, missing, unexpected

def tone(freq):
    t = np.arange(0, DUR * FS) / FS
    return (AMP * np.sin(2 * np.pi * freq * t)).astype(np.float32)

def probe_dir(model, x):
    xb = bandpass(x)
    gs = []
    for i in range(int(len(xb) // FS)):
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

def main():
    models = [
        ('纯合成预训练', str(ROOT / 'SceneZone_Scene/models/MIMO_M5_DirectWeight_Pretrain.pth')),
        ('v2 (合成预训+微调)', str(ROOT / 'SceneZone_Scene/models/MIMO_M5_DirectWeight_Real_v2.pth')),
        ('当前部署 (从零真实)', str(ROOT / 'SceneZone_Scene/models/MIMO_M5_DirectWeight_Real.pth')),
        ('baseline_35pct', str(ROOT / 'SceneZone_Scene/models/MIMO_M5_DirectWeight_Real_baseline_35pct.pth')),
    ]
    freqs = [250, 500, 1000, 1500]
    print(f'{"模型":28s}  {"250↔500":>10s} {"250↔1000":>10s} {"250↔1500":>10s}  {"方向区分":>10s}')
    print('-' * 78)
    for name, path in models:
        model, K, miss, unexp = load_from_pth(path)
        dirs = {f: probe_dir(model, tone(f)) for f in freqs}
        d250 = dirs[250]
        c500 = float(np.dot(d250, dirs[500]))
        c1000 = float(np.dot(d250, dirs[1000]))
        c1500 = float(np.dot(d250, dirs[1500]))
        # 区分度: 与 250Hz 的最大方向差 (1 - min cos). 越高越好
        sep = 1.0 - min(c500, c1000, c1500)
        verdict = '频率敏感' if min(c500, c1000, c1500) < 0.8 else '频率盲'
        print(f'{name:28s}  {c500:+.3f}   {c1000:+.3f}   {c1500:+.3f}   {verdict} (分离度 {sep:.2f})')
    print('\n  cos<0.8 = 不同频率会触发 OCG 新簇 (可切换). 全>0.9 = 频率盲.')

if __name__ == '__main__':
    main()
