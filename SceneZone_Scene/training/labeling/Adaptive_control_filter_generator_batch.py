"""
自适应控制滤波器批量生成 - LMS 求解最优滤波器系数.

adaptive_control_filter_batch_mimo(): 批量 FxNLMS/LMS 训练每个噪声样本的最优 Wc.
train_adaptive_gain_batch_mimo(): 批量求解子滤波器组合增益.
"""
# MIMO版本：批量训练自适应控制滤波器的脚本/类
# 系统配置：E 误差麦, S 扬声器 (运行时从滤波器/路径自动推断)
# 标签维度: S×C（每扬声器独立子带增益，梯度在 E 维度聚合但不跨 S 聚合）
# 示例: S=2, C=15 → 30 维; S=4, C=15 → 60 维
#
# ★ FIX v3.0: 添加参数验证和错误检查

import torch
import numpy as np
from tqdm import tqdm

#--------------------------------------------------------------------
# Class: adaptive_control_filter_batch_mimo()
# MIMO 自适应增益算法：1 ref, S speakers, E error mics
# W_gain: [Batch, S, C] — 每扬声器独立子带增益（60维）
#--------------------------------------------------------------------
class adaptive_control_filter_batch_mimo():
    
    def __init__(self, Control_filter_groups, Batch_size, muw, device):
        """MIMO adaptive algorithm: generates control filter from pre-trained
        sub-filters using batch processing.

        Args:
            Control_filter_groups (float32 tensor): Sub-filters [C x S x Len_c]
                C = number of sub-bands (e.g. 15)
                S = number of speakers (e.g. 4)
                Len_c = filter length (e.g. 2048)
            Batch_size (int): The size of the batch
            muw (float32): The step size value
            device: 'cuda' or 'cpu'
        """
        # ★ FIX v3.0: 验证滤波器维度
        if Control_filter_groups.ndim != 3:
            raise ValueError(
                f"Control_filter_groups 应为 3D (C, S, Len_c)，"
                f"实际 {Control_filter_groups.ndim}D")
        
        self.C = Control_filter_groups.shape[0]      # 子带数量
        self.S = Control_filter_groups.shape[1]      # 扬声器数量
        self.Len_c = Control_filter_groups.shape[2]  # 滤波器长度

        # W_gain: [Batch, S, C] — 每扬声器独立的子带增益
        self.W_gain = torch.zeros(Batch_size, self.S, self.C, 
                                 requires_grad=False,
                                 dtype=torch.float, device=device)

        self.Filters = Control_filter_groups.to(device)  # [C x S x Len_c]
        self.muw = muw
        self.device = device

        # 延迟线缓冲区
        self.E = None
        self.Xd = None
        self._ptr = 0

    def filter_processing(self, Fx_in, Dir):
        """Construct the anti-noise by combining the different output signals
        of the pre-trained MIMO control filters.

        Args:
            Fx_in (float32 tensor): Filtered-x input [Batch, E, S]
                E = number of error mics (e.g. 5)
                S = number of speakers (e.g. 4)
            Dir (float32 tensor): Disturbance [Batch, E]

        Returns:
            float32 tensor: Error signal [Batch, E]
        """
        # ★ FIX v3.0: 验证输入维度
        if Fx_in.ndim != 3:
            raise ValueError(
                f"Fx_in 应为 3D (Batch, E, S)，实际 {Fx_in.ndim}D")
        
        if Dir.ndim != 2:
            raise ValueError(
                f"Dir 应为 2D (Batch, E)，实际 {Dir.ndim}D")
        
        Batch_size, E, S = Fx_in.shape
        
        if S != self.S:
            raise ValueError(
                f"Fx_in 扬声器数 ({S}) 与滤波器不匹配 ({self.S})")

        # 首次调用时初始化延迟线环形缓冲区
        if self.Xd is None:
            self.E = E
            self.Xd = torch.zeros(Batch_size, E, S, self.Len_c,
                                 dtype=torch.float, device=self.device)
            self._ptr = 0

        # ---- 前馈过程 ----
        self.Xd[:, :, :, self._ptr] = Fx_in
        self._ptr = (self._ptr + 1) % self.Len_c

        # 将滤波器转动以对齐环形缓冲区的读取顺序
        rolled_F = torch.roll(torch.flip(self.Filters, dims=[2]),
                             shifts=self._ptr, dims=2)

        # Y_outs[b, e, s, c] = sum_k (Xd[b, e, s, k] * rolled_F[c, s, k])
        Y_outs = torch.einsum('besk,csk->besc', self.Xd, rolled_F)  # [Batch, E, S, C]

        # anti_noise[b, e] = sum_s sum_c (W_gain[b, s, c] * Y_outs[b, e, s, c])
        anti_noise = torch.einsum('bsc,besc->be', self.W_gain, Y_outs)  # [Batch, E]

        # 误差信号
        Err_vec = Dir - anti_noise  # [Batch, E]

        # ---- 反向更新过程 ----
        # ★ SISO-style 总功率归一化 (SUM over E error mics and C bands):
        #   SISO: Y_powers = Σ_c Y[c]²  → 匹配band主导分母
        #         非匹配band的步长 ∝ 1/总功率 → 极小 → 增益≈0 ✓
        #   MIMO(旧): per-band → 非匹配band分母小 → 步长大 → 增益暴涨 ✗
        #   修复: per-speaker总功率 = Σ_{e,c} Y[e,s,c]², 与SISO一致
        Y_powers = torch.einsum('besc,besc->bs', Y_outs, Y_outs)  # [Batch, S]
        Y_powers = Y_powers.unsqueeze(-1) + 1e-10                   # [Batch, S, 1]

        # 梯度: grad[b, s, c] = sum_e (Err[b, e] * Y_outs[b, e, s, c])
        gradient = torch.einsum('be,besc->bsc', Err_vec, Y_outs)  # [Batch, S, C]

        # 更新增益 (Y_powers 广播到所有 C bands)
        self.W_gain += self.muw * (1.0 / Y_powers) * gradient

        return Err_vec

    def get_coeffiecients_(self):
        """Extracting the coefficients from the generators.

        Returns:
            float32: The coefficients of the adaptive gain vector [Batch, S, C].
        """
        return self.W_gain


