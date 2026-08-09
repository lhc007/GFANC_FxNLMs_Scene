"""
两阶段训练: 合成预训练 → 真实微调 (修复 CNN 欠拟合 / 输入失聪).

背景 (2026-08-09 诊断): 真实 4 类噪声(道路/儿童/施工/铁路)全低频主导、谱形接近,
从零在真实数据上训练 → CNN 输出坍缩到同一低频处方 (判别力仅 35.8%, 输入谱可分 75%).
合成数据用多样谱形(窄带/宽带/1-f^α/谐波)覆盖 20-1500Hz 全子带空间, 逼 CNN 必须
用输入 → 预训练学出光谱→增益映射, 再低学习率在真实数据微调适配真实统计量,
防止微调阶段把多样映射又坍缩回去.

阶段:
  1) 预训练: D:\\Dataset\\Synthetic_Dataset 合成 60000 + 验证 7500 (MSE + tanh 回归)
  2) 微调:   D:\\Dataset\\Real_world_Dataset 真实 全量 + 验证 (低 LR, 保映射)

与 Train_validate.py 完全同机制: m5_scene(K=30) + 带通(20-1500Hz) + minmax +
MSE(tanh vs 归一化增益) + Adam + StepLR. 选模指标 = 验证集整向量余弦相似度.

运行:
    python training/network/Train_validate_synth.py                      # 默认两阶段
    python training/network/Train_validate_synth.py --epochs-pre 40 --epochs-ft 25
    python training/network/Train_validate_synth.py --no-finetune        # 只预训练
    python training/network/Train_validate_synth.py --pretrain-pth <path>  # 跳过预训练, 直接微调

输出:
    models/MIMO_M5_DirectWeight_Pretrain.pth   预训练检查点 (合成, 保留)
    models/MIMO_M5_DirectWeight_Real.pth       最终模型 (微调后, export_bin 自动加载)
"""
import sys, os, argparse
from pathlib import Path

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader
import torch.optim as optim
from noise_dataset import MyNoiseDataset, make_bandpass_tensor, prepare_batch as _pbatch
from Bcolors import bcolors

from gfanc.Network import m5_scene

# ═══════════════════════════════════════════════════════════════
# 配置 (均可 CLI 覆盖)
# ═══════════════════════════════════════════════════════════════
N_SPEAKERS = 2
N_BANDS = 15
SC = N_SPEAKERS * N_BANDS

BATCH_SIZE     = 250
EPOCHS_PRE     = 40          # 合成预训练
EPOCHS_FT      = 25          # 真实微调
LR_PRE         = 0.01        # 与 Train_validate.py 从零训练一致
LR_FT          = 0.003       # 微调低 LR — 防止把多样光谱映射又坍缩回低频处方
WEIGHT_DECAY   = 1e-4
LR_STEP        = 5
LR_GAMMA       = 0.5

# 数据路径
SYNTH_DIR      = r'D:\Dataset\Synthetic_Dataset'
SYNTH_TRAIN    = os.path.join(SYNTH_DIR, 'Index_synth_Training_data.csv')
SYNTH_VALID    = os.path.join(SYNTH_DIR, 'Index_synth_Validate_data.csv')
REAL_DIR       = r'D:\Dataset\Real_world_Dataset'
REAL_TRAIN     = os.path.join(REAL_DIR, 'Index_real_Training_data.csv')
REAL_VALID     = os.path.join(REAL_DIR, 'Index_real_Validate_data.csv')

PRETRAIN_PTH   = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_DirectWeight_Pretrain.pth')
OUTPUT_PTH     = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_DirectWeight_Real.pth')

_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_filter_20_1500Hz.mat'
_bp_w = _bp_pad = None


def _init_bandpass(device):
    global _bp_w, _bp_pad
    _bp_w, _bp_pad = make_bandpass_tensor(_BP_PATH, device)


def prepare_batch(x):
    return _pbatch(x, _bp_w, _bp_pad)


def init_weights(m):
    if isinstance(m, torch.nn.Conv1d):
        torch.nn.init.xavier_uniform_(m.weight.data)


def create_data_loader(data, batch_size, shuffle=True):
    return DataLoader(data, batch_size=batch_size, shuffle=shuffle,
                      num_workers=0, pin_memory=True)


def batch_cosine_sim(pred, target):
    pn = F.normalize(pred.float(), dim=-1, eps=1e-12)
    tn = F.normalize(target.float(), dim=-1, eps=1e-12)
    return (pn * tn).sum(dim=-1).mean().item()


# ═══════════════════════════════════════════════════════════════
# 单阶段训练循环 (参数化 lr/epochs, 其余与 Train_validate.py 相同)
# ═══════════════════════════════════════════════════════════════

