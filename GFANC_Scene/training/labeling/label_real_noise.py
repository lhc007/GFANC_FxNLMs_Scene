r"""
真实噪声标注脚本 — 切割(一次性) + LMS标注(可重复).

用法:
    # 第一次: 切割 + 标注
    python training/labeling/label_real_noise.py

    # 子滤波器重训后: 只重标注 (读已有WAV, 不重切)
    python training/labeling/label_real_noise.py

输出:
    D:\Dataset\Real_world_Dataset\Training_data\  (1s WAV 片段)
    D:\Dataset\Real_world_Dataset\Index_real_*.csv (文件索引 + LMS增益)
    D:\Dataset\Real_world_Dataset\Gains_real_*.npy  (LMS增益矩阵)

聚类由 recluster_real.py 单独完成:
    python training/labeling/recluster_real.py
    → scene_definitions_real.json + SoftLabels + scene_id
"""
import os, sys, json
import numpy as np
import scipy.io as sio
import torch
import torchaudio
import soundfile
from collections import Counter, defaultdict
from tqdm import tqdm
from pathlib import Path

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

from gfanc._paths import MODELS_DIR
from training.control_filters.path_loader import load_multichannel_paths_with_variable_names
from training.control_filters.Disturbance_generation import disturbance_generation_batch_gpu
from training.labeling.Adaptive_control_filter_generator_batch import (
    adaptive_control_filter_batch_mimo, train_adaptive_gain_batch_mimo)

# ══════════════════════════════════════════════════
# 路径配置
# ══════════════════════════════════════════════════
NOISE_DIR   = r'D:\Dataset\Real_world_Dataset\raw'   # 原始噪声 WAV 目录
OUTPUT_DIR  = r'D:\Dataset\Real_world_Dataset'        # 输出根目录

# ══════════════════════════════════════════════════
# 数据集配置
# ══════════════════════════════════════════════════
TOTAL_SEGMENTS = 80000  # 标注总目标段数
TRAIN_RATIO = 0.8        # 训练集比例
VAL_RATIO   = 0.1        # 验证集比例
TEST_RATIO  = 0.1        # 测试集比例
BLOCK_SIZE   = 200       # 块拆分: 每块片段数 (避免同录音相邻片段泄漏到 train/val)
RANDOM_SEED  = 42        # 全局随机种子 (采样/拆分可复现)

# ══════════════════════════════════════════════════
# 噪声类别配置  (step=切割步长(s), max_sec=每文件最大使用时长, train_ratio=占总目标比例)
# ══════════════════════════════════════════════════
CATEGORY_CONFIG = {
    'road':          {'step': 1.0, 'max_sec': 2 * 3600, 'train_ratio': 0.25},  # 道路
    # 'street':      {'step': 1.0, 'max_sec': 2 * 3600, 'train_ratio': 0.20},  # 街道
    'children':      {'step': 1.0, 'max_sec': 1 * 3600, 'train_ratio': 0.25},  # 儿童
    # 'square_dance':{'step': 1.0, 'max_sec': 99 * 3600, 'train_ratio': 0.00}, # 广场舞 (缺数据)
    'construction':  {'step': 1.0, 'max_sec': 1 * 3600, 'train_ratio': 0.25},  # 施工
    'railway':       {'step': 1.0, 'max_sec': 99 * 3600, 'train_ratio': 0.25}, # 铁路
}

# ══════════════════════════════════════════════════
# LMS 标注参数
# ══════════════════════════════════════════════════
FS          = 16000     # 采样率 (Hz)
CHUNK_SEC   = 1.0       # 每片段时长 (秒)
LMS_MU      = 0.001     # LMS 步长
LMS_REPET   = 3         # 噪声重复次数 (1→1s迭代, 3→3s, 提高增益收敛)
BATCH_SIZE  = 128       # GPU 批大小
FX_NOISE_DB = -30       # Fx 注入噪声 SNR (dB, 防止增益坍缩到零)
DEVICE      = 'cuda' if torch.cuda.is_available() else 'cpu'

# ══════════════════════════════════════════════════
# 子滤波器配置
# ══════════════════════════════════════════════════
# 必须与部署基一致: 部署 (export_bin.py → data/sub_filters.bin → C 运行时) 和
# MIMO_GFANC 参考均用 broadband。logspacing 与 broadband 内容不同 (最大差 0.33),
# 用 logspacing 标出的增益在部署 broadband 基下不是最优权重 → 必须用 broadband 重标。
USE_LOG_SPACING = False  # False=均匀间距 20~1500Hz (部署基, 与 export_bin.py/MIMO 参考一致)
# ══════════════════════════════════════════════════

chunk_len = int(CHUNK_SEC * FS)

