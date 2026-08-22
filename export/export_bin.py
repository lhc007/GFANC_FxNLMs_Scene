"""PyTorch → C .bin 二进制文件导出 (对齐 c_mimo_gfanc 格式).

用法: python export/export_bin.py

输出: data/*.bin + data/cnn_bank_info.json + data/scenezone_config.json

只导出 SFANC 硬选库部署集: 分类 CNN (MIMO_M5_Scene_Bank.pth → data/cnn_bank_*.bin)
+ 声学路径 + 带通 + 批次指纹 + 全局配置. 回归 CNN 线 (cnn_*.bin / sub_filters) 已移除.

R-16-②: v2 格式添加 16B 头 {magic"GFNC", version, n_floats, crc32}.
C 端 bin_load_float 自动检测 magic — 新格式校验, 旧格式直接加载.
"""
import os, sys, json, struct, zlib, glob
import numpy as np
import scipy.io as sio
import torch
from pathlib import Path

# R-16-②: .bin v2 格式常量
BIN_MAGIC   = b'GFNC'          # 4 bytes magic
BIN_VERSION = 1                 # uint32 LE
BIN_HDR_FMT = '<4sIII'          # magic(4s) + version(I) + n_floats(I) + crc32(I) = 16B

# ═══════════════════════════════════════════════════════════════
# 路径配置 — 优先使用 GFANC_PYTHON_PROJ 环境变量, 否则查找项目内
# ═══════════════════════════════════════════════════════════════
_env_proj = os.environ.get('GFANC_PYTHON_PROJ', '')
if _env_proj:
    PY_PROJ = Path(_env_proj)
else:
    # 默认: 项目根目录下的 SceneZone_Scene
    PY_PROJ = Path(os.path.dirname(os.path.abspath(__file__))).parent / 'SceneZone_Scene'

# CNN 模型 — SFANC 分类 CNN (硬标签 CrossEntropy 训练, K=N 通用类)
CNN_MODEL = PY_PROJ / 'models' / 'MIMO_M5_Scene_Bank.pth'
CNN_BASE  = 'cnn_bank'          # 权重集前缀 → data/cnn_bank_*.bin
CNN_MODE  = 'classification'    # argmax 硬选库槽
CNN_ACT   = 'none'              # 分类: logits 直接 argmax, 无 tanh/softmax
INFO_NAME = 'cnn_bank_info.json'
if not CNN_MODEL.exists():
    raise SystemExit(f'ERROR: 需要 {CNN_MODEL.name} (先跑 '
                     f'training/network/train_real_bank_cnn.py)')

# 主/次声学路径 — R-58-6: 统一到真实硬件录制路径 (用户硬件 3E-2S-1R)!
# 训练 (Pre_training_broadband_and_decompose.py) 必须同步用这两个文件,
# 否则运行时 Ŝ/P 与训练世界不同 → Wc 初值错位 → FxNLMS 发散.
PRI_PATH   = PY_PROJ / 'Primary and Secondary Path' / 'primary_path.npy'
SEC_PATH   = PY_PROJ / 'Primary and Secondary Path' / 'secondary_path.npy'

# 带通 FIR
BP_FIR     = PY_PROJ / 'models' / 'bandpass_fir.mat'

# 输出目录
SCRIPT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR    = SCRIPT_DIR.parent / 'data'
OUT_DIR.mkdir(exist_ok=True)

# 系统参数 (S/C/E 固定)
S, C, E = 2, 15, 3   # S=扬声器, C=子滤波器数, E=误差麦克风数

# ── CNN 输出维度: 库分类 K = ckpt 输出维 (槽序 == 库槽序, 无语义 JSON) ──
ckpt = torch.load(str(CNN_MODEL), map_location='cpu', weights_only=True)
n_out = ckpt['linear.weight'].shape[0]
K = n_out
print(f'CNN mode: classification (K={K} 维, activation=none)')
print(f'CNN checkpoint 输出 {n_out} 维 OK ({CNN_BASE}_*.bin)')

