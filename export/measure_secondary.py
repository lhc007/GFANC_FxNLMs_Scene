#!/usr/bin/env python3
# Fix encoding for Windows terminals that default to GBK
import sys, os
if sys.platform == 'win32':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

"""
=============================================================================
  Secondary Path Measurement — 次级路径测量
  使用 ASIO 设备统一播放+录制, 一步完成测量+反卷积
=============================================================================

  初次使用:
    python scripts/measure_secondary.py --interactive    # 配置设备

  日常测量:
    python scripts/measure_secondary.py                  # 使用已保存配置
    python scripts/measure_secondary.py --duration 8 --repetitions 4 --amplitude 0.9

  配套初级路径:
    python scripts/measure_primary.py --source-channel 0
"""

import argparse
import json
import textwrap
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from measurement import MeasureSession, MeasureConfig


# ═══════════════════════════════════════════════════════════
# 交互式设备配置
# ═══════════════════════════════════════════════════════════

CONFIG_DIR = 'Primary and Secondary Path'
CONFIG_FILE = os.path.join(CONFIG_DIR, 'measurement_config.json')


def _validate_input_device_early(device_id: int):
    try:
        import sounddevice as sd
        dev_info = sd.query_devices(device_id)
        if 'WDM-KS' in str(dev_info.get('hostapi', '')):
            print(f"  [警告] 设备 {device_id} ('{dev_info['name']}') 是 WDM-KS, "
                  f"建议改用同名的 MME/DirectSound 设备.")
    except Exception:
        pass


def interactive_setup(show_all=False):
    """交互式发现音频设备并生成配置文件."""
    from measurement import print_device_list

    hostapi_filter = None if show_all else "ASIO,WASAPI"
    print_device_list(hostapi_filter=hostapi_filter,
                      include_multichannel=show_all)

    has_asio = False
    try:
        import sounddevice as sd
        for i in range(len(sd.query_hostapis())):
            if 'ASIO' in sd.query_hostapis(i)['name']:
                has_asio = True
                break
    except Exception:
        pass

    print("\n请根据上方设备列表回答以下问题 (留空使用默认):")
    if has_asio:
        print("  [提示] ASIO 设备推荐用于声学测量.")
    print()

    # 输入设备
    try:
        in_dev = input("  录音设备 ID: ").strip()
        input_device = int(in_dev) if in_dev else None
    except (ValueError, EOFError):
        input_device = None
    if input_device is not None:
        _validate_input_device_early(input_device)

    # 输出设备
    output_device_map = {}
    try:
        out_dev = input("  播放设备 ID (留空=系统默认): ").strip()
        output_device = int(out_dev) if out_dev else None
    except (ValueError, EOFError):
        output_device = None

    use_map = input("\n  扬声器是否跨多个设备? (y/n, 默认 n): ").strip().lower()
    if use_map == 'y':
        for ch in range(4):
            try:
                dev = input(f"    SPK ch{ch} → 设备 ID (留空=默认): ").strip()
                if dev:
                    output_device_map[ch] = int(dev)
            except (ValueError, EOFError):
                pass

    # 通道映射
    print("\n  麦克风布局: ch0=参考, ch1-3=误差")
    mic_str = input("  误差麦克风通道号 (逗号分隔, 默认 1,2,3): ").strip()
    mic_channels = [int(x) for x in mic_str.split(',')] if mic_str else [1, 2, 3]

    ref_str = input("  参考麦克风通道号 (逗号分隔, 默认 0): ").strip()
    ref_mic_channels = tuple(int(x) for x in ref_str.split(',')) if ref_str else (0,)

    spk_str = input("  扬声器通道号 (逗号分隔, 默认 0,1): ").strip()
    spk_channels = [int(x) for x in spk_str.split(',')] if spk_str else [0, 1]

    # 扫频参数
    print("\n  扫频参数 (默认值直接回车):")
    try:
        import sounddevice as _sd
        if input_device is not None:
            dev_info = _sd.query_devices(input_device)
            native_fs = int(dev_info['default_samplerate'])
            print(f"  [提示] 设备 {input_device} 原生采样率: {native_fs} Hz")
        else:
            native_fs = 16000
    except Exception:
        native_fs = 16000

    try:
        fs_str = input(f"  测量采样率 Hz (默认 {native_fs}): ").strip()
        fs = int(fs_str) if fs_str else native_fs
    except (ValueError, EOFError):
        fs = native_fs

    target_fs = 16000 if fs != 16000 else fs

    try:
        dur_str = input("  扫频时长 秒 (默认 5): ").strip()
        sweep_duration = float(dur_str) if dur_str else 5.0
    except (ValueError, EOFError):
        sweep_duration = 5.0

    try:
        reps_str = input("  重复次数 (默认 2): ").strip()
        repetitions = int(reps_str) if reps_str else 2
    except (ValueError, EOFError):
        repetitions = 2

    try:
        amp_str = input("  播放幅度 0~1 (默认 0.7): ").strip()
        sweep_amplitude = float(amp_str) if amp_str else 0.7
    except (ValueError, EOFError):
        sweep_amplitude = 0.7

    # 保存
    config = {
        'input_device': input_device,
        'output_device': output_device,
        'output_device_map': output_device_map,
        'spk_channels': spk_channels,
        'mic_channels': mic_channels,
        'ref_mic_channels': list(ref_mic_channels),
        'fs': fs,
        'target_fs': target_fs,
        'sweep_duration': sweep_duration,
        'sweep_amplitude': sweep_amplitude,
        'repetitions': repetitions,
        'timestamp': datetime.now().isoformat(),
    }
    return config


