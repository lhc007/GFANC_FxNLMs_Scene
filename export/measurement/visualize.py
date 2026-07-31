"""
测量结果可视化.

plot_measured_ir():          单次测量 IR 的多通道波形和频谱展示.
compare_measured_vs_simulated(): 实测 IR 与模拟 IR 的对比分析 (用于量化环境建模误差).
"""

import numpy as np
import matplotlib.pyplot as plt
from scipy import signal
from pathlib import Path
from typing import Optional


def plot_measured_ir(
    S_matrix: np.ndarray,
    fs: int = 16000,
    spk_labels: Optional[list] = None,
    mic_labels: Optional[list] = None,
    freq_range: tuple = (20, 4000),
    figsize_per_subplot: tuple = (4, 3),
    save_path: Optional[str] = None,
):
    """
    绘制测量到的多通道次级路径 IR (时域 + 频域).

    参数:
        S_matrix:   (E, S, L) 次级路径矩阵.
        fs:         采样率 (Hz).
        spk_labels: 扬声器标签列表 (长度 S).
        mic_labels: 误差麦克风标签列表 (长度 E).
        freq_range: 频域显示范围 (Hz).
        save_path:  保存路径 (可选).
    """
    E, S, L = S_matrix.shape

    if spk_labels is None:
        spk_labels = [f"SPK_{i}" for i in range(S)]
    if mic_labels is None:
        mic_labels = [f"MIC_{i}" for i in range(E)]

    fig, axes = plt.subplots(
        E, S * 2,
        figsize=(figsize_per_subplot[0] * S * 2, figsize_per_subplot[1] * E),
        squeeze=False,
    )

    for mic_idx in range(E):
        for spk_idx in range(S):
            ir = S_matrix[mic_idx, spk_idx, :]
            t = np.arange(L) / fs * 1000  # ms

            # 时域
            ax_time = axes[mic_idx, spk_idx * 2]
            ax_time.plot(t, ir, linewidth=0.6)
            ax_time.set_xlabel("Time (ms)")
            ax_time.set_ylabel("Amplitude")
            ax_time.set_title(f"{spk_labels[spk_idx]} → {mic_labels[mic_idx]}")
            ax_time.grid(True, alpha=0.3)

            # 频域
            ax_freq = axes[mic_idx, spk_idx * 2 + 1]
            f, h = signal.freqz(ir, worN=2048, fs=fs)
            ax_freq.semilogx(f, 20 * np.log10(np.abs(h) + 1e-12), linewidth=0.6)
            ax_freq.set_xlim(freq_range)
            ax_freq.set_xlabel("Frequency (Hz)")
            ax_freq.set_ylabel("Magnitude (dB)")
            ax_freq.set_title(f"{spk_labels[spk_idx]} → {mic_labels[mic_idx]} (Freq)")
            ax_freq.grid(True, alpha=0.3)

    fig.suptitle(
        f"Measured Secondary Paths ({E} mics × {S} speakers, L={L})",
        fontsize=14, y=1.01,
    )
    plt.tight_layout()

    if save_path:
        plt.savefig(save_path, dpi=150, bbox_inches='tight')
        print(f"图表已保存至: {save_path}")
    plt.show()


def compare_measured_vs_simulated(
    measured: np.ndarray,
    simulated: np.ndarray,
    fs: int = 16000,
    channel_pairs: Optional[list] = None,
    freq_range: tuple = (20, 7500),
    save_path: Optional[str] = None,
):
    """
    对比实测 IR 与模拟 IR (幅频 + 相频 + 时域波形).

    用于量化仿真模型 (Room_impulse_response.py) 与实际物理环境的偏差,
    帮助评估不同窗户开角下次级路径的变化幅度.

    参数:
        measured:     实测路径 (E, S, L) 或 (L,).
        simulated:    模拟路径, 形状与 measured 相同.
        fs:           采样率.
        channel_pairs: 要对比的通道对列表, 如 [(0,0,'SPK_L→MIC_1')].
                       None = 自动选择前 4 个非零通道.
        freq_range:   频域显示范围.
        save_path:    保存路径 (可选).
    """
    # 统一形状
    measured = np.atleast_3d(measured)
    simulated = np.atleast_3d(simulated)
    E, S, _ = measured.shape

    if channel_pairs is None:
        channel_pairs = []
        for mic_idx in range(min(E, 2)):
            for spk_idx in range(min(S, 2)):
                channel_pairs.append((mic_idx, spk_idx, f"MIC{mic_idx}←SPK{spk_idx}"))

    n_pairs = len(channel_pairs)
    fig, axes = plt.subplots(n_pairs, 3, figsize=(14, 2.5 * n_pairs), squeeze=False)

    for row, (mic, spk, label) in enumerate(channel_pairs):
        ir_meas = measured[mic, spk, :]
        ir_sim = simulated[mic, spk, :]
        t = np.arange(len(ir_meas)) / fs * 1000

        # 列 1: 时域波形对比
        ax = axes[row, 0]
        ax.plot(t, ir_meas, 'b-', linewidth=0.6, alpha=0.8, label='Measured')
        ax.plot(t, ir_sim, 'r--', linewidth=0.6, alpha=0.8, label='Simulated')
        ax.set_xlabel("Time (ms)")
        ax.set_ylabel("Amplitude")
        ax.set_title(f"{label} — Time Domain")
        ax.legend(fontsize=7)
        ax.grid(True, alpha=0.3)

        # 列 2: 幅频响应对比
        ax = axes[row, 1]
        f_m, h_m = signal.freqz(ir_meas, worN=2048, fs=fs)
        f_s, h_s = signal.freqz(ir_sim, worN=2048, fs=fs)
        ax.semilogx(f_m, 20 * np.log10(np.abs(h_m) + 1e-12), 'b-', linewidth=0.6, label='Measured')
        ax.semilogx(f_s, 20 * np.log10(np.abs(h_s) + 1e-12), 'r--', linewidth=0.6, label='Simulated')
        ax.set_xlim(freq_range)
        ax.set_xlabel("Frequency (Hz)")
        ax.set_ylabel("Magnitude (dB)")
        ax.set_title(f"{label} — Magnitude")
        ax.legend(fontsize=7)
        ax.grid(True, alpha=0.3)

        # 列 3: 相频响应对比
        ax = axes[row, 2]
        ax.semilogx(f_m, np.unwrap(np.angle(h_m)), 'b-', linewidth=0.6, label='Measured')
        ax.semilogx(f_s, np.unwrap(np.angle(h_s)), 'r--', linewidth=0.6, label='Simulated')
        ax.set_xlim(freq_range)
        ax.set_xlabel("Frequency (Hz)")
        ax.set_ylabel("Phase (rad)")
        ax.set_title(f"{label} — Phase")
        ax.legend(fontsize=7)
        ax.grid(True, alpha=0.3)

        # 计算并显示差异指标
        mse_db = 10 * np.log10(np.mean((ir_meas - ir_sim) ** 2) / (np.mean(ir_sim ** 2) + 1e-12))
        print(f"  {label}: 归一化 MSE = {mse_db:.1f} dB")

    fig.suptitle("Measured vs Simulated Impulse Response Comparison", fontsize=14, y=1.01)
    plt.tight_layout()

    if save_path:
        plt.savefig(save_path, dpi=150, bbox_inches='tight')
        print(f"对比图表已保存至: {save_path}")
    plt.show()