def write_bin(name, arr):
    """R-16-②: v2 格式 — 16B 头 + float32 payload."""
    arr = np.asarray(arr, dtype=np.float32)
    path = OUT_DIR / f'{name}.bin'
    payload = arr.tobytes()
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(BIN_HDR_FMT, BIN_MAGIC, BIN_VERSION, arr.size, crc)
    with open(path, 'wb') as f:
        f.write(header)
        f.write(payload)
    return arr.size  # 总元素数 (不是 len, 多维数组 len 只返回第一维)

def write_json(name, data):
    with open(OUT_DIR / name, 'w') as f:
        json.dump(data, f, indent=2)

# ── 1. CNN 权重 (分类选库) ──
print('Exporting CNN weights...')
sys.path.insert(0, str(PY_PROJ))
from gfanc.Network import m5_scene

model = m5_scene(K=K, dropout=0.3)
model.load_state_dict(ckpt)    # ckpt 已在上面校验环节加载过
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
            f'{CNN_BASE}_{tag}_conv_weight': f'{prefix}.0.weight',
            f'{CNN_BASE}_{tag}_conv_bias':   f'{prefix}.0.bias',
            f'{CNN_BASE}_{tag}_bn_gamma':    f'{prefix}.1.weight',
            f'{CNN_BASE}_{tag}_bn_beta':     f'{prefix}.1.bias',
            f'{CNN_BASE}_{tag}_bn_mean':     f'{prefix}.1.running_mean',
            f'{CNN_BASE}_{tag}_bn_var':      f'{prefix}.1.running_var',
        }
    elif tag == 'linear':
        keys_map = {
            f'{CNN_BASE}_{tag}_weight': f'{prefix}.weight',
            f'{CNN_BASE}_{tag}_bias':   f'{prefix}.bias',
        }
    else:
        idx = int(tag[3])
        keys_map = {
            f'{CNN_BASE}_{tag}_conv1_weight': f'{prefix}.res.0.weight',
            f'{CNN_BASE}_{tag}_conv1_bias':   f'{prefix}.res.0.bias',
            f'{CNN_BASE}_{tag}_bn1_gamma':    f'{prefix}.res.1.weight',
            f'{CNN_BASE}_{tag}_bn1_beta':     f'{prefix}.res.1.bias',
            f'{CNN_BASE}_{tag}_bn1_mean':     f'{prefix}.res.1.running_mean',
            f'{CNN_BASE}_{tag}_bn1_var':      f'{prefix}.res.1.running_var',
            f'{CNN_BASE}_{tag}_conv2_weight': f'{prefix}.res.3.weight',
            f'{CNN_BASE}_{tag}_conv2_bias':   f'{prefix}.res.3.bias',
            f'{CNN_BASE}_{tag}_bn2_gamma':    f'{prefix}.res.4.weight',
            f'{CNN_BASE}_{tag}_bn2_beta':     f'{prefix}.res.4.bias',
            f'{CNN_BASE}_{tag}_bn2_mean':     f'{prefix}.res.4.running_mean',
            f'{CNN_BASE}_{tag}_bn2_var':      f'{prefix}.res.4.running_var',
        }
        proj_key = f'{prefix}.proj.weight'
        if proj_key in sd:
            keys_map[f'{CNN_BASE}_{tag}_proj_weight'] = proj_key

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
    # C 端推理激活: 分类无激活 (logits 直接 argmax)
    'mode': CNN_MODE,
    'activation': CNN_ACT,
    # 库分类: 无语义类名 — 槽 k 即滤波器 k (类序 == 库槽序)
    'classes': [f'filter_{k}' for k in range(K)],
}
write_json(INFO_NAME, cnn_info)
print(f'  {INFO_NAME} saved')

# ── 2. 声学路径 ──
print('Exporting acoustic paths...')
Pri = np.load(str(PRI_PATH))
Sec = np.load(str(SEC_PATH))
# R-58-7: 主路径裁剪到第 0 参考 (E,1,L) — 训练 (Disturbance_generation.py
# _multi_channel_filter_pri) 写死用 pri_path[:,0,:], npy 的 (E,2,L) 第二维
# 是复制占位从不使用; 裁剪后 C 端 e*PRI_LEN 布局与训练语义逐样本一致.
if Pri.ndim == 3 and Pri.shape[1] > 1:
    print(f'  primary_path.npy 裁剪: {list(Pri.shape)} → (E,1,L), 取第 0 参考 (与训练一致)')
    Pri = Pri[:, :1, :]
