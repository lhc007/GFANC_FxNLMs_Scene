"""
CNN 直接权重回归训练入口 (MIMO_GFANC Train_validate.py 适配版).

把 MIMO_GFANC 的 m13_res_mimo + BCE 硬标签训练, 改成当前项目的直接权重方案,
三个不匹配点全部按当前代码来:
  1) CNN 架构   → m5_scene (C 运行时已实现, 输出 30 维权重)
  2) 损失/标签  → 带符号 Gains_real + tanh 输出 + MSE (软混合 Wc 构造语义)
  3) 数据管道   → CSV(gain_*) + WAV 目录 + 带通(20-1500Hz)+minmax (与 C 端一致)

运行:
    python training/network/Train_validate.py          # 用下方默认路径
    或
    python -c "from training.network.Train_validate import Train_Validate_CNN; \
               Train_Validate_CNN(TRAIN_CSV, VALID_CSV, OUT_PTH)"

输出: models/MIMO_M5_DirectWeight_Real.pth (export_bin.py 自动优先加载)
"""
import sys, os, time
from pathlib import Path

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

import torch
from torch import nn
import torch.nn.functional as F
from torch.utils.data import DataLoader
import torch.optim as optim
from noise_dataset import MyNoiseDataset, make_bandpass_tensor, prepare_batch as _pbatch
from Bcolors import bcolors

from gfanc.Network import m5_scene

# ═══════════════════════════════════════════════════════════════
# 配置
# ═══════════════════════════════════════════════════════════════
BATCH_SIZE = 250
EPOCHS = 50
N_SPEAKERS = 2       # 扬声器数 (统一配置: 2 扬声器, 3 误差麦克风)
N_BANDS = 15         # 每扬声器子带数
SC = N_SPEAKERS * N_BANDS     # 输出维度 = 30 直接权重
LR = 0.01            # MIMO 原配置 (Adam, 每 5 轮 ×0.5)
WEIGHT_DECAY = 1e-4
LR_STEP = 5
LR_GAMMA = 0.5
# 幅度不变性增强 (2026-08-10): 归一化后乘对数均匀随机增益, 教 CNN"输入整体缩放
# ≠ 增益方向改变" → 治部署端每秒独立 minmax 的 denom 漂移导致的实机抖动.
# None=不增强; 验证/评估不走增强 (选模公平). 若验证 cos 明显下降, 收窄范围.
GAIN_RANGE = (0.5, 2.0)
# 进度打印 (2026-08-10): 一轮有 254 batch, 脚本原本一轮才打印一次, 中途无输出
# 会让用户误以为卡住. 每 LOG_EVERY 个 batch 打印一次 loss.
LOG_EVERY = 50

# 数据路径 — 与 label_wavs.py / recluster_real.py 输出一致
DATA_DIR      = r'D:\Dataset\Synthetic_Dataset'
TRAIN_CSV     = os.path.join(DATA_DIR, 'Index_synth_Training_data.csv')
VALID_CSV     = os.path.join(DATA_DIR, 'Index_synth_Validate_data.csv')
# WAV 目录 = CSV 同目录下 {Training,Validate}_data (File_path 仅为文件名)
TRAIN_WAV_DIR = os.path.join(DATA_DIR, 'Training_data')
VALID_WAV_DIR = os.path.join(DATA_DIR, 'Validate_data')
# 输出模型 (export_bin.py 的 CNN_CKPT_DW)
OUTPUT_PTH    = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_DirectWeight_Real.pth')

# 带通 FIR (与 C 端部署的 CNN 输入严格对齐)
_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_filter_20_1500Hz.mat'
# ═══════════════════════════════════════════════════════════════

# 带通权重 (init 后设定)
_bp_w = _bp_pad = None


def _init_bandpass(device):
    global _bp_w, _bp_pad
    _bp_w, _bp_pad = make_bandpass_tensor(_BP_PATH, device)


def prepare_batch(x, gain_range=None):
    """[B,1,16000] -> 带通 -> minmaxscaler (与部署端 scene_ctrl_process 一致).
    gain_range=(lo,hi): 幅度不变性增强 (训练用; 验证/评估不传, 保证选模公平)."""
    return _pbatch(x, _bp_w, _bp_pad, gain_range)


# 使用均匀分布初始化卷积层权重
def init_weights(m):
    if isinstance(m, torch.nn.Conv1d):
        torch.nn.init.xavier_uniform_(m.weight.data)


def create_data_loader(data, batch_size, shuffle=True):
    """创建 DataLoader, 验证集不 shuffle, 训练集 shuffle (num_workers=0 同现有脚本)."""
    return DataLoader(
        data,
        batch_size=batch_size,
        shuffle=shuffle,
        num_workers=0,      # 与 train_real_cnn.py 一致 (Windows 下进程池收益低)
        pin_memory=True     # 加速 CPU→GPU 数据传输
    )