def plot_window_angle_comparison(
    ir_dict: dict,
    fs: int = 16000,
    spk_idx: int = 0,
    mic_idx: int = 0,
    freq_range: tuple = (20, 4000),
    save_path: Optional[str] = None,
):
    """
    对比不同窗户开角下同一通道对的次级路径变化.

    用于分析窗户开角对次级路径的影响程度, 辅助判断是否需要在线辨识.

    参数:
        ir_dict:  {angle_label: S_matrix} 字典, 如 {'0°': ir_open, '45°': ir_half, '90°': ir_closed}.
        fs:      采样率.
        spk_idx: 要对比的扬声器索引.
        mic_idx: 要对比的麦克风索引.
        freq_range: 频域显示范围.
        save_path: 保存路径.
    """
    angles = list(ir_dict.keys())
    n_angles = len(angles)
    colors = plt.cm.viridis(np.linspace(0, 0.9, n_angles))

    fig, axes = plt.subplots(1, 3, figsize=(16, 4.5))

    for i, angle in enumerate(angles):
        ir = ir_dict[angle][mic_idx, spk_idx, :]
        t = np.arange(len(ir)) / fs * 1000
        f, h = signal.freqz(ir, worN=2048, fs=fs)

        # 时域
        axes[0].plot(t, ir, color=colors[i], linewidth=0.6, alpha=0.8, label=f'{angle}')
        # 幅频
        axes[1].semilogx(f, 20 * np.log10(np.abs(h) + 1e-12), color=colors[i], linewidth=0.6, alpha=0.8)
        # 相频
        axes[2].semilogx(f, np.unwrap(np.angle(h)), color=colors[i], linewidth=0.6, alpha=0.8)

    axes[0].set_xlabel("Time (ms)")
    axes[0].set_ylabel("Amplitude")
    axes[0].set_title(f"IR: SPK{spk_idx} → MIC{mic_idx}")
    axes[0].legend()
    axes[0].grid(True, alpha=0.3)

    axes[1].set_xlim(freq_range)
    axes[1].set_xlabel("Frequency (Hz)")
    axes[1].set_ylabel("Magnitude (dB)")
    axes[1].set_title("Magnitude Response")
    axes[1].grid(True, alpha=0.3)

    axes[2].set_xlim(freq_range)
    axes[2].set_xlabel("Frequency (Hz)")
    axes[2].set_ylabel("Phase (rad)")
    axes[2].set_title("Phase Response")
    axes[2].grid(True, alpha=0.3)

    fig.suptitle(f"Window Angle Comparison — SPK{spk_idx} → MIC{mic_idx}", fontsize=14)
    plt.tight_layout()

    if save_path:
        plt.savefig(save_path, dpi=150, bbox_inches='tight')
        print(f"窗户开角对比图已保存至: {save_path}")
    plt.show()

    # 打印差异统计
    if len(angles) >= 2:
        ref_angle = angles[0]
        ref_ir = ir_dict[ref_angle][mic_idx, spk_idx, :]
        ref_energy = np.mean(ref_ir ** 2)
        print(f"\n  归一化差异 (以 {ref_angle} 为参考):")
        for angle in angles[1:]:
            ir = ir_dict[angle][mic_idx, spk_idx, :]
            diff_db = 10 * np.log10(np.mean((ir - ref_ir) ** 2) / (ref_energy + 1e-12))
            # 频域加权误差 (对 FxNLMS 更相关)
            f_ref, h_ref = signal.freqz(ref_ir, worN=1024, fs=fs)
            f, h = signal.freqz(ir, worN=1024, fs=fs)
            # 相位误差 (低频到中频, ANC 关键频段)
            phase_err = np.mean(np.abs(np.angle(h[:500]) - np.angle(h_ref[:500])))
            phase_err_deg = np.degrees(phase_err)
            print(f"    {angle}: ΔIR = {diff_db:.1f} dB, 平均相角差 = {phase_err_deg:.1f}° "
                  f"({'⚠ 可能影响 FxNLMS' if phase_err_deg > 30 else '✓ 可接受'})")
