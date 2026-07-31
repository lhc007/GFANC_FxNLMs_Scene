"""
子滤波器预训练 - 宽带 FxNLMS 训练 + sqrt-Hann 加窗 DFT 频域分解.

流程: 宽带 FIR -> 白噪声 -> MIMO FxNLMS 训练主滤波器 Wc
      -> FFT -> sqrt-Hann 加窗分割为 15 个功率互补子带 -> IFFT
      -> 子滤波器组 [15, S, Len_control]

功率互补: sum(W_c^2) ~= W_main^2 (频域功率守恒)
完美重构: sum(W_c) ~= W_main   (时域幅度重构)
子带隔离: ~40-50 dB (vs 矩形窗 ~13 dB)

输出: models/MIMO_Pretrained_Control_filters_broadband.mat
"""
# Pre_training_broadband_and_decompose.py
# 仿照 SISO GFANC 流程：宽带训练主滤波器 + DFT 分解为子滤波器
#
# 与原 Pre_trianing_control_filters.py 的核心区别:
#   原方案: 15 个窄带各自独立训练 FxNLMS → 窄带收敛极慢 → 子滤波器增益 -43 dB
#   新方案: 1 个宽带主滤波器训练 → 快速收敛 → DFT 分解为 15 个子滤波器
#
# 流程:
#   Step 1: 创建宽带滤波器 (20-1500 Hz)
#   Step 2: MIMO FxNLMS 在宽带噪声上训练主滤波器 → Wc_main (S=4, Len=Len_control)
#   Step 3: 对每个扬声器的主滤波器做 sqrt-Hann DFT 分解 → sub_filters (C=15, S=4, Len=Len_control)
#   Step 4: 保存
# ============================================================================
import os
import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from scipy.io import savemat
from scipy.fft import fft, ifft
import scipy.signal as signal

# 兼容直接运行 (python file.py) 和包内导入
try:
    from .path_loader import load_multichannel_paths_with_variable_names
    from .Disturbance_generation import Disturbance_reference_generation_from_Afilter
    from .FxNLMS_algorithm import FxNLMS_MIMO, DEVICE
except ImportError:
    _here = os.path.dirname(os.path.abspath(__file__))
    if _here not in sys.path:
        sys.path.insert(0, _here)
    from path_loader import load_multichannel_paths_with_variable_names
    from Disturbance_generation import Disturbance_reference_generation_from_Afilter
    from FxNLMS_algorithm import FxNLMS_MIMO, DEVICE


# ═══════════════════════════════════════════════════════════════════
# 非均匀 sqrt-Hann 加窗 DFT 滤波器分解
#
# 与均匀分解的区别:
#   均匀: 0~fs/2 等分 15 段 → 20-1500 Hz 只覆盖 3 段, 其余浪费
#   非均匀: 只对 [f_low, f_high] 均分 15 段 → 全在有效区, ~100 Hz/子带
#
# 原理: 零填充 FFT (4x) → 频域精细加窗 → IFFT → 截断回原长度
# ═══════════════════════════════════════════════════════════════════

def _freq_bin(freq_hz, n_fft, fs):
    """频率 → FFT bin 索引 (浮点)."""
    return freq_hz * n_fft / fs


