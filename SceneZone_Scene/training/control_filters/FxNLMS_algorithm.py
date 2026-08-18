"""
MIMO FxNLMS 算法实现 - 多通道滤波-X 最小均方自适应.

支持 GPU 加速的多扬声器、多误差麦克风联合梯度更新,
逐扬声器独立功率归一化.
"""
import numpy as np
import scipy.signal as signal
import matplotlib.pyplot as plt
import time
import torch
from tqdm import tqdm  # 比 progressbar 更现代

DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

class FxNLMS_Optimized:
    """
    优化版FxNLMS：直接实现算法公式，避免PyTorch自动微分开销
    """
    def __init__(self, Len, mu=0.0001, device=None):
        self.mu = mu  # 步长参数
        self.Len = Len
        self.device = device if device is not None else DEVICE
        
        # 初始化滤波器系数和延迟线
        self.Wc = np.zeros(Len, dtype=np.float32)
        self.buffer = np.zeros(Len, dtype=np.float32)
        
    def reset(self):
        """重置滤波器状态"""
        self.Wc = np.zeros(self.Len, dtype=np.float32)
        self.buffer = np.zeros(self.Len, dtype=np.float32)
    
    def process_sample(self, x, d):
        """
        处理单个样本（核心优化：避免PyTorch开销）
        x: 参考信号样本
        d: 期望信号样本
        返回: 误差信号 e
        """
        # 更新延迟线
        self.buffer[1:] = self.buffer[:-1]
        self.buffer[0] = x
        
        # 计算输出 y = Wc·buffer
        y = np.dot(self.Wc, self.buffer)
        
        # 计算误差
        e = d - y
        
        # 计算参考信号功率 (避免除以0)
        power = np.dot(self.buffer, self.buffer) + 1e-10
        
        # FxNLMS更新规则: Wc(n+1) = Wc(n) + mu * e(n) * buffer(n) / power(n)
        self.Wc += self.mu * e * self.buffer / power
        
        return e
    
    def train(self, Ref, Disturbance, show_progress=True):
        """
        训练滤波器
        Ref: 参考信号 [N,]
        Disturbance: 期望信号 [N,]
        show_progress: 是否显示进度条
        返回: 误差信号数组
        """
        N = len(Disturbance)
        errors = np.zeros(N, dtype=np.float32)
        
        # 确保输入是numpy数组
        if isinstance(Ref, torch.Tensor):
            Ref = Ref.cpu().numpy()
        if isinstance(Disturbance, torch.Tensor):
            Disturbance = Disturbance.cpu().numpy()
        
        # 使用tqdm显示进度
        iterator = tqdm(range(N), desc='训练FxNLMS', unit='样本') if show_progress else range(N)
        
        start_time = time.time()
        for i in iterator:
            errors[i] = self.process_sample(Ref[i], Disturbance[i])
        
        elapsed = time.time() - start_time
        print(f'训练完成: {N:,} 样本, 耗时: {elapsed:.2f}秒, 速度: {N/elapsed/1000:.1f}k 样本/秒')
        
        return errors
    
    def get_coefficients(self):
        """获取滤波器系数"""
        return self.Wc.copy()
    
    def apply(self, Ref):
        """应用训练好的滤波器"""
        N = len(Ref)
        output = np.zeros(N, dtype=np.float32)
        
        if isinstance(Ref, torch.Tensor):
            Ref = Ref.cpu().numpy()
        
        for i in range(N):
            self.buffer[1:] = self.buffer[:-1]
            self.buffer[0] = Ref[i]
            output[i] = np.dot(self.Wc, self.buffer)

        return output


class FxNLMS_MIMO:
    """
    MIMO FxNLMS — 联合训练 S 个扬声器，最小化 E 个误差麦克风处的总误差。

    更新规则:
        y_e[n] = Σ_s (Wc[s] · X_{e,s}[n])
        err_e[n] = d_e[n] - y_e[n]
        Wc[s] += μ · Σ_e(err_e[n] · X_{e,s}[n]) / (Σ_e ||X_{e,s}[n]||² + ε)

    相比单通道独立训练的改进:
      1. 所有 E 个误差麦克风的信号都参与梯度计算 (修复单麦克风训练偏差)
      2. S 个扬声器联合优化，自动协调分工，避免互相抵消
    """
    def __init__(self, Len, E, S, mu=0.0001, device=None,
                 power_norm='mean', dtype=np.float64):
        self.mu = mu
        self.Len = Len
        self.E = E
        self.S = S
        self.device = device if device is not None else DEVICE
        self.power_norm = power_norm  # 'mean' or 'sum'
        self.dtype = dtype

        self.Wc = np.zeros((S, Len), dtype=dtype)
        self.buffers = np.zeros((E, S, Len), dtype=dtype)

    def reset(self):
        self.Wc = np.zeros((self.S, self.Len), dtype=self.dtype)
        self.buffers = np.zeros((self.E, self.S, self.Len), dtype=self.dtype)

    def train(self, Fx, Dis, show_progress=True):
        """
        训练 MIMO 滤波器
        Fx:  (E, S, N) 滤波参考信号
        Dis: (E, N)   扰动信号
        返回: (E, N) 误差信号
        """
        E, S, N = Fx.shape
        assert E == self.E and S == self.S

        if isinstance(Fx, torch.Tensor):
            Fx = Fx.cpu().numpy()
        if isinstance(Dis, torch.Tensor):
            Dis = Dis.cpu().numpy()

        Fx = Fx.astype(self.dtype)
        Dis = Dis.astype(self.dtype)
        errors = np.zeros((E, N), dtype=self.dtype)

        iterator = tqdm(range(N), desc='训练 MIMO FxNLMS', unit='样本') if show_progress else range(N)

        start_time = time.time()
        for n in iterator:
            self.buffers[:, :, 1:] = self.buffers[:, :, :-1]
            self.buffers[:, :, 0] = Fx[:, :, n]

            y = np.sum(self.Wc[np.newaxis, :, :] * self.buffers, axis=(1, 2))
            err = Dis[:, n] - y
            errors[:, n] = err

            for s in range(S):
                gradient = np.dot(err, self.buffers[:, s, :])
                if self.power_norm == 'sum':
                    power = np.sum(self.buffers[:, s, :] ** 2) + 1e-10
                else:
                    power = np.mean(self.buffers[:, s, :] ** 2) + 1e-10
                self.Wc[s] += self.mu * gradient / power

        elapsed = time.time() - start_time
        print(f'MIMO训练完成: {N:,} 样本, E={E}, S={S}, '
              f'耗时: {elapsed:.1f}秒, 速度: {N/elapsed/1000:.1f}k 样本/秒')

        return errors

    def get_coefficients(self, s=None):
        if s is not None:
            return self.Wc[s].copy()
        return self.Wc.copy()
