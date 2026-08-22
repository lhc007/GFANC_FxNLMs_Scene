"""
CNN 分类器从零训练 — SFANC 硬选库 (deploy) 决策 CNN, 4 类真实噪声.

与 train_real_cnn.py (直接权重回归) 的区别 (计划 Phase 3):
  - 硬标签: category → one-hot (不用软标签 SoftLabels_real_*.npy)
  - 损失:   CrossEntropyLoss (删 KLDivLoss/软标签)
  - K:      读 scene_definitions_bank.json (n_scenes=4, 类序 = 库槽序)
  - 输出:   MIMO_M5_Scene_Bank.pth
回归 CNN (K=30) 保留给 calibrate 模式 (标定暖启动), 互不影响.

前置: verify_discrimination_bank.py 判定 4 类谱可分 (MLP 91.8% ≥70%) 已通过.

用法:
    python training/network/train_real_bank_cnn.py

输入:
    D:\\Dataset\\Real_world_Dataset\\Index_real_Training_data.csv   (category 列)
    D:\\Dataset\\Real_world_Dataset\\Index_real_Validate_data.csv
    D:\\Dataset\\Real_world_Dataset\\Training_data/  (WAV 片段)
    models/scene_definitions_bank.json

输出:
    models/MIMO_M5_Scene_Bank.pth
"""
import os, sys, json
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

# ── 预处理缓存 (prep_bank_cache.py 生成): 存在则从 RAM 读, 否则回退 on-the-fly 加载 ──
_CACHE_X = _PROJECT_ROOT / 'models' / 'bank_cache_train_x.npy'
_CACHE_Y = _PROJECT_ROOT / 'models' / 'bank_cache_train_y.npy'
_CACHE_VX = _PROJECT_ROOT / 'models' / 'bank_cache_valid_x.npy'
_CACHE_VY = _PROJECT_ROOT / 'models' / 'bank_cache_valid_y.npy'
_HAS_CACHE = all(p.exists() for p in (_CACHE_X, _CACHE_Y, _CACHE_VX, _CACHE_VY))
if _HAS_CACHE:
    print(f'  缓存模式: 从 {_CACHE_X.name} 等加载 (跳过 on-the-fly WAV 读取)')


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


class CachedBankDataset(Dataset):
    """缓存数据集: prep_bank_cache.py 预打包的 RAM 数组 (float16 [N,16000] + int64 标签).

    增广与 RealSceneBankDataset 一致 (时间遮蔽/增益抖动/低噪注入), 仅 train 用.
    内存: 63k×16000×2B ≈ 2GB float16 — 加载后 epoch 读取为 RAM 切片, GPU 计算为主.
    """

    def __init__(self, x_path, y_path, augment=False):
        x = np.load(x_path, mmap_mode='r')
        y = np.load(y_path, mmap_mode='r')
        self.x = x
        self.y = y
        self.augment = augment
        cnt = Counter(y.tolist())
        print(f'  CachedBankDataset: {len(self)} samples, {self.K} classes, '
              f'augment={augment}')
        print(f'  Distribution: min={min(cnt.values())}, max={max(cnt.values())}')

    @property
    def class_idx(self):
        return self.y

    @property
    def K(self):
        return int(self.y.max()) + 1

    def __len__(self):
        return len(self.y)

    def __getitem__(self, index):
        sig = torch.from_numpy(np.asarray(self.x[index], dtype=np.float32))
        if sig.dim() == 1:
            sig = sig.unsqueeze(0)
        if self.augment:
            mask_len = torch.randint(800, 2400, (1,)).item()
            mask_start = torch.randint(0, 16000 - mask_len, (1,)).item()
            sig[:, mask_start:mask_start + mask_len] = 0
            sig = sig * (0.7 + 0.6 * torch.rand(1).item())
            noise_rms = sig.std() * (10 ** (-1.25))
            sig = sig + torch.randn_like(sig) * noise_rms
        return sig, torch.tensor(int(self.y[index]), dtype=torch.long)


