#!/usr/bin/env python3
"""
=============================================================================
  Primary Path Measurement — 独立初级路径测量
  使用 ASIO 设备统一播放+录制, 一步完成测量+反卷积
=============================================================================

  用法:
    python scripts/measure_primary.py --source-channel 0
    python scripts/measure_primary.py --source-channel 0 --duration 8 --repetitions 4

  前提:
    1. 已运行过 measure_paths_realtime.py --interactive (有保存的配置文件)
    2. 外接扬声器连接到指定 source_channel (从窗框拆下, 搬到室外噪声源位置)
    3. 麦克风阵列连接不变
"""

import sys
import os
import argparse
import json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from measurement import MeasureSession, MeasureConfig


def load_saved_config(save_dir):
    """加载已有配置."""
    config_path = os.path.join(save_dir, 'measurement_config.json')
    if not os.path.exists(config_path):
        print(f"\n  [错误] 未找到配置文件: {config_path}")
        print(f"  请先运行: python scripts/measure_paths_realtime.py --interactive")
        return None
    with open(config_path, 'r') as f:
        return json.load(f)


def main():
    parser = argparse.ArgumentParser(
        description='Primary Path Measurement — 独立初级路径测量',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python scripts/measure_primary.py --source-channel 0
  python scripts/measure_primary.py --source-channel 0 --duration 8 --repetitions 4 --amplitude 0.9
        """,
    )
    parser.add_argument('--source-channel', type=int, required=True,
                        help='外接噪声源扬声器的 ASIO 通道号 (必填)')
    parser.add_argument('--source-device', type=int, default=None,
                        help='外接扬声器设备 ID (默认同 output_device)')
    parser.add_argument('--duration', type=float, default=None,
                        help='扫频时长 秒 (默认使用保存配置)')
    parser.add_argument('--amplitude', type=float, default=None,
                        help='播放幅度 0~1 (默认使用保存配置)')
    parser.add_argument('--repetitions', type=int, default=None,
                        help='重复次数 (默认使用保存配置)')
    parser.add_argument('--save-dir', type=str, default='Primary and Secondary Path',
                        help='输出目录')
    parser.add_argument('--skip-validate', action='store_true',
                        help='跳过质量检验')
    parser.add_argument('--snr-min', type=float, default=20.0,
                        help='SNR 拒绝阈值 dB (默认 20, 初级路径允许更低)')
    parser.add_argument('--phase-rmse-max', type=float, default=2.0,
                        help='相位 RMSE 上限 rad (默认 2.0≈115°, 初级路径允许更大)')
    args = parser.parse_args()

    # ── 加载配置 ──
    saved = load_saved_config(args.save_dir)
    if saved is None:
        return

    mic_channels = saved.get('mic_channels', [1, 2, 3, 4])
    ref_mic_channels = saved.get('ref_mic_channels', [0])
    fs = saved.get('fs', 16000)
    target_fs = saved.get('target_fs', 16000)

    # ── 构建 MeasureConfig (CLI 参数优先) ──
    cfg = MeasureConfig(
        fs=fs,
        target_fs=target_fs,
        sweep_duration=args.duration or saved.get('sweep_duration', 5.0),
        sweep_amplitude=args.amplitude or saved.get('sweep_amplitude', 0.7),
        repetitions=args.repetitions or saved.get('repetitions', 2),
        input_device=saved.get('input_device'),
        output_device=args.source_device or saved.get('output_device'),
        ir_length=saved.get('ir_length', 1024) if 'ir_length' in saved else 1024,
        save_dir=args.save_dir,
    )

    # ── 打印测量信息 ──
    print("\n" + "=" * 60)
    print("  初级路径测量 (Primary Path)")
    print("=" * 60)
    print(f"  输入设备: {cfg.input_device}")
    print(f"  输出设备: {cfg.output_device}")
    print(f"  噪声源通道: {args.source_channel}")
    print(f"  误差麦克风: {mic_channels}")
    print(f"  参考麦克风: {list(ref_mic_channels)}")
    print(f"  扫频: {cfg.f1}~{cfg.f2} Hz, {cfg.sweep_duration}s")
    print(f"  采样率: meas@{cfg.fs}Hz → target@{cfg.target_fs}Hz")
    print(f"  重复次数: {cfg.repetitions}")
    print(f"  幅度: {cfg.sweep_amplitude}")
    print("=" * 60)
    print()
    print("  [重要] 确保外接扬声器已放置在噪声源位置 (室外)。")
    input("  按回车开始测量...")

    # ── 测量 ──
    sess = MeasureSession(cfg)

    print("\n" + "█" * 60)
    print("█  初级路径测量")
    print("█" * 60)

    P_matrix = sess.measure_primary_path(
        source_channel=args.source_channel,
        source_device=args.source_device or saved.get('output_device'),
        mic_channels=mic_channels,
        ref_mic_channels=ref_mic_channels,
        source_distance_m=3.0,
        source_label="noise_source",
    )

    # ── 质量检验 ──
    if not args.skip_validate and P_matrix is not None:
        print("\n" + "█" * 60)
        print("█  质量检验")
        print("█" * 60)
        sess.validate_measurement_quality(
            P_matrix, "Primary Path",
            snr_min_reject=args.snr_min,
            phase_rmse_max_rad=args.phase_rmse_max,
        )

    # ── 完成 ──
    save_path = os.path.join(args.save_dir, 'primary_path.npy')
    print("\n" + "=" * 60)
    print("  初级路径测量完成!")
    print(f"  形状: {P_matrix.shape if P_matrix is not None else 'N/A'}")
    print(f"  已保存: {save_path}")
    print("=" * 60 + "\n")


if __name__ == '__main__':
    main()