def _build_nonuniform_sqrt_hann_window(n_fft, c_i, M, is_first=False,
                                         is_last=False):
    """为单个子带构造 sqrt-Hann 频域窗 (非均匀子带版本).

    功率互补:
      - 内部子带: 标准 sqrt-Hann, 相邻 w_i^2 + w_{i+1}^2 = 1
      - 首子带: DC~c_i 段 w=1 (独占低频), c_i~c_i+M 用 cos 下降 1→0
      - 末子带: c_i-M~c_i 用 sin 上升 0→1, c_i~Nyquist 段 w=1 (独占高频)

    Args:
        n_fft: 零填充 FFT 点数
        c_i: 子带中心 bin (整数)
        M: 半个子带宽度 (整数, bin 数)
        is_first: 首子带 (只保留右半窗 + DC 侧全通)
        is_last:  末子带 (只保留左半窗 + Nyquist 侧全通)

    Returns:
        ndarray (n_fft,): 全 FFT 频谱窗, 保证 IFFT 实值
    """
    n_pos = n_fft // 2 + 1
    w_pos = np.zeros(n_pos, dtype=np.float64)
    two_M = 2 * M

    if is_first:
        # 右半窗: cos 下降 1→0, k in [c_i, c_i+M]
        lo = max(0, c_i)
        hi = min(n_pos - 1, c_i + M)
        for k in range(lo, hi + 1):
            pos = k - c_i
            w_pos[k] = np.cos(np.pi * pos / two_M)
        # DC 侧全通: 低于 c_i 的部分 w=1
        if c_i > 0:
            w_pos[:c_i] = 1.0

    elif is_last:
        # 左半窗: sin 上升 0→1, k in [c_i-M, c_i]
        lo = max(0, c_i - M)
        hi = min(n_pos - 1, c_i)
        for k in range(lo, hi + 1):
            pos = k - (c_i - M)
            w_pos[k] = np.sin(np.pi * pos / two_M)
        # Nyquist 侧全通: 高于 c_i 的部分 w=1
        if c_i + 1 < n_pos:
            w_pos[c_i:] = 1.0

    else:
        # 完整 sqrt-Hann: sin 上升 0→1, cos 下降 1→0
        lo_rise = max(0, c_i - M)
        hi_rise = min(n_pos - 1, c_i)
        for k in range(lo_rise, hi_rise + 1):
            pos = k - (c_i - M)
            if pos >= 0:
                w_pos[k] = np.sin(np.pi * pos / two_M)

        lo_fall = max(0, c_i)
        hi_fall = min(n_pos - 1, c_i + M)
        for k in range(lo_fall, hi_fall + 1):
            pos = k - c_i
            if pos <= M:
                w_pos[k] = np.cos(np.pi * pos / two_M)

    # 构造共轭对称全频谱窗 → IFFT 实值
    w_full = np.zeros(n_fft, dtype=np.float64)
    w_full[:n_pos] = w_pos
    if n_pos > 2:
        w_full[n_pos:] = w_pos[1:-1][::-1]

    return w_full


def filter_decompose(filter_coeff, num_subfilters, fs=16000,
                     f_low=20.0, f_high=1500.0):
    """非均匀 sqrt-Hann DFT 分解: 只在 [f_low, f_high] 内均布子带.

    原理:
      - 零填充 FFT (4x) 获得频域细分 bin
      - 在 [f_low, f_high] 内均布 15 个 sqrt-Hann 功率互补窗
      - 带外 (<f_low, >f_high): 首/末子带各独占一侧 (w=1), 其余子带 w=0
      - 零填充 IFFT → 截断回 Len_control 长度

    Args:
        filter_coeff: 主滤波器系数 (Len,)
        num_subfilters: 子滤波器数量
        fs: 采样率
        f_low, f_high: 有效频段 (Hz)

    Returns:
        ndarray: 子滤波器组 (num_subfilters, Len)
    """
    N = len(filter_coeff)
    n_fft = N * 4                    # 零填充 → 4x 频域分辨率
    sub_filters = np.zeros((num_subfilters, N), dtype=np.float64)

    # 频域 bin 映射
    bin_low = _freq_bin(f_low, n_fft, fs)
    bin_high = _freq_bin(f_high, n_fft, fs)
    M_float = (bin_high - bin_low) / (2.0 * num_subfilters)
    M = max(1, round(M_float))      # 半窗宽度 (整数 bin)

    # 子带中心: c_i = bin_low + (2i+1)*M
    #   i=0:  c_0 = bin_low + M   (首子带, 左半窗裁掉)
    #   i=14: c_14 = bin_high - M (末子带, 右半窗裁掉)
    Fre_filter = fft(filter_coeff, n=n_fft)

    for ii in range(num_subfilters):
        c_i = round(bin_low + (2 * ii + 1) * M)
        is_first = (ii == 0)
        is_last = (ii == num_subfilters - 1)

        w_full = _build_nonuniform_sqrt_hann_window(
            n_fft, c_i, M, is_first=is_first, is_last=is_last)
        Temper_spectrum = Fre_filter * w_full
        sub_full = ifft(Temper_spectrum).real
        sub_filters[ii, :] = sub_full[:N]  # 截断回原长度

    return sub_filters


