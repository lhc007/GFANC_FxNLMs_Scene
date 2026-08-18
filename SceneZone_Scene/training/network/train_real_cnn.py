"""
CNN 从零训练 — 使用真实噪声数据集 + 新场景定义.

用法:
    python scripts/train_cnn_from_scratch.py

输入:
    D:\Dataset\Real_world_Dataset\Index_real_Training_data.csv
    D:\Dataset\Real_world_Dataset\Index_real_Validate_data.csv
    D:\Dataset\Real_world_Dataset\Training_data/  (WAV 片段)
    models/scene_definitions_real.json
    gfanc/Network.py (m5_scene)

输出:
    models/MIMO_M5_Scene_Real.pth
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


def minmaxscaler(data):
    denom = data.max() - data.min()
    if denom < 1e-10:
        return data
    return data / denom


# 带通 FIR (50-1500Hz) — 与 C 端部署的 CNN 输入严格对齐
# (场景标签由 LMS 增益决定, 只编码 50-1500Hz 频谱模式; 全带训练会引入
#  与标签无因果关系的高频特征, 且部署端输入本就是带通后的信号)
_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_fir.mat'


def _load_bandpass():
    import scipy.io as sio
    mat = sio.loadmat(str(_BP_PATH))
    return mat['fir_bandpass_coeff'].squeeze().astype(np.float32)


class RealSceneDataset(Dataset):
    """真实噪声场景分类数据集, 支持软标签.

    带通滤波不在此处做 — 训练循环里整批 GPU 卷积 (快 5-10 倍, 数学等价).

    Args:
        augment: True=训练集 (时间遮蔽/增益抖动/噪声注入), False=验证集 (干净)
    """

    def __init__(self, data_dir, csv_path, soft_label_path=None,
                 augment=True):
        df = pd.read_csv(csv_path)
        self.data_dir = data_dir
        self.augment = augment
        self.file_paths = df['File_path'].values
        self.scene_ids = df['scene_id'].values.astype(np.int64)      # 硬标签 (采样用)
        self.categories = df['category'].values
        self.K = len(set(self.scene_ids))
        if soft_label_path and os.path.exists(soft_label_path):
            self.soft_labels = np.load(soft_label_path)               # (N, K) 概率分布
        else:
            # 退化为 one-hot
            self.soft_labels = np.eye(self.K)[self.scene_ids].astype(np.float32)
        cnt = Counter(self.scene_ids)
        print(f'  RealSceneDataset: {len(self)} samples, {self.K} classes, '
              f'augment={augment}')
        label_type = 'Yes' if soft_label_path else 'No (one-hot fallback)'
        print(f'  Soft labels: {label_type}')
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
        soft_target = torch.from_numpy(self.soft_labels[index]).float()
        return signal, soft_target


# ═══════════════════════════════════════════════════════════════
# 配置
# ═══════════════════════════════════════════════════════════════
# 数据集根目录
DATA_DIR      = r'D:\Dataset\Real_world_Dataset'
# 训练集索引文件（CSV）
TRAIN_CSV     = os.path.join(DATA_DIR, 'Index_real_Training_data.csv')
# 验证集索引文件（CSV）
VALID_CSV     = os.path.join(DATA_DIR, 'Index_real_Validate_data.csv')
# 训练集音频文件目录（WAV）
TRAIN_WAV_DIR = os.path.join(DATA_DIR, 'Training_data')
# 验证集音频文件目录（WAV）
VALID_WAV_DIR = os.path.join(DATA_DIR, 'Validate_data')
# 场景定义配置文件（JSON）
SCENE_DEF     = str(_PROJECT_ROOT / 'models' / 'scene_definitions_real.json')
# 软标签 .npy 文件 (概率分布, N×K)
TRAIN_SOFT    = os.path.join(DATA_DIR, 'SoftLabels_real_Training_data.npy')
VALID_SOFT    = os.path.join(DATA_DIR, 'SoftLabels_real_Validate_data.npy')
# 模型权重输出路径（.pth）
OUTPUT_PTH    = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_Scene_Real.pth')

EPOCHS = 50
BATCH_SIZE = 128
LR = 0.001
WEIGHT_DECAY = 1e-3
EARLY_STOP_PATIENCE = 20   # valid_acc 连续未提升则提前停止
                           # (需 > 余弦退火的高LR噪声期, 10 会在 lr≈8e-4 时误停)
EARLY_STOP_MIN_DELTA = 0.001
DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

# ═══════════════════════════════════════════════════════════════

print('=' * 60)
print('  CNN 从零训练')
print(f'  设备: {DEVICE}')
print('=' * 60)

# 加载场景定义
with open(SCENE_DEF) as f:
    scene_doc = json.load(f)
K = scene_doc['n_scenes']
print(f'  场景数: K={K}')
print(f'  来源: {scene_doc.get("source", "unknown")}')

# 加载数据 (优先使用软标签, 不存在则退化为 one-hot)
# 训练集: 带通+增强; 验证集: 带通+无增强 (与部署条件一致, 早停/选模不被增强噪声污染)
train_ds = RealSceneDataset(TRAIN_WAV_DIR, TRAIN_CSV,
                            os.path.exists(TRAIN_SOFT) and TRAIN_SOFT or None,
                            augment=True)
valid_ds = RealSceneDataset(VALID_WAV_DIR, VALID_CSV,
                            os.path.exists(VALID_SOFT) and VALID_SOFT or None,
                            augment=False)

# 场景均衡采样: 按 scene 逆频率加权 (K=4 时 scene_3 仅 ~6%, 直接平衡目标类,
# 替代原 CATEGORY_WEIGHTS — 其与实际启用类别脱节且按 category 而非目标 scene 加权)
scene_cnt = Counter(train_ds.scene_ids)
sample_weights = [1.0 / scene_cnt[s] for s in train_ds.scene_ids]

sampler = WeightedRandomSampler(
    weights=torch.tensor(sample_weights, dtype=torch.float),
    num_samples=len(train_ds),
    replacement=True,
)

train_loader = DataLoader(train_ds, batch_size=BATCH_SIZE, sampler=sampler,
                          num_workers=0, pin_memory=True, drop_last=True)
valid_loader = DataLoader(valid_ds, batch_size=BATCH_SIZE, shuffle=False,
                          num_workers=0, pin_memory=True)

# 模型 (dropout + label smoothing 防过拟合)
model = m5_scene(K=K, dropout=0.3)
model.apply(lambda m: torch.nn.init.xavier_uniform_(m.weight.data)
            if isinstance(m, nn.Conv1d) else None)
model = model.to(DEVICE)

total_params = sum(p.numel() for p in model.parameters())
print(f'\n  m5_scene: {total_params:,} params (K={K})')

# GPU 整批带通 (50-1500Hz) + minmax — 与 C 端 fir_tick 因果 FIR 数学等价:
# F.conv1d 是互相关, 权重 flip + padding=L-1 + 截前 16000 点 = 因果卷积
_bp = torch.from_numpy(_load_bandpass()).to(DEVICE)
_bp_w = _bp.flip(0).view(1, 1, -1)
_bp_pad = _bp.numel() - 1

def prepare_batch(x):
    """[B,1,16000] -> 带通 -> minmaxscaler (与部署端 scene_ctrl_process 一致)"""
    x = nn.functional.conv1d(x, _bp_w, padding=_bp_pad)[:, :, :16000]
    scale = x.amax(dim=-1, keepdim=True) - x.amin(dim=-1, keepdim=True)
    return torch.where(scale > 1e-10, x / scale.clamp(min=1e-10), x)

# 训练
loss_fn = nn.KLDivLoss(reduction='batchmean')   # 软标签: log_softmax(logits) vs target概率
optimizer = optim.Adam(model.parameters(), lr=LR, weight_decay=WEIGHT_DECAY)
scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=EPOCHS)

print(f'\n{"=" * 50}')
print(f'  训练 ({EPOCHS} epochs, lr={LR}, batch={BATCH_SIZE})')
print(f'{"=" * 50}')

best_acc = 0.0
stale_epochs = 0

for epoch in range(EPOCHS):
    # Train
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
        loss = loss_fn(torch.nn.functional.log_softmax(logits, dim=1), targets)
        loss.backward()
        optimizer.step()

        train_loss += loss.item() * inputs.size(0)
        train_correct += (logits.argmax(dim=1) == targets.argmax(dim=1)).sum().item()
        train_total += targets.size(0)

    scheduler.step()
    train_loss /= train_total
    train_acc = train_correct / train_total

    # Validate
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
            loss = loss_fn(torch.nn.functional.log_softmax(logits, dim=1), targets)

            valid_loss += loss.item() * inputs.size(0)
            valid_correct += (logits.argmax(dim=1) == targets.argmax(dim=1)).sum().item()
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
        true_ids = targets.argmax(dim=1).tolist()
        for t, p in zip(true_ids, preds):
            per_class_total[t] = per_class_total.get(t, 0) + 1
            per_class_correct[t] = per_class_correct.get(t, 0) + (1 if t == p else 0)

for c in sorted(per_class_total.keys()):
    acc = per_class_correct.get(c, 0) / per_class_total[c]
    print(f'  Scene {c}: {acc:.3f} ({per_class_correct.get(c,0)}/{per_class_total[c]})')
