"""
真实数据微调 (可选路线) — 在 2-③ 纯合成产物上, 用真实噪声低 LR 微调.

背景 (2026-08-09 诊断): 真实 4 类噪声(道路/儿童/施工/铁路)全低频主导、谱形接近,
从零在真实数据上训练 → CNN 输出坍缩到同一低频处方 (判别力仅 35.8%, 输入谱可分 75%).
纯合成训练 (Train_validate.py) 用多样谱形逼 CNN 学「输入谱→增益」映射, 微调阶段用
低学习率 (LR_FT=0.003) 在真实数据适配真实统计量, 防止把多样映射又坍缩回去.

与 Train_validate.py 同机制: m5_scene(K=30) + 带通(50-1500Hz) + minmax + MSE(tanh vs
归一化增益) + Adam + StepLR. 选模指标 = 验证集整向量余弦相似度. 训练集带 GAIN_RANGE
幅度不变性增强 (与 Train_validate.py 一致, 治部署端实机抖动); cos_max 从 -inf 起
(回归首轮 valid_cos 可为负, 从 0 起则一个检查点都不存 → 重载崩溃).

用法:
    # 默认: 加载 models/MIMO_M5_DirectWeight_Real.pth (2-③ 纯合成产物) → 微调 → 就地覆盖同路径
    python training/network/finetune_real.py

    # 换起始检查点 / 输出到别处 (保留纯合成产物)
    python training/network/finetune_real.py --pretrain-pth <path> --out models/MIMO_M5_DirectWeight_Real_ft.pth

    # 调参 (默认 EPOCHS_FT=25, LR_FT=0.003)
    python training/network/finetune_real.py --epochs-ft 25 --lr-ft 0.003 --batch 250

输出:
    models/MIMO_M5_DirectWeight_Real.pth   微调后模型 (export_bin.py 自动加载)

注意: 默认**就地覆盖** 2-③ 的纯合成产物 (当前部署模型 cos=0.9706). 保留原产物需
先备份, 或用 --out 指到别处再手动改名. 重跑 2-③ 亦可重新生成纯合成产物.
"""
import sys, os, argparse
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
# 配置 (均可 CLI 覆盖)
# ═══════════════════════════════════════════════════════════════
N_SPEAKERS = 2       # 扬声器数
N_BANDS = 15         # 每扬声器子带数
SC = N_SPEAKERS * N_BANDS     # 输出维度 = 30 直接权重

BATCH_SIZE   = 250
EPOCHS_FT    = 25          # 真实微调
LR_FT        = 0.003       # 微调低 LR — 防止把多样光谱映射又坍缩回低频处方
WEIGHT_DECAY = 1e-4
LR_STEP      = 5
LR_GAMMA     = 0.5
# 幅度不变性增强 (与 Train_validate.py 一致): 归一化后乘对数均匀随机增益, 教 CNN
# "输入整体缩放 ≠ 增益方向改变" → 治部署端每秒独立 minmax 的 denom 漂移导致的实机抖动.
# None=不增强; 验证不走增强 (选模公平).
GAIN_RANGE = (0.5, 2.0)
# 进度打印: 一轮有 ~337 batch, 脚本一轮才打印一次, 中途无输出会让用户误以为卡住.
LOG_EVERY = 50

# 数据路径 — 与 label_wavs.py --tag real / recluster_real.py 输出一致
REAL_DIR      = r'D:\Dataset\Real_world_Dataset'
REAL_TRAIN    = os.path.join(REAL_DIR, 'Index_real_Training_data.csv')
REAL_VALID    = os.path.join(REAL_DIR, 'Index_real_Validate_data.csv')
# WAV 目录 = CSV 同目录下 {Training,Validate}_data (File_path 仅为文件名)
TRAIN_WAV_DIR = os.path.join(REAL_DIR, 'Training_data')
VALID_WAV_DIR = os.path.join(REAL_DIR, 'Validate_data')

# 模型路径: 默认起点 = 2-③ 纯合成产物 (Train_validate.py 写的 Real.pth); 默认输出 = 同路径就地覆盖
DEFAULT_PRETRAIN = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_DirectWeight_Real.pth')
OUTPUT_PTH       = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_DirectWeight_Real.pth')

# 带通 FIR (与 C 端部署的 CNN 输入严格对齐)
_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_fir.mat'
# ═══════════════════════════════════════════════════════════════

# 带通权重 (init 后设定)
_bp_w = _bp_pad = None


def _init_bandpass(device):
    global _bp_w, _bp_pad
    _bp_w, _bp_pad = make_bandpass_tensor(_BP_PATH, device)


def prepare_batch(x, gain_range=None):
    """[B,1,16000] -> 带通 -> minmaxscaler (与部署端 scene_ctrl_process 一致).
    gain_range=(lo,hi): 幅度不变性增强 (训练用; 验证不传, 保证选模公平)."""
    return _pbatch(x, _bp_w, _bp_pad, gain_range)


def create_data_loader(data, batch_size, shuffle=True):
    """训练集 shuffle, 验证集不 shuffle (num_workers=0 同现有脚本)."""
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


# ═══════════════════════════════════════════════════════════════
# 微调训练循环 (单阶段, 参数化 lr/epochs; 与 Train_validate.py 机制一致)
# ═══════════════════════════════════════════════════════════════