# ═══════════════════════════════════════════════════════════
# 测量
# ═══════════════════════════════════════════════════════════

def load_config(save_dir):
    if not os.path.exists(CONFIG_FILE):
        return None
    with open(CONFIG_FILE, 'r') as f:
        return json.load(f)


def main():
    parser = argparse.ArgumentParser(
        description='Secondary Path Measurement — 次级路径测量',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python scripts/measure_secondary.py --interactive     # 首次: 配置设备
  python scripts/measure_secondary.py                   # 测量
  python scripts/measure_secondary.py --list-devices    # 列出音频设备
        """,
    )
    parser.add_argument('--interactive', '-i', action='store_true',
                        help='交互式配置设备 (首次使用)')
    parser.add_argument('--list-devices', action='store_true',
                        help='列出音频设备并退出')
    parser.add_argument('--all-devices', action='store_true',
                        help='与 --list-devices 一起使用, 显示全部设备')
    parser.add_argument('--duration', type=float, default=None,
                        help='扫频时长 秒')
    parser.add_argument('--amplitude', type=float, default=None,
                        help='播放幅度 0~1')
    parser.add_argument('--repetitions', type=int, default=None,
                        help='重复次数')
    parser.add_argument('--skip-check', action='store_true',
                        help='跳过通道检查')
    parser.add_argument('--skip-validate', action='store_true',
                        help='跳过质量检验')
    parser.add_argument('--save-dir', type=str, default=CONFIG_DIR,
                        help='输出目录')
    args = parser.parse_args()

    # --list-devices
    if args.list_devices:
        from measurement import print_device_list
        hostapi_filter = None if args.all_devices else "ASIO,WASAPI"
        print_device_list(hostapi_filter=hostapi_filter)
        return

    # --interactive: 配置设备
    if args.interactive:
        config = interactive_setup(show_all=args.all_devices)
        print("\n" + "=" * 60)
        print("  配置摘要:")
        print("=" * 60)
        for k, v in config.items():
            print(f"  {k}: {v}")
        print("=" * 60)
        if input("\n  确认并保存? (y/n): ").strip().lower() != 'n':
            os.makedirs(CONFIG_DIR, exist_ok=True)
            with open(CONFIG_FILE, 'w') as f:
                json.dump(config, f, indent=2)
            print(f"  已保存: {CONFIG_FILE}")
            do_measure = input("\n  立即开始测量? (y/n, 默认 y): ").strip().lower()
            if do_measure == 'n':
                return
        else:
            print("  已取消.")
            return

    # ── 加载配置 ──
    saved = load_config(args.save_dir)
    if saved is None:
        print(f"\n  [错误] 未找到配置文件: {CONFIG_FILE}")
        print(f"  请先运行: python scripts/measure_secondary.py --interactive")
        return

    spk_channels = saved.get('spk_channels', [0, 1])
    mic_channels = saved.get('mic_channels', [1, 2, 3])
    ref_mic_channels = tuple(saved.get('ref_mic_channels', []))
    fs = saved.get('fs', 16000)
    target_fs = saved.get('target_fs', 16000)

    cfg = MeasureConfig(
        fs=fs,
        target_fs=target_fs,
        sweep_duration=args.duration or saved.get('sweep_duration', 5.0),
        sweep_amplitude=args.amplitude or saved.get('sweep_amplitude', 0.7),
        repetitions=args.repetitions or saved.get('repetitions', 2),
        input_device=saved.get('input_device'),
        output_device=saved.get('output_device'),
        output_device_map={int(k): v for k, v in saved.get('output_device_map', {}).items()},
        save_dir=args.save_dir,
    )

    # ── 打印信息 ──
    S = len(spk_channels)
    E = len(mic_channels)
    print("\n" + "=" * 60)
    print("  次级路径测量 (Secondary Path)")
    print("=" * 60)
    print(f"  输入设备: {cfg.input_device}  输出设备: {cfg.output_device}")
    print(f"  扬声器: {spk_channels} → 误差麦: {mic_channels}")
    if ref_mic_channels:
        print(f"  参考麦: {list(ref_mic_channels)}")
    print(f"  扫频: {cfg.f1}~{cfg.f2} Hz, {cfg.sweep_duration}s, "
          f"重复×{cfg.repetitions}, 幅度={cfg.sweep_amplitude}")
    print(f"  采样率: meas@{cfg.fs}Hz → target@{cfg.target_fs}Hz")
    print("=" * 60)
    print()

    sess = MeasureSession(cfg)

    # ── 1. 通道检查 ──
    if not args.skip_check:
        print("█" * 60)
        print("█  步骤 1/3: 通道连通性检查")
        print("█" * 60)
        all_chs = list(mic_channels)
        if ref_mic_channels:
            all_chs.extend(ref_mic_channels)
        status = sess.check_channels(spk_channels=spk_channels, mic_channels=all_chs)
        if not all(status.values()):
            print("\n⚠ 部分通道无信号!")
            if input("\n  是否继续? (y/n): ").strip().lower() != 'y':
                return

    # ── 2. 测量 ──
    print("\n" + "█" * 60)
    print("█  步骤 2/3: 次级路径测量")
    print("█" * 60)
    S_matrix = sess.measure_secondary_path(
        spk_channels=spk_channels,
        mic_channels=mic_channels,
        spk_device_map=cfg.output_device_map if cfg.output_device_map else None,
    )

    # ── 3. 检验 ──
    print("\n" + "█" * 60)
    print("█  步骤 3/3: 质量检验")
    print("█" * 60)
    if not args.skip_validate and S_matrix is not None:
        sess.validate_measurement_quality(S_matrix, "Secondary Path")

    save_path = os.path.join(args.save_dir, 'secondary_path.npy')
    print("\n" + "=" * 60)
    print("  次级路径测量完成!")
    print(f"  形状: {S_matrix.shape if S_matrix is not None else 'N/A'}")
    print(f"  已保存: {save_path}")
    print("=" * 60 + "\n")


if __name__ == '__main__':
    main()
