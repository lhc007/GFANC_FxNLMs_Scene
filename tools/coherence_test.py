"""Phase-1.5 中频相干性实测 (2026-08-07).

测什么: 窗外参考麦 ↔ 窗内误差麦在 100-800Hz 的相干性 — 决定"窗外 1m 单参考
能否预测窗内胎噪"这个方案命门。

设备: UMC404HD 4入 (ch0=参考麦窗外, ch1-3=误差麦窗内) + sounddevice.

用法:
    # 录音 + 分析 (默认录 120s, 覆盖几辆车经过)
    python tools/coherence_test.py

    # 指定时长/输出文件
    python tools/coherence_test.py --dur 180 --out street_1.wav

    # 只分析已有录音 (不同阈值/频段重看)
    python tools/coherence_test.py --analyze street_1.wav

判定:
    - 车经过时 100-800Hz 相干性 >0.8 → 单参考可行 ✅
    - 0.5-0.8           → 勉强, 需参考阵列或拉近参考
    - <0.5              → 单参考不行, 方案要改 ❌
"""
import os, sys, argparse
import numpy as np
import soundfile as sf
from scipy import signal as sp

# 控制台 UTF-8: 修 Windows GBK 下打印 ⚠/✅ 等字符崩溃
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

FS = 48000
BAND = (100.0, 800.0)   # 目标频段 (中频胎噪)

def find_umc_input():
    import sounddevice as sd
    for i, d in enumerate(sd.query_devices()):
        n = d['name']
        if 'UMC' in n and 'IN 1-4' in n:
            return i
    # 兜底: 第一个 ≥4 入的设备
    for i, d in enumerate(sd.query_devices()):
        if d['max_input_channels'] >= 4:
            return i
    raise SystemExit("未找到 UMC404HD 4入设备")


def record(dur, out_wav):
    import sounddevice as sd
    dev = find_umc_input()
    print(f"录音设备: {sd.query_devices(dev)['name']} (4ch, {FS}Hz, {dur}s)")
    print("摆放: ch0=参考麦(窗外1m朝马路) ch1-3=误差麦(窗内不同位置)")
    print("提示: 保持安静等车经过, 录 2-3 分钟覆盖多辆车...")
    data = sd.rec(int(dur * FS), samplerate=FS, channels=4, device=dev, dtype='float32')
    sd.wait()
    # float32 子类型: 保精度, 避免 16-bit PCM 量化/削波
    sf.write(out_wav, data, FS, subtype='FLOAT')
    print(f"已保存: {out_wav} ({data.shape[0]/FS:.0f}s, FLOAT32)")
    return data


def coherence_welch(x, y, fs, nperseg=4096):
    """两个信号在目标频段的 Welch 相干性 (magnitude-squared coherence).
    nperseg=4096: 0.5s 帧(24000样本)内 ~10 段平均, 相干估计稳定
    (nperseg 过大 → 段数少 → 估计退化, 噪声也报高相干)."""
    f, c = sp.coherence(x, y, fs=fs, nperseg=nperseg, noverlap=nperseg // 2)
    return f, c


def analyze(wav_path, f1, f2, frame_s=0.5, active_pct=10):
    data, fs = sf.read(wav_path, always_2d=True)
    if data.shape[1] < 2:
        raise SystemExit("需至少 2 通道 (ch0=参考, ch1=误差)")
    print(f"\n分析: {wav_path} ({data.shape[0]/fs:.0f}s, {fs}Hz, {data.shape[1]}ch)")

    ref = data[:, 0]
    frame_n = int(frame_s * fs)
    n_frames = data.shape[0] // frame_n
    ref_rms = np.array([np.sqrt(np.mean(ref[i*frame_n:(i+1)*frame_n]**2))
                        for i in range(n_frames)])
    # 活动帧 = 参考有信号 (车经过) — 底噪(低百分位) ×5 作为阈值, 跳过安静段
    # 安静街: 底噪低, 车经过 RMS 高 10-50× → ×5 稳健分离
    floor = np.percentile(ref_rms, active_pct)
    thr = max(floor * 5.0, 1e-4)
    active = ref_rms > thr
    print(f"活动帧 (车经过): {active.sum()}/{n_frames} ({100*active.sum()/n_frames:.0f}%), "
          f"底噪={floor:.4f} → 阈值={thr:.4f}")

    if active.sum() < 3:
        print("⚠️ 活动帧太少 — 录音可能没有车经过, 或阈值太高。用 --active-pct 调低。")
        return

    print(f"\n通道对 (参考 vs 误差) | 100-800Hz 相干性(活动帧中值) | 相干>0.8 的帧占比")
    print("-" * 72)
    for e in range(1, data.shape[1]):
        err = data[:, e]
        c_med = []; c_high = []
        for i in np.where(active)[0]:
            f, c = coherence_welch(ref[i*frame_n:(i+1)*frame_n],
                                   err[i*frame_n:(i+1)*frame_n], fs)
            band = (f >= f1) & (f <= f2)
            cm = float(np.median(c[band]))
            c_med.append(cm)
            c_high.append(cm > 0.8)
        med = float(np.median(c_med))
        frac = 100 * np.mean(c_high)
        verdict = "✅ 可行" if med > 0.8 else ("🟡 勉强" if med > 0.5 else "❌ 不行")
        print(f"  ref↔err{e:<2d}          | {med:>18.3f} | {frac:>14.1f}%   {verdict}")

    print(f"\n判定标准: 车经过时 100-800Hz 相干性中值 >0.8 单参考可行; 0.5-0.8 需参考阵列; <0.5 方案要改")
    print(f"(频段 {f1}-{f2}Hz, 帧 {frame_s}s, 活动阈值 {active_pct} 百分位)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dur', type=float, default=120, help='录音秒数 (默认 120)')
    ap.add_argument('--out', default='street_coherence.wav')
    ap.add_argument('--analyze', metavar='WAV', help='只分析已有 WAV, 不录音')
    ap.add_argument('--f1', type=float, default=BAND[0])
    ap.add_argument('--f2', type=float, default=BAND[1])
    ap.add_argument('--active-pct', type=float, default=10, help='底噪百分位 (默认 10, 阈值=底噪×5)')
    args = ap.parse_args()

    if args.analyze:
        analyze(args.analyze, args.f1, args.f2)
    else:
        data = record(args.dur, args.out)
        analyze(args.out, args.f1, args.f2)


if __name__ == '__main__':
    main()
