"""
CNN 分类器从零训练 — SFANC 硬选库 (deploy) 决策 CNN, 通用 N 类.

与 train_real_cnn.py (直接权重回归) 的区别 (Phase 2/3):
  - 标签: 离线打分标签 (export/generate_bank.py --labels 生成),
          CSV 列 File_path, filter_idx — 无语义场景, 槽 k = 第 k 条滤波器.
  - 硬标签: one-hot + CrossEntropyLoss (删 KLDivLoss/软标签)
  - K:      从标签推导 (max filter_idx + 1), 无需 scene_definitions_bank.json
  - 输出:   models/MIMO_M5_Scene_Bank.pth
回归 CNN (K=30) 保留给 calibrate 模式 (标定暖启动), 互不影响.

用法:
    python training/network/train_real_bank_cnn.py
    # 或指定标签 CSV (默认 data/bank_labels_{train,valid}.csv):
    python training/network/train_real_bank_cnn.py \\
        --train data/bank_labels_train.csv --valid data/bank_labels_valid.csv

输入:
    data/bank_labels_train.csv / bank_labels_valid.csv  (File_path 绝对路径 + filter_idx)
    对应 WAV 片段 (1s, 任意噪声)

输出:
    models/MIMO_M5_Scene_Bank.pth
"""
import os, sys, argparse
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, Dataset, WeightedRandomSampler
import torchaudio
import pandas as pd
from pathlib import Path
from collections import Counter

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

from gfanc.Network import m5_scene


def minmaxscaler(data):
    denom = data.max() - data.min()
    if denom < 1e-10:
        return data
    return data / denom


# 带通 FIR (50-1500Hz) — 与 C 端部署的 CNN 输入严格对齐
_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_fir.mat'


def _load_bandpass():
    import scipy.io as sio
    mat = sio.loadmat(str(_BP_PATH))
    return mat['fir_bandpass_coeff'].squeeze().astype(np.float32)


class BankLabelDataset(Dataset):
    """SFANC 分类数据集 (通用 N 类): 读离线打分标签 CSV, 硬标签 filter_idx.

    无语义场景 — 类序 == 库槽序 (deploy 决策层 argmax 索引).
    File_path 为绝对路径; 若为相对路径则相对 wav_dir 解析.
    带通滤波不在此处做 — 训练循环里整批 GPU 卷积 (数学等价).
    """

    def __init__(self, csv_path, wav_dir=None, augment=True):
        df = pd.read_csv(csv_path)
        assert 'File_path' in df.columns and 'filter_idx' in df.columns, \
            f'{csv_path} 需含 File_path, filter_idx 列 (export/generate_bank.py --labels 生成)'
        self.wav_dir = wav_dir
        self.augment = augment
        self.file_paths = df['File_path'].values
        self.class_idx = np.asarray(df['filter_idx'], dtype=np.int64)
        if self.class_idx.min() < 0:
            print(f'  [WARN] 丢弃 {int((self.class_idx < 0).sum())} 个负标签样本')
            keep = self.class_idx >= 0
            self.file_paths = self.file_paths[keep]
            self.class_idx = self.class_idx[keep]
        self.K = int(self.class_idx.max()) + 1
        cnt = Counter(self.class_idx.tolist())
        print(f'  BankLabelDataset: {len(self)} samples, {self.K} classes, '
              f'augment={augment}')
        print(f'  Distribution: min={min(cnt.values())}, max={max(cnt.values())}')

    def _resolve(self, path):
        p = Path(path)
        if p.is_absolute() or self.wav_dir is None:
            return str(p)
        return str(self.wav_dir / p)

    def __len__(self):
        return len(self.file_paths)

    def __getitem__(self, index):
        signal, sr = torchaudio.load(self._resolve(self.file_paths[index]))
        if signal.shape[0] > 1:
            signal = signal.mean(dim=0, keepdim=True)
        if sr != 16000:
            signal = torchaudio.functional.resample(signal, sr, 16000)
        if signal.shape[1] < 16000:
            signal = nn.functional.pad(signal, (0, 16000 - signal.shape[1]))
        else:
            signal = signal[:, :16000]
        if self.augment:
            # 时间遮蔽: 随机掩码 ~100ms (模拟瞬时干扰, 强迫CNN学全局频谱)
            mask_len = torch.randint(800, 2400, (1,)).item()
            mask_start = torch.randint(0, 16000 - mask_len, (1,)).item()
            signal[:, mask_start:mask_start + mask_len] = 0
            # 增益抖动: 0.7x ~ 1.3x
            signal = signal * (0.7 + 0.6 * torch.rand(1).item())
            # 低噪注入: -25dB
            noise_rms = signal.std() * (10 ** (-1.25))
            signal = signal + torch.randn_like(signal) * noise_rms
        return signal, torch.tensor(self.class_idx[index], dtype=torch.long)


