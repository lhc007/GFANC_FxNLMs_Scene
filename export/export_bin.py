"""PyTorch → C .bin 二进制文件导出 (对齐 c_mimo_gfanc 格式).

用法: python export/export_bin.py

输出: data/*.bin + data/cnn_info.json + data/gfanc_config.json
"""
import os, sys, json, struct
import numpy as np
import scipy.io as sio
import torch
from pathlib import Path

SCRIPT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = SCRIPT_DIR.parent / 'data'
PY_PROJ = Path(r'd:/VSCodeRepository/GFANC_Scene')
MODELS_DIR = PY_PROJ / 'models'
PATHS_DIR = PY_PROJ / 'Primary and Secondary Path'

K, S, C, E = 8, 2, 15, 3
OUT_DIR.mkdir(exist_ok=True)

def write_bin(name, arr):
    arr = np.asarray(arr, dtype=np.float32)
    path = OUT_DIR / f'{name}.bin'
    arr.tofile(path)
    return len(arr)

def write_json(name, data):
    with open(OUT_DIR / name, 'w') as f:
        json.dump(data, f, indent=2)

# ── 1. CNN 权重 ──
print('Exporting CNN weights...')
sys.path.insert(0, str(PY_PROJ))
from gfanc.Network import m5_scene

model = m5_scene(K=K, dropout=0.3)
state = torch.load(str(MODELS_DIR / 'MIMO_M5_Scene_Real.pth'), map_location='cpu', weights_only=True)
model.load_state_dict(state)
model.eval()

sd = model.state_dict()
# Export each weight as a separate .bin file, following c_mimo_gfanc naming
weight_map = {
    'stem': 'conv_block',
    'res0': 'res_blocks.0.0',
    'res1': 'res_blocks.0.1',
    'res2': 'res_blocks.1.0',
    'res3': 'res_blocks.1.1',
    'linear': 'linear',
}

for tag, prefix in weight_map.items():
    if tag == 'stem':
        keys_map = {
            f'cnn_{tag}_conv_weight': f'{prefix}.0.weight',
            f'cnn_{tag}_conv_bias':   f'{prefix}.0.bias',
            f'cnn_{tag}_bn_gamma':    f'{prefix}.1.weight',
            f'cnn_{tag}_bn_beta':     f'{prefix}.1.bias',
            f'cnn_{tag}_bn_mean':     f'{prefix}.1.running_mean',
            f'cnn_{tag}_bn_var':      f'{prefix}.1.running_var',
        }
    elif tag == 'linear':
        keys_map = {
            f'cnn_{tag}_weight': f'{prefix}.weight',
            f'cnn_{tag}_bias':   f'{prefix}.bias',
        }
    else:
        idx = int(tag[3])
        keys_map = {
            f'cnn_{tag}_conv1_weight': f'{prefix}.res.0.weight',
            f'cnn_{tag}_conv1_bias':   f'{prefix}.res.0.bias',
            f'cnn_{tag}_bn1_gamma':    f'{prefix}.res.1.weight',
            f'cnn_{tag}_bn1_beta':     f'{prefix}.res.1.bias',
            f'cnn_{tag}_bn1_mean':     f'{prefix}.res.1.running_mean',
            f'cnn_{tag}_bn1_var':      f'{prefix}.res.1.running_var',
            f'cnn_{tag}_conv2_weight': f'{prefix}.res.3.weight',
            f'cnn_{tag}_conv2_bias':   f'{prefix}.res.3.bias',
            f'cnn_{tag}_bn2_gamma':    f'{prefix}.res.4.weight',
            f'cnn_{tag}_bn2_beta':     f'{prefix}.res.4.bias',
            f'cnn_{tag}_bn2_mean':     f'{prefix}.res.4.running_mean',
            f'cnn_{tag}_bn2_var':      f'{prefix}.res.4.running_var',
        }
        proj_key = f'{prefix}.proj.weight'
        if proj_key in sd:
            keys_map[f'cnn_{tag}_proj_weight'] = proj_key

    for fname, key in keys_map.items():
        if key in sd:
            n = write_bin(fname, sd[key].cpu().numpy())
            print(f'  {fname}.bin: {n} floats')

# CNN metadata
cnn_info = {
    'K': K, 'input_len': 16000,
    'stem_out': 64, 'stem_kernel': 80, 'stem_stride': 4, 'stem_pad': 38,
    'pool0_kernel': 4, 'pool0_stride': 8,
    'res_groups': 2, 'res_per_group': 2, 'res_channels': 64,
    'res_kernel': 3, 'res_stride': 1, 'res_pad': 1,
    'pool_kernel': 4, 'pool_stride': 4,
    'fc_in': 64, 'fc_out': K,
    'bn_eps': 1e-5,
}
write_json('cnn_info.json', cnn_info)
print(f'  cnn_info.json saved')

# ── 2. 子滤波器 ──
print('Exporting sub-filters...')
Wc_v = sio.loadmat(str(MODELS_DIR / 'MIMO_Pretrained_Control_filters_broadband.mat'))['Wc_v']
write_bin('sub_filters', Wc_v)
sub_info = {'C': C, 'S': S, 'filter_len': int(Wc_v.shape[2])}
write_json('sub_filters_info.json', sub_info)
print(f'  sub_filters.bin: shape={list(Wc_v.shape)}')

# ── 3. 声学路径 ──
print('Exporting acoustic paths...')
Pri = np.load(str(PATHS_DIR / 'primary_path_angle_0deg_left_3mic.npy'))
Sec = np.load(str(PATHS_DIR / 'secondary_path_measured.npy'))
write_bin('primary_path', Pri)
write_bin('secondary_path', Sec)
print(f'  primary_path.bin: {list(Pri.shape)}')
print(f'  secondary_path.bin: {list(Sec.shape)}')

# ── 4. 场景定义 ──
print('Exporting scene definitions...')
with open(MODELS_DIR / 'scene_definitions_real.json') as f:
    scene_doc = json.load(f)
centroids = np.array([scene_doc['scenes'][str(k)]['centroid'] for k in range(K)], dtype=np.float32)
write_bin('scene_defs', centroids)
print(f'  scene_defs.bin: {list(centroids.shape)}')

# ── 5. 带通 FIR ──
print('Exporting bandpass FIR...')
bp = sio.loadmat(str(MODELS_DIR / 'bandpass_filter_20_1500Hz.mat'))
bp_coeff = bp['fir_bandpass_coeff'].flatten().astype(np.float32)
write_bin('bandpass_fir', bp_coeff)
print(f'  bandpass_fir.bin: {len(bp_coeff)} taps')

# ── 6. 全局配置 ──
config = {
    'fs': 16000, 'E': E, 'S': S, 'C': C, 'K': K,
    'filter_len': int(Wc_v.shape[2]),
    'pri_len': int(Pri.shape[2]), 'sec_len': int(Sec.shape[2]),
    'input_len': 16000, 'bp_len': int(len(bp_coeff)),
    'dsp_delay': 16, 'fade_len': 16, 'sc_dim': S * C,
}
write_json('gfanc_config.json', config)

print(f'\nDone. All .bin files in {OUT_DIR}/')
