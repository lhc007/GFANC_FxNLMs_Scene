#!/usr/bin/env python3
"""离线 FxLMS 收敛仿真 — 复现实机逐样本数学 (main_realtime.c 回调 + fxnlms_tick_rt).

验证: step 大小 × 反馈抵消开关 → err 衰减时间常数, 并对照实机日志 anti 平台值.

数据全部取自 data/ (与实机同文件):
  - 物理次级路径: secondary_path_measured.bin (原始尺度, 峰@60 → 接口延迟 135)
  - 模型次级路径: secondary_path.bin (合成, 当前 GFANC_SEC_FILE 所指, 峰@10 → dsp=185)
  - 反馈路径:     feedback_path_{0,1}.bin.disabled (R-50 新标定, 峰@232/236, 有效)
  - 带通:         bandpass_anc.bin (64tap, ref/Fx/err 三处同系数, R-13/R-58-10)
  - 环路延迟:     sec_bulk_delay.bin → 195 样本 (12.2ms)

复现要点 (与 C 一致):
  - step/leak 自动缩放: s_scale=(0.02/Ŝ_rms)²∈[0.1,4], 逐模型计算
  - 模型延迟补偿: dsp_delay = 环路195 − Ŝ峰位 (BUG-2 逻辑)
  - Fx = (Ŝ⊗bp)(ref_anc) 组合滤波; err_meas = bp(err_raw)
  - 反馈/AGC/anti 逐样本顺序 (fb_est 用上一样本 anti, 同 C 因果); Fx/物理路径批处理
  - AGC (env>0.06 压增益), cold_hold 2s (前1s 冻梯度+cap0.12, 后1s cap 线性 0.12→1.0),
    ramp 400ms, ±1.0 钳位, anti-windup (|anti|>1.2 → 200×leak), 自适应 leak
    (anti_rms 连续映射 1→10×, EMA), VS-LMS (err_env/err_base)
  - 块 LMS (M=64, μ×64/块) — 纯音下与逐样本收敛速率等价, 加速 ~30×

用法: python tools/sim_step_sweep.py [freq] [T_sec]
"""
import sys
import numpy as np

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # Windows GBK 控制台
except Exception:
    pass

FS = 16000
E, S, L = 3, 2, 1024
M = 64                      # 块长 (与收敛速率等价, 见头注)
LOOP_DELAY = 195            # sec_bulk_delay.bin 实测环路 @16k
STEP_BASE = 1e-7            # gfanc_types.h 默认 (GFANC_STEP 覆盖前)
LEAK_BASE = 5e-7
S_TARGET = 0.02             # step 自动缩放设计目标 Ŝ_RMS

def rd(f):
    d = open(f, 'rb').read()
    n = len(d) // 4
    return np.frombuffer(d[:4 * n], dtype='<f4').copy()

def peak_norm(x):
    return x / np.max(np.abs(x))

# ── 加载真实文件 (与实机同源; 合成=4浮头+6144数据, 实测=裸6144) ──
# 物理路径保留原始尺度 (真实声学增益, 峰0.165, |H|@250≈0.3 — 实机 anti0.38→err0.04 吻合)
sec_phys = rd('data/secondary_path_measured.bin').reshape(E, S, L).copy()
# 模型 Ŝ 与实机一致 peak→1.0 归一化 (main_realtime.c 1037 行逻辑)
sec_synth = peak_norm(rd('data/secondary_path.bin')[4:].reshape(E, S, L).copy())
bp = rd('data/bandpass_anc.bin')[4:]                       # 64tap
fb = [rd('data/feedback_path_0.bin.disabled')[4:],
      rd('data/feedback_path_1.bin.disabled')[4:]]

# Ŝ 峰位 → dsp_delay 自动补偿 (main_realtime.c BUG-2 逻辑)
def build_h_sec(sec, peak_min):
    dsp = LOOP_DELAY - peak_min
    h = np.zeros((E, S, L + dsp))
    for e in range(E):
        for s in range(S):
            h[e, s, dsp:] = sec[e, s]
    return h

