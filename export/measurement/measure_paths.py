"""
声学路径测量工具 - 指数扫频法 (Farina Method).

用于实测次级路径 (SPK -> Error Mic) 和主路径 (噪声源 -> Error Mic / Ref Mic).

硬件接入前准备:
  1. print_device_list() 确认音频接口的通道布局
  2. 在 MeasureConfig 中配置 input_device / output_device / channel 映射
  3. calibrate_latency() 测量往返延迟 (物理环回)
  4. check_channels() 快速验证所有通道连通
  5. measure_secondary_path() / measure_primary_path() 执行测量
  6. validate_measurement_quality() 检查 SNR / 削波 / 互相关一致性

测量标准 (Measurement Standards)
=================================
方法:     指数正弦扫频 (Exponential Sine Sweep, Farina 2000 AES)
采样率:   16000 Hz (与 ANC 系统一致, 避免重采样误差)
扫频范围: 20 ~ 7500 Hz (覆盖 ANC 有效频带, 避开奈奎斯特频率)
扫频时长: ≥ 5 秒 (确保各频段 SNR ≥ 40 dB)
重复次数: ≥ 2 次 (时域平均降噪, 每次独立播放+录制)
播放幅度: 0.5 ~ 0.8 (确保声压足够又不削波, 需用 check_channels 验证)
环境要求: 低背景噪声 (< 40 dBA), 测量期间避免走动/关门/讲话
温度/湿度: 记录环境条件, 后续测量条件应一致
输出格式: float32 .npy, 形状 (E, S, L) 或 (K, I, L)

验收标准:
  - 单通道 SNR ≥ 35 dB (优质), ≥ 25 dB (可接受), < 20 dB (拒绝)
  - 无削波 (峰值 < 0.95 满量程)
  - 重复测量互相关 ≥ 0.98 (验证时不变性)
  - 所有通道连通性确认通过

扩展性设计:
  - _play_and_record() 是唯一接触硬件的入口点, 替换/重载此方法即可切换音频后端.
  - MeasureConfig 集中管理所有可配置参数, 支持 per-channel gain 和多种设备拓扑.

自定义硬件后端:
    class MyHardwareSession(MeasureSession):
        def _play_and_record(self, sweep_data, output_channels, input_channels, amplitude):
            # 用你自己的硬件 API 替代 sounddevice
            ...  # 返回 (num_input_channels, N) float64
"""

import os
import json
import time
import numpy as np
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional, Sequence, Dict, List, Tuple
from scipy.io import wavfile
from scipy import signal as _scipy_signal

from .sweep import (
    generate_sweep,
    deconvolve_ir,
    deconvolve_ir_aligned,
    measure_snr_from_ir,
    measure_coherence,
    check_causality,
    check_phase_linearity,
    sound_speed,
    list_audio_devices,
    print_device_list,
)


CALIBRATION_FILE = "measurement_calibration.json"
METADATA_FILE = "measurement_metadata.json"


@dataclass
class MeasureConfig:
    """测量会话配置.

    参数:
        fs:                     测量采样率 (Hz). 必须匹配硬件能力.
                                 默认 16000, 但如果设备仅支持 48000 则设为 48000.
        target_fs:              ANC 系统目标采样率 (Hz). 如果 != fs, 测量后 IR
                                 会自动降采样到 target_fs. 默认 16000.
        f1:                     扫频起始频率 (Hz). 通常 20 Hz.
        f2:                     扫频终止频率 (Hz). 应 < target_fs/2, 通常 7500 @ 16kHz.
        sweep_duration:         扫频持续时间 (s). ≥5s 推荐, 更长 = 更高 SNR.
        sweep_amplitude:        播放幅度 (0~1). 0.5~0.8 推荐, 需避免削波.
        ir_length:              IR 目标长度 (样本数, 在 target_fs 下).
                                 实际测量时在 fs 下使用更长 IR, 降采样后裁切到此长度.
        repetitions:            重复测量次数 (用于时域平均降噪).
        input_device:           录音设备 ID (None = 系统默认).
        output_device:          默认播放设备 ID (None = 系统默认).
        output_device_map:      {物理通道号: 设备 ID} — 当扬声器跨多个声卡时使用.
                                例: {0: 2, 1: 2, 2: 3, 3: 3} 表示 ch0-1 用设备2, ch2-3 用设备3.
        latency_calibration_samples: 系统延迟补偿 (样本数, 在 target_fs 下),
                                      由 calibrate_latency() 测量并自动转换.
        output_gains:           {通道号: 增益} — 逐扬声器增益补偿.
        input_gains:            {通道号: 增益} — 逐麦克风增益补偿 (仅用于信息记录).
        save_dir:               保存目录.
    """

    fs: int = 16000
    target_fs: int = 16000
    f1: float = 20.0
    f2: float = 7500.0
    sweep_duration: float = 5.0
    sweep_amplitude: float = 0.7
    ir_length: int = 1024
    repetitions: int = 2

    input_device: Optional[int] = None
    output_device: Optional[int] = None
    output_device_map: Dict[int, int] = field(default_factory=dict)
    latency_calibration_samples: int = 0

    # Per-channel gain: {channel_index: gain}
    output_gains: Dict[int, float] = field(default_factory=dict)
    input_gains: Dict[int, float] = field(default_factory=dict)

    save_dir: str = "Primary and Secondary Path"
    force_exclusive: bool = False  # 强制 WASAPI 独占模式

    def __post_init__(self):
        Path(self.save_dir).mkdir(parents=True, exist_ok=True)
        if self.target_fs > self.fs:
            raise ValueError(
                f"target_fs ({self.target_fs}) 不能大于 fs ({self.fs}). "
                f"测量采样率必须 ≥ 目标采样率."
            )

    @property
    def rate_ratio(self) -> float:
        """fs / target_fs 降采样比例."""
        return self.fs / self.target_fs

    @property
    def needs_resample(self) -> bool:
        """是否需要降采样."""
        return self.fs != self.target_fs

    @property
    def meas_ir_length(self) -> int:
        """测量时的 IR 长度 (在 fs 下), 保证降采样后 ≥ ir_length."""
        return int(self.ir_length * self.rate_ratio)

    def get_output_gain(self, channel: int) -> float:
        return self.output_gains.get(channel, 1.0)

    def get_input_gain(self, channel: int) -> float:
        return self.input_gains.get(channel, 1.0)

    def get_output_device_for_channel(self, channel: int) -> Optional[int]:
        """返回指定扬声器通道的物理设备 ID. 如果未映射则返回默认 output_device."""
        return self.output_device_map.get(channel, self.output_device)

    def get_device_channel_for_spk(self, spk_channel: int) -> int:
        """将全局扬声器通道号映射到设备端物理通道号.

        当扬声器分布在多个声卡上时 (output_device_map), 每个设备的通道
        从 0 开始独立编号:

           output_device_map = {0: 19, 1: 19, 2: 20, 3: 20}
           → SPK 0 → device 19 ch0
           → SPK 1 → device 19 ch1
           → SPK 2 → device 20 ch0    ← 重新编号!
           → SPK 3 → device 20 ch1

        如果未配置 output_device_map, 直接返回 spk_channel (恒等映射).
        """
        if not self.output_device_map:
            return spk_channel
        target_dev = self.get_output_device_for_channel(spk_channel)
        if target_dev is None or target_dev == self.output_device:
            return spk_channel
        # 统计映射到同一设备且编号更小的扬声器, 得到设备端通道号
        same_dev = sorted(
            ch for ch, dev in self.output_device_map.items()
            if dev == target_dev
        )
        return same_dev.index(spk_channel)


# ================================================================
# 内部辅助: 输出设备验证
# ================================================================

def _validate_output_device(sd, device_id: int):
    """验证 device_id 是否为有效的输出设备, 不是则抛出 ValueError."""
    info = sd.query_devices(device_id)
    if info['max_output_channels'] < 1:
        raise ValueError(
            f"设备 {device_id} ('{info['name']}') 不是输出设备 "
            f"(max_output_channels={info['max_output_channels']}). "
            f"请检查配置中的 output_device 或 output_device_map."
        )


def _ensure_output_device(sd, device_id):
    """确保 device_id 是有效的输出设备; 如果不是, 查找第一个可用输出设备."""
    try:
        _validate_output_device(sd, device_id)
        return device_id
    except Exception:
        for i, dev in enumerate(sd.query_devices()):
            if dev['max_output_channels'] > 0:
                return i
        raise RuntimeError("未找到任何可用的音频输出设备.")


def _find_device_by_name_api(sd, name: str, api_substring: str,
                             min_channels: int = 1) -> Optional[int]:
    """在指定 Host API 下查找同名设备, 返回设备 ID 或 None.

    用于 WASAPI 不支持多通道时自动回退到 MME/DirectSound.
    """
    hostapis = {i: sd.query_hostapis(i)['name'] for i in range(len(sd.query_hostapis()))}
    for i, dev in enumerate(sd.query_devices()):
        if dev['name'] != name:
            continue
        api_name = hostapis.get(dev['hostapi'], '')
        if api_substring.lower() in api_name.lower():
            if dev['max_input_channels'] >= min_channels:
                return i
    return None


def resample_ir(ir: np.ndarray, orig_fs: int, target_fs: int) -> np.ndarray:
    """将 IR 从 orig_fs 降采样到 target_fs (支持非整数倍).

    参数:
        ir:        (L,) 或 (..., L) 脉冲响应.
        orig_fs:   原始采样率.
        target_fs: 目标采样率 (必须 ≤ orig_fs).

    返回:
        降采样后的 IR, 保持输入维度.
    """
    if orig_fs == target_fs:
        return ir.copy()
    if orig_fs < target_fs:
        raise ValueError(f"orig_fs ({orig_fs}) < target_fs ({target_fs}), 不支持升采样.")
    from scipy.signal import resample_poly
    return resample_poly(ir, target_fs, orig_fs, axis=-1)


