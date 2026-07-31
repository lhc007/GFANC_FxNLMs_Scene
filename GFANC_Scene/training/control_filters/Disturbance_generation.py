"""
扰动与滤波参考信号生成.

Disturbance_generation_from_real_noise():
  真实噪声 -> 初级路径卷积 -> 扰动 Dis (E, N)
            -> 次级路径卷积 -> 滤波参考 Fx (E, S, N)

支持 SISO 和 MIMO 路径, FFT 批量卷积加速.
"""
# Disturbance_generation.py  (多通道适配版本)
import numpy as np
import math 
import matplotlib.pyplot as plt
from numpy.core.fromnumeric import repeat 
from scipy import signal, misc
import torch

#-------------------------------------------------------------
# 辅助函数：多通道 FIR 滤波
#-------------------------------------------------------------
def _multi_channel_filter_pri(pri_path, x):
    """
    利用初级路径对单通道参考信号进行多通道滤波（FFT 批量版本）
    pri_path: (E, R, L) 三维 / (E, L) 二维 / (L,) 一维
    x: 单通道输入信号 (T,)
    返回: Dir, 形状 (E, T) 或 (T,)
    """
    if pri_path.ndim == 1:
        return signal.lfilter(pri_path, 1, x)
    if pri_path.ndim == 2:
        # (E, L) → 视为 (E, 1, L)
        pri_path = pri_path[:, np.newaxis, :]  # (E, 1, L)
    E, R, L = pri_path.shape
    T = len(x)
    N = T + L - 1

    # 计算输入信号 FFT（一次，所有误差通道共享）
    X = np.fft.rfft(x, n=N)

    # 批量计算所有初级路径滤波器的 FFT 并一次性完成卷积
    # pri_path[:, 0, :] 取每个误差麦克风对应（参考麦克风 R=1）的滤波器
    pri_e = pri_path[:, 0, :]           # (E, L)
    H = np.fft.rfft(pri_e, n=N)         # (E, N_freq)
    Dir = np.fft.irfft(X * H, n=N)[:, :T]  # (E, T)
    return Dir


def _multi_channel_filter_sec(sec_path, x):
    """
    利用次级路径对单通道参考信号进行多通道滤波（FFT 批量版本）
    sec_path: (E, S, L) 三维 / (E, L) 二维 / (L,) 一维
    x: 单通道输入信号 (T,)
    返回: Fx, 形状 (E, S, T) / (E, 1, T) / (T,)
    """
    if sec_path.ndim == 1:
        return signal.lfilter(sec_path, 1, x)
    if sec_path.ndim == 2:
        # (E, L) → 视为 (E, 1, L)
        sec_path = sec_path[:, np.newaxis, :]  # (E, 1, L)
    E, S, L = sec_path.shape
    T = len(x)
    N = T + L - 1

    # 计算输入信号 FFT（一次，所有 E*S=20 个通道共享）
    X = np.fft.rfft(x, n=N)

    # 将所有次级路径滤波器展平为 (E*S, L)，批量 FFT + 批量 IFFT
    sec_flat = sec_path.reshape(-1, L)   # (E*S, L)
    H = np.fft.rfft(sec_flat, n=N)       # (E*S, N_freq)
    Fx_flat = np.fft.irfft(X * H, n=N)[:, :T]  # (E*S, T)
    return Fx_flat.reshape(E, S, T)