# ── 加载资源 ──
print('=' * 60); print('  加载资源...'); print('=' * 60)
Pri_path, Sec_path = load_multichannel_paths_with_variable_names(
    folder=str(_PROJECT_ROOT / 'Primary and Secondary Path'), subfolder='',
    Pri_path_file_name='primary_path.npy',
    Sec_path_file_name='secondary_path.npy')
# ── 子滤波器加载 ──
_FILTER_FILE = ('MIMO_Pretrained_Control_filters_logspacing.mat' if USE_LOG_SPACING
               else 'MIMO_Pretrained_Control_filters_broadband.mat')
sub_filters = sio.loadmat(str(MODELS_DIR / _FILTER_FILE))['Wc_v']
sub_T = torch.from_numpy(sub_filters).type(torch.float)
C, S, _ = sub_T.shape
print(f'  C={C}, S={S}, device={DEVICE}')

os.makedirs(os.path.join(OUTPUT_DIR, 'Training_data'), exist_ok=True)

# ── 切割: 由 cut_config.json 控制, 用户手动将待切文件从 cut 移到 uncut ──
train_dir = os.path.join(OUTPUT_DIR, 'Training_data')
os.makedirs(train_dir, exist_ok=True)
existing_wavs = sorted([f for f in os.listdir(train_dir) if f.endswith('.wav')])
cut_config_path = os.path.join(_SCRIPT_DIR, 'cut_config.json')

# 加载或初始化 cut_config
if os.path.exists(cut_config_path):
    with open(cut_config_path, 'r', encoding='utf-8') as f:
        cut_config = json.load(f)
else:
    # 首次: 所有 raw 文件放入 cut, uncut 为空
    cut_config = {}
    for cat_name in CATEGORY_CONFIG:
        cat_dir = os.path.join(NOISE_DIR, cat_name)
        if os.path.isdir(cat_dir):
            cut_config[cat_name] = {
                'cut': sorted([f for f in os.listdir(cat_dir) if f.lower().endswith('.wav')]),
                'uncut': []
            }
    with open(cut_config_path, 'w', encoding='utf-8') as f:
        json.dump(cut_config, f, indent=2, ensure_ascii=False)
    print(f'  cut_config.json 已初始化: training/labeling/cut_config.json')
    print(f'  用法: 将需要切割的文件名从 cut 移到 uncut, 然后重跑本脚本')

