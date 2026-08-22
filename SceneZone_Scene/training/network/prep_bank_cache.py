"""
预处理缓存 — SFANC 分类 CNN 训练用.

问题: train_real_bank_cnn.py 原始 on-the-fly 加载 (num_workers=0) 单线程读 63k WAV,
      ~25ms/文件 → 单 epoch 载入 ~35min, 50 epochs 不可行.
解决: 一次性并行加载全部训练/验证音频到打包 .npy 缓存 (float16 [N,16000]),
      训练改为从 RAM 读 (GPU 计算为主, epoch 秒级).

用法:
    python training/network/prep_bank_cache.py

输出 (models/):
    bank_cache_train_x.npy  float16 [N,16000]
    bank_cache_train_y.npy  int64  [N]
    bank_cache_valid_x.npy  float16 [N,16000]
    bank_cache_valid_y.npy  int64  [N]

类序 = scene_definitions_bank.json classes (= 库槽序), 过滤逻辑与
train_real_bank_cnn.py 的 RealSceneBankDataset 一致 (未知类丢弃).
"""
import os, sys, json, time
import numpy as np
import torchaudio
import pandas as pd
from pathlib import Path
from multiprocessing import Pool

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
DATA_DIR    = r'D:\Dataset\Real_world_Dataset'
TRAIN_CSV   = os.path.join(DATA_DIR, 'Index_real_Training_data.csv')
VALID_CSV   = os.path.join(DATA_DIR, 'Index_real_Validate_data.csv')
SCENE_BANK  = _PROJECT_ROOT / 'models' / 'scene_definitions_bank.json'
OUT_PREFIX  = str(_PROJECT_ROOT / 'models' / 'bank_cache')
PROCS = 8          # 并行加载进程数
N_SAMPLES = 16000  # 1s @16kHz, 与部署端一致 (pad/truncate)


def _load_one(args):
    """加载单文件 → float16 [16000] (mono, pad/截断). 失败返回 None."""
    path, subdir = args
    try:
        signal, sr = torchaudio.load(os.path.join(DATA_DIR, subdir, path))
    except Exception:
        return None
    if signal.shape[0] > 1:
        signal = signal.mean(dim=0, keepdim=True)
    if signal.shape[1] < N_SAMPLES:
        import torch.nn.functional as F
        signal = F.pad(signal, (0, N_SAMPLES - signal.shape[1]))
    else:
        signal = signal[:, :N_SAMPLES]
    return signal.numpy().astype(np.float16).reshape(-1)


def build_split(subdir, csv_path, class_to_idx, out_tag):
    df = pd.read_csv(csv_path)
    keep = np.array([c in class_to_idx for c in df['category'].values])
    if not keep.all():
        print(f'  [{out_tag}] 丢弃 {int((~keep).sum())} 个未知类样本')
        df = df[keep]
    paths = df['File_path'].values
    y = np.array([class_to_idx[c] for c in df['category'].values], dtype=np.int64)
    N = len(paths)
    print(f'  [{out_tag}] 并行加载 {N} 文件 ({PROCS} 进程)...', flush=True)

    x = np.zeros((N, N_SAMPLES), dtype=np.float16)
    t0 = time.time()
    ok = 0
    with Pool(PROCS) as pool:
        for i, sig in enumerate(pool.imap(_load_one,
                                          ((p, subdir) for p in paths),
                                          chunksize=64)):
            if sig is not None:
                x[i] = sig
                ok += 1
            else:
                y[i] = -1    # 加载失败 → 标记, 训练端跳过
    dt = time.time() - t0
    print(f'  [{out_tag}] {ok}/{N} 成功, {dt:.0f}s ({dt/max(N,1)*1000:.1f} ms/file)')

    good = y >= 0
    x = x[good]
    y = y[good]
    np.save(f'{OUT_PREFIX}_{out_tag}_x.npy', x)
    np.save(f'{OUT_PREFIX}_{out_tag}_y.npy', y)
    print(f'  [{out_tag}] saved: {OUT_PREFIX}_{out_tag}_x.npy '
          f'{list(x.shape)} float16 + y {list(y.shape)} int64')
    return len(y)


if __name__ == '__main__':
    with open(SCENE_BANK, encoding='utf-8') as f:
        bank_doc = json.load(f)
    classes = list(bank_doc['classes'])
    class_to_idx = {c: i for i, c in enumerate(classes)}
    print(f'  类序 (库槽序): {classes}', flush=True)

    n_tr = build_split('Training_data', TRAIN_CSV, class_to_idx, 'train')
    n_va = build_split('Validate_data', VALID_CSV, class_to_idx, 'valid')
    print(f'\n  缓存完成: train={n_tr}, valid={n_va}  (总 {n_tr + n_va})')
