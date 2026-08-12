r"""
真实噪声切割 — 长录音切 1s 片段 + 首建分层拆分（打标签交给 label_wavs.py）.

背景:
  真实原始录音在 D:\Dataset\Real_world_Dataset\raw\{类别}\*.wav (长录音, 分钟级).
  训练需要 1s 片段 → 本脚本按类别切块, 首建时按 0.8/0.1/0.1 分层拆
  Training/Validate/Testing_data (连续块防同录音相邻片段泄漏到 train/val).
  打标签用 label_wavs.py --tag real.

用法:
    # 首次: 初始化 cut_config.json (所有 raw 文件进 cut 列表)
    python training/labeling/cut_real_noise.py

    # 切割: 把想切的文件名从 cut_config.json 的 cut 移到 uncut, 再重跑本脚本
    python training/labeling/cut_real_noise.py

    # 类别平衡: 每类最多保留 20000 段 (旧流程复现规模 = 4 类 × 20000)
    python training/labeling/cut_real_noise.py --max-per-category 20000

输出:
    D:\Dataset\Real_world_Dataset\{Training,Validate,Testing}_data\{类别}_*.wav (1s)
    下一步打标签: python training/labeling/label_wavs.py --wav-dir <out> --tag real

注意: 切割为增量状态机 — cut_config.json 里的 cut=已切 / uncut=待切 文件名列表.
"""
import os, sys, json, argparse, re, shutil
import numpy as np
import scipy.signal as signal
import soundfile
import torch
import torchaudio
from collections import Counter
from pathlib import Path
from tqdm import tqdm

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# ══════════════════════════════════════════════════
# 路径配置
# ══════════════════════════════════════════════════
NOISE_DIR   = r'D:\Dataset\Real_world_Dataset\raw'   # 原始噪声 WAV 目录
OUTPUT_DIR  = r'D:\Dataset\Real_world_Dataset'        # 输出根目录

# ══════════════════════════════════════════════════
# 数据集配置
# ══════════════════════════════════════════════════
TRAIN_RATIO = 0.8        # 训练集比例
VAL_RATIO   = 0.1        # 验证集比例
TEST_RATIO  = 0.1        # 测试集比例
BLOCK_SIZE   = 200       # 块拆分: 每块片段数 (避免同录音相邻片段泄漏到 train/val)
RANDOM_SEED  = 42        # 全局随机种子 (拆分可复现)

# ══════════════════════════════════════════════════
# 噪声类别配置  (step=切割步长(s), max_sec=每文件最大使用时长)
# ══════════════════════════════════════════════════
CATEGORY_CONFIG = {
    'road':          {'step': 1.0, 'max_sec': 2 * 3600},  # 道路
    # 'street':      {'step': 1.0, 'max_sec': 2 * 3600},  # 街道
    'children':      {'step': 1.0, 'max_sec': 1 * 3600},  # 儿童
    # 'square_dance':{'step': 1.0, 'max_sec': 99 * 3600}, # 广场舞 (缺数据)
    'construction':  {'step': 1.0, 'max_sec': 1 * 3600},  # 施工
    'railway':       {'step': 1.0, 'max_sec': 99 * 3600}, # 铁路
}

# ══════════════════════════════════════════════════
# 切割参数
# ══════════════════════════════════════════════════
FS          = 16000     # 采样率 (Hz)
CHUNK_SEC   = 1.0       # 每片段时长 (秒)
chunk_len   = int(CHUNK_SEC * FS)

DEFAULT_MAX_PER_CATEGORY = None   # 每类最多保留段数 (None=不限)


def _script_config_path():
    return os.path.join(_SCRIPT_DIR, 'cut_config.json')


def load_or_init_cut_config(config_path, noise_dir, category_config):
    """加载 cut_config.json; 首次初始化 = 所有 raw 文件进 cut 列表. 返回 config dict."""
    if os.path.exists(config_path):
        with open(config_path, 'r', encoding='utf-8') as f:
            return json.load(f)
    cut_config = {}
    for cat_name in category_config:
        cat_dir = os.path.join(noise_dir, cat_name)
        if os.path.isdir(cat_dir):
            cut_config[cat_name] = {
                'cut': sorted([f for f in os.listdir(cat_dir) if f.lower().endswith('.wav')]),
                'uncut': []
            }
    with open(config_path, 'w', encoding='utf-8') as f:
        json.dump(cut_config, f, indent=2, ensure_ascii=False)
    print(f'  cut_config.json 已初始化: {config_path}')
    print(f'  用法: 将需要切割的文件名从 cut 移到 uncut, 然后重跑本脚本')
    return cut_config


