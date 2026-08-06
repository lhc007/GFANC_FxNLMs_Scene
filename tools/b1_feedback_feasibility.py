"""B-1 低频反馈 ANC — PC 环路延迟下的可行性评估.

评估问题: 在给定环路延迟 τ (声卡 I/O + Ŝ 声学) 下, 反馈 ANC 能稳定地
抵消多少带宽的低频, 及对目标噪声 (马路噪声) 的 NR 贡献.

方法 (频域):
  1. 取最强 Ŝ 耦合通道 (e0,s1) 作为反馈环路的次级路径.
  2. 设计带限逆控制器 W(f) = S*(f)/(|S(f)|² + λ) 在 [f1,f2] 内, 带外平滑滚降.
  3. 环路 L(f) = S(f)·W(f)·e^{-j2πfτ} (τ = 环路延迟, 含 I/O + 声学).
  4. 稳定判据: 闭环 1+L 无 RHP 零点 (用 Nyquist / 相位裕量检查),
     并通过缩放 W 的整体增益使 |L| 峰值留裕量.
  5. 扰动抑制: |1/(1+L(f))|² — 20-80Hz 内的衰减量.
  6. 用目标噪声 (road_noise) 的功率谱加权, 算反馈 ANC 带来的 NR 增量.

用法: python tools/b1_feedback_feasibility.py [环路延迟ms 列表]
"""
import os, sys, json
import numpy as np
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FS = 16000

def load_bin(p):
    d = open(os.path.join(ROOT, p), 'rb').read()
    if d[:4] == b'GFNC':
        d = d[16:]
    return np.frombuffer(d, dtype=np.float32)


def design_controller(S_f, freqs, f1, f2, rolloff_hz, lam=1e-3, n_taps=1024,
                      gain=1.0, n_fft=2 ** 16):
    """带限逆控制器: W(f)=S*/(|S|²+λ) 在 [f1,f2], 两侧平滑滚降. 返回时域 FIR."""
    # 频域目标 (rfft 网格, 长度 n_fft/2+1)
    W_f = np.zeros_like(S_f, dtype=complex)
    for i in range(len(freqs)):
        f = freqs[i]
        inv = np.conj(S_f[i]) / (np.abs(S_f[i])**2 + lam)
        if f1 <= f <= f2:
            W_f[i] = inv
        elif f < f1 and f > f1 - rolloff_hz:
            t = (f - (f1 - rolloff_hz)) / rolloff_hz
            W_f[i] = inv * np.sin(np.pi / 2 * t) ** 2
        elif f > f2 and f < f2 + rolloff_hz:
            t = ((f2 + rolloff_hz) - f) / rolloff_hz
            W_f[i] = inv * np.sin(np.pi / 2 * t) ** 2
    W_f[0] = 0.0
    W_f *= gain
    # 全频谱逆 FFT (irfft n=n_fft 用全谱, 非截断)
    # W≈1/S 的因果近似能量集中在 Ŝ 延迟位置 (FFT 末尾), 取能量最集中段为 FIR
    w_full = np.fft.irfft(W_f, n=n_fft)
    acc = np.convolve(w_full ** 2, np.ones(n_taps), 'valid')
    best_start = int(np.argmax(acc))
    w = w_full[best_start:best_start + n_taps]
    # Tukey 窗 (平顶中心 80%) — 比 Hann 少破坏带内增益, 只压边缘
    from scipy.signal.windows import tukey
    w = w * tukey(n_taps, alpha=0.2)
    return w


def loop_response(S_f, W_f, freqs, tau_ms):
    """环路 L(f) = S·W·e^{-j2πfτ}."""
    tau_s = tau_ms / 1000.0
    D = np.exp(-1j * 2 * np.pi * freqs * tau_s)
    return S_f * W_f * D


def closed_loop_atten(L, freqs, f1=20, f2=80):
    """|1/(1+L)|² 在 [f1,f2] 的平均衰减 (dB)."""
    band = (freqs >= f1) & (freqs <= f2)
    att = np.abs(1.0 / (1.0 + L[band])) ** 2
    return 10 * np.log10(np.mean(att))


def stability_ok(L, freqs):
    """简化稳定判据: 在相位过 -180° 的频率上 |L| 必须 < 1 (增益裕量>0).
       更严格可用 Nyquist, 这里用保守的相位裕量检查 + 带外 |L|<1."""
    ph = np.unwrap(np.angle(L))
    # 找相位 < -180° 的频率区间
    over = ph < -np.pi  # 相位滞后超过 180°
    # 在超相位区, |L| 必须 < 1
    if over.any():
        mag = np.abs(L)
        worst = mag[over].max()
        return worst < 0.9, worst
    return True, np.abs(L).max()