# ═══════════════════════════════════════════════════════════════
# 指标 — 余弦相似度 (与 C 端 Reset cos_sim / 直接权重构造同语义)
# ═══════════════════════════════════════════════════════════════
def batch_cosine_sim(pred, target):
    """整向量 (SC=30) 逐样本 cos, 对 batch 求平均."""
    pn = F.normalize(pred.float(), dim=-1, eps=1e-12)
    tn = F.normalize(target.float(), dim=-1, eps=1e-12)
    return (pn * tn).sum(dim=-1).mean().item()


def batch_cosine_sim_spk(pred, target, spk):
    """第 spk 个扬声器 (15维) 逐样本 cos, 对 batch 求平均."""
    B, D = pred.shape
    p = F.normalize(pred.reshape(B, N_SPEAKERS, N_BANDS)[:, spk].float(), dim=-1, eps=1e-12)
    t = F.normalize(target.reshape(B, N_SPEAKERS, N_BANDS)[:, spk].float(), dim=-1, eps=1e-12)
    return (p * t).sum(dim=-1).mean().item()


def train_single_epoch(model, data_loader, loss_fn, optimizer, device, scaler=None):
    train_loss = 0
    train_cos = 0
    train_cos_spk = [0.0, 0.0]
    model.train()

    n_batch = len(data_loader)
    for bi, (input, target) in enumerate(data_loader):
        input, target = input.to(device, non_blocking=True), target.to(device, non_blocking=True)
        input = prepare_batch(input, gain_range=GAIN_RANGE)

        # 混合精度上下文
        with torch.amp.autocast(device_type=device.type, enabled=scaler.is_enabled()):
            prediction = torch.tanh(model(input))
            loss = loss_fn(prediction, target)

        # 反向传播
        optimizer.zero_grad(set_to_none=True)
        if scaler.is_enabled():
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
        else:
            loss.backward()
            optimizer.step()

        # 记录损失与余弦相似度
        batch_loss = loss.item()
        train_loss += batch_loss * input.size(0)
        train_cos += batch_cosine_sim(prediction, target)
        for spk in range(N_SPEAKERS):
            train_cos_spk[spk] += batch_cosine_sim_spk(prediction, target, spk)

        # 进度: 每 LOG_EVERY 个 batch 打印一次 (flush 即时刷新)
        if (bi + 1) % LOG_EVERY == 0 or (bi + 1) == n_batch:
            print(f'    [train {bi+1}/{n_batch}] loss={batch_loss:.4f}', flush=True)

    n = len(data_loader)
    avg_loss = train_loss / len(data_loader.dataset)
    avg_cos = train_cos / n
    avg_cos_spk = [c / n for c in train_cos_spk]
    print(f"训练损失: {avg_loss:.4f}  cos: {avg_cos:.4f}  "
          f"逐扬声器 cos: {[f'{c:.4f}' for c in avg_cos_spk]}")
    return avg_cos, avg_loss, avg_cos_spk


def validate_single_epoch(model, data_loader, loss_fn, device):
    eval_loss = 0
    eval_cos = 0
    eval_cos_spk = [0.0, 0.0]
    model.eval()

    n_batch = len(data_loader)
    with torch.no_grad():
        for bi, (input, target) in enumerate(data_loader):
            input, target = input.to(device, non_blocking=True), target.to(device, non_blocking=True)
            input = prepare_batch(input)
            prediction = torch.tanh(model(input))
            loss = loss_fn(prediction, target)

            batch_loss = loss.item()
            eval_loss += batch_loss * input.size(0)
            eval_cos += batch_cosine_sim(prediction, target)
            for spk in range(N_SPEAKERS):
                eval_cos_spk[spk] += batch_cosine_sim_spk(prediction, target, spk)

            # 进度: 每 LOG_EVERY 个 batch 打印一次 (flush 即时刷新)
            if (bi + 1) % LOG_EVERY == 0 or (bi + 1) == n_batch:
                print(f'    [valid {bi+1}/{n_batch}] loss={batch_loss:.4f}', flush=True)

    n = len(data_loader)
    avg_loss = eval_loss / len(data_loader.dataset)
    avg_cos = eval_cos / n
    avg_cos_spk = [c / n for c in eval_cos_spk]
    print(f"验证损失: {avg_loss:.4f}  cos: {avg_cos:.4f}  "
          f"逐扬声器 cos: {[f'{c:.4f}' for c in avg_cos_spk]}")
    return avg_cos, avg_loss, avg_cos_spk


