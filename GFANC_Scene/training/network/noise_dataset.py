"""
PyTorch 噪声数据集 — 直接权重回归 (MIMO_GFANC noise_dataset.py 适配版).

MyNoiseDataset: 从 CSV(File_path + gain_*) + WAV 目录加载 (signal, target).
- 标签 = LMS 子带增益 gain_0..gain_{SC-1} (带符号, 与直接权重 Wc 构造语义一致)
- 归一化 = 每样本 max-abs → [-1,1] (与推理端 tanh 输出对齐)
- 输入 = 原始 1s 16kHz 波形; 带通(20-1500Hz)+minmax 在训练循环 prepare_batch
  整批 GPU 卷积做 (快 5-10 倍, 数学等价, 与 C 端 fir_tick 严格一致)
"""
import os
import math
import numpy as np
import pandas as pd
import torch
import torch.nn.functional as F
import torchaudio
from torch.utils.data import Dataset

SAMPLE_LEN = 16000   # 1s @ 16kHz


def minmaxscaler(data):
    """保留 MIMO 原接口 (数据集的 minmax 已在 prepare_batch 整批做, 此处仅兼容)."""
    min_val = data.min()
    max_val = data.max()
    return data / (max_val - min_val)


# ═══════════════════════════════════════════════════════════════
# 带通 (20-1500Hz) — 与 C 端 CNN 输入严格对齐
# (LMS 增益只编码 20-1500Hz 频谱模式; 全带训练会引入与标签无因果关系的高频
#  特征, 且部署端输入本就是带通后的信号)
# ═══════════════════════════════════════════════════════════════

def _load_bandpass_coeff(bp_path):
    """加载 20-1500Hz 带通 FIR 系数 [L]."""
    import scipy.io as sio
    mat = sio.loadmat(str(bp_path))
    return mat['fir_bandpass_coeff'].squeeze().astype(np.float32)


def make_bandpass_tensor(bp_path, device):
    """返回因果卷积权重 (1,1,L) + padding.

    F.conv1d 是互相关, 权重 flip + padding=L-1 + 截前 16000 点 = 因果卷积,
    与 C 端 fir_tick 数学等价.
    """
    bp = torch.from_numpy(_load_bandpass_coeff(bp_path)).to(device)
    return bp.flip(0).view(1, 1, -1), bp.numel() - 1


def prepare_batch(x, bp_w, bp_pad, gain_range=None):
    """[B,1,16000] -> 带通 -> minmaxscaler (与部署端 scene_ctrl_process 一致).

    gain_range=(lo,hi): 归一化后乘对数均匀随机增益, 模拟部署端缩放基准漂移
    (scene_controller.c 每秒独立 max-min 的 denom 在漂 → CNN 输入逐秒抖,
     2026-08-10 实机抖动根因). 训练时教 CNN"输入整体缩放 ≠ 增益方向改变".
    """
    x = F.conv1d(x, bp_w, padding=bp_pad)[:, :, :SAMPLE_LEN]
    scale = x.amax(dim=-1, keepdim=True) - x.amin(dim=-1, keepdim=True)
    x = torch.where(scale > 1e-10, x / scale.clamp(min=1e-10), x)
    if gain_range is not None:
        lo, hi = gain_range
        # 对数均匀: 几何均值=1, 不改变训练分布中心
        g = torch.exp(torch.rand(x.size(0), 1, 1, device=x.device) *
                      (math.log(hi) - math.log(lo)) + math.log(lo))
        x = x * g
    return x


# ═══════════════════════════════════════════════════════════════

def _load_signal(wav_dir, file_path):
    signal, _ = torchaudio.load(os.path.join(wav_dir, file_path))
    if signal.shape[0] > 1:
        signal = signal.mean(dim=0, keepdim=True)
    if signal.shape[1] < SAMPLE_LEN:
        signal = F.pad(signal, (0, SAMPLE_LEN - signal.shape[1]))
    else:
        signal = signal[:, :SAMPLE_LEN]
    return signal


def _augment(signal):
    """训练集增强 — 与 train_real_cnn.py 分类器完全一致:
    时间遮蔽/增益抖动/低噪注入."""
    # 时间遮蔽: 随机掩码 ~100ms (模拟瞬时干扰, 强迫CNN学全局频谱)
    mask_len = torch.randint(800, 2400, (1,)).item()
    mask_start = torch.randint(0, SAMPLE_LEN - mask_len, (1,)).item()
    signal[:, mask_start:mask_start + mask_len] = 0
    # 增益抖动: 0.7x ~ 1.3x
    signal = signal * (0.7 + 0.6 * torch.rand(1).item())
    # 低噪注入: -25dB
    noise_rms = signal.std() * (10 ** (-1.25))
    signal = signal + torch.randn_like(signal) * noise_rms
    return signal


class MyNoiseDataset(Dataset):
    """直接权重回归数据集.

    Args:
        csv_path:  索引 CSV 路径 (含 File_path / category / gain_* 列)
        wav_dir:   WAV 片段目录 (CSV File_path 相对此目录)
        augment:   True=训练集增强, False=验证集 (干净, 与部署条件一致)
    """

    def __init__(self, csv_path, wav_dir, augment=False):
        df = pd.read_csv(csv_path)
        self.wav_dir = wav_dir
        self.augment = augment
        self._file_path_col = df['File_path'].values
        self.categories = df['category'].values
        gain_cols = [c for c in df.columns if c.startswith('gain_')]
        gains = df[gain_cols].values.astype(np.float32)          # (N, SC) 带符号
        # 每样本 max-abs 归一化 → [-1,1] (全零样本保底, 避免除零)
        per_max = np.abs(gains).max(axis=1, keepdims=True)
        per_max = np.where(per_max < 1e-12, 1.0, per_max)
        self._labels = gains / per_max
        self._num_gains = len(gain_cols)
        print(f'  MyNoiseDataset: {len(self)} samples, {self._num_gains} gains '
              f'(S*C), augment={augment}')

    def __len__(self):
        return len(self._file_path_col)

    def __getitem__(self, index):
        signal = _load_signal(self.wav_dir, self._file_path_col[index])
        if self.augment:
            signal = _augment(signal)
        label = self._labels[index]
        return signal, torch.from_numpy(label)


class MyNoiseDataset1(Dataset):
    """与 MyNoiseDataset 相同, 但返回 (audio_path, signal, label) — 逐样本测试用."""

    def __init__(self, csv_path, wav_dir, augment=False):
        df = pd.read_csv(csv_path)
        self.wav_dir = wav_dir
        self.augment = augment
        self._file_path_col = df['File_path'].values
        gain_cols = [c for c in df.columns if c.startswith('gain_')]
        gains = df[gain_cols].values.astype(np.float32)
        per_max = np.abs(gains).max(axis=1, keepdims=True)
        per_max = np.where(per_max < 1e-12, 1.0, per_max)
        self._labels = gains / per_max
        self._num_gains = len(gain_cols)

    def __len__(self):
        return len(self._file_path_col)

    def __getitem__(self, index):
        audio_sample_path = self._file_path_col[index]
        signal = _load_signal(self.wav_dir, audio_sample_path)
        if self.augment:
            signal = _augment(signal)
        label = self._labels[index]
        return audio_sample_path, signal, torch.from_numpy(label)
