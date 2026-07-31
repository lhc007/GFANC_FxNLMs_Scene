"""
指数正弦扫频 (Exponential Sine Sweep / Farina Method) 信号生成与反卷积.

业界标准方法: Angelo Farina, "Simultaneous measurement of impulse response and
distortion with a swept-sine technique", AES 2000.

原理:
  - 指数扫频信号在频域具有 -3 dB/oct 的幅度包络 (粉噪声谱), 能在各频段
    分配均衡的测量能量, 高频不过曝、低频不欠曝.
  - 反滤波器 = 时间反转 + 幅度补偿 (-6 dB/oct), 使得反卷积后谐波失真脉冲
    被压缩到因果脉冲之前, 实现线性 IR 与非线性失真的分离.

用法:
    from gfanc.measurement.sweep import generate_sweep, deconvolve_ir

    sweep, inv_filter = generate_sweep(f1=20, f2=7500, duration=5.0, fs=16000)
    # 播放 sweep, 同步录制得到 rec
    ir = deconvolve_ir(rec, inv_filter)
"""

import numpy as np
from scipy import signal


# ──────────────────────────────────────────────────────────────
# 1. 指数扫频信号生成
# ──────────────────────────────────────────────────────────────

def generate_sweep(
    f1: float = 20.0,
    f2: float = 7500.0,
    duration: float = 5.0,
    fs: int = 16000,
    fade_in_sec: float = 0.05,
    fade_out_sec: float = 0.02,
    prepend_silence_sec: float = 0.1,
    append_silence_sec: float = 0.5,
) -> tuple[np.ndarray, np.ndarray]:
    """
    生成指数正弦扫频信号及其反滤波器 (Farina 方法).

    参数:
        f1:                 起始频率 (Hz), 通常 20 Hz.
        f2:                 终止频率 (Hz), 应 < fs/2 (通常 7500 @ 16kHz).
        duration:           扫频持续时间 (s). 越长 SNR 越高, 5~10s 为典型值.
        fs:                 采样率 (Hz).
        fade_in_sec:        淡入时长 (s), 避免扬声器启动瞬态.
        fade_out_sec:       淡出时长 (s), 避免截止瞬态.
        prepend_silence_sec: 扫频前静音 (s), 用于录制环境底噪参考.
        append_silence_sec:  扫频后静音 (s), 用于捕获混响尾部.

    返回:
        (sweep_signal, inverse_filter):
          - sweep_signal:     (N,) float64 带淡入淡出的完整待播放信号.
          - inverse_filter:   (N_sweep,) float64 反滤波器, 用于 deconvolve_ir().

    参考文献:
        Farina, A. (2000). "Simultaneous measurement of impulse response and
        distortion with a swept-sine technique." AES 108th Convention.
    """
    # ── 扫频主体 ──
    N_sweep = int(duration * fs)
    t = np.arange(N_sweep) / fs                     # 时间轴 (s)

    # 指数扫频瞬时频率: f(t) = f1 * exp(t/T * ln(f2/f1))
    # 相位 = ∫ 2π f(t) dt
    omega1 = 2.0 * np.pi * f1
    omega2 = 2.0 * np.pi * f2
    rate = np.log(omega2 / omega1) / duration       # ω(t) = ω1 * exp(rate * t)

    phase = omega1 * (np.exp(rate * t) - 1.0) / rate
    sweep_raw = np.sin(phase)

    # ── 反滤波器 (时间反转 + -6 dB/oct 幅度补偿) ──
    # Farina 2000: x_inv(t) = x(T-t) * exp(-t / L), 其中 L = T / ln(f2/f1).
    # 时间反转后, 瞬时频率从高到低; 指数衰减包络补偿扫频的粉噪谱,
    # 使反卷积后的有效脉冲达到 ~-45 dB 旁瓣水平.
    L_sweep = duration / np.log(f2 / f1)            # 特征时间常数
    amp_env_rev = np.exp(-t / L_sweep)              # 衰减包络 (应用于反转信号)
    inverse_filter = sweep_raw[::-1] * amp_env_rev

    # ── 淡入淡出 ──
    fade_in_samples = int(fade_in_sec * fs)
    fade_out_samples = int(fade_out_sec * fs)

    window_in = np.linspace(0, 1, fade_in_samples) if fade_in_samples > 0 else np.array([])
    window_out = np.linspace(1, 0, fade_out_samples) if fade_out_samples > 0 else np.array([])

    # 合并: window_in 在前面 + 全幅度中间段 + window_out 在尾部
    sweep_faded = np.concatenate([
        sweep_raw[:fade_in_samples] * window_in,
        sweep_raw[fade_in_samples:N_sweep - fade_out_samples],
        sweep_raw[N_sweep - fade_out_samples:] * window_out,
    ])

    # ── 前后加静音 ──
    prepend = np.zeros(int(prepend_silence_sec * fs), dtype=np.float64)
    append = np.zeros(int(append_silence_sec * fs), dtype=np.float64)
    sweep_signal = np.concatenate([prepend, sweep_faded, append])

    return sweep_signal, inverse_filter