def train(model, train_loader, valid_loader, epochs, device, model_path=None, use_amp=True):
    """训练主循环, 支持自动混合精度 (AMP). use_amp: 若 GPU 支持 Tensor Core 则 True."""
    cos_max = 0.0
    loss_fn = nn.MSELoss()                     # 回归: tanh 输出 vs [-1,1] 归一化增益
    # 通过自适应学习率调整参数 (MIMO 原配置)
    optimizer = optim.Adam(model.parameters(), lr=LR, weight_decay=WEIGHT_DECAY)
    # 学习率调度器: 每 LR_STEP 轮 ×LR_GAMMA
    scheduler = optim.lr_scheduler.StepLR(optimizer, step_size=LR_STEP, gamma=LR_GAMMA)

    # 混合精度梯度缩放器 (仅在 GPU 且启用 AMP 时使用)
    scaler = torch.amp.GradScaler(device.type, enabled=use_amp and device.type == 'cuda')

    train_loss_epochs = []
    validate_loss_epochs = []

    for i in range(epochs):
        t0 = time.time()
        print(f"\n第 {i+1}/{epochs} 轮")
        print(f"学习率: {optimizer.param_groups[0]['lr']:.6f}")

        cos_train, loss_train, _ = train_single_epoch(
            model, train_loader, loss_fn, optimizer, device, scaler
        )
        cos_valid, loss_valid, cos_valid_spk = validate_single_epoch(
            model, valid_loader, loss_fn, device
        )

        scheduler.step()
        train_loss_epochs.append(loss_train)
        validate_loss_epochs.append(loss_valid)

        # 保存最佳模型 (选模指标 = 验证集整向量余弦相似度)
        marker = ' *' if cos_valid > cos_max else ''
        if cos_valid > cos_max:
            cos_max = cos_valid
            if model_path:
                torch.save(model.state_dict(), model_path)
                print(bcolors.OKCYAN + f"最佳模型已保存至 {model_path} "
                      f"(valid cos={cos_max:.4f})" + bcolors.ENDC)
        else:
            print(f"  最佳 cos 仍为 {cos_max:.4f}{marker}")
        print(f"  本轮用时 {time.time()-t0:.0f}s")
        print("-" * 40)

    print(f"\n训练完成, 最佳验证 cos={cos_max:.4f}")
    return cos_train, cos_max, train_loss_epochs, validate_loss_epochs


def Train_Validate_CNN(TRAIN_DATASET_FILE, VALIDATION_DATASET_FILE, MODEL_PTH,
                       File_sheet=None):
    """入口 (MIMO_GFANC 同名同参兼容).

    TRAIN/VALIDATION_DATASET_FILE: CSV 路径; WAV 目录 = CSV 同目录下
    {Training,Validate}_data. File_sheet 保留仅为签名兼容, 不再使用.
    """
    data_dir = os.path.dirname(TRAIN_DATASET_FILE)
    train_wav = os.path.join(data_dir, 'Training_data')
    valid_wav = os.path.join(data_dir, 'Validate_data')

    # 加载数据 (训练集增强, 验证集干净 — 早停/选模不被增强噪声污染)
    train_data = MyNoiseDataset(TRAIN_DATASET_FILE, train_wav, augment=True)
    valid_data = MyNoiseDataset(VALIDATION_DATASET_FILE, valid_wav, augment=False)

    # 创建 DataLoader (训练集 shuffle, 验证集不 shuffle)
    train_loader = create_data_loader(train_data, BATCH_SIZE, shuffle=True)
    valid_loader = create_data_loader(valid_data, BATCH_SIZE, shuffle=False)

    # 模型: m5_scene 直接权重回归头 (K=SC 维)
    model = m5_scene(K=SC, dropout=0.3)
    print(f"模型最后一层: {model.linear}")
    print(f"输出维度: {SC} = {N_SPEAKERS} 扬声器 × {N_BANDS} 子带 (直接权重)")

    model.apply(init_weights)

    # 设置设备
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    _init_bandpass(device)
    model = model.to(device)
    print(f"设备: {device}")

    # 开始训练 (AMP 默认开启, 若环境不支持会自动降级)
    cos_train, cos_validate, train_loss_epochs, validate_loss_epochs = train(
        model, train_loader, valid_loader,
        EPOCHS, device, MODEL_PTH, use_amp=True
    )

    return cos_train, cos_validate, train_loss_epochs, validate_loss_epochs


if __name__ == '__main__':
    print('=' * 60)
    print('  直接权重 CNN 训练 (m5_scene → 30 维权重回归)')
    print(f'  训练集: {TRAIN_CSV}')
    print(f'  验证集: {VALID_CSV}')
    print(f'  输出:   {OUTPUT_PTH}')
    print('=' * 60)
    Train_Validate_CNN(TRAIN_CSV, VALID_CSV, OUTPUT_PTH)