# ═══════════════════════════════════════════════════════════════════
# 对数间距 sqrt-Hann 加窗 DFT 滤波器分解
#
# 与均匀分解的区别:
#   均匀: [f_low, f_high] 等间距 15 段 → 每段 ~99 Hz
#   对数: 15 个对数间距中心频率 (23~1299 Hz) → 低频密/高频疏,
#         符合人耳听觉 Bark/ERB 尺度, 每段恒定 Q ≈ 3.5
#
# 频带边界由显式 edge 数组定义 (16 个边界 → 15 个频带):
#   edge[0]=20, edge[1]=27, ..., edge[15]=1500
# 每个子带 i: sin 上升 edge[i]→center[i], cos 下降 center[i]→edge[i+1]
# 带外 (<edge[0] 或 >edge[-1]) 无覆盖 (ANC 有效频段之外)
# ═══════════════════════════════════════════════════════════════════

# 对数间距子带中心频率 (Hz), 恒定 Q ≈ 3.5
# Band   0:  23 Hz ( 20- 27)    Band   5:  97 Hz ( 84-112)
# Band   1:  31 Hz ( 27- 36)    Band   6: 130 Hz (112-150)
# Band   2:  41 Hz ( 36- 47)    Band   7: 173 Hz (150-200)
# Band   3:  55 Hz ( 47- 63)    Band   8: 231 Hz (200-267)
# Band   4:  73 Hz ( 63- 84)    Band   9: 308 Hz (267-356)
# Band  10: 411 Hz (356-474)    Band  12: 730 Hz (633-843)
# Band  11: 548 Hz (474-633)    Band  13: 974 Hz (843-1125)
# Band  14: 1299 Hz (1125-1500)
LOG_BAND_CENTERS_HZ = np.array(
    [23, 31, 41, 55, 73, 97, 130, 173, 231, 308, 411, 548, 730, 974, 1299],
    dtype=np.float64)

# 对数间距频带边界 (Hz) — 16 个边界定义 15 个频带
# edge[0]=20, edge[1]=27, ..., edge[15]=1500
# 相邻频带在 edge 处交叉 (w=0), 频带外无覆盖
LOG_BAND_EDGES_HZ = np.array(
    [20, 27, 36, 47, 63, 84, 112, 150, 200, 267, 356, 474, 633, 843, 1125, 1500],
    dtype=np.float64)


def _build_log_sqrt_hann_window(n_fft, edges_bin, centers_bin, i):
    """为对数间距子带构造 sqrt-Hann 频域窗 (显式边界, 可变过渡宽度).

    每个子带 i 的窗:
      - sin 上升 0→1: edge[i] → center[i]   (宽度 M_left)
      - cos 下降 1→0: center[i] → edge[i+1]  (宽度 M_right)
      - 带外 w=0 (包括首子带 <20Hz 和末子带 >1500Hz)

    首/末子带与内部子带处理完全相同 — 不使用 DC/Nyquist 全通.

    Args:
        n_fft: 零填充 FFT 点数
        edges_bin: 频带边界 bin 索引 (int array, length=num_subfilters+1)
        centers_bin: 子带中心 bin 索引 (int array, length=num_subfilters)
        i: 当前子带索引

    Returns:
        ndarray (n_fft,): 全 FFT 频谱窗, 保证 IFFT 实值
    """
    n_pos = n_fft // 2 + 1
    w_pos = np.zeros(n_pos, dtype=np.float64)

    c_i = int(centers_bin[i])
    lo_edge = int(edges_bin[i])       # 左边界: edge[i]
    hi_edge = int(edges_bin[i + 1])   # 右边界: edge[i+1]

    # 过渡宽度 (bin), 最小 1 避免除零
    M_left = max(1, c_i - lo_edge)
    M_right = max(1, hi_edge - c_i)

    # sin 上升 0→1: k in [lo_edge, c_i]
    lo = max(0, lo_edge)
    hi = min(n_pos - 1, c_i)
    two_M_left = 2.0 * M_left
    for k in range(lo, hi + 1):
        pos = k - lo_edge
        if pos >= 0:
            w_pos[k] = np.sin(np.pi * pos / two_M_left)

    # cos 下降 1→0: k in [c_i, hi_edge]
    lo = max(0, c_i)
    hi = min(n_pos - 1, hi_edge)
    two_M_right = 2.0 * M_right
    for k in range(lo, hi + 1):
        pos = k - c_i
        if pos <= M_right:
            w_pos[k] = np.cos(np.pi * pos / two_M_right)

    # 构造共轭对称全频谱窗 → IFFT 实值
    w_full = np.zeros(n_fft, dtype=np.float64)
    w_full[:n_pos] = w_pos
    if n_pos > 2:
        w_full[n_pos:] = w_pos[1:-1][::-1]

    return w_full