#--------------------------------------------------------------------
# Function: train_adaptive_gain_batch_mimo()
#--------------------------------------------------------------------
def train_adaptive_gain_batch_mimo(model, Fx, Dis, device, show_progress=False):
    """Train the gain of the MIMO adaptive filter algorithm.

    Args:
        model (adaptive_control_filter_batch_mimo): The MIMO adaptive filter model.
        Fx (float32 tensor): Filtered reference signal [Batch, E, S, Len_data]
        Dis (float32 tensor): Disturbance signal [Batch, E, Len_data]
        device: "cuda" or "cpu"
        show_progress: 是否显示逐样本进度条

    Returns:
        np.array: Error signal trajectory [Len_data, E] (mean over batch).
    """
    # ★ FIX v3.0: 验证输入维度
    if Fx.ndim != 4:
        raise ValueError(f"Fx 应为 4D (Batch, E, S, T)，实际 {Fx.ndim}D")
    
    if Dis.ndim != 3:
        raise ValueError(f"Dis 应为 3D (Batch, E, T)，实际 {Dis.ndim}D")
    
    Batch, E, S, Len_data = Fx.shape
    assert Dis.shape == (Batch, E, Len_data), \
        f"Fx/Dis 维度不匹配: Fx={Fx.shape}, Dis={Dis.shape}"
    
    Fx = Fx.to(device)
    Dis = Dis.to(device)

    # 在 GPU/设备上预分配
    Erro_signal_gpu = torch.empty((Len_data, E), dtype=torch.float, device=device)

    iterator = tqdm(range(Len_data), desc='  自适应训练', unit='样本',
                   leave=False, ncols=80) if show_progress else range(Len_data)

    for itera in iterator:
        fx_t = Fx[:, :, :, itera]   # [Batch, E, S]
        dis_t = Dis[:, :, itera]    # [Batch, E]
        erro_v = model.filter_processing(fx_t, dis_t)  # [Batch, E]
        Erro_signal_gpu[itera] = erro_v.mean(dim=0)

    return Erro_signal_gpu.cpu().numpy()