# 收集待切文件
to_cut = []
for cat_name, cfg in CATEGORY_CONFIG.items():
    cat_dir = os.path.join(NOISE_DIR, cat_name)
    if not os.path.isdir(cat_dir): continue
    cat_cfg = cut_config.get(cat_name, {'cut': [], 'uncut': []})
    uncut_set = set(cat_cfg.get('uncut', []))
    if not uncut_set:
        continue
    for fname in sorted(uncut_set):
        fpath = os.path.join(cat_dir, fname)
        if not os.path.exists(fpath):
            print(f'  [警告] {cat_name}/{fname} 不存在, 已跳过')
            continue
        try:
            info = soundfile.info(fpath)
            dur = info.duration
        except Exception:
            print(f'  [跳过] 格式错误: {fname}')
            continue
        max_frames = min(int(dur * info.samplerate), int(cfg['max_sec'] * FS))
        step_len = int(cfg['step'] * FS)
        n = max(0, (max_frames - chunk_len) // step_len + 1)
        if n > 0:
            to_cut.append((cat_name, cfg, fname, fpath, n))

if to_cut:
    print(f'\n{"=" * 60}')
    print(f'  切割: {len(to_cut)} 个文件 (来自 cut_config.json uncut 列表)')
    for cat_name, _, fname, _, n in to_cut:
        print(f'    {cat_name}: {fname} → ~{n} 片段')
    print(f'{"=" * 60}')
    # 找到当前最大 global_idx
    max_idx = 0
    for f in existing_wavs:
        for cat in CATEGORY_CONFIG:
            if f.startswith(cat + '_'):
                try:
                    num = int(f.replace(cat + '_', '').replace('.wav', ''))
                    max_idx = max(max_idx, num)
                except: pass
                break
    global_idx = max_idx
    for cat_name, cfg, fname, fpath, n_est in to_cut:
        try:
            data, sr = soundfile.read(fpath)
        except Exception:
            print(f'  [跳过] 格式错误: {fname}')
            continue
        if data.ndim > 1: data = np.mean(data, axis=1)
        if sr != FS:
            import scipy.signal as signal
            data = signal.resample(data, int(len(data) * FS / sr))
        peak = np.max(np.abs(data))
        if peak > 1e-12: data = data / peak
        max_frames = min(len(data), int(cfg['max_sec'] * FS))
        step_len = int(cfg['step'] * FS)
        n = max(0, (max_frames - chunk_len) // step_len + 1)
        if n == 0: continue
        for i in tqdm(range(n), desc=f'  切割 {cat_name}', unit='段', leave=False):
            chunk = data[i*step_len : i*step_len+chunk_len].astype(np.float32)
            global_idx += 1
            torchaudio.save(os.path.join(train_dir, f'{cat_name}_{global_idx:06d}.wav'),
                           torch.from_numpy(chunk).unsqueeze(0), FS)
        # 切割完成 → 移回 cut
        cat_cfg = cut_config[cat_name]
        cat_cfg['uncut'].remove(fname)
        cat_cfg['cut'].append(fname)
    print(f'  切割完成: {global_idx - max_idx} 个新片段')
    with open(cut_config_path, 'w', encoding='utf-8') as f:
        json.dump(cut_config, f, indent=2, ensure_ascii=False)
    print(f'  cut_config.json 已更新 (文件已移入 cut)')
else:
    print(f'\n  uncut 为空, 跳过切割')

# ── LMS 标注 (从 Train/Val/Test 三目录读已有片段) ──
wav_locations = {}  # filename → dir
for split_dir in ['Training_data', 'Validate_data', 'Testing_data']:
    d = os.path.join(OUTPUT_DIR, split_dir)
    if os.path.isdir(d):
        for f in os.listdir(d):
            if f.endswith('.wav'):
                wav_locations[f] = d

existing_wavs = sorted(wav_locations.keys())
cat_files = defaultdict(list)
for f in existing_wavs:
    for cat in CATEGORY_CONFIG:
        if f.startswith(cat + '_'):
            cat_files[cat].append(f); break

print(f'\n{"=" * 60}')
print(f'  LMS 标注: {len(existing_wavs)} 片段, {len(cat_files)} 类别')
print(f'{"=" * 60}')

# 计算每类目标段数 = TOTAL_SEGMENTS × train_ratio (不超过可用量)
cat_targets = {}
for cat_name, fnames in cat_files.items():
    target = int(TOTAL_SEGMENTS * CATEGORY_CONFIG[cat_name]['train_ratio'])
    cat_targets[cat_name] = min(target, len(fnames))

print(f'\n  总目标: {TOTAL_SEGMENTS}, 实际可标: {sum(cat_targets.values())}')
for cat_name, fnames in cat_files.items():
    t = cat_targets[cat_name]
    print(f'    {cat_name}: {t}/{len(fnames)} ({100*t/max(1,len(fnames)):.0f}%) [{t/max(1,sum(cat_targets.values())):.1%}]')

all_entries = []
rng = np.random.RandomState(RANDOM_SEED)
_lms_diag_printed = [False]  # 首批 LMS 收敛诊断只打印一次

for cat_name, fnames in cat_files.items():
    fnames = sorted(fnames)
    n_avail = len(fnames)
    n_seg = cat_targets[cat_name]
    if n_avail > n_seg:
        fnames = list(rng.choice(fnames, n_seg, replace=False))
    print(f'\n  [{cat_name}] {n_seg} 片段')
    chunks = np.zeros((n_seg, chunk_len), dtype=np.float32)
    for i, fname in enumerate(sorted(fnames)):
        sig, _ = torchaudio.load(os.path.join(wav_locations[fname], fname))
        sig = sig.squeeze().numpy().astype(np.float32)
        chunks[i] = sig[:chunk_len] if len(sig) >= chunk_len else np.pad(sig, (0, chunk_len - len(sig)))

    for bs in tqdm(range(0, n_seg, BATCH_SIZE), desc=f'  标注 {cat_name}', unit='批'):
        be = min(bs + BATCH_SIZE, n_seg)
        ba = be - bs
        batch_chunks = [torch.from_numpy(chunks[i]) for i in range(bs, be)]

        Dis_batch, Fx_batch, _ = disturbance_generation_batch_gpu(
            batch_chunks, Pri_path, Sec_path, fs=FS, Repet=LMS_REPET)
        # Fx 注入 30dB 低噪 — 防止滤波器增益坍缩到零 (GFANC-generative 已验证)
        fx_power = Fx_batch.norm(p=2, dim=(1,2), keepdim=True) / (Fx_batch.shape[1]*Fx_batch.shape[2])**0.5
        noise_power = fx_power * (10 ** (FX_NOISE_DB / 20))
        fx_noise = torch.randn_like(Fx_batch) * noise_power
        Fx_batch = Fx_batch + fx_noise
        gen = adaptive_control_filter_batch_mimo(sub_T, Batch_size=ba, muw=LMS_MU, device=DEVICE)
        err_traj = train_adaptive_gain_batch_mimo(gen, Fx_batch, Dis_batch, device=DEVICE)
        gains_batch = gen.get_coeffiecients_().cpu().numpy()

        # 首批: LMS 收敛诊断 (1s 迭代是否足够 — 误差应明显衰减)
        if not _lms_diag_printed[0]:
            _lms_diag_printed[0] = True
            e_abs = np.abs(err_traj)  # (T, E)
            head = e_abs[:1600].mean(); tail = e_abs[-1600:].mean()
            decay_db = 20 * np.log10(max(tail, 1e-15) / max(head, 1e-15))
            status = '✓ 收敛良好' if decay_db < -3 else ('△ 收敛中' if decay_db < 0 else '✗ 未收敛, 考虑增大 LMS_MU 或 Repet')
            print(f'\n  [LMS收敛诊断] 首批误差: 前0.1s RMS={head:.4f} → 末0.1s RMS={tail:.4f} '
                  f'({decay_db:+.1f}dB) {status}\n')

        for j in range(ba):
            all_entries.append({
                'idx': len(all_entries) + 1, 'file_path': fnames[bs + j],
                'source': cat_name, 'lms_gains': gains_batch[j].copy()})

# ── 拆分 + 保存 (不聚类, 聚类由 recluster_real.py 完成) ──
import pandas as pd
import shutil, re

# 首次标注: 集中在一个目录 → 分层拆分 WAV; 之后: WAV 固定不动
has_split_dirs = (os.path.isdir(os.path.join(OUTPUT_DIR, 'Validate_data')) and
                  os.path.isdir(os.path.join(OUTPUT_DIR, 'Testing_data')) and
                  any(f.endswith('.wav') for f in os.listdir(os.path.join(OUTPUT_DIR, 'Validate_data'))))

if not has_split_dirs:
    # 按连续块拆分 + 移动 WAV (避免同一原始录音的相邻片段同时落入 train/val)
    np.random.seed(RANDOM_SEED)
    split_map = {}
    for cat in sorted(cat_files.keys()):
        cat_entries = [e for e in all_entries if e['source'] == cat]
        cat_entries.sort(key=lambda e: int(re.search(r'_(\d+)\.wav$', e['file_path']).group(1)))
        blocks = [cat_entries[i:i + BLOCK_SIZE] for i in range(0, len(cat_entries), BLOCK_SIZE)]
        np.random.shuffle(blocks)
        n = len(cat_entries); nt = int(n * TRAIN_RATIO); nv = int(n * VAL_RATIO)
        cum = 0
        for blk in blocks:
            sp = 'Training_data' if cum < nt else ('Validate_data' if cum < nt + nv else 'Testing_data')
            for e in blk:
                split_map[e['idx']] = sp
            cum += len(blk)
    for split_name in ['Training_data', 'Validate_data', 'Testing_data']:
        out_dir = os.path.join(OUTPUT_DIR, split_name)
        os.makedirs(out_dir, exist_ok=True)
        for e in all_entries:
            if split_map.get(e['idx']) != split_name: continue
            fname = e['file_path']
            src = os.path.join(wav_locations.get(fname, train_dir), fname)
            dst = os.path.join(out_dir, fname)
            if os.path.exists(src) and src != dst:
                if not os.path.exists(dst):
                    shutil.move(src, dst)
                    wav_locations[fname] = out_dir
    print(f'  首次拆分完成: Train/Val/Test = {TRAIN_RATIO:.0%}/{VAL_RATIO:.0%}/{TEST_RATIO:.0%}')

# 写 CSV + Gains npy (按 WAV 当前所在目录确定 split)
SC = S * C
wav_to_split = {fname: os.path.basename(d) for fname, d in wav_locations.items()}

for split_name in ['Training_data', 'Validate_data', 'Testing_data']:
    entries = [e for e in all_entries if wav_to_split.get(e['file_path']) == split_name]
    if not entries:
        continue
    rows = []
    gains_arr = np.zeros((len(entries), SC), dtype=np.float32)
    for i, e in enumerate(entries):
        rows.append({'': i+1, 'File_path': e['file_path'], 'category': e['source']})
        for b in range(SC):
            rows[-1][f'gain_{b}'] = float(e['lms_gains'].flatten()[b])
        gains_arr[i] = e['lms_gains'].flatten()
    df = pd.DataFrame(rows)
    df.to_csv(os.path.join(OUTPUT_DIR, f'Index_real_{split_name}.csv'), index=False)
    np.save(os.path.join(OUTPUT_DIR, f'Gains_real_{split_name}.npy'), gains_arr)
    print(f'  {split_name}: {len(rows)} 片段')

print(f'\n总: {len(all_entries)} 片段')
source_counts = Counter(e['source'] for e in all_entries)
for src, cnt in source_counts.most_common():
    print(f'  {src:15s}: {cnt:6d} ({100*cnt/len(all_entries):.1f}%)')

print(f'\n  标注完成. 下一步: python training/labeling/recluster_real.py')