class RealSceneBankDataset(Dataset):
    """SFANC 分类数据集: 4 类真实噪声, 硬标签 (category → 类索引).

    带通滤波不在此处做 — 训练循环里整批 GPU 卷积 (数学等价).
    类序 = scene_definitions_bank.json classes (= 库槽序, 与 C 类名表一致).
    """

    def __init__(self, data_dir, csv_path, class_to_idx, augment=True):
        df = pd.read_csv(csv_path)
        self.data_dir = data_dir
        self.augment = augment
        self.file_paths = df['File_path'].values
        self.categories = df['category'].values
        # category → 类索引 (硬标签). 未在库类序中的 category → 丢弃 (防御)
        keep = np.array([c in class_to_idx for c in self.categories])
        if not keep.all():
            print(f'  [WARN] 丢弃 {int((~keep).sum())} 个未知类样本 '
                  f'(库类: {sorted(class_to_idx.keys())})')
            self.file_paths = self.file_paths[keep]
            self.categories = self.categories[keep]
        self.class_idx = np.array([class_to_idx[c] for c in self.categories]).astype(np.int64)
        self.K = len(class_to_idx)
        cnt = Counter(self.class_idx)
        print(f'  RealSceneBankDataset: {len(self)} samples, {self.K} classes, '
              f'augment={augment}')
        print(f'  Distribution: min={min(cnt.values())}, max={max(cnt.values())}')

    def __len__(self):
        return len(self.file_paths)

    def __getitem__(self, index):
        signal, sr = torchaudio.load(os.path.join(self.data_dir, self.file_paths[index]))
        if signal.shape[0] > 1:
            signal = signal.mean(dim=0, keepdim=True)
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
DATA_DIR      = r'D:\Dataset\Real_world_Dataset'
TRAIN_CSV     = os.path.join(DATA_DIR, 'Index_real_Training_data.csv')
VALID_CSV     = os.path.join(DATA_DIR, 'Index_real_Validate_data.csv')
TRAIN_WAV_DIR = os.path.join(DATA_DIR, 'Training_data')
VALID_WAV_DIR = os.path.join(DATA_DIR, 'Validate_data')
# 单一事实源: 类序 == 库槽序 (deploy 决策层 argmax 索引)
SCENE_BANK    = _PROJECT_ROOT / 'models' / 'scene_definitions_bank.json'
OUTPUT_PTH    = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_Scene_Bank.pth')

EPOCHS = 50
BATCH_SIZE = 128
LR = 0.001
WEIGHT_DECAY = 1e-3
EARLY_STOP_PATIENCE = 20
EARLY_STOP_MIN_DELTA = 0.001
DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

# ═══════════════════════════════════════════════════════════════

print('=' * 60)
print('  SFANC 分类 CNN 训练 (4 类硬标签, CrossEntropy)')
print(f'  设备: {DEVICE}')
print('=' * 60)

with open(SCENE_BANK, encoding='utf-8') as f:
    bank_doc = json.load(f)
CLASSES = list(bank_doc['classes'])
K = bank_doc['n_scenes']
assert len(CLASSES) == K, f'scene_definitions_bank.json: {len(CLASSES)} classes != n_scenes {K}'
class_to_idx = {c: i for i, c in enumerate(CLASSES)}
print(f'  库槽序 (类序): {CLASSES}')
print(f'  K={K}')

if _HAS_CACHE:
    train_ds = CachedBankDataset(_CACHE_X, _CACHE_Y, augment=True)
    valid_ds = CachedBankDataset(_CACHE_VX, _CACHE_VY, augment=False)
else:
    train_ds = RealSceneBankDataset(TRAIN_WAV_DIR, TRAIN_CSV, class_to_idx, augment=True)
    valid_ds = RealSceneBankDataset(VALID_WAV_DIR, VALID_CSV, class_to_idx, augment=False)

# 类均衡采样 (4 类近似均衡, 仍按逆频率加权防样本量偏差)
class_cnt = Counter(train_ds.class_idx)
sample_weights = [1.0 / class_cnt[c] for c in train_ds.class_idx]
sampler = WeightedRandomSampler(
    weights=torch.tensor(sample_weights, dtype=torch.float),
    num_samples=len(train_ds),
    replacement=True,
)
# 缓存模式: RAM 切片 + GPU 计算为主 → num_workers=0 足够 (避免多进程拷贝开销)
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

# 快速验证: per-class accuracy
print(f'\n{"=" * 50}')
print(f'  Per-class validation accuracy')
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
    print(f'  {CLASSES[c]:12s} (类 {c}): {acc:.3f} '
          f'({per_class_correct.get(c,0)}/{per_class_total[c]})')