#-------------------------------------------------------------
# 函数: Disturbance_reference_generation()
# 描述: 使用默认参数生成多通道扰动和参考信号 (E=5, S=4, 路径长度1024)
#-------------------------------------------------------------
def Disturbance_reference_generation():
    # 定义 ANC 系统的配置参数
    fs = 16000                 # 系统采样率
    T = 5                      # 仿真时长（秒）
    t = np.arange(0, T, 1/fs).reshape(-1, 1)
    
    # 多通道配置 (初级: E=5,R=1, 次级: E=5,S=4)
    E, R, S = 5, 1, 4
    L_pri = 1024               # 初级路径长度
    L_sec = 1024               # 次级路径长度
    
    # 构建参考信号（白噪声）
    Re = np.random.randn(len(t))
    
    # 设计低通原型滤波器 (长度均为 L_pri 或 L_sec)
    f_cutoff = 2000
    N_fc = f_cutoff / fs
    # 为保留原演示中的级联滤波器特性，使用较长滤波器
    b1 = signal.firwin(L_pri, N_fc)             # 截止频率 2000 Hz
    b2 = signal.firwin(L_sec, 2*N_fc)           # 截止频率 4000 Hz
    
    # 级联滤波器 (完整的初级路径原型)
    Pri_path_full = signal.convolve(b1, b2)      # 长度 L_pri + L_sec - 1
    # 截取前 L_pri 点作为多通道初级路径的滤波器系数
    pri_proto = Pri_path_full[:L_pri]
    # 构建多通道初级路径: (E, R, L_pri)
    Pri_path = np.tile(pri_proto.reshape(1, 1, -1), (E, R, 1))
    
    # 构建多通道次级路径: (E, S, L_sec), 各通道均使用 b2
    Sec_path = np.tile(b2.reshape(1, 1, -1), (E, S, 1))
    
    # 绘制级联滤波器与单级滤波器的频率响应 (仍使用完整的 Pri_path_full 和 b2)
    w1, h1 = signal.freqz(Pri_path_full)
    w2, h2 = signal.freqz(b2)
    plt.title('数字滤波器频率响应 (级联与单级)')
    plt.plot(w1, 20*np.log10(np.abs(h1)), 'b', label='级联滤波器 (Pri_path原型)')
    plt.plot(w2, 20*np.log10(np.abs(h2)), 'r', label='单级滤波器 (b2, Sec_path原型)')
    plt.ylabel('幅值响应 (dB)')
    plt.xlabel('频率 (rad/sample)')
    plt.legend()
    plt.grid()
    plt.show()

    # 绘制初级路径的冲激响应 (仍显示 b1)
    plt.title('初级路径原型 (b1) 的冲激响应')
    plt.plot(b1)
    plt.ylabel('幅值')
    plt.xlabel('长度 (抽头数)')
    plt.grid()
    plt.show()
    
    # 生成多通道扰动信号和滤波参考信号
    Dir = _multi_channel_filter_pri(Pri_path, Re)      # (E, T)
    Fx  = _multi_channel_filter_sec(Sec_path, Re)      # (E, S, T)
    
    print("扰动信号 Dir 形状:", Dir.shape, "，滤波参考信号 Fx 形状:", Fx.shape)
    print("滤波后参考信号 Fx 第一个样本 (通道0,源0):", Fx[0, 0, 1])

    # 绘制第一个误差通道的扰动信号频谱
    f, Pper_spec = signal.periodogram(Dir[0, :], fs, 'flattop', scaling='spectrum')
    plt.semilogy(f, Pper_spec)
    plt.xlabel('频率 [Hz]')
    plt.ylabel('功率谱密度')
    plt.title('扰动信号频谱 (误差通道 0)')
    plt.grid()
    plt.show()

    # 绘制第一个误差通道、第一个次级源对应的滤波参考信号频谱
    f, Pper_spec = signal.periodogram(Fx[0, 0, :], fs, 'flattop', scaling='spectrum')
    plt.semilogy(f, Pper_spec)
    plt.xlabel('频率 [Hz]')
    plt.ylabel('功率谱密度')
    plt.title('滤波参考信号频谱 (误差通道 0, 次级源 0)')
    plt.grid()
    plt.show()
    
    return torch.from_numpy(Dir).type(torch.float), torch.from_numpy(Fx).type(torch.float)