def filter_decompose_log(filter_coeff, band_centers_hz=None,
                         band_edges_hz=None, fs=16000):
    """对数间距 sqrt-Hann DFT 分解: 显式边界定义频带.

    与 filter_decompose (均匀间距) 的区别:
      - 频带由 band_edges_hz (边界) + band_centers_hz (中心) 显式定义
      - 每个子带左右过渡区宽度独立 (低频窄/高频宽)
      - 首子带不从 DC 开始, 末子带不延伸至 Nyquist — 严格匹配频带表
      - **自适应 n_fft**: 保证最窄过渡带至少 4 个 FFT bin,
        避免低频 band (≤3Hz 过渡) 因频域分辨率不足而塌缩

    Args:
        filter_coeff: 主滤波器系数 (Len,)
        band_centers_hz: 中心频率数组 (Hz), 默认 LOG_BAND_CENTERS_HZ
        band_edges_hz: 频带边界数组 (Hz), 默认 LOG_BAND_EDGES_HZ
                       长度 = len(centers) + 1
        fs: 采样率

    Returns:
        ndarray: 子滤波器组 (num_subfilters, Len)
    """
    if band_centers_hz is None:
        band_centers_hz = LOG_BAND_CENTERS_HZ
    if band_edges_hz is None:
        band_edges_hz = LOG_BAND_EDGES_HZ

    N = len(filter_coeff)
    num_subfilters = len(band_centers_hz)
    assert len(band_edges_hz) == num_subfilters + 1, \
        f'edges 长度应为 {num_subfilters + 1}, 实际 {len(band_edges_hz)}'

    # ── 自适应 n_fft: 保证最窄过渡带至少 MIN_BINS_PER_TRANSITION 个 bin ──
    MIN_BINS_PER_TRANSITION = 4
    # 计算所有过渡带宽 (Hz): 左过渡=center-edge[i], 右过渡=edge[i+1]-center
    all_transitions = []
    for i in range(num_subfilters):
        all_transitions.append(band_centers_hz[i] - band_edges_hz[i])       # M_left
        all_transitions.append(band_edges_hz[i + 1] - band_centers_hz[i])   # M_right
    min_transition_hz = min(all_transitions)
    # n_fft 需要满足: fs / n_fft ≤ min_transition_hz / MIN_BINS_PER_TRANSITION
    required_n_fft = fs * MIN_BINS_PER_TRANSITION / min_transition_hz
    # 向上取到 2 的幂, 且不低于 N*4
    n_fft = 2 ** int(np.ceil(np.log2(max(required_n_fft, N * 4))))
    actual_bins = min_transition_hz * n_fft / fs
    print(f'  对数分解 n_fft: {n_fft} (最窄过渡带 {min_transition_hz:.1f} Hz → '
          f'{actual_bins:.1f} bins, 频域分辨率 {fs / n_fft:.2f} Hz/bin)')

    sub_filters = np.zeros((num_subfilters, N), dtype=np.float64)

    # 频率 → bin 索引
    centers_bin = np.round(band_centers_hz * n_fft / fs).astype(int)
    edges_bin = np.round(band_edges_hz * n_fft / fs).astype(int)

    Fre_filter = fft(filter_coeff, n=n_fft)

    for ii in range(num_subfilters):
        w_full = _build_log_sqrt_hann_window(
            n_fft, edges_bin, centers_bin, ii)
        Temper_spectrum = Fre_filter * w_full
        sub_full = ifft(Temper_spectrum).real
        sub_filters[ii, :] = sub_full[:N]  # 截断回原长度

    return sub_filters