write_bin('primary_path', Pri)
write_bin('secondary_path', Sec)
print(f'  primary_path.bin: {list(Pri.shape)}')
print(f'  secondary_path.bin: {list(Sec.shape)}')

# ── 3. 带通 FIR ──
print('Exporting bandpass FIR...')
bp = sio.loadmat(str(BP_FIR))
bp_coeff = bp['fir_bandpass_coeff'].flatten().astype(np.float32)
write_bin('bandpass_fir', bp_coeff)
print(f'  bandpass_fir.bin: {len(bp_coeff)} taps')

# ── 3b. ANC 专用短带通 FIR (R-13: 64tap, 群延迟 2ms vs 8ms — 砍环路延迟) ──
print('Exporting ANC bandpass FIR (64tap)...')
from scipy.signal import firwin
# P0-7 (2026-08-14): 低截止 20→50Hz(折中) — S 在 100Hz 以下滚降 25dB(实测), <50Hz
# 的 anti 既消不动又驱动扬声器低频失真(滋滋声). 高截止 1500 不变.
bp_anc_coeff = firwin(64, [50, 1500], fs=16000, pass_zero='bandpass',
                       window='hamming').astype(np.float32)
write_bin('bandpass_anc', bp_anc_coeff)
print(f'  bandpass_anc.bin: {len(bp_anc_coeff)} taps, '
      f'gd={(64-1)/(2*16000)*1000:.1f}ms (vs 1024tap gd={(1024-1)/(2*16000)*1000:.1f}ms)')

# ── 4. 批次指纹 (R-27) — 防 cnn_bank/bandpass 跨批混配 ──
# 指纹 = 对 [排序后的 cnn_bank_*.bin + bandpass_fir + bandpass_anc]
#       原始字节做链式 crc32 (与 C 端 binary_loader.c bin_crc32_chain 语义一致:
#       zlib.crc32(data, prev) 续算). 声学路径 (primary/secondary/feedback…) 是
#       安装态可替换的测量值, 不入指纹 — 换 Ŝ 属设计行为 (R-16-①/BUG-8).
def _batch_crc32():
    files = sorted(glob.glob(str(OUT_DIR / 'cnn_bank_*.bin'))) + [
        str(OUT_DIR / 'bandpass_fir.bin'),
        str(OUT_DIR / 'bandpass_anc.bin'),
    ]
    crc = 0
    for p in files:
        with open(p, 'rb') as f:
            data = f.read()
        crc = zlib.crc32(data, crc) & 0xFFFFFFFF
    return crc, files

batch_crc, batch_files = _batch_crc32()
with open(OUT_DIR / 'batch_id.bin', 'w') as f:
    f.write('0x%08x\n' % batch_crc)
batch_info = {
    'batch_id': '0x%08x' % batch_crc,
    'count': len(batch_files),
    'files': [os.path.relpath(p, OUT_DIR.parent).replace('\\', '/') for p in batch_files],
}
write_json('batch_info.json', batch_info)
print(f'  batch_id.bin: 0x{batch_crc:08x} ({len(batch_files)} files)')

# ── 5. 全局配置 ──
config = {
    'fs': 16000, 'E': E, 'S': S, 'C': C, 'K': K,
    'pri_len': int(Pri.shape[2]), 'sec_len': int(Sec.shape[2]),
    'input_len': 16000, 'bp_len': int(len(bp_coeff)),
    'bp_anc_len': 256,
    'dsp_delay': 16, 'fade_len': 1600,
    # CNN 模式: SFANC 分类 (argmax 硬选库槽, 无激活)
    'cnn_mode': CNN_MODE,
    'cnn_activation': CNN_ACT,
    'n_cnn_out': K,
}
write_json('scenezone_config.json', config)

print(f'\nDone. All .bin files in {OUT_DIR}/')