h_sec_synth = build_h_sec(sec_synth, 10)   # 峰@10 → dsp=185
h_sec_phys  = build_h_sec(sec_phys, 64)    # 峰@64 → 接口延迟 131
def g_of(h_sec):                            # 组合 Ŝ⊗bp (R-58-10), 逐路径 (E,S,len)
    gl = len(h_sec[0, 0]) + len(bp) - 1
    g = np.zeros((E, S, gl))
    for e in range(E):
        for s in range(S):
            g[e, s] = np.convolve(h_sec[e, s], bp)
    return g
g_synth = g_of(h_sec_synth)
g_phys  = g_of(h_sec_phys)

def s_scale_of(sec):
    s_rms = float(np.sqrt(np.mean(sec ** 2)))
    return min(4.0, max(0.1, (S_TARGET / s_rms) ** 2))

def fir_block(h, tail, x_block):
    """状态化 FIR 批处理: y[n]=Σ_k h[k]·x[n-k]. tail=上块末 len(h)-1 样本 (chrono)."""
    x_long = np.concatenate([tail, x_block])
    w = np.lib.stride_tricks.sliding_window_view(x_long, len(h))[:len(x_block)]
    y = w @ h[::-1]
    return y, x_long[-(len(h) - 1):]


def run(freq, step_base, fb_on, model='synth', T_sec=60.0, wc_init=None):
    """逐块 MIMO FxNLMS. 返回 (err_rms_per_sec, anti_rms_per_sec). wc_init=(S,L) 可种子 CNN init Wc."""
    # 模型 Ŝ 始终 peak→1.0 归一化 (同实机加载逻辑); 物理路径保持原始尺度
    if model == 'synth':
        h_sec, g = h_sec_synth, g_synth
        m_rms = s_scale_of(sec_synth)
    else:
        sec_norm = peak_norm(sec_phys)
        h_sec, g = build_h_sec(sec_norm, 64), g_of(build_h_sec(sec_norm, 64))
        m_rms = s_scale_of(sec_norm)
    scale = m_rms
    step = step_base * scale          # 自动缩放 (GFANC_STEP 语义)
    leak = LEAK_BASE * scale

    N = int(T_sec * FS)
    n_blocks = N // M
    t = np.arange(N) / FS
    w = 2 * np.pi * freq
    dist = 0.083 * np.sqrt(2.0) * np.sin(w * t + 0.3 * np.arange(E)[:, None])  # RMS=0.083 (实机 INIT err)
    ref_noise = 0.048 * np.sin(w * t)

    # ── 状态 ──
    xd_buf = np.zeros((E, S, L))         # Fx 末 L 样本 (chrono)
    ref_ring = np.zeros(len(bp))         # ref_sample 历史 (最新[0])
    xh_ring = np.zeros(L)                # ref_anc 历史 (最新[0])
    fb_ring = [np.zeros(len(fb[0])), np.zeros(len(fb[1]))]   # anti 历史 (最新[0])
    g_tail = [[np.zeros(len(g[0, 0]) - 1) for _ in range(S)] for _ in range(E)]
    phys_tail = [[np.zeros(len(h_sec_phys[e, s]) - 1) for s in range(S)] for e in range(E)]
    errbp_tail = [np.zeros(len(bp) - 1) for _ in range(E)]
    wc = np.zeros((S, L)) if wc_init is None else wc_init.astype(np.float64).copy()
    err_env = err_base = 1e-4
    leak_ema = 1.0
    anti_rms_prev = 0.0
    ref_env = 0.0
    cold_hold = 2 * FS
    ramp_cnt = int(0.4 * FS)             # ramp_ms=400

    err_rms = np.zeros(int(T_sec)); anti_rms = np.zeros(int(T_sec))
    acc_err = acc_anti = 0.0; sec = 0

    for nb in range(n_blocks):
        n0 = nb * M
        ref_anc_block = np.zeros(M)
        anti_raw_all = np.zeros((M, S)); anti_fin = np.zeros((M, S))

        # ── 顺序段 (同 C 因果): fb_est(用上样本 anti) → AGC → bp → anti → 钳位 → fb 更新 ──
        for i in range(M):
            n = n0 + i
            if fb_on:
                fb_est = float(np.dot(fb[0], fb_ring[0]) + np.dot(fb[1], fb_ring[1]))
            else:
                fb_est = 0.0
            ra = abs(ref_noise[n] - fb_est)
            ref_env += (ra - ref_env) * (0.003 if ra > ref_env else 0.0003)
            agc = 1.0
            if ref_env > 0.06:
                agc = max(0.08, 0.06 / ref_env)
            ref_sample = (ref_noise[n] - fb_est) * agc
            ref_ring[1:] = ref_ring[:-1]; ref_ring[0] = ref_sample
            ref_anc = float(np.dot(bp, ref_ring))
            ref_anc_block[i] = ref_anc

            xh_ring[1:] = xh_ring[:-1]; xh_ring[0] = ref_anc
            a = np.array([float(np.dot(wc[s], xh_ring)) for s in range(S)])
            anti_raw_all[i] = a
            # 冷启动 cap / ±1.0 钳位 / ramp (输出生效路径, 同 C)
            if cold_hold > 0:
                cap = 0.12 if cold_hold > FS else 0.12 + 0.88 * (1.0 - cold_hold / FS)
                a = np.clip(a, -cap, cap)
            a = np.clip(a, -1.0, 1.0)
            if ramp_cnt > 0:
                a *= 1.0 - ramp_cnt / (0.4 * FS)
                ramp_cnt -= 1
            anti_fin[i] = a
            for s in range(S):
                fb_ring[s][1:] = fb_ring[s][:-1]; fb_ring[s][0] = a[s]

        sat = bool(np.any(np.abs(anti_raw_all) > 1.2))

        # ── Fx 块: (Ŝ⊗bp)(ref_anc), 每条 (e,s) 独立状态 ──
        xd_block = np.zeros((E, S, M))
        for e in range(E):
            for s in range(S):
                y, g_tail[e][s] = fir_block(g[e, s], g_tail[e][s], ref_anc_block)
                xd_block[e, s] = y

        # ── 物理路径: err_raw = dist + Σ_s h_phys ⊗ anti_s; err_meas = bp(err_raw) ──
        err_raw = np.zeros((M, E))
        for e in range(E):
            for s in range(S):
                y, phys_tail[e][s] = fir_block(h_sec_phys[e, s], phys_tail[e][s], anti_fin[:, s])
                err_raw[:, e] += y
        err_raw += dist[:, n0:n0 + M].T
        err_meas = np.zeros((M, E))
        for e in range(E):
            y, errbp_tail[e] = fir_block(bp, errbp_tail[e], err_raw[:, e])
            err_meas[:, e] = y

        # ── VS-LMS + 自适应 leak (块等效 EMA) ──
        p = float(np.mean(err_meas ** 2))
        err_env += (p - err_env) * (1.0 - (1.0 - 0.01) ** M)
        err_base += (p - err_base) * (1.0 - (1.0 - 0.002) ** M)
        vs = min(1.0, max(0.05, np.sqrt(err_base + 1e-12) / (np.sqrt(err_env) + 1e-6)))
        if anti_rms_prev > 0.18: mult = 10.0
        elif anti_rms_prev > 0.06: mult = 1.0 + 9.0 * (anti_rms_prev - 0.06) / 0.12
        else: mult = 1.0
        leak_ema += (mult - leak_ema) * (1.0 - (1.0 - 0.001) ** M)

        # ── NLMS 更新 (mean 归一化 + cap 1000, 同 fxnlms_tick_rt) ──
        if cold_hold <= FS:
            xd_full = np.concatenate([xd_buf.reshape(E * S, L),
                                      xd_block.reshape(E * S, M)], axis=1)  # (E*S, L+M)
            power = np.zeros(S)
            for s in range(S):
                power[s] = float(np.mean(xd_buf[:, s, :] ** 2)) + 1e-6
            inv_pwr = np.minimum(1.0 / power, 1000.0)
            lk = leak * 200.0 if sat else leak
            decay = (1.0 - lk * leak_ema) ** M
            for s in range(S):
                grad = np.zeros(L)
                for e in range(E):
                    w_w = np.lib.stride_tricks.sliding_window_view(xd_full[e * S + s], M)
                    grad += w_w[:L][::-1] @ err_meas[:, e]
                wc[s] -= step * M * vs * inv_pwr[s] * grad
                wc[s] *= decay

        # ── 状态推进 + 秒级 RMS ──
        xd_buf = np.concatenate([xd_buf.reshape(E * S, L), xd_block.reshape(E * S, M)],
                                axis=1)[:, M:].reshape(E, S, L)
        if cold_hold > 0:
            cold_hold -= M
        acc_err += float(np.sum(err_raw ** 2)); acc_anti += float(np.sum(anti_fin ** 2))
        if (n0 + M) % FS == 0:
            err_rms[sec] = np.sqrt(acc_err / (FS * E))
            anti_rms[sec] = np.sqrt(acc_anti / (FS * S))
            anti_rms_prev = anti_rms[sec]
            acc_err = acc_anti = 0.0
            sec += 1
    return err_rms, anti_rms