# ──────────────────────────────────────────────────────────────
# 2. 反卷积提取脉冲响应
# ──────────────────────────────────────────────────────────────

def deconvolve_ir(
    recorded: np.ndarray,
    inverse_filter: np.ndarray,
    ir_length: int = 1024,
) -> np.ndarray:
    """
    用反滤波器从录制信号中提取线性脉冲响应 (多通道兼容).

    参数:
        recorded:       录制信号.
                        - 单通道: (N,) 或 (1, N)
                        - 多通道: (Ch, N)
        inverse_filter: generate_sweep() 返回的反滤波器 (M,).
        ir_length:      期望的 IR 长度 (样本数), 默认 1024.

    返回:
        ir: 提取的脉冲响应.
            - 单通道: (ir_length,)
            - 多通道: (Ch, ir_length)

    原理:
        Farina 方法的核心:
          - 录制信号 r[n] = h[n] * x[n] (真实 IR 与扫频的线性卷积)
          - 反滤波器 x_inv[n] 是时间反转 + -6dB/oct 幅度补偿的扫频信号
          - 反卷积: h_est[n] = r[n] * x_inv[n] (线性卷积)
          - 由于 x[n] * x_inv[n] ≈ δ[n - M] (M = 扫频长度),
            h_est[n] ≈ h[n - M], 即 IR 出现在偏移 M 处
          - 谐波失真脉冲在反卷积中出现在因果脉冲之前 (反因果区域),
            因为失真分量产生于扫频的高频段, 经过反滤波后被压缩到负时间
    """
    recorded = np.atleast_2d(recorded).astype(np.float64)  # (1, N) or (Ch, N)
    Ch, N = recorded.shape
    M = len(inverse_filter)

    # 频域线性卷积: IFFT(FFT(rec) * FFT(x_inv))
    # 注意: inverse_filter 已经是时间反转且幅度补偿过的, 不再需要 conj
    fft_len = next_power_of_two(N + M - 1)
    INV_F = np.fft.rfft(inverse_filter, n=fft_len)

    irs = np.zeros((Ch, ir_length), dtype=np.float64)
    for ch in range(Ch):
        REC_F = np.fft.rfft(recorded[ch], n=fft_len)
        conv = np.fft.irfft(REC_F * INV_F, n=fft_len)

        # 线性 IR 预期出现在偏移量 ≈ M (扫频长度) 处.
        # 收窄搜索范围到 M ± window, 排除反因果区的谐波失真脉冲
        # (Farina 方法中失真出现在因果峰之前, 严重削波时失真峰可能超过真实 IR 峰).
        search_window = max(ir_length * 4, M // 10)
        search_start = max(0, M - search_window)
        search_end = min(len(conv), M + search_window)
        if search_end <= search_start:
            search_start, search_end = 0, len(conv)  # fallback
        # Hilbert 包络替代裸样本 abs: 包络峰值是 IR 主瓣中心,
        # 不会在相邻振荡峰之间跳动 → 重复性稳定
        search_region = np.abs(signal.hilbert(conv[search_start:search_end]))
        if len(search_region) > 0:
            peak_idx = search_start + int(np.argmax(search_region))
        else:
            peak_idx = int(np.argmax(np.abs(signal.hilbert(conv))))  # fallback

        # 从峰值前若干样本开始提取, 保留直达声之前的零点.
        # 偏移量需考虑降采样: 44100→16000 时 30 样本 → ~11 样本,
        # 确保即使在目标采样率下峰值也不紧贴窗口开头.
        pre_peak_margin = 30
        ir_start = max(0, peak_idx - pre_peak_margin)
        ir_end = min(len(conv), ir_start + ir_length)
        segment = conv[ir_start:ir_end]

        irs[ch, :len(segment)] = segment

    if Ch == 1:
        return irs[0]
    return irs


# ──────────────────────────────────────────────────────────────
# 3. 反卷积提取脉冲响应 (双通道法: 精确对齐)
# ──────────────────────────────────────────────────────────────

def deconvolve_ir_aligned(
    recorded: np.ndarray,
    sweep_signal: np.ndarray,
    inverse_filter: np.ndarray,
    ir_length: int = 1024,
    fs: int = 16000,
) -> np.ndarray:
    """
    精确对齐版反卷积: 通过互相关找到录制信号中扫频的起始位置,
    然后从该位置开始反卷积, 避免静音/延迟导致的 IR 偏移.

    这是推荐使用的提取函数, 尤其当音频接口延迟不确定时.

    参数:
        recorded:       录制信号 (Ch, N) 或 (N,).
        sweep_signal:   完整的播放扫频信号 (含静音前后缀).
        inverse_filter: 反滤波器.
        ir_length:      IR 目标长度.
        fs:             采样率.

    返回:
        ir: (Ch, ir_length) 或 (ir_length,).
    """
    recorded = np.atleast_2d(recorded).astype(np.float64)
    Ch, N_rec = recorded.shape

    # 用完整扫频信号做互相关找延迟 (含静音前后缀的 sweep 直接匹配)
    # FFT 方法: O(N log N), 48kHz 5s 信号 (~240k 样本) 可秒级完成
    try:
        import scipy.signal as _scipy_signal
        xcorr = _scipy_signal.correlate(recorded[0], sweep_signal, mode='full',
                                        method='fft')
    except ImportError:
        xcorr = np.correlate(recorded[0], sweep_signal, mode='full')
    delay = np.argmax(np.abs(xcorr)) - (len(sweep_signal) - 1)
    delay = max(0, delay)

    # 从 delay 开始裁剪录制信号, 保留足够长度用于反卷积
    # 需要: 反滤波器长度 + IR 长度 + 一些余量
    needed_len = len(inverse_filter) + ir_length
    available = N_rec - delay
    if available < needed_len:
        # 录制不够长, 在末尾补零
        extract = np.zeros((Ch, needed_len), dtype=np.float64)
        copy_len = min(available, needed_len)
        extract[:, :copy_len] = recorded[:, delay:delay + copy_len]
    else:
        extract = recorded[:, delay:delay + needed_len]

    # 直接用反卷积提取 IR
    return deconvolve_ir(extract, inverse_filter, ir_length=ir_length)


# ──────────────────────────────────────────────────────────────
# 4. 辅助函数
# ──────────────────────────────────────────────────────────────

def _trim_silence(signal: np.ndarray, threshold_db: float = -40) -> np.ndarray:
    """裁掉信号两端的静音部分 (低于阈值的区域)."""
    abs_sig = np.abs(signal)
    threshold = 10.0 ** (threshold_db / 20.0) * abs_sig.max()
    active = np.where(abs_sig > threshold)[0]
    if len(active) == 0:
        return signal  # 全部静音, 不裁剪
    return signal[active[0]:active[-1] + 1]


def next_power_of_two(n: int) -> int:
    """返回 >= n 的最小 2 的幂 (FFT 友好长度)."""
    return 1 << (n - 1).bit_length()


def measure_snr_from_ir(
    ir: np.ndarray,
    peak_region_samples: int = 100,
    noise_region_start: int = 900,
) -> dict:
    """
    从测量到的 IR 估算信噪比 (ISO 18233 峰前噪声法).

    噪声优先使用因果峰前的短窗口 — Farina 反卷积将谐波失真脉冲
    压缩到远在因果峰之前 (例如 2 次谐波在峰前 ~1.17s), 峰前
    数十样本为干净噪声区, 且不受 IR 截断影响.
    若峰前样本不足 (峰值在 IR 起始附近), 回退到 IR 尾部估计.

    参数:
        ir:                    脉冲响应 (单通道).
        peak_region_samples:   信号区域 (峰值附近) 的样本数.
        noise_region_start:    回退模式下的噪声区域起始样本 (IR 尾部).

    返回:
        dict with keys: 'snr_db', 'peak_db', 'noise_floor_db'.
    """
    ir_len = ir.shape[-1]
    peak_idx = np.argmax(np.abs(ir))
    sig_start = max(0, peak_idx - peak_region_samples // 2)
    sig_end = min(ir_len, peak_idx + peak_region_samples // 2)
    signal_energy = np.mean(ir[sig_start:sig_end] ** 2)

    # ── 噪声估计: 优先峰前区域 (Farina 失真已压缩到远前区) ──
    pre_guard = max(10, peak_region_samples // 10)  # 距峰的安全间距
    pre_avail = peak_idx - pre_guard               # 峰前可用样本数
    if pre_avail >= 20:
        # 峰前窗口: 取距峰 10~60 样本处, 远离失真脉冲区
        noise_start = max(0, pre_avail - 50)
        noise_end = pre_avail
    else:
        # 回退: IR 尾部 (最后 1/8), 动态起始避免与信号区重叠
        noise_start = max(ir_len * 7 // 8, sig_end)
        noise_end = ir_len

    if noise_end > noise_start:
        noise_energy = np.mean(ir[noise_start:noise_end] ** 2)
    else:
        noise_energy = 1e-12

    snr_db = 10.0 * np.log10(max(float(signal_energy), 1e-12) / max(float(noise_energy), 1e-12))
    peak_db = 20.0 * np.log10(float(np.max(np.abs(ir))) + 1e-12)
    noise_floor_db = 10.0 * np.log10(max(float(noise_energy), 1e-12))

    return {'snr_db': round(snr_db, 1), 'peak_db': round(peak_db, 1),
            'noise_floor_db': round(noise_floor_db, 1)}


def measure_coherence(
    ir1: np.ndarray,
    ir2: np.ndarray,
    fs: int = 16000,
    freq_range: tuple = (20, 4000),
    n_fft: int = 512,
) -> dict:
    """
    计算两次重复测量的频域 Magnitude-Squared Coherence (MSC).

    工业标准: γ²(f) > 0.95 在所有 ANC 有效频段内.
    MSC 衡量两次独立测量的频域线性相关性, 是验证系统 LTI 假设的
    标准方法 (优于时域互相关系数).

    参数:
        ir1, ir2:   两次独立测量的同通道 IR, 形状 (L,) 或可广播.
        fs:         采样率 (Hz).
        freq_range: 关注的频段 (Hz), 默认 ANC 有效频带.
        n_fft:      FFT 点数.

    返回:
        dict: {
            'mean_coherence': 关注频段内的平均 MSC,
            'min_coherence':  关注频段内的最小 MSC,
            'freqs':          频率轴 (Hz),
            'msc':            MSC 频谱 (n_freqs,),
            'pass':           MSC_min > 0.95,
        }
    """
    L = min(len(ir1), len(ir2))
    ir1 = np.atleast_1d(ir1[:L]).astype(np.float64)
    ir2 = np.atleast_1d(ir2[:L]).astype(np.float64)

    # 计算互功率谱和自功率谱 (Welch 方法)
    from scipy import signal as scipy_signal
    f, Pxx = scipy_signal.welch(ir1, fs=fs, nperseg=min(n_fft, L//2, 256), return_onesided=True)
    _, Pyy = scipy_signal.welch(ir2, fs=fs, nperseg=min(n_fft, L//2, 256), return_onesided=True)
    _, Pxy = scipy_signal.csd(ir1, ir2, fs=fs, nperseg=min(n_fft, L//2, 256), return_onesided=True)

    # MSC = |Pxy|^2 / (Pxx * Pyy)
    denom = Pxx * Pyy
    msc = np.zeros_like(Pxx)
    valid = denom > 1e-20
    msc[valid] = np.abs(Pxy[valid]) ** 2 / denom[valid]

    # 关注频段
    f1, f2 = freq_range
    mask = (f >= f1) & (f <= f2)
    band_msc = msc[mask]

    if len(band_msc) == 0:
        return {'mean_coherence': 0.0, 'min_coherence': 0.0,
                'freqs': f, 'msc': msc, 'pass': False}

    mean_msc = float(np.mean(band_msc))
    min_msc = float(np.min(band_msc))
    passed = bool(min_msc > 0.95)

    return {
        'mean_coherence': round(mean_msc, 4),
        'min_coherence': round(min_msc, 4),
        'freqs': f,
        'msc': msc,
        'pass': passed,
    }


def check_causality(
    ir: np.ndarray,
    onset_threshold_db: float = -20.0,
) -> dict:
    """
    检验脉冲响应的因果性 (Causality Check) — 基于包络起始点检测.

    理想 IR 在直达声到达之前应为零。实际测量中, 反卷积可能产生
    微小的非因果伪影。

    方法:
      1. 计算 Hilbert 包络, 找到包络峰值位置
      2. 从峰值向前扫描, 找到包络首次低于阈值的采样点 (IR 起始点)
      3. 起始点之前的能量 = 非因果能量, 应 < 总能量的 5%

    这与 REW/ARTA 的 IR 窗口前能量检验等价, 但使用包络起始点
    而非绝对采样峰值作为参考 — 避免将声学 IR 的正常上升沿
    (波前到达后的第一个半周期) 误判为"非因果能量".

    参数:
        ir:                   脉冲响应 (单通道).
        onset_threshold_db:   起始点检测阈值 (dB, 相对于包络峰值).

    返回:
        dict: {
            'peak_idx':             包络峰值位置,
            'onset_idx':            IR 包络起始点,
            'pre_peak_energy_pct':  起始点之前的能量占比 (%),
            'is_causal':            是否因果 (非因果能量 < 5%),
            'peak_at_start':        峰值是否在信号开头 (异常),
        }
    """
    from scipy import signal as scipy_signal

    ir = np.atleast_1d(ir).astype(np.float64)
    N = len(ir)

    # ── Hilbert 包络 ──
    envelope = np.abs(scipy_signal.hilbert(ir))

    # ── 包络峰值 ──
    env_peak_idx = int(np.argmax(envelope))
    env_peak = envelope[env_peak_idx]

    # ── 起始点检测: 从峰值向前扫描, 找包络首次低于阈值的点 ──
    threshold = env_peak * (10.0 ** (onset_threshold_db / 20.0))
    onset_idx = 0
    for i in range(env_peak_idx, -1, -1):
        if envelope[i] < threshold:
            onset_idx = i + 1  # 起始点是阈值之上的第一个点
            break

    # ── 非因果能量 = 起始点之前的能量 ──
    total_energy = np.sum(ir ** 2)
    if onset_idx > 0:
        pre_onset_energy = np.sum(ir[:onset_idx] ** 2)
    else:
        pre_onset_energy = 0.0

    pre_pct = float(100.0 * pre_onset_energy / max(total_energy, 1e-20))
    is_causal = bool(pre_pct < 5.0)       # 业内通用阈值 (REW/ARTA)
    peak_at_start = bool(env_peak_idx < 10)

    return {
        'peak_idx': int(env_peak_idx),
        'onset_idx': int(onset_idx),
        'pre_peak_energy_pct': round(pre_pct, 2),
        'is_causal': is_causal,
        'peak_at_start': peak_at_start,
    }


def check_phase_linearity(
    ir: np.ndarray,
    fs: int = 16000,
    freq_range: tuple = (100, 1000),
    phase_rmse_max_rad: float = 1.0,
    n_fft: int = 2048,
) -> dict:
    """
    检验 IR 在 ANC 通带内的相位线性度 (Phase Linearity Check).

    非线性相位会导致频率相关的群延迟, 降低 ANC 性能。
    工程标准: FxLMS 容忍约 90° (1.57 rad) 的相位估计误差,
    通带内相位 RMSE < 1.0 rad (~57°) 为可接受, < 0.5 rad (~29°) 为优质.

    方法:
      1. 计算 IR 的频率响应 H(f)
      2. 在通带内做线性拟合 unwrap(angle(H)) → 直线斜率 = 平均群延迟
      3. 计算实际相位与理想线性相位的残差 RMSE
      4. RMSE < threshold → 相位足够线性, ANC 可用

    参数:
        ir:                   脉冲响应 (单通道).
        fs:                   采样率 (Hz).
        freq_range:           ANC 通带 (Hz), 默认 100-1000.
        phase_rmse_max_rad:  相位 RMSE 上限 (rad), 默认 1.0.
        n_fft:                FFT 点数.

    返回:
        dict: {
            'group_delay_mean_ms':  平均群延迟,
            'group_delay_std_ms':   群延迟标准差 (仅供参考, 短 IR 噪声敏感),
            'phase_rmse_rad':       相位拟合 RMSE (主要判据),
            'is_linear':            是否线性 (RMSE < threshold),
        }
    """
    H = np.fft.rfft(ir, n=n_fft)
    freqs = np.fft.rfftfreq(n_fft, d=1.0/fs)
    phase = np.unwrap(np.angle(H))

    f1, f2 = freq_range
    mask = (freqs >= f1) & (freqs <= f2)
    f_band = freqs[mask]
    phase_band = phase[mask]

    if len(f_band) < 10:
        return {'group_delay_mean_ms': 0.0, 'group_delay_std_ms': 0.0,
                'phase_rmse_rad': 0.0, 'is_linear': True}

    # 线性拟合: phase = a * f + b
    # a = -2π * τ_g  (群延迟)
    A = np.column_stack([f_band, np.ones_like(f_band)])
    slope, intercept = np.linalg.lstsq(A, phase_band, rcond=None)[0]
    phase_linear = slope * f_band + intercept

    # 残差 RMSE — 主要判据, 对短 IR 噪声鲁棒
    phase_residual = phase_band - phase_linear
    phase_rmse = float(np.sqrt(np.mean(phase_residual ** 2)))

    # 群延迟 (仅供参考)
    group_delay_s = -slope / (2.0 * np.pi)
    group_delay_ms = group_delay_s * 1000.0

    # 群延迟标准差 — 对短 IR 噪声敏感, 仅作参考
    group_delays = -np.diff(phase_band) / (2.0 * np.pi * np.diff(f_band))
    gd_std_ms = float(np.std(group_delays)) * 1000.0

    is_linear = bool(phase_rmse < phase_rmse_max_rad)

    return {
        'group_delay_mean_ms': round(group_delay_ms, 3),
        'group_delay_std_ms': round(gd_std_ms, 3),
        'phase_rmse_rad': round(phase_rmse, 4),
        'is_linear': is_linear,
    }


def sound_speed(temp_celsius: float = 20.0) -> float:
    """计算当前温度下的声速 (m/s). c = 331.3 + 0.606 * T (°C)."""
    return 331.3 + 0.606 * temp_celsius

def list_audio_devices(hostapi_filter: str = "ASIO",
                       include_multichannel: bool = False) -> dict:
    """列出可用的音频设备, 返回 {device_id: info_dict}.

    参数:
        hostapi_filter:      仅显示包含此字符串的 host API 设备.
                             设为 None 或 "" 显示全部.
                             默认 "ASIO" (ASIO4ALL 独占模式, 绕过 Windows 音频管线).
                             也可以是逗号分隔的多个 Host API 名称.
        include_multichannel: 如果为 True, 除了 hostapi_filter 匹配的设备外,
                             还额外包含其他 Host API 下输入通道 > 2 的设备.
                             用于发现 WASAPI 共享模式无法完整呈现的多通道麦克风.
    """
    try:
        import sounddevice as sd
    except ImportError:
        raise ImportError("需要 sounddevice 库. 请执行: pip install sounddevice")

    devices = {}
    hostapis = {i: sd.query_hostapis(i)['name'] for i in range(len(sd.query_hostapis()))}

    # 解析 hostapi_filter: 支持逗号分隔的多个 API
    if hostapi_filter:
        api_filters = [f.strip().lower() for f in hostapi_filter.split(',')]
    else:
        api_filters = []

    for i, dev in enumerate(sd.query_devices()):
        api_name = hostapis.get(dev['hostapi'], 'Unknown')

        included = False
        if not api_filters:
            included = True  # 无过滤 = 全部
        else:
            for f in api_filters:
                if f in api_name.lower():
                    included = True
                    break

        # 额外包含: 其他 Host API 下的多通道输入设备
        if not included and include_multichannel and dev['max_input_channels'] > 2:
            included = True

        if not included:
            continue

        # Host API 缩写 (用于紧凑显示)
        api_short = _hostapi_short_name(api_name)

        devices[i] = {
            'name': dev['name'],
            'max_input_channels': dev['max_input_channels'],
            'max_output_channels': dev['max_output_channels'],
            'default_samplerate': dev['default_samplerate'],
            'hostapi': api_name,
            'hostapi_short': api_short,
        }
    return devices


def _hostapi_short_name(api_name: str) -> str:
    """Host API 全名 → 缩写."""
    mapping = {
        'MME': 'MME',
        'Windows DirectSound': 'DS',
        'Windows WASAPI': 'WASAPI',
        'Windows WDM-KS': 'WDM-KS',
        'ASIO': 'ASIO',
    }
    for full, short in mapping.items():
        if full in api_name:
            return short
    return api_name[:6]  # fallback: 截取前6字符


def print_device_list(hostapi_filter: str = "ASIO",
                      include_multichannel: bool = False):
    """友好格式打印音频设备列表.

    默认显示 ASIO 设备 (ASIO4ALL 独占模式, 绕过 Windows 音频管线).
    使用 --all-devices 或传入 hostapi_filter=None 显示全部.

    参数:
        hostapi_filter:      过滤 Host API, None 显示全部.
                             逗号分隔支持多个, 如 "ASIO,WASAPI".
        include_multichannel: 额外包含其他 Host API 下输入通道 > 2 的设备.
    """
    try:
        devices = list_audio_devices(
            hostapi_filter=hostapi_filter,
            include_multichannel=include_multichannel,
        )
    except ImportError as e:
        print(f"错误: {e}")
        return

    try:
        import sounddevice as sd
        default_in = sd.default.device[0]
        default_out = sd.default.device[1]
    except Exception:
        default_in = default_out = None

    n_total = len(sd.query_devices())
    label_parts = []
    if hostapi_filter:
        label_parts.append(f"Host API: {hostapi_filter}")
    if include_multichannel:
        label_parts.append("+ 其他API多通道输入设备")
    label = ", ".join(label_parts) if label_parts else "全部 Host API"

    print("=" * 80)
    print(f"  {label}  ({len(devices)} 设备)")
    print("=" * 80)
    print(f"{'ID':<4} {'API':<8} {'IN':<5} {'OUT':<5} {'SR(Hz)':<10} Name")
    print("-" * 80)
    for idx, dev in devices.items():
        in_ch = dev['max_input_channels']
        out_ch = dev['max_output_channels']
        sr = dev['default_samplerate']
        api = dev.get('hostapi_short', dev.get('hostapi', '?'))
        marker = ""
        if idx == default_in:
            marker += "[输入] "
        if idx == default_out:
            marker += "[输出] "
        print(f"{idx:<4} {api:<8} {in_ch:<5} {out_ch:<5} {sr:<10.0f} {marker}{dev['name']}")
    if hostapi_filter and not include_multichannel:
        print("-" * 80)
        print(f"  提示: 仅显示 {hostapi_filter} 设备. 使用 --all-devices 查看所有 {n_total} 个设备.")
        print(f"  多通道设备可能在其他 Host API 下, 使用 --interactive 自动包含.")
    print("=" * 80)