def train_phase(model, train_loader, valid_loader, epochs, lr, wd, device,
                model_path, tag='微调'):
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
        tr_cos_spk = [0.0] * N_SPEAKERS
        n_batch = len(train_loader)
        for bi, (input, target) in enumerate(train_loader):
            input, target = input.to(device, non_blocking=True), target.to(device, non_blocking=True)
            input = prepare_batch(input, gain_range=GAIN_RANGE)
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
            for spk in range(N_SPEAKERS):
                tr_cos_spk[spk] += batch_cosine_sim_spk(prediction, target, spk)
            if (bi + 1) % LOG_EVERY == 0 or (bi + 1) == n_batch:
                print(f'    [train {bi+1}/{n_batch}] loss={loss.item():.4f}', flush=True)

        # 验证 (无增强 — 选模公平)
        model.eval()
        va_loss, va_cos, m = 0.0, 0.0, 0
        va_cos_spk = [0.0] * N_SPEAKERS
        with torch.no_grad():
            for input, target in valid_loader:
                input, target = input.to(device), target.to(device)
                input = prepare_batch(input)
                prediction = torch.tanh(model(input))
                loss = loss_fn(prediction, target)
                va_loss += loss.item() * input.size(0)
                va_cos += batch_cosine_sim(prediction, target)
                for spk in range(N_SPEAKERS):
                    va_cos_spk[spk] += batch_cosine_sim_spk(prediction, target, spk)
                m += input.size(0)

        scheduler.step()
        # cos 指标取跨批均值 (显示/选模都用均值, 不是累加和 — 累加和会打印成 >1 的误导值)
        tr_cos_avg = tr_cos / len(train_loader)
        va_cos_avg = va_cos / len(valid_loader)
        marker = ' *' if va_cos_avg > cos_max else ''
        if va_cos_avg > cos_max:
            cos_max = va_cos_avg
            if model_path:
                torch.save(model.state_dict(), model_path)
                print(bcolors.OKCYAN + f'  [{tag}] Epoch {i+1:3d}/{epochs}: '
                      f'train_loss={tr_loss/n:.4f} train_cos={tr_cos_avg:.4f} '
                      f'valid_loss={va_loss/m:.4f} valid_cos={va_cos_avg:.4f} '
                      f'逐扬声器 cos={[f"{c/len(valid_loader):.4f}" for c in va_cos_spk]}  '
                      f'lr={optimizer.param_groups[0]["lr"]:.2e}  '
                      f'最佳已存 {model_path} (cos={cos_max:.4f})' + bcolors.ENDC)
            else:
                print(f'  [{tag}] Epoch {i+1:3d}/{epochs}: '
                      f'train_loss={tr_loss/n:.4f} train_cos={tr_cos_avg:.4f}  '
                      f'valid_loss={va_loss/m:.4f} valid_cos={va_cos_avg:.4f}  '
                      f'lr={optimizer.param_groups[0]["lr"]:.2e}{marker}')
        else:
            print(f'  [{tag}] Epoch {i+1:3d}/{epochs}: '
                  f'train_loss={tr_loss/n:.4f} train_cos={tr_cos_avg:.4f}  '
                  f'valid_loss={va_loss/m:.4f} valid_cos={va_cos_avg:.4f}  '
                  f'lr={optimizer.param_groups[0]["lr"]:.2e}  最佳 cos 仍为 {cos_max:.4f}{marker}')
    print(f'  [{tag}] 完成, 最佳 valid_cos={cos_max:.4f} → {model_path}')
    return cos_max


# ═══════════════════════════════════════════════════════════════
# 入口
# ═══════════════════════════════════════════════════════════════

def main():
    global BATCH_SIZE, EPOCHS_FT, LR_FT
    ap = argparse.ArgumentParser(description='真实数据微调 (加载纯合成产物, 低 LR 微调)')
    ap.add_argument('--pretrain-pth', default=DEFAULT_PRETRAIN,
                    help='起始检查点 (默认 2-③ 纯合成产物 Real.pth)')
    ap.add_argument('--out', default=OUTPUT_PTH, help='输出路径 (默认就地覆盖 Real.pth)')
    ap.add_argument('--epochs-ft', type=int, default=EPOCHS_FT)
    ap.add_argument('--lr-ft', type=float, default=LR_FT)
    ap.add_argument('--batch', type=int, default=BATCH_SIZE)
    args = ap.parse_args()
    EPOCHS_FT, LR_FT, BATCH_SIZE = args.epochs_ft, args.lr_ft, args.batch

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    _init_bandpass(device)
    print('=' * 60)
    print('  真实数据微调 (在纯合成产物上低 LR 微调)')
    print(f'  起始权重: {args.pretrain_pth}')
    print(f'  真实数据: {REAL_TRAIN}')
    print(f'  设备:     {device}')
    print('=' * 60)

    model = m5_scene(K=SC, dropout=0.3)
    model.load_state_dict(torch.load(args.pretrain_pth, map_location='cpu',
                                     weights_only=True))
    print(f'  已加载 {args.pretrain_pth} (输出维度 {SC} = {N_SPEAKERS}×{N_BANDS})')
    model = model.to(device)

    train_ds = MyNoiseDataset(REAL_TRAIN, TRAIN_WAV_DIR, augment=True)
    valid_ds = MyNoiseDataset(REAL_VALID, VALID_WAV_DIR, augment=False)
    train_loader = create_data_loader(train_ds, BATCH_SIZE, shuffle=True)
    valid_loader = create_data_loader(valid_ds, BATCH_SIZE, shuffle=False)

    train_phase(model, train_loader, valid_loader,
                EPOCHS_FT, LR_FT, WEIGHT_DECAY, device, args.out, tag='微调')

    print(f'\n  完成: 最终模型 {args.out}')
    print('  注意: 默认就地覆盖 2-③ 纯合成产物; 保留原产物需先备份 (重跑 2-③ 亦可重新生成)')
    print('  下一步: verify_discrimination.py 检查点3 (CNN 输出判别力 → 70% 目标)')


if __name__ == '__main__':
    main()
