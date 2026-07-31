"""
声学路径测量模块 — 指数扫频法 (Farina Method).

提供业界标准的指数正弦扫频信号生成、反卷积脉冲响应提取、
以及主/次级路径的完整测量流程.

快速开始:
    >>> from gfanc.measurement import quick_measure_secondary, print_device_list
    >>> print_device_list()          # 列出可用音频设备
    >>> S = quick_measure_secondary()  # 测量次级路径

完整测量流程:
    >>> from gfanc.measurement import MeasureSession, MeasureConfig
    >>> cfg = MeasureConfig(fs=16000, sweep_duration=5.0)
    >>> sess = MeasureSession(cfg)
    >>> sess.calibrate_latency()                  # 首次使用必须校准
    >>> S = sess.measure_secondary_path(
    ...     spk_channels=[0, 1, 2, 3],            # ANC 系统自带扬声器
    ...     mic_channels=[1, 2, 3, 4],             # 误差麦克风
    ... )
    >>> sess.validate_measurement_quality(S, "Secondary")  # 质量检验
    >>> # 主路径: 使用外部扬声器在噪声源位置播放
    >>> P = sess.measure_primary_path(
    ...     source_channel=4,                      # 外部扬声器, 非 ANC 扬声器!
    ...     mic_channels=[1,2,3,4],
    ...     ref_mic_channels=[0])
    >>> # 多角度主路径: 模拟不同方向噪声源
    >>> multi_P = sess.measure_primary_path_multi_angle(
    ...     source_channel=4,
    ...     angle_configs=[
    ...         {'angle_deg': 0,  'distance_m': 3.0, 'label': '0deg'},
    ...         {'angle_deg': 45, 'distance_m': 3.0, 'label': '45deg'},
    ...         {'angle_deg': 90, 'distance_m': 3.0, 'label': '90deg'},
    ...     ])
"""

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

from .measure_paths import (
    MeasureConfig,
    MeasureSession,
    quick_measure_secondary,
    quick_measure_primary,
)

from .visualize import (
    plot_measured_ir,
    compare_measured_vs_simulated,
)

__all__ = [
    # 扫频信号
    'generate_sweep',
    'deconvolve_ir',
    'deconvolve_ir_aligned',
    'measure_snr_from_ir',
    'measure_coherence',
    'check_causality',
    'check_phase_linearity',
    'sound_speed',
    # 设备管理
    'list_audio_devices',
    'print_device_list',
    # 测量
    'MeasureConfig',
    'MeasureSession',
    'quick_measure_secondary',
    'quick_measure_primary',
    # 可视化
    'plot_measured_ir',
    'compare_measured_vs_simulated',
]
