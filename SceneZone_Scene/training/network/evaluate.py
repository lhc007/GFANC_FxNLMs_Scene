"""
CNN 模型评估 — 直接权重回归 (MIMO_GFANC evaluate.py 适配版).

在测试集上计算: 整向量(SC=30)余弦相似度 / 逐扬声器余弦 / MSE.
输入管道与训练/部署一致: 带通(50-1500Hz) + minmax.
"""
import sys, os
from pathlib import Path

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader
from noise_dataset import MyNoiseDataset, MyNoiseDataset1, make_bandpass_tensor, prepare_batch as _pbatch

from gfanc.Network import m5_scene

BATCH_SIZE = 250     # 训练/验证时的批量大小
N_SPEAKERS = 2
N_BANDS = 15
SC = N_SPEAKERS * N_BANDS

_BP_PATH = _PROJECT_ROOT / 'models' / 'bandpass_fir.mat'
_bp_w = _bp_pad = None


def _init_bandpass(device):
    global _bp_w, _bp_pad
    _bp_w, _bp_pad = make_bandpass_tensor(_BP_PATH, device)


def prepare_batch(x):
    return _pbatch(x, _bp_w, _bp_pad)


def create_data_loader(data, batch_size):
    """创建数据加载器."""
    return DataLoader(data, batch_size)


def load_weigth_for_model(model, pretrained_path):
    """加载预训练权重到模型中 (逐参数覆盖)."""
    model_dict = model.state_dict()
    pretrained_dict = torch.load(pretrained_path, map_location="cpu", weights_only=True)
    for k, v in model_dict.items():
        model_dict[k] = pretrained_dict[k]
    model.load_state_dict(model_dict)


def validate_single_epoch(model, eva_data_loader, device):
    """回归质量: 整向量 cos / 逐扬声器 cos / MSE."""
    eval_cos = 0
    eval_cos_spk = [0.0, 0.0]
    eval_mse = 0
    model.eval()

    with torch.no_grad():
        for input, target in eva_data_loader:
            input, target = input.to(device), target.to(device)
            input = prepare_batch(input)
            prediction = torch.tanh(model(input))
            eval_mse += F.mse_loss(prediction.float(), target.float()).item() * input.size(0)

            pn = F.normalize(prediction.float(), dim=-1, eps=1e-12)
            tn = F.normalize(target.float(), dim=-1, eps=1e-12)
            eval_cos += (pn * tn).sum(dim=-1).mean().item()

            B, _ = prediction.shape
            pred4 = prediction.reshape(B, N_SPEAKERS, N_BANDS)
            tgt4 = target.reshape(B, N_SPEAKERS, N_BANDS)
            for spk in range(N_SPEAKERS):
                p = F.normalize(pred4[:, spk].float(), dim=-1, eps=1e-12)
                t = F.normalize(tgt4[:, spk].float(), dim=-1, eps=1e-12)
                eval_cos_spk[spk] += (p * t).sum(dim=-1).mean().item()

    n = len(eva_data_loader)
    return (eval_cos / n,
            [c / n for c in eval_cos_spk],
            eval_mse / len(eva_data_loader.dataset))


def Test_model_accuracy_original(TESTING_DATASET_FILE, MODLE_PTH, File_sheet=None):
    """回归版测试入口 (MIMO 同名兼容): 整向量 cos + 逐扬声器 cos + MSE.

    TESTING_DATASET_FILE: 测试集 CSV; WAV 目录 = 同目录下 Testing_data.
    """
    data_dir = os.path.dirname(TESTING_DATASET_FILE)
    testing_dataset = MyNoiseDataset(TESTING_DATASET_FILE,
                                     os.path.join(data_dir, 'Testing_data'),
                                     augment=False)
    testing_loader = create_data_loader(testing_dataset, int(BATCH_SIZE / 10))

    model = m5_scene(K=SC, dropout=0.3)
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    _init_bandpass(device)
    model = model.to(device)
    load_weigth_for_model(model, MODLE_PTH)
    model.eval()

    cos, cos_spk, mse = validate_single_epoch(model, testing_loader, device)
    print(f"\n测试回归质量 (带通+minmax 输入):")
    print(f"  整向量 cos: {cos:.4f}")
    print(f"  逐扬声器 cos: {[f'{c:.4f}' for c in cos_spk]}")
    print(f"  MSE: {mse:.6f}")
    return cos, cos_spk, mse


def Output_Test_Error_Samples(TESTING_DATASET_FILE, MODLE_PTH, File_sheet=None, top_n=10):
    """逐样本分析: 列出余弦相似度最低的 top_n 个样本 (预测与标签偏差最大)."""
    data_dir = os.path.dirname(TESTING_DATASET_FILE)
    testing_dataset = MyNoiseDataset1(TESTING_DATASET_FILE,
                                      os.path.join(data_dir, 'Testing_data'),
                                      augment=False)

    model = m5_scene(K=SC, dropout=0.3)
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    _init_bandpass(device)
    model = model.to(device)
    load_weigth_for_model(model, MODLE_PTH)
    model.eval()

    total = len(testing_dataset)
    print(f'测试数据集长度: {total}')

    results = []
    with torch.no_grad():
        for i in range(total):
            audio_sample_path, signal, label = testing_dataset[i]
            signal = prepare_batch(signal.to(device).unsqueeze(0))
            prediction = torch.tanh(model(signal)).squeeze(0)
            pn = F.normalize(prediction.float(), dim=-1, eps=1e-12)
            tn = F.normalize(label.to(device).float(), dim=-1, eps=1e-12)
            cos = (pn * tn).sum(dim=-1).item()
            mse = F.mse_loss(prediction.float(), label.to(device).float()).item()
            results.append((cos, mse, audio_sample_path))

    results.sort(key=lambda r: r[0])   # 升序: cos 最小 = 偏差最大
    all_cos = [r[0] for r in results]
    print(f'\n余弦相似度分布: min={min(all_cos):.4f} 中位={sorted(all_cos)[len(all_cos)//2]:.4f} '
          f'max={max(all_cos):.4f} 均值={sum(all_cos)/len(all_cos):.4f}')

    print(f'\n偏差最大的 {top_n} 个样本:')
    for cos, mse, path in results[:top_n]:
        print(f'  {path}: cos={cos:.4f}  mse={mse:.4f}')

    return all_cos


if __name__ == '__main__':
    TESTING_CSV = os.path.join(r'D:\Dataset\Real_world_Dataset', 'Index_real_Testing_data.csv')
    MODEL_PTH = str(_PROJECT_ROOT / 'models' / 'MIMO_M5_DirectWeight_Real.pth')
    print('=' * 60)
    print('  直接权重 CNN 测试集评估')
    print(f'  测试集: {TESTING_CSV}')
    print(f'  模型:   {MODEL_PTH}')
    print('=' * 60)
    Test_model_accuracy_original(TESTING_CSV, MODEL_PTH)