def _corr_bar(value: float, width: int = 20) -> str:
    """归一化互相关可视化进度条. value ∈ [0, 1]."""
    value = max(0.0, min(1.0, value))
    level = int(value * width)
    fill = "=" * max(level - 1, 0) + (">" if level > 0 else "")
    return "[" + fill.ljust(width) + "]"


class MeasureSession:
    """
    声学路径测量会话.

    所有硬件交互都通过 _play_and_record() 这一个方法.
    要支持自定义硬件, 继承 MeasureSession 并重载该方法即可.
    """

    def __init__(self, config=None, **kwargs):
        if config is None:
            config = MeasureConfig(**kwargs)
        elif kwargs:
            for k, v in kwargs.items():
                if hasattr(config, k):
                    setattr(config, k, v)
        self.cfg = config

        self._sd = None
        self._try_load_sounddevice()

        self._sweep_signal, self._inv_filter = generate_sweep(
            f1=config.f1, f2=config.f2,
            duration=config.sweep_duration, fs=config.fs,
        )
        self._total_frames = len(self._sweep_signal)

    def _try_load_sounddevice(self):
        try:
            import sounddevice as sd
            self._sd = sd
        except ImportError:
            pass

    def _require_sd(self):
        if self._sd is None:
            raise RuntimeError(
                "需要 sounddevice 库. pip install sounddevice\n"
                "如使用自定义硬件, 继承 MeasureSession 并重载 _play_and_record()."
            )

    # ================================================================
    # 延迟校准 (带持久化)
    # ================================================================

    def calibrate_latency(self, loopback_channel_out=0, loopback_channel_in=0,
                          force=False):
        """测量音频接口的往返延迟.

        要求: 用物理环回线连接指定扬声器输出到指定麦克风输入.
        结果自动保存到 {save_dir}/measurement_calibration.json, 后续测量自动加载.

        参数:
            loopback_channel_out: 用于环回的扬声器通道号.
            loopback_channel_in:  用于环回的麦克风通道号.
            force:                强制重新测量 (忽略已保存的校准值).

        返回:
            latency_calibration_samples: 往返延迟 (样本数).
        """
        cal_path = os.path.join(self.cfg.save_dir, CALIBRATION_FILE)

        if not force and os.path.exists(cal_path):
            try:
                with open(cal_path, 'r') as f:
                    saved = json.load(f)
                # ── 设备变更检测: 输入/输出设备或采样率变了 → 校准失效 ──
                cal_in = saved.get('input_device')
                cal_out = saved.get('output_device')
                cal_fs = saved.get('meas_fs')
                cur_in = getattr(self.cfg, 'input_device', None)
                cur_out = getattr(self.cfg, 'output_device', None)
                cur_fs = self.cfg.fs

                if (cal_in is not None and cal_in != cur_in) or \
                   (cal_out is not None and cal_out != cur_out) or \
                   (cal_fs is not None and cal_fs != cur_fs):
                    print("[校准] 设备或采样率已变更, 旧校准失效, 重新测量...")
                else:
                    self.cfg.latency_calibration_samples = saved.get('latency_samples', 0)
                    ms = self.cfg.latency_calibration_samples / self.cfg.fs * 1000
                    print(f"[校准] 已加载: {self.cfg.latency_calibration_samples} 采样点 ({ms:.2f} ms)")
                    self._calibration_fresh = False
                    return self.cfg.latency_calibration_samples
            except (json.JSONDecodeError, KeyError):
                pass

        self._require_sd()

        print("\n" + "=" * 60)
        print("  环回延迟校准")
        print("=" * 60)
        print(f"  连接 输出 ch{loopback_channel_out} -> 输入 ch{loopback_channel_in}")
        input("  按回车开始...")

        cal_sweep, inv_filt = generate_sweep(
            f1=200, f2=7000, duration=0.5, fs=self.cfg.fs,
            prepend_silence_sec=0.05, append_silence_sec=0.1,
        )

        rec = self._play_and_record(
            cal_sweep,
            output_channels=[loopback_channel_out],
            input_channels=[loopback_channel_in],
            amplitude=0.3,
        )

        sweep_active = cal_sweep[int(0.05 * self.cfg.fs):-int(0.1 * self.cfg.fs)]
        xcorr = np.correlate(rec[:, 0], sweep_active, mode='full')
        delay_meas = np.argmax(np.abs(xcorr)) - (len(sweep_active) - 1)
        delay_meas = max(0, int(delay_meas))

        # 换算到 target_fs
        if self.cfg.needs_resample:
            ratio = self.cfg.rate_ratio
            delay_target = max(1, int(round(delay_meas / ratio)))
            self.cfg.latency_calibration_samples = delay_target
        else:
            delay_target = delay_meas
            self.cfg.latency_calibration_samples = delay_target

        with open(cal_path, 'w') as f:
            json.dump({
                'latency_samples': self.cfg.latency_calibration_samples,
                'latency_samples_meas_fs': delay_meas,
                'meas_fs': self.cfg.fs,
                'target_fs': self.cfg.target_fs,
                'input_device': getattr(self.cfg, 'input_device', None),
                'output_device': getattr(self.cfg, 'output_device', None),
                'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
            }, f, indent=2)

        ms_target = delay_target / self.cfg.target_fs * 1000
        if self.cfg.needs_resample:
            ms_meas = delay_meas / self.cfg.fs * 1000
            print(f"  延迟 (@{self.cfg.fs}Hz): {delay_meas} 采样点 ({ms_meas:.2f} ms)")
            print(f"  延迟 (@{self.cfg.target_fs}Hz): {delay_target} 采样点 ({ms_target:.2f} ms)")
        else:
            print(f"  延迟: {delay_target} 采样点 ({ms_target:.2f} ms)")
        print(f"  已保存至: {cal_path}")
        print("=" * 60 + "\n")
        self._calibration_fresh = True
        return self.cfg.latency_calibration_samples

    # ================================================================
    # 通道连通性检查
    # ================================================================

    def check_channels(self, spk_channels=(0, 1, 2, 3), mic_channels=(0, 1, 2, 3),
                       check_duration_sec=0.5, check_freq=500, corr_threshold=0.2):
        """检查每个扬声器→麦克风的连通性 (归一化互相关法).

        方法: 播放已知纯音, 在各麦克风上计算录制信号与参考纯音的
        归一化互相关峰值. 峰值 ∈ [0, 1], 与信号绝对幅度无关 —
        即使低灵敏度数字麦克风 (如 ICS-43434, -26 dBFS) 也能可靠检测.

        公式: peak(|cross-correlate(rec, tone)|) / sqrt(||tone||² · ||rec||²)

        参照: AES 标准 / pyroomacoustics / REW 的相干性检测实践.

        参数:
            corr_threshold: 互相关判定阈值 (默认 0.2, 足以检出 SNR=-10dB 信号).

        返回:
            status: {f"SPK{spk}->MIC{mic}": bool} 字典.
        """
        self._require_sd()
        status = {}

        print("\n" + "=" * 60)
        print("  通道连通性检查 (归一化互相关法)")
        print("=" * 60)

        # ── 预生成参考纯音 ──
        t = np.arange(int(check_duration_sec * self.cfg.fs)) / self.cfg.fs
        tone_ref = np.sin(2 * np.pi * check_freq * t) * 0.3
        fade = min(int(0.01 * self.cfg.fs), len(tone_ref) // 4)
        if fade > 0:
            tone_ref[:fade] *= np.linspace(0, 1, fade)
            tone_ref[-fade:] *= np.linspace(1, 0, fade)
        tone_norm = np.sqrt(np.sum(tone_ref ** 2))  # ||tone||, 预计算

        # ── 环境噪声底噪 (仅供参考, 不参与判断) ──
        print("  环境噪声底噪 (仅供参考):")
        noise_frames = int(0.3 * self.cfg.fs)
        noise_rec = self._play_and_record(
            np.zeros(noise_frames, dtype=np.float64),
            output_channels=[spk_channels[0]],
            input_channels=list(mic_channels),
            amplitude=0.0,
        )
        noise_rms = [float(np.sqrt(np.mean(noise_rec[i] ** 2))) for i in range(len(mic_channels))]
        print(f"    RMS: {[f'{r:.6f}' for r in noise_rms]}")
        print(f"  检测阈值: 归一化互相关峰值 > {corr_threshold} (与信号幅度无关)")
        print()

        for spk_idx, spk_ch in enumerate(spk_channels):
            out_dev = self.cfg.get_output_device_for_channel(spk_ch)
            dev_ch = self.cfg.get_device_channel_for_spk(spk_ch)
            dev_label = f"dev={out_dev}" if out_dev is not None else "dev=default"

            print(f"\n  [{spk_idx+1}/{len(spk_channels)}] SPK {spk_ch} ({dev_label}, ch{dev_ch}):")
            print("  " + "-" * 56)

            rec = self._play_and_record(
                tone_ref,
                output_channels=[dev_ch],
                input_channels=list(mic_channels),
                amplitude=self.cfg.get_output_gain(spk_ch),
                output_device=out_dev,
            )

            ok_count = 0
            for m_idx, mic_ch in enumerate(mic_channels):
                key = f"SPK{spk_ch}->MIC{mic_ch}"
                rms = float(np.sqrt(np.mean(rec[m_idx] ** 2)))
                # 归一化互相关峰值 (与绝对幅度无关)
                rec_norm = np.sqrt(np.sum(rec[m_idx] ** 2))
                if rec_norm < 1e-12:
                    corr_peak = 0.0
                else:
                    xcorr = np.correlate(rec[m_idx], tone_ref, mode='full')
                    corr_peak = float(np.max(np.abs(xcorr)) / (tone_norm * rec_norm))
                ok = corr_peak > corr_threshold
                if ok:
                    ok_count += 1
                status[key] = ok
                bar = _corr_bar(corr_peak)
                label = "OK" if ok else "无信号"
                print(f"    MIC{mic_ch}: Corr={corr_peak:.3f} {bar} [{label}]  (RMS={rms:.4f})")

            print(f"  -- {ok_count}/{len(mic_channels)} OK --")

        all_ok = all(status.values())
        print("\n  结果: " + ("所有通道已连接!" if all_ok else "请检查连接!"))
        print("=" * 60 + "\n")
        return status

    # ================================================================
    # 次级路径测量
    # ================================================================

    def measure_secondary_path(
        self,
        spk_channels=(0, 1, 2, 3),
        mic_channels=(0, 1, 2, 3),
        spk_labels=None,
        spk_device_map=None,
        progress_callback=None,
    ):
        """测量次级路径: 每个扬声器 → 所有误差麦克风.

        方法: 逐扬声器播放指数扫频信号, 所有麦克风同步录制.
              对录制信号做反卷积提取脉冲响应, 多次重复取平均.

        参数:
            spk_channels:    扬声器物理通道号列表, 如 [0, 1, 2, 3].
            mic_channels:    误差麦克风物理通道号列表, 如 [1, 2, 3, 4].
            spk_labels:      扬声器标签 (用于日志), 默认 SPK_0, SPK_1, ...
            spk_device_map:  {扬声器通道号: 输出设备 ID} — 当扬声器跨多个声卡时使用.
                             如果提供, 会覆盖 cfg.output_device_map.
                             例如你的扬声器分布在两个 USB 声卡上:
                               spk_device_map={0: 2, 1: 2, 2: 3, 3: 3}
            progress_callback: 进度回调 callable(spk_index, spk_label).

        返回:
            S_matrix: (E, S, L) float64 次级路径脉冲响应矩阵.
                      E = 误差麦克风数, S = 扬声器数, L = IR 长度.

        测量标准:
            - 扫频时长 ≥ 5s, 频率范围 20~7500 Hz @ 16kHz
            - 重复 ≥ 2 次取平均
            - 每个路径 SNR ≥ 25 dB (可接受), ≥ 35 dB (优质)
        """
        self._require_sd()
        S = len(spk_channels)
        E = len(mic_channels)
        L_target = self.cfg.ir_length          # 最终 IR 长度 (@ target_fs)
        L_meas = self.cfg.meas_ir_length       # 测量时 IR 长度 (@ fs)
        do_resample = self.cfg.needs_resample

        if spk_labels is None:
            spk_labels = [f"SPK_{i}" for i in range(S)]

        # 合并设备映射: per-call 参数优先于全局配置
        effective_device_map = dict(self.cfg.output_device_map)
        if spk_device_map:
            effective_device_map.update(spk_device_map)

        fs_info = f"meas@{self.cfg.fs}Hz"
        if do_resample:
            fs_info += f" → target@{self.cfg.target_fs}Hz"
        print("\n" + "=" * 60)
        print(f"  次级路径测量: {S} 扬声器 -> {E} 麦克风")
        print(f"  扫频: {self.cfg.f1}~{self.cfg.f2} Hz, {self.cfg.sweep_duration}s")
        print(f"  采样率: {fs_info}")
        print(f"  重复次数: {self.cfg.repetitions}")
        print(f"  幅度: {self.cfg.sweep_amplitude}")
        if effective_device_map:
            print(f"  扬声器设备映射: {effective_device_map}")
        print(f"  增益: {self._describe_gains(spk_channels, mic_channels)}")
        print("=" * 60)

        S_matrix = np.zeros((E, S, L_target), dtype=np.float64)
        quality_report = {}
        repeatabilities = []

        for spk_idx in range(S):
            spk_label = spk_labels[spk_idx]
            spk_ch = spk_channels[spk_idx]
            gain = self.cfg.get_output_gain(spk_ch)
            out_dev = effective_device_map.get(spk_ch, self.cfg.output_device)

            dev_info = f"dev={out_dev}" if out_dev is not None else "dev=default"
            print(f"\n  [{spk_idx+1}/{S}] {spk_label} (DA ch{spk_ch}, {dev_info}, gain={gain:.2f})")

            if progress_callback:
                progress_callback(spk_idx, spk_label)

            # 多次重复: 各自先反卷积得到 IR, 再平均 IR
            # (避免因每次录制延迟不同导致直接平均原始录音产生梳状滤波)
            ir_accum = np.zeros((E, L_target), dtype=np.float64)
            ir_list = [[] for _ in range(E)]  # 每个 mic 保存各次 IR, 用于重复性验证
            peak_max = 0.0
            dev_ch = self.cfg.get_device_channel_for_spk(spk_ch)
            for rep in range(self.cfg.repetitions):
                rec = self._play_and_record(
                    self._sweep_signal,
                    output_channels=[dev_ch],
                    input_channels=list(mic_channels),
                    amplitude=gain * self.cfg.sweep_amplitude,
                    output_device=out_dev,
                )
                # 削波检测 (每次录制独立检查)
                peak = float(np.max(np.abs(rec)))
                if peak > peak_max:
                    peak_max = peak

                # 每个 mic 独立反卷积 (aligned 版: 互相关找扫频起始点)
                for mic_idx in range(E):
                    ir = deconvolve_ir_aligned(
                        rec[mic_idx], self._sweep_signal, self._inv_filter,
                        ir_length=L_meas, fs=self.cfg.fs,
                    )
                    if do_resample:
                        ir = resample_ir(ir, self.cfg.fs, self.cfg.target_fs)
                        ir = ir[:L_target]
                    ir_accum[mic_idx, :] += ir
                    ir_list[mic_idx].append(ir)

            # 削波告警
            if peak_max > 0.95:
                print(f"    [警告] 检测到削波! peak={peak_max:.3f}, 请降低 sweep_amplitude")

            # ── 对齐 + 智能平均 ──
            # deconvolve_ir 提取的 IR 在不同 rep 之间偏移 1-3 样本,
            # 先互相关对齐再聚类平均 (n_reps >= 2 都做对齐)
            n_reps = self.cfg.repetitions
            clusters = []
            for mic_idx in range(E):
                irs_raw = ir_list[mic_idx]
                L = len(irs_raw[0])
                # 互相关找偏移, 对齐所有 IR
                ref = irs_raw[0]
                irs_aligned = [ref.copy()]
                for i in range(1, n_reps):
                    xcorr = np.correlate(ref, irs_raw[i], mode='full')
                    lag = np.argmax(xcorr) - (L - 1)
                    if lag > 0:
                        shifted = np.concatenate([np.zeros(lag, dtype=np.float64),
                                                  irs_raw[i][:L - lag]])
                    elif lag < 0:
                        shifted = np.concatenate([irs_raw[i][-lag:],
                                                  np.zeros(-lag, dtype=np.float64)])
                    else:
                        shifted = irs_raw[i].copy()
                    irs_aligned.append(shifted)
                # 聚类 (n_reps > 2 时做异常值剔除)
                if n_reps > 2:
                    r_mat = np.eye(n_reps)
                    for i in range(n_reps):
                        for j in range(i + 1, n_reps):
                            r_mat[i, j] = r_mat[j, i] = np.corrcoef(
                                irs_aligned[i], irs_aligned[j])[0, 1]
                    centrality = np.sum(np.maximum(r_mat, 0), axis=1)
                    seed = int(np.argmax(centrality))
                    cl = [i for i in range(n_reps) if r_mat[seed, i] > 0.5 or i == seed]
                    clusters.append(cl)
                    ir_accum[mic_idx, :] = np.sum([irs_aligned[i] for i in cl],
                                                  axis=0) / len(cl)
                else:
                    clusters.append(list(range(n_reps)))
                    ir_accum[mic_idx, :] = np.sum(irs_aligned, axis=0) / n_reps

            # 重复一致性验证 (对齐后阵营内中位数)
            xcorr_reps = 1.0
            if self.cfg.repetitions >= 2:
                cluster_medians = []
                for m in range(E):
                    cl = clusters[m]
                    if len(cl) >= 2:
                        # 重新从对齐的 IR 计算
                        irs_raw = ir_list[m]
                        L = len(irs_raw[0])
                        ref = irs_raw[0]
                        aligned = [ref.copy()]
                        for i in range(1, n_reps):
                            xcorr = np.correlate(ref, irs_raw[i], mode='full')
                            lag = np.argmax(xcorr) - (L - 1)
                            if lag > 0:
                                aligned.append(np.concatenate(
                                    [np.zeros(lag, dtype=np.float64), irs_raw[i][:L - lag]]))
                            elif lag < 0:
                                aligned.append(np.concatenate(
                                    [irs_raw[i][-lag:], np.zeros(-lag, dtype=np.float64)]))
                            else:
                                aligned.append(irs_raw[i].copy())
                        pair_rs = [np.corrcoef(aligned[cl[i]], aligned[cl[j]])[0, 1]
                                   for i in range(len(cl)) for j in range(i + 1, len(cl))]
                        if len(pair_rs) >= 3:
                            cluster_medians.append(float(np.median(pair_rs)))
                        else:
                            cluster_medians.append(max(pair_rs) if pair_rs else 1.0)
                    else:
                        cluster_medians.append(1.0)
                xcorr_reps = min(cluster_medians)
                n_outliers = sum(n_reps - len(clusters[m]) for m in range(E))
                if xcorr_reps < 0.98:
                    msg = f"阵营内中位数, 丢弃{n_outliers}个离群值" if n_outliers else "阵营内中位数"
                    print(f"    [警告] 重复性低 (IR r={xcorr_reps:.4f} < 0.98, "
                          f"{msg}), 可能存在时变噪声或时钟不同步")
            quality_report[f'_repeatability_{spk_label}'] = float(xcorr_reps)
            repeatabilities.append(xcorr_reps)

            # 存入 S_matrix 并计算每通道 SNR
            for mic_idx in range(E):
                mic_ch = mic_channels[mic_idx]
                ir = ir_accum[mic_idx, :]
                S_matrix[mic_idx, spk_idx, :] = ir
                snr = measure_snr_from_ir(ir)
                quality_report[f"{spk_label}->MIC{mic_ch}"] = {
                    'snr_db': snr['snr_db'],
                    'peak_db': snr['peak_db'],
                    'noise_floor_db': snr['noise_floor_db'],
                    'clipping': peak_max > 0.95,
                }

                # 打印 SNR (所有麦克风)
                flag = self._snr_flag(snr['snr_db'])
                print(f"      MIC{mic_ch}: SNR={snr['snr_db']}dB {flag}  "
                      f"peak={snr['peak_db']}dB  noise_floor={snr['noise_floor_db']}dB")

        # 保存最差重复性到 session, 供 validate_measurement_quality 使用
        if repeatabilities:
            self._last_repeatability = float(min(repeatabilities))
        else:
            self._last_repeatability = None

        # ── 标量归一化: 将 IR 缩放到 RMS≈1 ──
        # 抵消 Farina 反卷积的能量压缩效应 (5s 扫频 → 1024 点 IR),
        # 使次级路径 IR 幅度与 lab 校准环境一致 (RMS ≈ 1).
        # 数学上等价于测量前调节硬件增益, 不改变频率响应/相位/延迟.
        sec_rms = float(np.sqrt(np.mean(S_matrix ** 2)))
        if sec_rms > 1e-10:
            S_matrix /= sec_rms
        normalization = {'sec_rms': round(sec_rms, 4),
                         'method': 'global_scalar (÷ RMS → RMS≈1)',
                         'note': '等价于 lab 硬件增益校准, 频响/相位/延迟全部保留'}

        # 保存数据
        save_path = os.path.join(self.cfg.save_dir, 'secondary_path.npy')
        np.save(save_path, S_matrix.astype(np.float32))
        print(f"\n  已保存: {save_path}")
        print(f"  形状: {S_matrix.shape} (E={E}, S={S}, L={L_target}@{self.cfg.target_fs}Hz)")
        print(f"  归一化: ÷ {sec_rms:.2f} (RMS {sec_rms:.1f} → 1.0, 频响/相位/延迟全部保留)")

        # 保存元数据
        self._save_metadata('secondary', S_matrix.shape, quality_report,
                            extra={'normalization': normalization})

        # 质量汇总
        self._print_quality_summary(quality_report, "次级路径")
        print("=" * 60 + "\n")
        return S_matrix

    # ================================================================
    # 主路径测量 (单角度)
    # ================================================================

    def measure_primary_path(
        self,
        source_channel=4,
        source_device=None,
        mic_channels=(1, 2, 3, 4),
        ref_mic_channels=(0,),
        source_distance_m=3.0,
        source_label="noise_source",
    ):
        """测量主路径: 噪声源位置的外部扬声器 → 误差麦克风 + 参考麦克风.

        **重要**: 这里使用的是放置在噪声源位置的**独立外部扬声器**,
        不是窗框上的 ANC 扬声器.

        典型测量拓扑:
                           [噪声源扬声器]  (室外, 或其他实际噪声方向)
                                  |
                   ┌──────────────┼──────────────┐
                   |  (空气传播 + 穿窗)            |  (空气传播)
                   v                              v
            [误差麦克风 1-4]               [参考麦克风]
             (室内侧 5cm)                  (靠近噪声源)

        参数:
            source_channel:    噪声源扬声器的物理通道号.
                               **这是独立的扬声器, 不是 ANC 系统的扬声器.**
            source_device:     噪声源扬声器所在的设备 ID.
            mic_channels:      误差麦克风物理通道号列表.
            ref_mic_channels:  参考麦克风物理通道号列表.
            source_distance_m: 噪声源距离 (m), 记录在元数据中.
            source_label:      噪声源标签 (用于文件名).

        返回:
            P_matrix: (E, R, L) float64, R=1+len(ref_mic_channels).
        """
        self._require_sd()
        E = len(mic_channels)
        R_total = 1 + len(ref_mic_channels)  # 1 噪声源路径 + N 参考路径
        L_target = self.cfg.ir_length
        L_meas = self.cfg.meas_ir_length
        do_resample = self.cfg.needs_resample
        has_ref = len(ref_mic_channels) > 0

        # 所有要录制的输入通道: 误差麦克风 + 参考麦克风
        all_input_channels = list(mic_channels)
        if has_ref:
            all_input_channels.extend(ref_mic_channels)

        print("\n" + "=" * 60)
        print(f"  主路径测量")
        print(f"  声源: ch{source_channel} (dev={source_device}), 距离={source_distance_m}m")
        print(f"  误差麦克风: {list(mic_channels)} ({E} 通道)")
        if has_ref:
            print(f"  参考麦克风: {list(ref_mic_channels)}")
        print(f"  输入通道总数: {len(all_input_channels)}")
        print(f"  扫频: {self.cfg.f1}~{self.cfg.f2} Hz, {self.cfg.sweep_duration}s")
        if do_resample:
            print(f"  采样率: meas@{self.cfg.fs}Hz → target@{self.cfg.target_fs}Hz")
        print(f"  重复次数: {self.cfg.repetitions}")
        print("=" * 60)

        gain = self.cfg.get_output_gain(source_channel)

        # 多次重复: 各自先反卷积得到 IR, 再平均 IR
        # (避免因每次录制延迟不同导致直接平均原始录音产生梳状滤波)
        num_ref = len(ref_mic_channels) if has_ref else 0
        total_ch = E + num_ref
        ir_accum_err = np.zeros((E, L_target), dtype=np.float64)
        ir_accum_ref = np.zeros((num_ref, L_target), dtype=np.float64) if has_ref else None
        ir_list = [[] for _ in range(total_ch)]  # 每个输入通道保存各次 IR
        peak_max = 0.0
        for rep in range(self.cfg.repetitions):
            rec = self._play_and_record(
                self._sweep_signal,
                output_channels=[source_channel],
                input_channels=all_input_channels,
                amplitude=gain * self.cfg.sweep_amplitude,
                output_device=source_device,
            )
            # 削波检测 (每次录制独立检查)
            peak = float(np.max(np.abs(rec)))
            if peak > peak_max:
                peak_max = peak

            # 误差麦克风反卷积 (aligned 版)
            for mic_idx in range(E):
                ir = deconvolve_ir_aligned(
                    rec[mic_idx], self._sweep_signal, self._inv_filter,
                    ir_length=L_meas, fs=self.cfg.fs,
                )
                if do_resample:
                    ir = resample_ir(ir, self.cfg.fs, self.cfg.target_fs)
                    ir = ir[:L_target]
                ir_accum_err[mic_idx, :] += ir
                ir_list[mic_idx].append(ir)

            # 参考麦克风反卷积
            if has_ref:
                for ri in range(num_ref):
                    ch_idx = E + ri
                    ir = deconvolve_ir_aligned(
                        rec[ch_idx], self._sweep_signal, self._inv_filter,
                        ir_length=L_meas, fs=self.cfg.fs,
                    )
                    if do_resample:
                        ir = resample_ir(ir, self.cfg.fs, self.cfg.target_fs)
                        ir = ir[:L_target]
                    ir_accum_ref[ri, :] += ir
                    ir_list[ch_idx].append(ir)

        # 削波告警
        if peak_max > 0.95:
            print(f"  [警告] 检测到削波! peak={peak_max:.3f}, 请降低 sweep_amplitude")

        # 重复一致性验证 (对 IR 做互相关, 而非原始录音)
        xcorr_reps = 1.0
        if self.cfg.repetitions >= 2:
            xcorr_reps = np.corrcoef(ir_list[0][0], ir_list[0][1])[0, 1]
            if xcorr_reps < 0.98:
                print(f"  [警告] 重复性低 (IR r={xcorr_reps:.4f} < 0.98), "
                      f"可能存在时变噪声或时钟不同步")
        self._last_repeatability = float(xcorr_reps)

        # 平均 IR
        ir_accum_err /= self.cfg.repetitions
        if has_ref:
            ir_accum_ref /= self.cfg.repetitions

        # 构建主路径矩阵: (E, R_total, L_target)
        P_matrix = np.zeros((E, R_total, L_target), dtype=np.float64)
        quality_report = {}

        # 噪声源 → 误差麦克风
        print(f"\n  误差麦克风路径 (声源 -> 误差麦):")
        for mic_idx in range(E):
            mic_ch = mic_channels[mic_idx]
            ir = ir_accum_err[mic_idx, :]
            P_matrix[mic_idx, 0, :] = ir
            snr = measure_snr_from_ir(ir)
            quality_report[f"Source->MIC{mic_ch}"] = {
                'snr_db': snr['snr_db'],
                'peak_db': snr['peak_db'],
                'noise_floor_db': snr['noise_floor_db'],
                'clipping': peak_max > 0.95,
            }
            flag = self._snr_flag(snr['snr_db'])
            print(f"    MIC{mic_ch}: SNR={snr['snr_db']}dB {flag}")

        # 噪声源 → 参考麦克风
        if has_ref:
            print(f"\n  参考麦克风路径 (声源 -> 参考麦):")
            for ri, ref_ch in enumerate(ref_mic_channels):
                ir = ir_accum_ref[ri, :]
                for mic_idx in range(E):
                    P_matrix[mic_idx, 1 + ri, :] = ir
                snr = measure_snr_from_ir(ir)
                quality_report[f"Source->REF{ref_ch}"] = {
                    'snr_db': snr['snr_db'],
                    'peak_db': snr['peak_db'],
                    'noise_floor_db': snr['noise_floor_db'],
                    'clipping': peak_max > 0.95,
                }
                flag = self._snr_flag(snr['snr_db'])
                print(f"    REF{ref_ch}: SNR={snr['snr_db']}dB {flag}")

        # ── 标量归一化: 与次级路径一致, 补偿 Farina 反卷积能量压缩 ──
        # 全局标量 ÷ RMS → RMS≈1, 保留通道间相对增益和频响/相位/延迟.
        pri_rms = float(np.sqrt(np.mean(P_matrix ** 2)))
        if pri_rms > 1e-10:
            P_matrix /= pri_rms
        normalization = {'pri_rms': round(pri_rms, 4),
                         'method': 'global_scalar (÷ RMS → RMS≈1)',
                         'note': '与次级路径归一化一致, 频响/相位/延迟全部保留'}

        # 保存
        filename = 'primary_path.npy'
        save_path = os.path.join(self.cfg.save_dir, filename)
        np.save(save_path, P_matrix.astype(np.float32))
        print(f"\n  已保存: {save_path}")
        print(f"  形状: {P_matrix.shape} (E={E}, R={R_total}, L={L_target}@{self.cfg.target_fs}Hz)")
        print(f"  归一化: ÷ {pri_rms:.2f} (RMS {pri_rms:.1f} → 1.0, 频响/相位/延迟全部保留)")

        self._save_metadata('primary', P_matrix.shape, quality_report,
                            extra={'source_distance_m': source_distance_m,
                                   'source_label': source_label,
                                   'normalization': normalization})
        self._print_quality_summary(quality_report, "主路径")
        print("=" * 60 + "\n")
        return P_matrix

    # ================================================================
    # 主路径测量 (多角度)
    # ================================================================

    def measure_primary_path_multi_angle(
        self,
        source_channel,
        source_device=None,
        mic_channels=(1, 2, 3, 4),
        ref_mic_channels=(0,),
        angle_configs=None,
    ):
        """多角度主路径测量 — 模拟不同方向的噪声源.

        将外部扬声器依次放到不同位置/角度, 在每个位置测量一次完整的
        主路径. 所有结果保存为独立文件, 便于后续分析不同噪声方向下
        主路径的变化.

        使用场景:
          - 窗户 ANC: 室外噪声可能来自正前方 (90°), 左前方 (45°), 右前方 (135°) 等
          - 通过多角度测量, 可以了解主路径对声源方向的敏感度
          - 训练时可以混合多角度数据增强鲁棒性

        参数:
            source_channel: 外部噪声源扬声器通道号.
            source_device:  外部噪声源扬声器设备 ID.
            mic_channels:   误差麦克风通道号列表.
            ref_mic_channels: 参考麦克风通道号列表.
            angle_configs:  list of dict, 每个 dict 定义一次测量:
                {'angle_deg': 0, 'distance_m': 3.0, 'label': '0deg_front'}
                如果为 None, 使用默认的 5 个角度配置.

        返回:
            results: dict, key=label, value=P_matrix.
                     同时每个角度单独存为 .npy 文件.

        示例:
            >>> sess.measure_primary_path_multi_angle(
            ...     source_channel=4,
            ...     angle_configs=[
            ...         {'angle_deg': 0,   'distance_m': 3.0, 'label': 'angle_0deg'},
            ...         {'angle_deg': 30,  'distance_m': 3.0, 'label': 'angle_30deg'},
            ...         {'angle_deg': 60,  'distance_m': 3.0, 'label': 'angle_60deg'},
            ...         {'angle_deg': 90,  'distance_m': 3.0, 'label': 'angle_90deg'},
            ...     ],
            ... )
        """
        if angle_configs is None:
            # 默认: 5 个角度, 覆盖典型噪声源方向
            angle_configs = [
                {'angle_deg': 0,   'distance_m': 3.0, 'label': 'angle_0deg_left'},
                {'angle_deg': 45,  'distance_m': 3.0, 'label': 'angle_45deg_left-front'},
                {'angle_deg': 90,  'distance_m': 3.0, 'label': 'angle_90deg_front'},
                {'angle_deg': 135, 'distance_m': 3.0, 'label': 'angle_135deg_right-front'},
                {'angle_deg': 180, 'distance_m': 3.0, 'label': 'angle_180deg_right'},
            ]

        results = {}
        n_angles = len(angle_configs)

        print("\n" + "#" * 60)
        print(f"#  多角度主路径测量")
        print(f"#  声源: ch{source_channel}, 角度数: {n_angles}")
        print(f"#  误差麦克风: {list(mic_channels)}")
        print(f"#  参考麦克风: {list(ref_mic_channels)}")
        print("#" * 60)

        for i, cfg_angle in enumerate(angle_configs):
            angle = cfg_angle['angle_deg']
            distance = cfg_angle.get('distance_m', 3.0)
            label = cfg_angle['label']

            print(f"\n  >>> 角度 [{i+1}/{n_angles}]: {label}")
            print(f"      角度={angle}°, 距离={distance} m")
            print(f"      [操作] 将外部扬声器移到该位置，然后按回车...")
            # 实际使用时取消注释:
            # input("      Press Enter when ready...")

            P = self.measure_primary_path(
                source_channel=source_channel,
                source_device=source_device,
                mic_channels=mic_channels,
                ref_mic_channels=ref_mic_channels,
                source_distance_m=distance,
                source_label=label,
            )
            results[label] = P

        # 保存汇总
        E = len(mic_channels)
        R = 1 + len(ref_mic_channels)
        L = self.cfg.ir_length
        all_P = np.zeros((n_angles, E, R, L), dtype=np.float32)
        for i, cfg_angle in enumerate(angle_configs):
            all_P[i] = results[cfg_angle['label']].astype(np.float32)

        summary_path = os.path.join(self.cfg.save_dir,
                                    f'primary_path_multi_angle_{n_angles}pos_{E}mic.npy')
        np.save(summary_path, all_P)
        print(f"\n  多角度汇总已保存: {summary_path}")
        print(f"  形状: {all_P.shape} (角度数={n_angles}, E={E}, R={R}, L={L})")
        print("#" * 60 + "\n")
        return results

    # ================================================================
    # 质量验证
    # ================================================================

    def validate_measurement_quality(
        self,
        ir_matrix: np.ndarray,
        label: str = "",
        snr_min_reject: float = 25.0,
        snr_min_acceptable: float = 35.0,
        phase_freq_range: tuple = (100, 1000),
        phase_rmse_max_rad: float = 1.0,
        repeatability_corr: Optional[float] = None,
        repeatability_min: float = 0.98,
    ) -> Dict:
        """对已测量的 IR 矩阵做工业级全项质量检验.

        参考标准:
          - ISO 18233:2006  Acoustic measurement by swept-sine
          - AES 108th (Farina 2000)  ESS method
          - SAE J2883  Automotive ANC validation (informative)

        SNR 阈值来自 ANC 领域工程实践 (Morgan 1980, Kuo & Morgan 1996):
        FxLMS 对次级路径估计的容忍度约 90° 相位误差; SNR ≥ 25 dB
        时相位通常可靠, ≥ 35 dB 时 ANC 性能无损耗.

        检验项目:
          1. 每通道 SNR (峰前噪声法)             ≥35dB 优秀, 25-35dB 可接受, <25dB 拒绝
          2. 因果性 (包络起始点前能量 < 5%)        业内通用阈值 (REW/ARTA)
          3. 相位线性度 (通带内相位 RMSE < 1.0rad) ANC FxLMS 容忍 90° 相位误差
          4. 重复性 (IR 相关系数 >= 0.98)          Farina 2000
          5. 通道间能量一致性 (检测断线/增益异常)
          6. IR 峰值位置对齐检查

        参数:
            ir_matrix:               脉冲响应矩阵 (E, S, L).
            label:                   测量标签 (用于报告).
            snr_min_reject:          拒绝阈值 (dB), 默认 25.
            snr_min_acceptable:      良好阈值 (dB), 默认 35.
            phase_freq_range:        ANC 通带 (Hz), 默认 100-1000.
            phase_rmse_max_rad:      相位 RMSE 上限 (rad), 默认 1.0 (~57°, < FxLMS 90° 容忍).
            repeatability_corr:      重复性相关系数 (None=自动从 session 获取).
            repeatability_min:       重复性最小阈值, 默认 0.98.

        返回:
            report: {
                'pass': bool, 'channels': [...], 'warnings': [...], 'failures': [...],
                'summary': { 'snr': {...}, 'causality': {...}, 'phase': {...} }
            }
        """
        if ir_matrix.ndim == 3:
            E, S, L = ir_matrix.shape
        else:
            E, S = ir_matrix.shape[0], 1
            L = ir_matrix.shape[1]
            ir_matrix = ir_matrix.reshape(E, S, L)

        report = {'pass': True, 'channels': [], 'warnings': [], 'failures': [],
                  'summary': {}}

        # ── 解析重复性: 参数优先, 其次从 session 获取 ──
        _rep_corr = repeatability_corr
        if _rep_corr is None:
            _rep_corr = getattr(self, '_last_repeatability', None)

        print(f"\n{'='*70}")
        print(f"  工业级质量检验: {label}")
        print(f"  参考标准: ISO 18233, AES 108th (Farina 2000)")
        print(f"{'='*70}")

        n_total = E * S
        snr_values = []
        causality_ok = 0
        phase_ok = 0

        for e in range(E):
            for s in range(S):
                ch_id = f"[{e},{s}]"
                ir = ir_matrix[e, s, :]
                snr = measure_snr_from_ir(ir)
                causality = check_causality(ir)
                phase = check_phase_linearity(
                    ir, fs=self.cfg.target_fs, freq_range=phase_freq_range,
                )
                rms = float(np.sqrt(np.mean(ir ** 2)))

                ch_info = {
                    'channel': ch_id,
                    'snr_db': snr['snr_db'],
                    'peak_db': snr['peak_db'],
                    'peak_idx': causality['peak_idx'],
                    'rms': rms,
                    'causality': causality,
                    'phase': phase,
                }
                snr_values.append(snr['snr_db'])

                # ── SNR 检验 (工程阈值: 25/35 dB) ──
                if snr['snr_db'] < snr_min_reject:
                    report['failures'].append(
                        f"CH{ch_id} SNR={snr['snr_db']}dB < {snr_min_reject}dB [拒绝]")
                    report['pass'] = False
                    ch_info['quality'] = 'REJECT'
                elif snr['snr_db'] < snr_min_acceptable:
                    ch_info['quality'] = 'ACCEPTABLE'
                else:
                    ch_info['quality'] = 'EXCELLENT'

                # ── 因果性检验 ──
                if not causality['is_causal']:
                    report['warnings'].append(
                        f"CH{ch_id} 峰值前能量={causality['pre_peak_energy_pct']:.1f}% "
                        f"(>5%), 可能存在反因果伪影")
                else:
                    causality_ok += 1

                if causality['peak_at_start']:
                    report['warnings'].append(
                        f"CH{ch_id} 峰值位于采样点 {causality['peak_idx']} "
                        f"(<10), 对齐可能异常")

                # ── 相位线性度检验 (phase RMSE: 相位偏离线性的程度) ──
                if phase['phase_rmse_rad'] >= phase_rmse_max_rad:
                    report['warnings'].append(
                        f"CH{ch_id} 相位RMSE={phase['phase_rmse_rad']:.4f}rad "
                        f"(>={phase_rmse_max_rad}rad ≈ {phase_rmse_max_rad*57.3:.0f}°), "
                        f"相位非线性可能影响 ANC")
                else:
                    phase_ok += 1

                report['channels'].append(ch_info)

        # ── 通道间能量一致性 ──
        rms_values = [ch['rms'] for ch in report['channels']]
        if rms_values:
            median_rms = np.median(rms_values)
            for ch in report['channels']:
                if ch['rms'] < median_rms * 0.1 and ch['rms'] > 0:
                    report['warnings'].append(
                        f"CH{ch['channel']} RMS={ch['rms']:.4f} << 中位数={median_rms:.4f}, "
                        f"请检查连接/增益")

        # ── 重复性检验 (整体指标, 非逐通道) ──
        if _rep_corr is not None and _rep_corr < repeatability_min:
            report['failures'].append(
                f"重复性 r={_rep_corr:.4f} < {repeatability_min} — "
                f"两次测量 IR 相关系数过低, 测量不可复现. "
                f"原因: 时变噪声或时钟不同步"
            )
            report['pass'] = False

        # ── 汇总 ──
        report['summary'] = {
            'snr': {
                'min': round(min(snr_values), 1) if snr_values else 0,
                'max': round(max(snr_values), 1) if snr_values else 0,
                'median': round(float(np.median(snr_values)), 1) if snr_values else 0,
                'excellent': sum(1 for ch in report['channels'] if ch['quality'] == 'EXCELLENT'),
                'acceptable': sum(1 for ch in report['channels'] if ch['quality'] == 'ACCEPTABLE'),
                'reject': sum(1 for ch in report['channels'] if ch['quality'] == 'REJECT'),
            },
            'causality': {'pass': causality_ok, 'total': n_total},
            'phase_linearity': {'pass': phase_ok, 'total': n_total},
            'repeatability': {
                'corr': round(_rep_corr, 4) if _rep_corr is not None else None,
                'pass': _rep_corr is not None and _rep_corr >= repeatability_min,
            },
        }

        # ── 打印工业级报告 ──
        print(f"\n  {'─' * 50}")
        print(f"  [1] SNR (峰前噪声法, 工程阈值)")
        print(f"      拒绝 < {snr_min_reject}dB, "
              f"可接受 >= {snr_min_reject}dB, "
              f"优秀 >= {snr_min_acceptable}dB")
        s = report['summary']['snr']
        print(f"      最小值={s['min']}  最大值={s['max']}  中位数={s['median']} dB")
        print(f"      优秀: {s['excellent']}/{n_total}  "
              f"可接受: {s['acceptable']}/{n_total}  "
              f"拒绝: {s['reject']}/{n_total}")
        if s['min'] < 30.0:
            print(f"\n  [!] SNR 偏低 (最低 {s['min']}dB < 30dB), 相位可能不可靠:")
            print(f"      1. 增加扫频时长 → 10~15s (时间加倍≈+3dB SNR)")
            print(f"      2. 增加重复次数 → 6~8 次 (对抗时变噪声, 每次重复≈+1.5dB)")
            print(f"      3. 提高播放幅度 → 0.9~0.95 (目前 peak 远未削波)")
            print(f"      4. 检查环境噪声, 关闭空调/风扇等宽带噪声源")

        print(f"\n  [2] 因果性 (峰值前能量 < 5%)")
        c = report['summary']['causality']
        print(f"      通过: {c['pass']}/{c['total']}")

        print(f"\n  [3] 相位线性度 (相位 RMSE < {phase_rmse_max_rad}rad ≈ {phase_rmse_max_rad*57.3:.0f}°)")
        p = report['summary']['phase_linearity']
        print(f"      通过: {p['pass']}/{p['total']}")
        if p['pass'] < p['total']:
            for ch in report['channels']:
                if ch['phase']['phase_rmse_rad'] >= phase_rmse_max_rad:
                    print(f"      CH{ch['channel']}: 相位RMSE={ch['phase']['phase_rmse_rad']:.4f}rad "
                          f"({ch['phase']['phase_rmse_rad']*57.3:.1f}°)  "
                          f"群延迟std={ch['phase']['group_delay_std_ms']:.1f}ms")

        # ── 重复性 ──
        if _rep_corr is not None:
            print(f"\n  [4] 重复性 (IR 相关系数 >= {repeatability_min})")
            rep_ok = _rep_corr >= repeatability_min
            rep_flag = "✓ 通过" if rep_ok else "✗ 失败"
            print(f"      r={_rep_corr:.4f}  [{rep_flag}]")
            if not rep_ok:
                print(f"      原因: 时变噪声或时钟不同步 → 两次测量 IR 不一致")
                print(f"      建议: 增加重复次数(>=8)、延长扫频(>=20s)、低噪声时段测量")

        print(f"\n  {'─' * 50}")
        print(f"  警告: {len(report['warnings'])}")
        print(f"  失败: {len(report['failures'])}")

        if report['warnings']:
            for w in report['warnings'][:10]:  # 最多显示 10 条
                print(f"    [警告] {w}")
            if len(report['warnings']) > 10:
                print(f"    ... 还有 {len(report['warnings']) - 10} 条")
        if report['failures']:
            for f in report['failures']:
                print(f"    [失败] {f}")

        result = "[通过]" if report['pass'] else "[失败]"
        print(f"\n  综合结果: {result}")
        print(f"{'='*70}\n")
        return report

    # ================================================================
    # 硬件交互接口 (扩展点)
    # ================================================================

    def _play_and_record(self, sweep_data, output_channels, input_channels,
                         amplitude=1.0, output_device=None):
        """
        同步播放扫频信号并录制多通道音频.

        使用独立输入/输出流 (非全双工), 支持:
          - 输入/输出设备在不同时钟域 (如 USB 麦克风 + 板载扬声器)
          - 独立的输入/输出通道映射
          - 逐通道输出设备切换 (output_device_map)

        *** 这是与音频硬件交互的唯一入口点. ***
        要支持自定义硬件后端:
          - 继承 MeasureSession
          - 重载此方法
          - 保持相同的参数签名和返回值格式

        参数:
          sweep_data:      (N,) float64 单通道音频信号.
          output_channels: 要播放的 DA 设备物理通道号列表 (0-based).
          input_channels:  要录制的 AD 设备物理通道号列表 (0-based).
          amplitude:       播放幅度缩放因子 (0~1).
          output_device:   覆盖默认输出设备 ID. 用于扬声器跨多个声卡的场景.

        返回:
          recording: (len(input_channels), len(sweep_data)) float64.
            录音长度可能略长于 sweep_data (含尾音余量),
            由反卷积/互相关自动对齐.
        """
        self._require_sd()
        import time

        num_out = len(output_channels)
        num_in = len(input_channels)
        fs = self.cfg.fs

        # ── 确定输入设备 ──
        dev_in = self.cfg.input_device
        if dev_in is None:
            dev_in = self._sd.default.device[0]

        # ── 确定输出设备 ──
        dev_out = output_device if output_device is not None else self.cfg.output_device
        if dev_out is not None:
            _validate_output_device(self._sd, dev_out)
        else:
            dev_out = self._sd.default.device[1]
            dev_out = _ensure_output_device(self._sd, dev_out)

        # ── 构建单通道播放数据 ──
        out_data_1ch = np.zeros((len(sweep_data), num_out), dtype=np.float32)
        for i in range(num_out):
            out_data_1ch[:, i] = sweep_data * amplitude

        # ── 输出侧: 展开到输出设备全部通道 ──
        out_dev_info = self._sd.query_devices(dev_out)
        out_dev_ch = out_dev_info['max_output_channels']
        out_ch = list(output_channels)  # 0-based, 直接使用
        out_needed_ch = max(out_ch, default=0) + 1
        out_use_exclusive = False
        if out_needed_ch > out_dev_ch:
            # WASAPI 共享模式可能将多通道设备下混为立体声
            # 使用独占模式绕过系统混音器
            out_use_exclusive = True
            out_dev_ch = out_needed_ch
            print(f"  [WASAPI] 输出设备 {dev_out} 共享模式仅 {out_dev_info['max_output_channels']} 通道,"
                  f" 自动切换独占模式 (请求 {out_dev_ch} 通道)")
        out_full = np.zeros((len(sweep_data), out_dev_ch), dtype=np.float32)
        for i, ch in enumerate(out_ch):
            out_full[:, ch] = out_data_1ch[:, i]

        # ── 输入侧: 录制全部通道, 再提取 ──
        in_dev_info = self._sd.query_devices(dev_in)
        in_dev_ch = in_dev_info['max_input_channels']
        in_ch = list(input_channels)  # 0-based
        in_needed_ch = max(in_ch, default=0) + 1
        in_use_exclusive = False
        if in_needed_ch > in_dev_ch:
            # WASAPI 共享模式将多通道设备下混为立体声 (报告 2ch).
            # 独占模式只支持标准通道数 (2/4/6/8), 找到 >= 需要的最小值.
            # 录制全部通道, 返回时只提取 in_ch 中指定的通道.
            for ch in (2, 4, 6, 8):
                if ch >= in_needed_ch:
                    in_dev_ch = ch
                    break
            else:
                in_dev_ch = in_needed_ch
            in_use_exclusive = True
            print(f"  [WASAPI] 输入设备 {dev_in} ('{in_dev_info['name']}') 共享模式仅 {in_dev_info['max_input_channels']} 通道,"
                  f" 自动切换独占模式 (请求 {in_dev_ch} 通道)")
        if in_needed_ch > in_dev_ch:
            raise ValueError(
                f"录音通道 {input_channels} 超出输入设备 {dev_in} "
                f"('{in_dev_info['name']}') 的可用通道数 {in_dev_ch}"
                f" (共享模式报告 {in_dev_info['max_input_channels']} 通道).\n"
                f"  提示: 使用 --list-devices --all-devices 确认设备通道数, 或检查驱动设置."
            )

        main_frames = len(sweep_data)
        tail_frames = int(fs * 0.3)  # 尾音余量
        total_rec_frames = main_frames + tail_frames

        recording_full = np.zeros((total_rec_frames, in_dev_ch), dtype=np.float32)
        rec_cursor = [0]

        def in_callback(indata, frames, time_info, status):
            if status:
                print(f"  [input] status: {status}")
            start = rec_cursor[0]
            end = min(start + frames, total_rec_frames)
            n = end - start
            if n > 0:
                recording_full[start:end] = indata[:n]
                rec_cursor[0] = end
            if rec_cursor[0] >= total_rec_frames:
                raise self._sd.CallbackStop

        # ── 检测是否需要全双工 (同设备 ASIO 不能分开打开输入/输出流) ──
        # ASIO4ALL 是软件聚合层, 底层多个独立 USB 设备没有硬件时钟同步,
        # 全双工会导致每次流启动时输入输出相位关系不固定 → 禁止双工
        in_dev_api = self._sd.query_hostapis(in_dev_info['hostapi'])['name'] if 'hostapi' in in_dev_info else ''
        out_dev_api = self._sd.query_hostapis(out_dev_info['hostapi'])['name'] if 'hostapi' in out_dev_info else ''
        in_dev_name = in_dev_info.get('name', '')
        out_dev_name = out_dev_info.get('name', '')
        use_duplex = (dev_in == dev_out and 'ASIO' in in_dev_api)

        if use_duplex:
            # ── 全双工: 输入+输出在同一个 sd.Stream 中 ──
            play_cursor = [0]

            def duplex_callback(indata, outdata, frames, time_info, status):
                if status:
                    print(f"  [stream] status: {status}")
                # 录音
                start = rec_cursor[0]
                end = min(start + frames, total_rec_frames)
                n = end - start
                if n > 0:
                    recording_full[start:end] = indata[:n]
                    rec_cursor[0] = end
                # 播放
                out_start = play_cursor[0]
                out_end = min(out_start + frames, len(out_full))
                out_n = out_end - out_start
                if out_n > 0:
                    outdata[:out_n] = out_full[out_start:out_end]
                    play_cursor[0] = out_end
                if out_n < frames:
                    outdata[out_n:] = 0
                if rec_cursor[0] >= total_rec_frames and play_cursor[0] >= len(out_full):
                    raise self._sd.CallbackStop

            stream = self._sd.Stream(
                samplerate=fs, device=dev_in,
                channels=(in_dev_ch, out_dev_ch),
                dtype='float32', callback=duplex_callback,
            )
            stream.start()
            deadline = time.time() + (total_rec_frames / fs) + 5.0
            while (rec_cursor[0] < total_rec_frames or play_cursor[0] < len(out_full)) and time.time() < deadline:
                time.sleep(0.02)
            stream.stop()
            stream.close()
        else:
            # ── 独立流: 先 Input, 再 Output (不同设备/时钟域) ──
            in_extra = self._sd.WasapiSettings(exclusive=True) if in_use_exclusive else None
            try:
                in_stream = self._sd.InputStream(
                    samplerate=fs, device=dev_in, channels=in_dev_ch,
                    dtype='float32', callback=in_callback,
                    extra_settings=in_extra,
                )
            except self._sd.PortAudioError:
                # WASAPI 失败 → 自动回退到 MME
                if in_use_exclusive:
                    mme_dev = _find_device_by_name_api(
                        self._sd, in_dev_info['name'], 'MME', min_channels=in_dev_ch
                    )
                    if mme_dev is not None:
                        mme_info = self._sd.query_devices(mme_dev)
                        mme_ch = mme_info['max_input_channels']
                        if mme_ch != in_dev_ch:
                            recording_full = np.zeros((total_rec_frames, mme_ch), dtype=np.float32)
                        in_stream = self._sd.InputStream(
                            samplerate=fs, device=mme_dev, channels=mme_ch,
                            dtype='float32', callback=in_callback,
                        )
                        in_dev_ch_actual = mme_ch
                        print(f"  [MME] WASAPI 不支持多通道, 已回退到 MME 设备 {mme_dev}"
                              f" ('{in_dev_info['name']}', {mme_ch} 通道)")
                    else:
                        raise
                else:
                    raise
            in_stream.start()
            time.sleep(0.02)

            try:
                out_extra = self._sd.WasapiSettings(exclusive=True) if out_use_exclusive else None
                try:
                    out_stream = self._sd.OutputStream(
                        samplerate=fs, device=dev_out,
                        channels=out_dev_ch, dtype='float32',
                        extra_settings=out_extra,
                    )
                except self._sd.PortAudioError as e:
                    native_fs = out_dev_info.get('default_samplerate', '?')
                    mode_hint = ""
                    if out_use_exclusive:
                        mode_hint = (
                            f"\n  WASAPI 独占模式失败, 可能原因:"
                            f"\n    1. 该设备不支持独占模式"
                            f"\n    2. 有其他应用正在使用该设备"
                            f"\n    3. 采样率 {fs} Hz 与设备原生格式不兼容"
                        )
                    raise RuntimeError(
                        f"无法以 {fs} Hz 打开播放设备 {dev_out} ('{out_dev_info['name']}').\n"
                        f"  该设备原生采样率为 {native_fs} Hz, 可能不支持 {fs} Hz.\n"
                        f"  请将 fs 改为 {native_fs} Hz, 并设置 target_fs=16000 自动降采样.{mode_hint}\n"
                        f"  PortAudio 原始错误: {e}"
                    ) from e
                out_stream.start()
                try:
                    out_stream.write(out_full)
                    tail = np.zeros((tail_frames, out_dev_ch), dtype=np.float32)
                    out_stream.write(tail)
                finally:
                    out_stream.stop()
                    out_stream.close()
            finally:
                deadline = time.time() + (total_rec_frames / fs) + 2.0
                while rec_cursor[0] < total_rec_frames and time.time() < deadline:
                    time.sleep(0.02)
                in_stream.stop()
                in_stream.close()

        # ── 提取指定输入通道 ──
        result = recording_full[:rec_cursor[0]]
        return np.asarray(result[:, in_ch], dtype=np.float64).T

    # ================================================================
    # 内部辅助方法
    # ================================================================

    def _describe_gains(self, spk_channels, mic_channels):
        parts = []
        for ch in spk_channels:
            g = self.cfg.get_output_gain(ch)
            if g != 1.0:
                parts.append(f"SPK{ch}={g:.1f}")
        for ch in mic_channels:
            g = self.cfg.get_input_gain(ch)
            if g != 1.0:
                parts.append(f"MIC{ch}={g:.1f}")
        return ", ".join(parts) if parts else "all=1.0"

    @staticmethod
    def _snr_flag(snr_db: float) -> str:
        if snr_db >= 35:
            return "[优秀]"
        elif snr_db >= 25:
            return "[可接受]"
        else:
            return "[较差]"

    def _save_metadata(self, measurement_type: str, shape: Tuple,
                       quality_report: Dict, extra: Dict = None):
        """保存测量元数据 (用于复现条件)."""
        meta = {
            'type': measurement_type,
            'shape': list(shape),
            'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
            'config': {
                'fs': self.cfg.fs,
                'f1': self.cfg.f1,
                'f2': self.cfg.f2,
                'sweep_duration': self.cfg.sweep_duration,
                'sweep_amplitude': self.cfg.sweep_amplitude,
                'ir_length': self.cfg.ir_length,
                'repetitions': self.cfg.repetitions,
                'input_device': self.cfg.input_device,
                'output_device': self.cfg.output_device,
                'latency_calibration_samples': self.cfg.latency_calibration_samples,
            },
            'quality': quality_report,
        }
        if extra:
            meta.update(extra)

        path = os.path.join(self.cfg.save_dir, METADATA_FILE)
        # 如果已有元数据, 合并而不是覆盖
        existing = {}
        if os.path.exists(path):
            try:
                with open(path, 'r') as f:
                    existing = json.load(f)
            except (json.JSONDecodeError, KeyError):
                pass
        existing[measurement_type] = meta
        with open(path, 'w') as f:
            json.dump(existing, f, indent=2)

    def _print_quality_summary(self, quality_report: Dict, title: str):
        """打印测量质量汇总."""
        if not quality_report:
            return
        snr_values = [v['snr_db'] for v in quality_report.values()
                      if isinstance(v, dict) and 'snr_db' in v]
        clips = [k for k, v in quality_report.items()
                 if isinstance(v, dict) and v.get('clipping')]
        excellent = sum(1 for s in snr_values if s >= 35)
        acceptable = sum(1 for s in snr_values if 25 <= s < 35)
        poor = sum(1 for s in snr_values if s < 25)

        print(f"\n  --- {title} 质量汇总 ---")
        print(f"  SNR: 最小值={min(snr_values):.1f}  最大值={max(snr_values):.1f}  "
              f"平均值={np.mean(snr_values):.1f} dB")
        print(f"  优秀 (≥35dB): {excellent}/{len(snr_values)}")
        print(f"  可接受 (25-35dB): {acceptable}/{len(snr_values)}")
        if poor:
            print(f"  较差 (<25dB): {poor}/{len(snr_values)}")
        if clips:
            print(f"  检测到削波: {clips}")

    # ================================================================
    # 手动测量模式 (生成 WAV + 纯录制 + 离线反卷积)
    # ================================================================

    def generate_sweep_wav(self, output_path=None, amplitude=0.8):
        """生成指数扫频 WAV 文件, 用于手动测量 (手机播放 → 电脑录制).

        使用场景:
          1. 扬声器不是电脑声卡的输出通道 (例如通过手机/播放器驱动)
          2. 需要将 WAV 拷贝到手机上, 连接 ANC 扬声器或外部扬声器播放
          3. 电脑只负责录制麦克风输入

        参数:
            output_path: WAV 保存路径, 默认 {save_dir}/sweep_20_7500Hz_5s_16kHz.wav.
            amplitude:   扫频幅度 (0~1), 默认 0.8.

        返回:
            output_path: 保存的 WAV 文件路径.
        """
        if output_path is None:
            output_path = os.path.join(
                self.cfg.save_dir,
                f'sweep_{self.cfg.f1:.0f}_{self.cfg.f2:.0f}Hz_'
                f'{self.cfg.sweep_duration:.0f}s_{self.cfg.fs}Hz.wav'
            )

        sweep_signal = self._sweep_signal * amplitude
        # 限幅
        sweep_signal = np.clip(sweep_signal, -0.99, 0.99)
        wav_data = (sweep_signal * 32767).astype(np.int16)

        wavfile.write(output_path, self.cfg.fs, wav_data)
        duration = len(sweep_signal) / self.cfg.fs
        print(f"\n  Sweep WAV saved: {output_path}")
        print(f"    Duration: {duration:.1f}s ({len(sweep_signal)} samples)")
        print(f"    Frequency: {self.cfg.f1:.0f} -> {self.cfg.f2:.0f} Hz")
        print(f"    Amplitude: {amplitude}")
        print(f"    Sample rate: {self.cfg.fs} Hz, 16-bit mono")
        print(f"  [ACTION] Copy to phone, connect to speaker, play at full volume.")
        return output_path

    def record_only(self, input_channels, duration_sec=None, output_path=None):
        """纯录制模式 — 不播放任何音频, 只录制麦克风输入.

        用于手动测量: 用户在手机上播放扫频 WAV, 电脑同步录制麦克风.

        参数:
            input_channels: 要录制的麦克风通道号列表, 如 [0, 1, 2, 3, 4].
            duration_sec:   录制时长 (秒), 默认 = 扫频信号全长.
            output_path:    录制保存路径 (.wav), 默认 {save_dir}/recording_{timestamp}.wav.

        返回:
            (recording, output_path):
              - recording: (num_input_channels, N) float64 录制数据.
              - output_path: 保存的 WAV 文件路径.
        """
        self._require_sd()
        if duration_sec is None:
            duration_sec = self._total_frames / self.cfg.fs

        num_in = len(input_channels)
        num_frames = int(duration_sec * self.cfg.fs)

        if output_path is None:
            timestamp = time.strftime('%Y%m%d_%H%M%S')
            output_path = os.path.join(self.cfg.save_dir, f'recording_{timestamp}.wav')

        # ── 设备信息 ──
        # 与 _play_and_record 一致: 始终录制设备全部通道再切片,
        # 不使用 PortAudio channel mapping (ASIO4ALL 不支持)
        dev_info = self._sd.query_devices(self.cfg.input_device) if self.cfg.input_device is not None else None
        api_name = self._sd.query_hostapis(dev_info['hostapi'])['name'] if dev_info else ''
        dev_ch = dev_info['max_input_channels'] if dev_info else num_in
        needed_ch = max(input_channels, default=0) + 1
        if needed_ch > dev_ch:
            raise ValueError(
                f"请求通道 {input_channels} 超出设备 {self.cfg.input_device} "
                f"('{dev_info['name']}') 的可用通道数 {dev_ch}."
            )

        # ── 倒计时 (录制开始前, 避免占用录制时长) ──
        for i in range(3, 0, -1):
            print(f"    {i}...")
            time.sleep(1)
        print(f"    RECORDING — keep quiet! (press play on phone NOW)")

        print(f"\n  Recording {duration_sec:.1f}s on channels {input_channels} ...")
        print(f"    Device: {self.cfg.input_device}, fs={self.cfg.fs}, "
              f"api={api_name}, ch={dev_ch} (record all, extract {input_channels})")

        rec = self._sd.rec(
            num_frames,
            samplerate=self.cfg.fs,
            channels=dev_ch,
            device=self.cfg.input_device,
            dtype='float32',
        )

        self._sd.wait()
        print(f"    Done.")

        # 录制全部通道, 提取需要的子集 (与 _play_and_record 策略一致)
        recording = np.asarray(rec, dtype=np.float64).T  # (dev_ch, N)
        recording = recording[input_channels, :]

        # 保存为多通道 WAV
        wav_data = (recording.T * 32767).astype(np.int16)
        wavfile.write(output_path, self.cfg.fs, wav_data)
        print(f"    Saved: {output_path}")
        print(f"    Shape: {recording.shape} (channels={num_in}, samples={num_frames})")

        return recording, output_path

    def deconvolve_from_recording(
        self,
        recording: np.ndarray,
        mic_channels=None,
        ir_length=None,
    ) -> np.ndarray:
        """从录制数据中离线反卷积提取多通道脉冲响应.

        参数:
            recording:    录制数据 (Ch, N) — 来自 record_only() 或外部录音文件.
            mic_channels: 要处理的麦克风索引列表, 默认全部.
            ir_length:    IR 目标长度, 默认使用 cfg.ir_length.

        返回:
            irs: (num_mic_channels, ir_length) float64 脉冲响应矩阵.
        """
        if ir_length is None:
            ir_length = self.cfg.ir_length
        if mic_channels is None:
            mic_channels = list(range(recording.shape[0]))

        recording_subset = recording[list(mic_channels), :]
        irs = deconvolve_ir(
            recording_subset, self._inv_filter,
            ir_length=ir_length,
        )
        return irs

    def deconvolve_from_file(
        self,
        wav_path: str,
        mic_channels=None,
        ir_length=None,
    ) -> np.ndarray:
        """从录制的 WAV 文件中离线反卷积提取脉冲响应.

        参数:
            wav_path:     录制保存的 .wav 文件路径.
            mic_channels: 要处理的麦克风索引列表, 默认全部.
            ir_length:    IR 目标长度, 默认使用 cfg.ir_length.

        返回:
            irs: (num_mic_channels, ir_length) float64 脉冲响应矩阵.
        """
        fs, data = wavfile.read(wav_path)
        if data.ndim == 1:
            data = data.reshape(1, -1)
        else:
            data = data.T  # (samples, Ch) -> (Ch, samples)
        recording = data.astype(np.float64) / 32767.0

        if fs != self.cfg.fs:
            print(f"  [WARN] WAV sample rate {fs} Hz != config {self.cfg.fs} Hz, "
                  f"results may be inaccurate")

        return self.deconvolve_from_recording(
            recording, mic_channels=mic_channels, ir_length=ir_length)


# ================================================================
# 便捷函数
# ================================================================

def quick_measure_secondary(spk_channels=(0, 1, 2, 3), mic_channels=(0, 1, 2, 3),
                            fs=16000, sweep_duration=5.0, ir_length=1024, **kwargs):
    """一行测量次级路径."""
    cfg = MeasureConfig(fs=fs, sweep_duration=sweep_duration,
                        ir_length=ir_length, **kwargs)
    sess = MeasureSession(cfg)
    return sess.measure_secondary_path(
        spk_channels=spk_channels, mic_channels=mic_channels)


def quick_measure_primary(source_channel=4, mic_channels=(1, 2, 3, 4),
                          ref_mic_channels=(0,),
                          fs=16000, sweep_duration=5.0, ir_length=1024, **kwargs):
    """一行测量主路径."""
    cfg = MeasureConfig(fs=fs, sweep_duration=sweep_duration,
                        ir_length=ir_length, **kwargs)
    sess = MeasureSession(cfg)
    return sess.measure_primary_path(
        source_channel=source_channel, mic_channels=mic_channels,
        ref_mic_channels=ref_mic_channels)