def main():
    freq = float(sys.argv[1]) if len(sys.argv) > 1 else 250.0
    T = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    steps_base = [1e-7, 2e-7, 5e-7, 1e-6, 2e-6]
    scale_s = s_scale_of(sec_synth)
    steps_eff = [b * scale_s for b in steps_base]
    # 反馈 FIR 在音调频率的增益 (解释 FB 效应)
    f_fb = np.fft.rfftfreq(512, 1.0 / FS)
    hfb = [np.abs(np.fft.rfft(np.pad(f, (0, 512 - len(f)))))[np.argmin(abs(f_fb - freq))]
           for f in fb]

    print(f"freq={freq:.0f}Hz  T={T:.0f}s  环路延迟={LOOP_DELAY}样本({LOOP_DELAY*1000/FS:.1f}ms)")
    print(f"合成Ŝ: s_scale={scale_s:.3f} → step={steps_eff[0]:.2e} leak={LEAK_BASE*scale_s:.1e}")
    print(f"物理Ŝ: s_scale={s_scale_of(sec_phys):.3f}")
    print(f"反馈 FIR |H|@{freq:.0f}Hz: spk0={hfb[0]:.3f} spk1={hfb[1]:.3f} "
          f"(anti_rms0.38 → fb_est≈{0.38*np.sqrt(2)*sum(hfb)/2:.3f} vs ref 0.048)")
    print("=" * 96)
    for fb_on in (False, True):
        print(f"\n反馈抵消: {'ON (R-50 标定 FIR)' if fb_on else 'OFF (当前实机)'}")
        print(f"{'t(s)':>5} | " + " | ".join(f"step{s:.1e}" for s in steps_eff) + " | anti(现状)")
        print("-" * 96)
        curves = [run(freq, b, fb_on, 'synth', T) for b in steps_base]
        for tt in range(5, int(T) + 1, 5):
            row = f"{tt:>5} | "
            for er, ar in curves:
                db = 20 * np.log10((er[min(tt, len(er) - 1)] + 1e-12) / 0.083)
                row += f"{db:8.1f} | "
            row += f"{curves[0][1][min(tt, len(curves[0][1]) - 1)]:.3f}"
            print(row)
        er, ar = curves[0]
        print(f"  现状(step{steps_eff[0]:.1e},FB {'ON' if fb_on else 'OFF'}): {T:.0f}s后 "
              f"err={er[-1]:.4f} ({20*np.log10(er[-1]/0.083):.1f}dB) anti={ar[-1]:.3f} "
              f"[实机日志 anti≈0.38]")
        er, ar = curves[-1]
        print(f"  最快(step{steps_eff[-1]:.1e}): {T:.0f}s后 err={er[-1]:.4f} "
              f"({20*np.log10(er[-1]/0.083):.1f}dB) anti={ar[-1]:.3f}")
    # 物理Ŝ 模型 sanity (收敛速率应与模型无关)
    print("\n物理Ŝ模型 (交叉验证, FB ON):")
    for b in steps_base[0::4]:
        er, ar = run(freq, b, True, 'measured', T)
        print(f"  step_base={b:.0e} (eff {b*s_scale_of(sec_phys):.1e}): {T:.0f}s后 "
              f"err={er[-1]:.4f} ({20*np.log10(er[-1]/0.083):.1f}dB)")


if __name__ == '__main__':
    main()