# ═══════════════════════════════════════════════════════════════
# 配置
# ═══════════════════════════════════════════════════════════════
ap = argparse.ArgumentParser(description='通用 N 类 SFANC 决策 CNN 训练')
ap.add_argument('--train', default=str(_PROJECT_ROOT / 'data' / 'bank_labels_train.csv'))
ap.add_argument('--valid', default=str(_PROJECT_ROOT / 'data' / 'bank_labels_valid.csv'))
ap.add_argument('--wav-dir', default=None,
                help='File_path 为相对路径时的基准目录 (绝对路径可省略)')
ap.add_argument('--epochs', type=int, default=50)
ap.add_argument('--batch-size', type=int, default=128)
ap.add_argument('--lr', type=float, default=0.001)
ap.add_argument('--out', default=str(_PROJECT_ROOT / 'models' / 'MIMO_M5_Scene_Bank.pth'))
args = ap.parse_args()

DATA_DIR      = None               # 不再有语义数据目录 — 标签 CSV 自带路径
TRAIN_CSV     = args.train
VALID_CSV     = args.valid
OUTPUT_PTH    = args.out

EPOCHS = args.epochs
BATCH_SIZE = args.batch_size
LR = args.lr
WEIGHT_DECAY = 1e-3
EARLY_STOP_PATIENCE = 20
EARLY_STOP_MIN_DELTA = 0.001
DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

# ═══════════════════════════════════════════════════════════════

print('=' * 60)
print('  SFANC 分类 CNN 训练 (通用 N 类硬标签, CrossEntropy)')
print(f'  设备: {DEVICE}')
print('=' * 60)

train_ds = BankLabelDataset(TRAIN_CSV, wav_dir=args.wav_dir, augment=True)
valid_ds = BankLabelDataset(VALID_CSV, wav_dir=args.wav_dir, augment=False)
K = train_ds.K
assert valid_ds.K == K, f'train/valid 类数不一致: {train_ds.K} vs {valid_ds.K}'
print(f'  K={K} (槽序 == 库槽序, 无语义类名)')

# 类均衡采样 (按逆频率加权防样本量偏差)
class_cnt = Counter(train_ds.class_idx.tolist())
sample_weights = [1.0 / class_cnt[c] for c in train_ds.class_idx.tolist()]
sampler = WeightedRandomSampler(
    weights=torch.tensor(sample_weights, dtype=torch.float),
    num_samples=len(train_ds),
    replacement=True,
)
train_loader = DataLoader(train_ds, batch_size=BATCH_SIZE, sampler=sampler,
                          num_workers=0, pin_memory=True, drop_last=True)
valid_loader = DataLoader(valid_ds, batch_size=BATCH_SIZE, shuffle=False,
                          num_workers=0, pin_memory=True)

model = m5_scene(K=K, dropout=0.3)
model.apply(lambda m: torch.nn.init.xavier_uniform_(m.weight.data)
            if isinstance(m, nn.Conv1d) else None)
model = model.to(DEVICE)
total_params = sum(p.numel() for p in model.parameters())
print(f'\n  m5_scene: {total_params:,} params (K={K})')

# GPU 整批带通 (50-1500Hz) + minmax — 与 C 端 fir_tick 因果 FIR 数学等价
_bp = torch.from_numpy(_load_bandpass()).to(DEVICE)
_bp_w = _bp.flip(0).view(1, 1, -1)
_bp_pad = _bp.numel() - 1