# ═══════════════════════════════════════════════════════════════════
# 宽带训练 + DFT 分解主流程
# ═══════════════════════════════════════════════════════════════════

def ensure_dir(dir_path):
    if not os.path.exists(dir_path):
        os.makedirs(dir_path)


def main():
    # ── 配置参数 ──────────────────────────────────────────────
    # ================== 全局开关：改这里即可切换 ==================
    USE_LOG_SPACING = True   # True: 对数间距 (23~1299Hz), False: 均匀间距 (20~1500Hz)
    # =============================================================

    if USE_LOG_SPACING:
        OUTPUT_FILE = 'models/MIMO_Pretrained_Control_filters_logspacing.mat'
    else:
        OUTPUT_FILE = 'models/MIMO_Pretrained_Control_filters_broadband.mat'
    PRI_PATH_FILE_NAME = 'primary_path.npy'
    SEC_PATH_FILE_NAME = 'secondary_path.npy'
    FIGURE_DIR = 'figures/broadband/'
    ensure_dir('models')
    ensure_dir(FIGURE_DIR)

    fs = 16000
    T = 60                     # 训练时长（秒）, 加倍保收敛
    Len_control = 1024          # 滤波器长度
    num_subfilters = 15         # 子滤波器数量
    mu = 0.05                   # sum 归一化 + float64, μ 可放大
    f_low = 20.0                # 训练宽带噪声低频截止
    f_high = 1500.0             # 训练宽带噪声高频截止

    print(f'使用设备: {DEVICE}')
    if DEVICE.type == 'cuda':
        import torch
        print(f'GPU: {torch.cuda.get_device_name(0)}')

    # ── Step 1: 创建宽带滤波器 ─────────────────────────────────
    print(f'\n[Step 1/4] 创建宽带训练滤波器 ({f_low}-{f_high} Hz)...')
    broadband_filter = signal.firwin(
        Len_control, [f_low, f_high],
        pass_zero='bandpass', window='hamming', fs=fs
    )

    # 绘制宽带滤波器频响（调试用）
    w, h = signal.freqz(broadband_filter)
    plt.figure(figsize=(10, 4))
    plt.plot(w * fs / (2 * np.pi), 20 * np.log10(np.abs(h) + 1e-15))
    plt.title(f'Broadband Training Filter ({f_low}-{f_high} Hz)')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(os.path.join(FIGURE_DIR, 'broadband_filter_response.png'), dpi=150)
    plt.close()
    print(f'  宽带滤波器长度: {len(broadband_filter)}, 通带: {f_low}-{f_high} Hz')

    # ── Step 2: 加载路径、生成宽带训练信号 ─────────────────────
    print('\n[Step 2/4] 加载 MIMO 路径 & 生成宽带训练信号...')
    Pri_path, Secon_path = load_multichannel_paths_with_variable_names(
        folder='Primary and Secondary Path', subfolder='',
        Pri_path_file_name=PRI_PATH_FILE_NAME,
        Sec_path_file_name=SEC_PATH_FILE_NAME)

    E, R, L_pri = Pri_path.shape
    E_s, S, L_sec = Secon_path.shape
    assert E == E_s, f'初级路径误差通道数{E}与次级路径误差通道数{E_s}不匹配'
    print(f'  MIMO 系统: E={E} 误差麦, R={R} 参考麦, S={S} 扬声器')
    print(f'  训练时长: {T} 秒 = {T * fs:,} 样本')

    # 生成宽带训练信号
    Dis, Fx = Disturbance_reference_generation_from_Afilter(
        fs=fs, T=T, f_vector=broadband_filter,
        Pri_path=Pri_path, Sec_path=Secon_path)
    print(f'  Dis 形状: {Dis.shape} (E={E}, N={Dis.shape[1]})')
    print(f'  Fx  形状: {Fx.shape}  (E={E}, S={S}, N={Fx.shape[2]})')

    # ── Step 3: MIMO FxNLMS 宽带训练 (float64 + sum norm + Fx归一化) ──
    print(f'\n[Step 3/4] MIMO FxNLMS 宽带训练 (mu={mu}, sum norm, float64)...')
    Fx_train = Fx.cpu().numpy().astype(np.float64)
    d_train = Dis.cpu().numpy().astype(np.float64)

    # Fx per-channel 归一化: 降低 Sec 高增益对收敛的影响
    fx_rms = float(np.sqrt(np.mean(Fx_train ** 2)))
    Fx_train = Fx_train / fx_rms
    d_train = d_train / fx_rms
    print(f'  Fx 归一化: RMS {fx_rms:.1f} → 1.0')

    controller = FxNLMS_MIMO(Len=Len_control, E=E, S=S, mu=mu,
                             power_norm='sum', dtype=np.float64)
    Erro = controller.train(Fx_train, d_train, show_progress=True)

    # 获取主滤波器 + 反归一化
    Wc_main = controller.get_coefficients().astype(np.float64)  # (S, Len_control)
    print(f'  主滤波器形状: {Wc_main.shape}')
    print(f'  主滤波器 RMS: {np.sqrt(np.mean(Wc_main**2)):.6f}')

    # 绘制训练误差曲线
    plt.figure(figsize=(12, 5))
    plot_samples = min(5 * fs, Erro.shape[1])
    time_axis = np.arange(plot_samples) / fs
    for e in range(min(E, 3)):
        plt.plot(time_axis, Erro[e, :plot_samples], alpha=0.6,
                 linewidth=0.5, label=f'Error Mic {e}')
    plt.legend(fontsize=8)
    plt.title(f'MIMO FxNLMS Training Error (Broadband {f_low:.0f}-{f_high:.0f} Hz)')
    plt.ylabel('Error Amplitude')
    plt.xlabel('Time (seconds)')
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(os.path.join(FIGURE_DIR, 'training_error_broadband.png'), dpi=150)
    plt.close()

    # ── Step 4: sqrt-Hann DFT 分解 → 子滤波器全部落在有效区 ──
    if USE_LOG_SPACING:
        spacing_label = ('对数间距 '
                         f'({LOG_BAND_CENTERS_HZ[0]:.0f}~{LOG_BAND_CENTERS_HZ[-1]:.0f} Hz)')
        print(f'\n[Step 4/4] sqrt-Hann DFT 分解 ({spacing_label}): '
              f'1 个主滤波器 → {num_subfilters} 个子滤波器')
    else:
        spacing_label = f'均匀间距 ({f_low:.0f}-{f_high:.0f} Hz)'
        print(f'\n[Step 4/4] sqrt-Hann DFT 分解 ({spacing_label}): '
              f'1 个主滤波器 → {num_subfilters} 个子滤波器')
        print(f'  零填充 FFT: {Len_control} → {Len_control * 4} 点 '
              f'(频域分辨率 {fs / (Len_control * 4):.1f} Hz/bin)')
    Wc_matrix = np.zeros((num_subfilters, S, Len_control), dtype=np.float64)

    for s in range(S):
        if USE_LOG_SPACING:
            sub_filters_s = filter_decompose_log(
                Wc_main[s], band_centers_hz=LOG_BAND_CENTERS_HZ,
                band_edges_hz=LOG_BAND_EDGES_HZ, fs=fs)
        else:
            sub_filters_s = filter_decompose(
                Wc_main[s], num_subfilters=num_subfilters, fs=fs,
                f_low=f_low, f_high=f_high)
        Wc_matrix[:, s, :] = sub_filters_s

    # 验证：分解后的子滤波器之和应接近主滤波器 (幅度重构)
    reconstruction = np.sum(Wc_matrix, axis=0)  # (S, Len)
    recon_error = np.mean((reconstruction - Wc_main) ** 2)
    print(f'  重构误差 (MSE): {recon_error:.2e}')
    print(f'  子滤波器形状: {Wc_matrix.shape} '
          f'(C={num_subfilters}, S={S}, Len={Len_control})')


        # ── 打印设计频带表 ──
    if USE_LOG_SPACING:
        print(f'\n  对数子带设计频带表 (C={num_subfilters}, Q≈3.5):')
        print(f'  {"Band":<6} {"Center(Hz)":>10} {"Low(Hz)":>8} {"High(Hz)":>8} {"BW(Hz)":>8} {"Q":>6}')
        for c in range(num_subfilters):
            center = LOG_BAND_CENTERS_HZ[c]
            low = LOG_BAND_EDGES_HZ[c]
            high = LOG_BAND_EDGES_HZ[c+1]
            bw = high - low
            q = center / bw if bw > 0 else 0
            print(f'  Band{c:<3d} {center:10.1f} {low:8.1f} {high:8.1f} {bw:8.1f} {q:6.2f}')
            
    # 统计子滤波器增益与频带覆盖
    print(f'\n  子滤波器频带内平均增益:')
    nfft = 8192
    ffreqs = np.fft.rfftfreq(nfft, d=1.0/fs)
    for c in range(num_subfilters):
        h = Wc_matrix[c, 0, :]  # speaker 0
        mag = np.abs(np.fft.rfft(h, n=nfft))
        peak_idx = np.argmax(mag)
        thresh = mag[peak_idx] / 2.0
        above = mag >= thresh
        idx = np.where(above)[0]
        if len(idx) >= 2:
            lo, hi = ffreqs[idx[0]], ffreqs[idx[-1]]
        else:
            lo, hi = 0, 0
        band_mask = (ffreqs >= lo) & (ffreqs <= hi)
        avg_gain = 20.0 * np.log10(max(np.mean(mag[band_mask]), 1e-15))
        peak_gain = 20.0 * np.log10(max(np.max(mag[band_mask]), 1e-15))
        # 估算旁瓣抑制 (峰值 vs 带外最大)
        out_mask = np.ones_like(band_mask, dtype=bool)
        if len(idx) >= 2:
            margin = max(len(idx) // 4, 10)
            out_mask[idx[0] - margin:idx[-1] + margin] = False
        out_of_band = mag[out_mask]
        sidelobe_db = peak_gain - 20.0 * np.log10(max(np.max(out_of_band), 1e-15))
        print(f'    Band {c:2d}: {lo:.0f}-{hi:.0f} Hz, '
              f'peak={peak_gain:.1f} dB, avg={avg_gain:.1f} dB, '
              f'sidelobe_suppr={sidelobe_db:.1f} dB')

    # ── 保存 ──────────────────────────────────────────────────
    savemat(OUTPUT_FILE, {'Wc_v': Wc_matrix})
    print(f'\n模型已保存: {OUTPUT_FILE}')
    print(f'Wc_v 形状: {Wc_matrix.shape}')

    # 绘制子滤波器频响汇总
    fig, axes = plt.subplots(3, 5, figsize=(20, 10))
    axes = axes.flatten()
    for c in range(num_subfilters):
        ax = axes[c]
        for s in range(S):
            h = Wc_matrix[c, s, :]
            mag = np.abs(np.fft.rfft(h, n=nfft))
            mag_db = 20 * np.log10(np.maximum(mag, 1e-15))
            ax.plot(ffreqs, mag_db, linewidth=0.7, alpha=0.8,
                    label=f'Spk{s}' if c == 0 else None)
        ax.set_xlim(0, f_high * 1.2)  # 显示至截止频率以上
        ax.set_ylim(-60, 10)
        ax.set_title(f'Band {c}')
        ax.grid(True, alpha=0.3)
        ax.axhline(y=0, color='k', linestyle=':', alpha=0.3)
        if c == 0:
            ax.legend(fontsize=6)
    if USE_LOG_SPACING:
        fig_suptitle = ('MIMO Sub-filters '
                        '(Broadband FxNLMS + Log-Spaced sqrt-Hann DFT Decomposition)')
        fig_filename = 'subfilters_logspacing_summary.png'
    else:
        fig_suptitle = ('MIMO Sub-filters '
                        '(Broadband FxNLMS + sqrt-Hann DFT Decomposition)')
        fig_filename = 'subfilters_broadband_summary.png'
    fig.suptitle(fig_suptitle,
                 fontsize=14, fontweight='bold')
    plt.tight_layout()
    plt.savefig(os.path.join(FIGURE_DIR, fig_filename), dpi=150)
    plt.close()

    print(f'\n所有图片已保存: {FIGURE_DIR}')
    print('完成！')


if __name__ == '__main__':
    import torch
    main()