def train_phase(model, train_loader, valid_loader, epochs, lr, wd, device,
                model_path, tag=''):
    loss_fn = nn.MSELoss()
    optimizer = optim.Adam(model.parameters(), lr=lr, weight_decay=wd)
    scheduler = optim.lr_scheduler.StepLR(optimizer, step_size=LR_STEP, gamma=LR_GAMMA)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == 'cuda')
    # cos_max 从 -inf 起: 回归训练首轮 valid_cos 可能为负, 若从 0 起则永不如 0,
    # 整个阶段一个检查点都不存 → 后续 reload 崩溃 (冒烟测试捕获)
    cos_max = -float('inf')
    print(f'\n{"="*60}')
    print(f'  {tag}训练: {epochs} epochs, lr={lr}, batch={BATCH_SIZE}')
    print(f'{"="*60}')

    for i in range(epochs):
        model.train()
        tr_loss, tr_cos, n = 0.0, 0.0, 0
        for input, target in train_loader:
            input, target = input.to(device, non_blocking=True), target.to(device, non_blocking=True)
            input = prepare_batch(input)
            with torch.amp.autocast(device_type=device.type, enabled=scaler.is_enabled()):
                prediction = torch.tanh(model(input))
                loss = loss_fn(prediction, target)
            optimizer.zero_grad(set_to_none=True)
            if scaler.is_enabled():
                scaler.scale(loss).backward(); scaler.step(optimizer); scaler.update()
            else:
                loss.backward(); optimizer.step()
            tr_loss += loss.item() * input.size(0)
            tr_cos += batch_cosine_sim(prediction, target)
            n += input.size(0)

        # 验证
        model.eval()
        va_loss, va_cos, m = 0.0, 0.0, 0
        with torch.no_grad():
            for input, target in valid_loader:
                input, target = input.to(device), target.to(device)
                input = prepare_batch(input)
                prediction = torch.tanh(model(input))
                loss = loss_fn(prediction, target)
                va_loss += loss.item() * input.size(0)
                va_cos += batch_cosine_sim(prediction, target)
                m += input.size(0)

        scheduler.step()
        marker = ' *' if va_cos > cos_max else ''
        if va_cos > cos_max:
            cos_max = va_cos
            if model_path:
                torch.save(model.state_dict(), model_path)
        print(f'  [{tag}] Epoch {i+1:3d}/{epochs}: '
              f'train_loss={tr_loss/n:.4f} train_cos={tr_cos/len(train_loader):.4f}  '
              f'valid_loss={va_loss/m:.4f} valid_cos={va_cos/len(valid_loader):.4f}  '
              f'lr={optimizer.param_groups[0]["lr"]:.2e}{marker}')
    print(f'  [{tag}] 完成, 最佳 valid_cos={cos_max:.4f} → {model_path}')
    return cos_max


# ═══════════════════════════════════════════════════════════════
# 入口
# ═══════════════════════════════════════════════════════════════

def main():
    global BATCH_SIZE, EPOCHS_PRE, EPOCHS_FT, LR_PRE, LR_FT
    ap = argparse.ArgumentParser(description='合成预训练 → 真实微调')
    ap.add_argument('--epochs-pre', type=int, default=EPOCHS_PRE)
    ap.add_argument('--epochs-ft',  type=int, default=EPOCHS_FT)
    ap.add_argument('--lr-pre', type=float, default=LR_PRE)
    ap.add_argument('--lr-ft',  type=float, default=LR_FT)
    ap.add_argument('--batch',  type=int, default=BATCH_SIZE)
    ap.add_argument('--no-finetune', action='store_true', help='只预训练')
    ap.add_argument('--pretrain-pth', default=None,
                    help='跳过预训练, 加载该检查点直接微调')
    ap.add_argument('--out', default=OUTPUT_PTH, help='最终模型路径')
    args = ap.parse_args()
    EPOCHS_PRE, EPOCHS_FT, LR_PRE, LR_FT, BATCH_SIZE = \
        args.epochs_pre, args.epochs_ft, args.lr_pre, args.lr_ft, args.batch

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    _init_bandpass(device)
    print('=' * 60)
    print('  两阶段训练: 合成预训练 → 真实微调')
    print(f'  设备: {device}')
    print('=' * 60)

    # ── 阶段 1: 合成预训练 ──────────────────────────────
    model = m5_scene(K=SC, dropout=0.3)
    if args.pretrain_pth:
        print(f'\n  跳过预训练, 加载 {args.pretrain_pth}')
        model.load_state_dict(torch.load(args.pretrain_pth, map_location='cpu',
                                         weights_only=True))
    else:
        print(f'\n  [预训练] 合成: {SYNTH_TRAIN}')
        train_ds = MyNoiseDataset(SYNTH_TRAIN, os.path.join(SYNTH_DIR, 'Training_data'),
                                  augment=True)
        valid_ds = MyNoiseDataset(SYNTH_VALID, os.path.join(SYNTH_DIR, 'Validate_data'),
                                  augment=False)
        model.apply(init_weights)
        model = model.to(device)
        train_phase(model,
                    create_data_loader(train_ds, BATCH_SIZE, shuffle=True),
                    create_data_loader(valid_ds, BATCH_SIZE, shuffle=False),
                    EPOCHS_PRE, LR_PRE, WEIGHT_DECAY, device, PRETRAIN_PTH,
                    tag='预训练')
        model.load_state_dict(torch.load(PRETRAIN_PTH, map_location='cpu',
                                         weights_only=True))

    if args.no_finetune:
        print(f'\n  --no-finetune: 只预训练完成 → {PRETRAIN_PTH}')
        return

    # ── 阶段 2: 真实微调 ────────────────────────────────
    print(f'\n  [微调] 真实: {REAL_TRAIN}')
    train_ds = MyNoiseDataset(REAL_TRAIN, os.path.join(REAL_DIR, 'Training_data'),
                              augment=True)
    valid_ds = MyNoiseDataset(REAL_VALID, os.path.join(REAL_DIR, 'Validate_data'),
                              augment=False)
    model = model.to(device)
    train_phase(model,
                create_data_loader(train_ds, BATCH_SIZE, shuffle=True),
                create_data_loader(valid_ds, BATCH_SIZE, shuffle=False),
                EPOCHS_FT, LR_FT, WEIGHT_DECAY, device, args.out,
                tag='微调')
    print(f'\n  完成: 最终模型 {args.out}')
    print('  下一步: verify_discrimination.py 检查点3 (CNN 输出判别力 → 70% 目标)')


if __name__ == '__main__':
    main()