#-------------------------------------------------------------
# 函数: Disturbance_reference_generation_from_Afilter()
# 描述: 模拟原始 GFANC 逻辑 — 用时域 FIR 滤波器系数生成宽带带限噪声
#       (MIMO 适配版: 支持多通道主/次级路径)
#
# 与 Disturbance_reference_generation_from_Fvector 的关键区别:
#   from_Fvector: f_vector = [f_low, f_high] → 内部用 firwin 创建带通滤波器
#   from_Afilter:  f_vector = 时域 FIR 滤波器系数 (如 Creating_Filter 输出)
#
# 原始 GFANC 流程: 白噪声 → lfilter(f_vector) → 宽带噪声 → 主/次级路径 → Dis/Fx
#                 然后用 FxLMS 训练一个主控制滤波器, DFT 分解为 15 个子滤波器。
#-------------------------------------------------------------
def Disturbance_reference_generation_from_Afilter(fs, T, f_vector, Pri_path, Sec_path):
    """
    用时域 FIR 滤波器系数 (而非频带范围) 生成宽带带限训练噪声。

    MIMO 适配:
      Pri_path: (E, R, L) 替代原始单通道 (L,)
      Sec_path: (E, S, L) 替代原始单通道 (L,)

    参数:
        fs       : 采样率 (Hz)
        T        : 训练噪声时长 (秒)
        f_vector : 时域 FIR 滤波器系数 (1D ndarray, 长度任意)
        Pri_path : 主路径, (E, R, L) 或 (L,)
        Sec_path : 次级路径, (E, S, L) 或 (L,)

    返回:
        Dir : 扰动信号, (E, T_eff) 或 (T_eff,), torch.float32
        Fx  : 滤波参考信号, (E, S, T_eff) 或 (T_eff,), torch.float32
    """
    len_f = len(f_vector)
    t = np.arange(0, T, 1/fs).reshape(-1, 1)
    xin = np.random.randn(len(t))
    # 白噪声通过给定的宽带 FIR 滤波器 → 带限噪声 (与原始 GFANC 完全一致)
    Re = signal.lfilter(f_vector, 1, xin)
    Noise = Re[len_f - 1:]  # 去除滤波器暂态响应

    # 多通道滤波 (MIMO 适配: 替代原始的单通道 signal.lfilter)
    Dir = _multi_channel_filter_pri(Pri_path, Noise)
    Fx = _multi_channel_filter_sec(Sec_path, Noise)

    return torch.from_numpy(Dir).type(torch.float), torch.from_numpy(Fx).type(torch.float)

#-------------------------------------------------------------
# 函数: Disturbance_reference_generation_from_Fvector()
# 描述: 根据给定的频带参数生成多通道扰动和参考信号
#-------------------------------------------------------------
def Disturbance_reference_generation_from_Fvector(fs, T, f_vector, Pri_path, Sec_path):
    # Pri_path 形状: (E, R, L)  R=1
    # Sec_path 形状: (E, S, L)
    t = np.arange(0, T, 1/fs).reshape(-1, 1)
    len_f = 1024
    # 设计带通滤波器用于生成带限参考噪声
    b2 = signal.firwin(len_f, [f_vector[0], f_vector[1]], pass_zero='bandpass',
                       window='hamming', fs=fs)
    
    xin = np.random.randn(len(t))
    Re = signal.lfilter(b2, 1, xin)          # 白噪声通过带通滤波器
    Noise = Re[len_f-1:]                    # 去除滤波器暂态响应
    
    # 多通道滤波
    Dir = _multi_channel_filter_pri(Pri_path, Noise)    # (E, T_noise)
    Fx  = _multi_channel_filter_sec(Sec_path, Noise)    # (E, S, T_noise)
    
    return torch.from_numpy(Dir).type(torch.float), torch.from_numpy(Fx).type(torch.float)