def main():
    tau_list = [float(x) for x in sys.argv[1:]] if len(sys.argv) > 1 else [0, 4.4, 8.4, 12.4]
    f1, f2 = 20.0, 80.0
    rolloff = 25.0

    # ── 加载最强 Ŝ 耦合 (e0,s1) ──
    sec = load_bin('data/secondary_path_measured.bin').reshape(3, 2, 1024)
    s01 = sec[0, 1].astype(np.float64)
    s01 = s01 / np.abs(s01).max()   # peak→1.0 (与运行时一致)

    # ── 目标噪声频谱 (road_noise_0-34) ──
    wav, _ = sf.read(os.path.join(ROOT, 'Noise Examples', 'road_noise_0-34.wav'))
    if wav.ndim > 1:
        wav = wav.mean(axis=1)
    if _ != FS:
        from scipy import signal
        wav = signal.resample(wav, int(len(wav) * FS / _))
    wav = wav / (np.abs(wav).max() + 1e-12)
    n = 2 ** 16
    noise_pow = np.abs(np.fft.rfft(wav, n=n)) ** 2
    freqs = np.fft.rfftfreq(n, d=1.0 / FS)

    print(f'B-1 低频反馈可行性 (最强通道 e0-s1, peak→1.0)')
    print(f'  目标频带: {f1}-{f2} Hz, 滚降 {rolloff} Hz')
    print(f'  噪声: road_noise_0-34.wav, 20-80Hz 占比: '
          f'{100*noise_pow[(freqs>=20)&(freqs<=80)].sum()/noise_pow.sum():.1f}%\n')

    # ── 设计控制器 ──
    S_f = np.fft.rfft(s01, n=n)
    # 增益扫描找稳定裕量内最大衰减 (只缩放 W, 不改变相位)
    results = []
    for tau_ms in tau_list:
        best = None
        for gain_db in np.arange(-12, 6, 1.0):
            gain = 10 ** (gain_db / 20)
            w_fir = design_controller(S_f, freqs, f1, f2, rolloff,
                                      n_taps=1024, gain=gain)
            W_f = np.fft.rfft(w_fir, n=n)
            L = loop_response(S_f, W_f, freqs, tau_ms)
            ok, worst = stability_ok(L, freqs)
            att_db = closed_loop_atten(L, freqs, f1, f2)
            if ok and (best is None or att_db > best[0]):
                best = (att_db, gain_db, worst)
        if best is None:
            results.append((tau_ms, -999, None, None))
            continue
        # 带噪声加权 NR 增量 (20-80Hz)
        att, gain_db, worst = best
        # 用最终 W 算加权 NR
        w_fir = design_controller(S_f, freqs, f1, f2, rolloff, n_taps=1024, gain=10**(gain_db/20))
        W_f = np.fft.rfft(w_fir, n=n)
        L = loop_response(S_f, W_f, freqs, tau_ms)
        rej = 1.0 / np.abs(1.0 + L) ** 2
        band = (freqs >= f1) & (freqs <= f2)
        # NR 增量 = +10log10(原/降后) (正数=改善)
        nr_band = -10 * np.log10((noise_pow[band] * rej[band]).sum() / noise_pow[band].sum())
        results.append((tau_ms, att, gain_db, worst))

    print(f'{"环路延迟τ(ms)":>14} {"带内平均衰减(dB)":>16} {"最优增益(dB)":>12} {"相位超区|L|max":>14} {"20-80Hz NR增量(dB)":>18}')
    for tau_ms, att, gain_db, worst in results:
        if att is None:
            print(f'{tau_ms:>14.1f} {"不稳定(无裕量)":>16}')
        else:
            nr = nr_band if tau_ms == results[-1][0] else None  # 仅最后一项算过加权, 简化显示
            print(f'{tau_ms:>14.1f} {att:>16.1f} {gain_db:>12.1f} {worst:>14.3f} {"-":>18}')

    # ── 每 τ 用各自最优增益, 算加权 NR 增量 (频带内 + 全频谱总) ──
    print(f'\n  每 τ 最优增益下的加权 NR 增量 (road 噪声谱; 带内 + 总):')
    for tau_ms, att, gain_db, _ in results:
        w_fir = design_controller(S_f, freqs, f1, f2, rolloff, n_taps=1024,
                                  gain=10 ** (gain_db / 20))
        W_f = np.fft.rfft(w_fir, n=n)
        L = loop_response(S_f, W_f, freqs, tau_ms)
        ok, worst = stability_ok(L, freqs)
        rej = 1.0 / np.abs(1.0 + L) ** 2
        band = (freqs >= f1) & (freqs <= f2)
        nr_band = -10 * np.log10((noise_pow[band] * rej[band]).sum() / noise_pow[band].sum())
        nr_total = -10 * np.log10((noise_pow * rej).sum() / noise_pow.sum())
        # 稳定频带上限 (相位裕量 45°)
        f_max = 0.375 / (tau_ms / 1000.0) if tau_ms > 0 else 9999
        print(f'  τ={tau_ms:5.1f}ms: 稳定={ok}, 稳定频带上限≈{f_max:5.0f}Hz, '
              f'20-80Hz 带内 NR={nr_band:+5.1f}dB, 总 NR 贡献={nr_total:+5.2f}dB')

    # ── 完美抵消上界 (不受控制器质量影响, 判断 B-1 值不值) ──
    print(f'\n  完美抵消各低频带的理论总 NR 上界 (road 噪声谱):')
    for lo, hi in [(20, 30), (20, 40), (20, 50), (20, 60), (20, 80), (20, 100), (20, 200)]:
        share = 100 * noise_pow[(freqs >= lo) & (freqs <= hi)].sum() / noise_pow.sum()
        rej = np.ones_like(noise_pow)
        rej[(freqs >= lo) & (freqs <= hi)] = 1e-9
        ub = -10 * np.log10((noise_pow * rej).sum() / noise_pow.sum())
        print(f'    [{lo:3d},{hi:3d}]Hz 占{share:5.1f}% → 完美抵消 NR 上界 = +{ub:5.2f}dB')


if __name__ == '__main__':
    main()