def collect_to_cut(cut_config, noise_dir, category_config, chunk_len):
    """收集 uncut 待切文件, 估算各文件可切段数. 返回 [(cat_name, cfg, fname, fpath, n)]."""
    to_cut = []
    for cat_name, cfg in category_config.items():
        cat_dir = os.path.join(noise_dir, cat_name)
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
    return to_cut


def scan_existing_counts(out_root, category_config):
    """统计 3 个 split 目录已有段数 (预算记账, 支持增量续切). 返回 {cat: total}."""
    counts = Counter()
    for split in ['Training_data', 'Validate_data', 'Testing_data']:
        d = os.path.join(out_root, split)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if not f.endswith('.wav'):
                continue
            for cat in category_config:
                if f.startswith(cat + '_'):
                    counts[cat] += 1
                    break
    return counts


def _max_global_idx(out_root, category_config):
    """跨 3 目录取 {cat}_NNNNNN 的最大序号 (切割命名用, 增量续切不冲突)."""
    max_idx = 0
    for split in ['Training_data', 'Validate_data', 'Testing_data']:
        d = os.path.join(out_root, split)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            for cat in category_config:
                if f.startswith(cat + '_'):
                    try:
                        num = int(f.replace(cat + '_', '').replace('.wav', ''))
                        max_idx = max(max_idx, num)
                    except Exception:
                        pass
                    break
    return max_idx