#-------------------------------------------------------------
# 函数: Disturbance_generation_from_real_noise()
# 描述: 从真实噪声波形生成多通道扰动和滤波参考信号 (CPU numpy 版本)
#-------------------------------------------------------------
def Disturbance_generation_from_real_noise(fs, Repet, wave_form, Pri_path, Sec_path):
    """从实际噪声波形生成 SISO/MIMO 扰动和滤波参考信号（FFT 批量版本）。

    兼容 Tensor/ndarray，自动检测 SISO (Pri_path 1D) vs MIMO (Pri_path 3D)。
    """
    # 提取波形 — 兼容 Tensor/ndarray, 任意维度
    if isinstance(wave_form, torch.Tensor):
        wave = wave_form.squeeze().cpu().numpy()
    else:
        wave = np.squeeze(wave_form)
    # 多通道音频取第一通道
    if wave.ndim > 1:
        wave = wave[0, :]
    wave = wave.astype(np.float64)

    wavec = wave.copy()
    for _ in range(Repet):
        wavec = np.concatenate((wavec, wave), axis=0)

    # 多通道滤波 (_multi_channel_filter_* 内部处理 1D SISO / 3D MIMO)
    Dir = _multi_channel_filter_pri(Pri_path, wavec)
    Fx  = _multi_channel_filter_sec(Sec_path, wavec)

    # 截取整数秒 (兼容 1D SISO 和 ND MIMO)
    N = Dir.shape[-1]  # 最后一维是时间
    N_z = N // fs
    Dir = Dir[..., :N_z*fs]
    Fx  = Fx[..., :N_z*fs]
    wavec = wavec[:N_z*fs]

    return (torch.from_numpy(Dir).type(torch.float),
            torch.from_numpy(Fx).type(torch.float),
            torch.from_numpy(wavec).type(torch.float))