def prepare_batch(x):
    """[B,1,16000] -> 带通 -> minmaxscaler (与部署端 scene_ctrl_process 一致)"""
    x = nn.functional.conv1d(x, _bp_w, padding=_bp_pad)[:, :, :16000]
    scale = x.amax(dim=-1, keepdim=True) - x.amin(dim=-1, keepdim=True)
    return torch.where(scale > 1e-10, x / scale.clamp(min=1e-10), x)


# 硬标签分类: CrossEntropyLoss (logits 直接算, 无 softmax 预激活)
loss_fn = nn.CrossEntropyLoss()
optimizer = optim.Adam(model.parameters(), lr=LR, weight_decay=WEIGHT_DECAY)
scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=EPOCHS)

print(f'\n{"=" * 50}')
print(f'  训练 ({EPOCHS} epochs, lr={LR}, batch={BATCH_SIZE})')
print(f'{"=" * 50}')

best_acc = 0.0
stale_epochs = 0

for epoch in range(EPOCHS):
    model.train()
    train_loss = 0
    train_correct = 0
    train_total = 0

    for inputs, targets in train_loader:
        inputs = inputs.to(DEVICE, non_blocking=True)
        inputs = prepare_batch(inputs)
        targets = targets.to(DEVICE, non_blocking=True)

        optimizer.zero_grad()
        logits = model(inputs)
        loss = loss_fn(logits, targets)
        loss.backward()
        optimizer.step()

        train_loss += loss.item() * inputs.size(0)
        train_correct += (logits.argmax(dim=1) == targets).sum().item()
        train_total += targets.size(0)

    scheduler.step()
    train_loss /= train_total
    train_acc = train_correct / train_total

    model.eval()
    valid_loss = 0
    valid_correct = 0
    valid_total = 0
    with torch.no_grad():
        for inputs, targets in valid_loader:
            inputs = inputs.to(DEVICE, non_blocking=True)
            inputs = prepare_batch(inputs)
            targets = targets.to(DEVICE, non_blocking=True)
            logits = model(inputs)
            loss = loss_fn(logits, targets)
            valid_loss += loss.item() * inputs.size(0)
            valid_correct += (logits.argmax(dim=1) == targets).sum().item()
            valid_total += targets.size(0)
    valid_loss /= valid_total
    valid_acc = valid_correct / valid_total

    lr_now = optimizer.param_groups[0]['lr']
    marker = ' *' if valid_acc > best_acc else ''
    print(f'  Epoch {epoch+1:3d}: train_loss={train_loss:.4f} train_acc={train_acc:.4f}  '
          f'valid_loss={valid_loss:.4f} valid_acc={valid_acc:.4f}  lr={lr_now:.2e}{marker}')

    if valid_acc > best_acc + EARLY_STOP_MIN_DELTA:
        best_acc = valid_acc
        stale_epochs = 0
        torch.save(model.state_dict(), OUTPUT_PTH)
    else:
        stale_epochs += 1

    if stale_epochs >= EARLY_STOP_PATIENCE:
        print(f'\n  早停: valid_acc {EARLY_STOP_PATIENCE} 轮未提升 (best={best_acc:.4f}), epoch {epoch+1}')
        break

print(f'\n  最佳验证准确率: {best_acc:.4f}')
print(f'  模型已保存: {OUTPUT_PTH}')

# 快速验证: per-class accuracy (槽 k 即类 k, 无语义名)
print(f'\n{"=" * 50}')
print(f'  Per-slot validation accuracy')
print(f'{"=" * 50}')
model.load_state_dict(torch.load(OUTPUT_PTH, map_location='cpu', weights_only=True))
model = model.to(DEVICE)
model.eval()

per_class_correct = {}
per_class_total = {}
with torch.no_grad():
    for inputs, targets in valid_loader:
        inputs = inputs.to(DEVICE)
        inputs = prepare_batch(inputs)
        logits = model(inputs)
        preds = logits.argmax(dim=1).tolist()
        for t, p in zip(targets.tolist(), preds):
            per_class_total[t] = per_class_total.get(t, 0) + 1
            per_class_correct[t] = per_class_correct.get(t, 0) + (1 if t == p else 0)

for c in sorted(per_class_total.keys()):
    acc = per_class_correct.get(c, 0) / per_class_total[c]
    print(f'  槽 {c:3d}: {acc:.3f} ({per_class_correct.get(c,0)}/{per_class_total[c]})')