def cut_files(to_cut, cut_config, out_root, category_config, budget, fs=FS, chunk_len=chunk_len):
    """滑窗切 1s 段写入 Training_data. 每类受 budget[cat] 上限约束 (None=不限).
    段被完整消费的文件从 uncut 移到 cut; 预算中途用尽的文件留在 uncut (提高预算可续切)."""
    train_dir = os.path.join(out_root, 'Training_data')
    os.makedirs(train_dir, exist_ok=True)
    global_idx = _max_global_idx(out_root, category_config)
    n_cut_total = 0
    for cat_name, cfg, fname, fpath, n_est in to_cut:
        remaining = budget.get(cat_name)
        if remaining is not None and remaining <= 0:
            print(f'  [跳过] {cat_name}: 已达预算上限')
            continue
        try:
            data, sr = soundfile.read(fpath)
        except Exception:
            print(f'  [跳过] 格式错误: {fname}')
            continue
        if data.ndim > 1:
            data = np.mean(data, axis=1)
        if sr != fs:
            data = signal.resample(data, int(len(data) * fs / sr))
        peak = np.max(np.abs(data))
        if peak > 1e-12:
            data = data / peak
        max_frames = min(len(data), int(cfg['max_sec'] * fs))
        step_len = int(cfg['step'] * fs)
        n = max(0, (max_frames - chunk_len) // step_len + 1)
        if n == 0:
            continue
        n_actual = 0
        for i in tqdm(range(n), desc=f'  切割 {cat_name}', unit='段', leave=False):
            if remaining is not None and remaining <= 0:
                break
            chunk = data[i*step_len : i*step_len+chunk_len].astype(np.float32)
            global_idx += 1
            torchaudio.save(os.path.join(train_dir, f'{cat_name}_{global_idx:06d}.wav'),
                            torch.from_numpy(chunk).unsqueeze(0), fs)
            n_actual += 1
            if remaining is not None:
                remaining -= 1
                budget[cat_name] = remaining
        # 预算中途用尽 → 文件留 uncut (续切); 否则移回 cut
        if remaining is not None and remaining <= 0 and n_actual < n:
            print(f'  {cat_name}/{fname}: 预算用尽, 切 {n_actual}/{n} 段, 留在 uncut (提高预算重跑可续)')
        else:
            cat_cfg = cut_config[cat_name]
            cat_cfg['uncut'].remove(fname)
            cat_cfg['cut'].append(fname)
        n_cut_total += n_actual
    print(f'  切割完成: 新增 {n_cut_total} 段')


def has_split_dirs(out_root):
    """Validate/Testing 目录已含 WAV 判定 (首建分层只做一次)."""
    return (os.path.isdir(os.path.join(out_root, 'Validate_data')) and
            os.path.isdir(os.path.join(out_root, 'Testing_data')) and
            any(f.endswith('.wav')
                for f in os.listdir(os.path.join(out_root, 'Validate_data'))))


def stratified_split(out_root, rng, category_config, train_ratio, val_ratio, block_size, test_ratio=0.1):
    """首建分层: 把 Training_data 下全部段按类别 + 连续块拆到 Training/Validate/Testing.
    连续 BLOCK_SIZE 块 shuffle — 避免同原始录音的相邻片段同时落入 train/val."""
    src_dir = os.path.join(out_root, 'Training_data')
    if not os.path.isdir(src_dir):
        return
    all_f = [f for f in os.listdir(src_dir) if f.endswith('.wav')]
    cat_files = {}
    for f in all_f:
        for cat in category_config:
            if f.startswith(cat + '_'):
                cat_files.setdefault(cat, []).append(f)
                break
    split_map = {}
    for cat in sorted(cat_files.keys()):
        cat_entries = sorted(cat_files[cat],
                             key=lambda f: int(re.search(r'_(\d+)\.wav$', f).group(1)))
        blocks = [cat_entries[i:i + block_size]
                  for i in range(0, len(cat_entries), block_size)]
        rng.shuffle(blocks)
        n = len(cat_entries)
        nt = int(n * train_ratio)
        nv = int(n * val_ratio)
        cum = 0
        for blk in blocks:
            sp = 'Training_data' if cum < nt else ('Validate_data' if cum < nt + nv else 'Testing_data')
            for f in blk:
                split_map[f] = sp
            cum += len(blk)
    for split_name in ['Training_data', 'Validate_data', 'Testing_data']:
        out_dir = os.path.join(out_root, split_name)
        os.makedirs(out_dir, exist_ok=True)
        for f in all_f:
            if split_map.get(f) != split_name:
                continue
            src = os.path.join(src_dir, f)
            dst = os.path.join(out_dir, f)
            if os.path.exists(src) and src != dst:
                if not os.path.exists(dst):
                    shutil.move(src, dst)
    print(f'  首建分层完成: Training/Validate/Testing = '
          f'{train_ratio:.0%}/{val_ratio:.0%}/{test_ratio:.0%}')  # noqa: F541


def main():
    ap = argparse.ArgumentParser(description='真实噪声切割 + 首建分层拆分')
    ap.add_argument('--noise-dir', default=NOISE_DIR, help='原始噪声 WAV 目录 (raw)')
    ap.add_argument('--out', default=OUTPUT_DIR, help='输出根目录')
    ap.add_argument('--config', default=None, help='cut_config.json 路径 (默认脚本同目录)')
    ap.add_argument('--max-per-category', type=int, default=DEFAULT_MAX_PER_CATEGORY,
                    help='每类最多保留段数 (类别平衡; 默认不限)')
    ap.add_argument('--seed', type=int, default=RANDOM_SEED)
    args = ap.parse_args()

    out_root = args.out
    noise_dir = args.noise_dir
    cfg_path = args.config or _script_config_path()
    rng = np.random.RandomState(args.seed)

    print('=' * 60)
    print(f'  真实噪声切割: {noise_dir} → {out_root}')
    print('=' * 60)

    cut_config = load_or_init_cut_config(cfg_path, noise_dir, CATEGORY_CONFIG)

    # 预算: --max-per-category 减去 3 目录已有段数 (支持增量续切)
    existing = scan_existing_counts(out_root, CATEGORY_CONFIG)
    budget = {}
    for cat in CATEGORY_CONFIG:
        have = existing.get(cat, 0)
        budget[cat] = None if args.max_per_category is None \
            else max(0, args.max_per_category - have)

    to_cut = collect_to_cut(cut_config, noise_dir, CATEGORY_CONFIG, chunk_len)
    to_cut = [t for t in to_cut if budget[t[0]] is None or budget[t[0]] > 0]

    if to_cut:
        print(f'\n  切割: {len(to_cut)} 个文件 (来自 cut_config.json uncut 列表)')
        for cat_name, _, fname, _, n in to_cut:
            print(f'    {cat_name}: {fname} → ~{n} 片段')
        cut_files(to_cut, cut_config, out_root, CATEGORY_CONFIG, budget)
        with open(cfg_path, 'w', encoding='utf-8') as f:
            json.dump(cut_config, f, indent=2, ensure_ascii=False)
        print(f'  cut_config.json 已更新 ({cfg_path})')
    else:
        print('  uncut 为空或已达预算, 跳过切割')

    if not has_split_dirs(out_root):
        stratified_split(out_root, rng, CATEGORY_CONFIG, TRAIN_RATIO, VAL_RATIO, BLOCK_SIZE)
    else:
        print('  已存在分层目录, 跳过首建拆分 (新增段保持留在 Training_data)')

    # 汇总每类 3 目录段数
    print('\n  各目录段数:')
    total = Counter()
    for split in ['Training_data', 'Validate_data', 'Testing_data']:
        d = os.path.join(out_root, split)
        n = len([f for f in os.listdir(d) if f.endswith('.wav')]) if os.path.isdir(d) else 0
        c = Counter()
        if os.path.isdir(d):
            for f in os.listdir(d):
                for cat in CATEGORY_CONFIG:
                    if f.startswith(cat + '_'):
                        c[cat] += 1
                        break
        total[split] = n
        print(f'    {split}: {n} 段 {dict(c)}')
    print(f'  合计: {sum(total.values())} 段')
    print('\n  下一步打标签: python training/labeling/label_wavs.py '
          f'--wav-dir {out_root} --tag real')


if __name__ == '__main__':
    main()