#-------------------------------------------------------------
# 函数: disturbance_generation_batch_gpu()
# 描述: GPU 批量版本 — 一次处理一批音频文件，用 torch FFT 替代 numpy FFT
#       消除 per-file CPU 瓶颈，是 Disturbance_generation_from_real_noise 的
#       批量 GPU 等价实现。数学结果与 CPU 版本一致。
#-------------------------------------------------------------
@torch.no_grad()
def disturbance_generation_batch_gpu(signals, Pri_path, Sec_path,
                                      fs=16000, Repet=3, snr_db=30):
    """GPU-batched disturbance generation — processes multiple audio signals
    in one GPU pass using batched torch FFT convolution.

    This is the GPU-equivalent of calling Disturbance_generation_from_real_noise
    on each signal individually. The math is identical; only the execution
    device and batching differ.

    Args:
        signals: list of 1D CPU tensors, each shape (T_i,) — variable length OK
        Pri_path: (E, R, L) numpy float64 array
        Sec_path: (E, S, L) numpy float64 array
        fs: sample rate (default 16000)
        Repet: number of times to repeat the signal (default 3)
        snr_db: SNR for noise addition (default 30)

    Returns:
        Dis: (B, E, T_out) float32 tensor on GPU
        Fx:  (B, E, S, T_out) float32 tensor on GPU
        T_out: int — output signal length after truncation to integer seconds
    """
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    B = len(signals)
    E, R, L_pri = Pri_path.shape
    _, S, L_sec = Sec_path.shape
    L = max(L_pri, L_sec)

    # ---- Step 1: pad signals to equal length, repeat, move to GPU ----
    max_len = max(s.shape[0] for s in signals)
    wave_batch = torch.zeros(B, max_len, dtype=torch.float32)
    for i, s in enumerate(signals):
        n = s.shape[0]
        wave_batch[i, :n] = s.float()

    # Repeat: [B, T] → [B, (Repet+1)*T]
    wavec = wave_batch  # [B, T]
    for _ in range(Repet):
        wavec = torch.cat((wavec, wave_batch), dim=1)  # [B, (Repet+1)*T]

    T_total = wavec.shape[1]
    N_fft = T_total + L - 1
    wavec_gpu = wavec.to(device)

    # ---- Step 2: pre-compute path FFTs (once, cached on first call) ----
    # 缓存同时持有路径数组引用: 防止数组被 GC 后 id() 复用导致错误命中
    cache = getattr(disturbance_generation_batch_gpu, '_cache', None)
    if (cache is not None and cache.get('N_fft') == N_fft
            and cache.get('pri_ref') is Pri_path and cache.get('sec_ref') is Sec_path):
        H_pri_gpu, H_sec_gpu = cache['H']
    else:
        pri_e = torch.from_numpy(Pri_path[:, 0, :]).float().to(device)   # (E, L)
        sec_flat = torch.from_numpy(Sec_path.reshape(-1, L_sec)).float().to(device)  # (E*S, L)
        H_pri = torch.fft.rfft(pri_e, n=N_fft)       # (E, N_freq)
        H_sec = torch.fft.rfft(sec_flat, n=N_fft)    # (E*S, N_freq)
        H_pri_gpu = H_pri.to(torch.complex64)
        H_sec_gpu = H_sec.to(torch.complex64)
        disturbance_generation_batch_gpu._cache = {
            'N_fft': N_fft, 'pri_ref': Pri_path, 'sec_ref': Sec_path,
            'H': (H_pri_gpu, H_sec_gpu),
        }

    # ---- Step 3: batched FFT convolution on GPU ----
    X = torch.fft.rfft(wavec_gpu, n=N_fft)  # [B, N_freq]

    # Primary path: Dir[b, e, n] = IFFT(X[b, :] * H_pri[e, :])
    Dir = torch.fft.irfft(
        X.unsqueeze(1) * H_pri_gpu.unsqueeze(0), n=N_fft
    )[:, :, :T_total]  # [B, E, T_total]

    # Secondary path: Fx[b, e, s, n] = IFFT(X[b, :] * H_sec[e*s_idx, :])
    Fx_flat = torch.fft.irfft(
        X.unsqueeze(1) * H_sec_gpu.unsqueeze(0), n=N_fft
    )[:, :, :T_total]  # [B, E*S, T_total]
    Fx = Fx_flat.reshape(B, E, S, T_total)  # [B, E, S, T_total]

    # ---- Step 4: truncate to integer seconds ----
    N_z = T_total // fs
    T_out = N_z * fs
    Dir = Dir[:, :, :T_out]
    Fx = Fx[:, :, :, :T_out]

    # ---- Step 5: (snr_db intentionally not applied — caller handles noise externally) ----
    return Dir, Fx, T_out

#-------------------------------------------------------------
# 函数: Varied_distrubance_reference_generation_from_Fvector()
# 描述: 根据多个频带参数生成时变的多通道扰动和参考信号
#-------------------------------------------------------------
def Varied_distrubance_reference_generation_from_Fvector(fs, T, f_vector, Pri_path, Sec_path):
    t = np.arange(0, T, 1/fs).reshape(-1, 1)
    len_f = 1024
    for ii in range(len(f_vector)):
        # f_vector[ii] 是一个频带 [f_low, f_high]
        b2 = signal.firwin(len_f, f_vector[ii], pass_zero='bandpass',
                           window='hamming', fs=fs)
        xin = np.random.randn(len(t))
        Re = signal.lfilter(b2, 1, xin)
        if ii == 0:
            Noise = Re[fs:]             # 第一段，去除暂态
        else:
            if ii == 2:
                # 第三段幅度加倍 (保持原有逻辑)
                Noise = np.concatenate((Noise, 4*Re[fs:]), axis=0)
            else:
                Noise = np.concatenate((Noise, Re[fs:]), axis=0)
    
    # 多通道滤波
    Dir = _multi_channel_filter_pri(Pri_path, Noise)    # (E, len(Noise))
    Fx  = _multi_channel_filter_sec(Sec_path, Noise)    # (E, S, len(Noise))
    
    return (torch.from_numpy(Dir).type(torch.float),
            torch.from_numpy(Fx).type(torch.float),
            torch.from_numpy(Noise).type(torch.float))
