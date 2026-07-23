/** howling_detect — DFT 频谱峰值检测 + IIR 陷波滤波器 实现 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "howling_detect.h"

/* ══════════════════════════════════════════════════════════
   预计算 DFT 旋转因子 (静态, 编译期初始化)
   ══════════════════════════════════════════════════════════ */
static float dft_cos[HW_MAX_BIN + 1][HW_FFT_N];
static float dft_sin[HW_MAX_BIN + 1][HW_FFT_N];
static int   dft_ready = 0;

static void dft_init(void)
{
    if (dft_ready) return;
    for (int k = HW_MIN_BIN; k <= HW_MAX_BIN; k++) {
        for (int n = 0; n < HW_FFT_N; n++) {
            float theta = -2.0f * 3.14159265f * k * n / HW_FFT_N;
            dft_cos[k][n] = cosf(theta);
            dft_sin[k][n] = sinf(theta);
        }
    }
    dft_ready = 1;
}

/* ══════════════════════════════════════════════════════════
   汉宁窗 (减少频谱泄漏, 提高峰值检测精度)
   ══════════════════════════════════════════════════════════ */
static void apply_hanning(float *buf, int N)
{
    for (int i = 0; i < N; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (N - 1)));
        buf[i] *= w;
    }
}

/* ══════════════════════════════════════════════════════════
   计算指定 bin 的 DFT 功率 (|X[k]|²)
   ══════════════════════════════════════════════════════════ */
static float dft_power(const float *buf, int k)
{
    float re = 0, im = 0;
    for (int n = 0; n < HW_FFT_N; n++) {
        re += buf[n] * dft_cos[k][n];
        im += buf[n] * dft_sin[k][n];
    }
    return re * re + im * im;
}

/* ══════════════════════════════════════════════════════════
   计算陷波器系数
   f0: 中心频率 (Hz), fs: 采样率, r: 带宽参数 (0.9~0.99)
   ══════════════════════════════════════════════════════════ */
static void notch_design(float f0, float fs, float r,
                         float *b1_out, float *a1_out, float *a2_out)
{
    float w0 = 2.0f * 3.14159265f * f0 / fs;
    *b1_out = -2.0f * cosf(w0);       /* b0=1, b1=-2cos(w0), b2=1 */
    *a1_out = -2.0f * r * cosf(w0);
    *a2_out = r * r;
}

/* ══════════════════════════════════════════════════════════
   应用 IIR 陷波器
   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
   b0=1, b2=1 (固定)
   ══════════════════════════════════════════════════════════ */
static float notch_apply(float x, float b1, float a1, float a2,
                         float *x1, float *x2, float *y1, float *y2)
{
    float y = x + b1 * (*x1) + (*x2) - a1 * (*y1) - a2 * (*y2);
    *x2 = *x1;
    *x1 = x;
    *y2 = *y1;
    *y1 = y;
    return y;
}

/* ══════════════════════════════════════════════════════════
   初始化
   ══════════════════════════════════════════════════════════ */
void howling_init(howling_detect_t *hw, int enabled)
{
    memset(hw, 0, sizeof(*hw));
    hw->enabled = enabled;
    if (enabled) dft_init();
}

/* ══════════════════════════════════════════════════════════
   检测一帧: DFT → 找峰值 → 判断是否为啸叫
   返回检测到的啸叫频率 (Hz), 0=无
   ══════════════════════════════════════════════════════════ */
static float detect_frame(howling_detect_t *hw, const float *frame, float fs)
{
    /* 计算所有 bin 功率 */
    float powers[HW_MAX_BIN + 1];
    float mean_pwr = 0;
    int n_bins = 0;

    for (int k = HW_MIN_BIN; k <= HW_MAX_BIN; k++) {
        powers[k] = dft_power(frame, k);
        mean_pwr += powers[k];
        n_bins++;
    }
    mean_pwr /= (float)n_bins;
    if (mean_pwr < 1e-12f) return 0;  /* 太安静, 不可能啸叫 */

    /* 找最大功率 bin 及其峰均值比 */
    int    peak_bin = HW_MIN_BIN;
    float  peak_pwr = powers[HW_MIN_BIN];
    for (int k = HW_MIN_BIN + 1; k <= HW_MAX_BIN; k++) {
        if (powers[k] > peak_pwr) { peak_pwr = powers[k]; peak_bin = k; }
    }

    float peak_db = 10.0f * log10f(peak_pwr / mean_pwr);

    /* 更新监控数据 */
    hw->dominant_freq = (float)peak_bin * fs / HW_FFT_N;
    hw->dominant_db   = peak_db;

    /* 峰值显著高于均值 → 候选啸叫 */
    if (peak_db > HW_THRESH_DB)
        return (float)peak_bin * fs / HW_FFT_N;

    return 0;  /* 无显著窄带峰值 */
}

/* ══════════════════════════════════════════════════════════
   检查频率是否已在激活列表中
   ══════════════════════════════════════════════════════════ */
static int freq_in_list(float freq, const float *list, int count)
{
    /* 容差: ±1 bin 宽度视为同一频率 */
    float tol = 16000.0f / HW_FFT_N;
    for (int i = 0; i < count; i++)
        if (fabsf(freq - list[i]) < tol * 1.5f) return 1;
    return 0;
}

/* ══════════════════════════════════════════════════════════
   添加陷波器
   ══════════════════════════════════════════════════════════ */
static int add_notch(howling_detect_t *hw, float freq)
{
    if (hw->active_count >= HW_MAX_NOTCHES) return -1;
    int idx = hw->active_count;
    notch_design(freq, 16000.0f, HW_NOTCH_R,
                 &hw->b1[idx], &hw->a1[idx], &hw->a2[idx]);
    for (int s = 0; s < HW_S; s++) {
        hw->x1[s][idx] = hw->x2[s][idx] = 0;
        hw->y1[s][idx] = hw->y2[s][idx] = 0;
    }
    hw->active_freqs[idx] = freq;
    hw->notch_age[idx] = 0;  /* 新陷波器从0开始计数 */
    hw->active_count++;
    return idx;
}

/* ══════════════════════════════════════════════════════════
   移除单个陷波器 (将最后一个移到当前位置)
   预留: 当前释放路径直接 active_count=0 批量清除,
   后续反馈环路需逐频率管理时启用此函数.
   ══════════════════════════════════════════════════════════ */
static void remove_notch(howling_detect_t *hw, int idx)
{
    if (idx < 0 || idx >= hw->active_count) return;
    hw->active_count--;
    if (idx < hw->active_count) {
        /* 用最后一个覆盖 (系数 + 所有扬声器状态) */
        hw->b1[idx] = hw->b1[hw->active_count];
        hw->a1[idx] = hw->a1[hw->active_count];
        hw->a2[idx] = hw->a2[hw->active_count];
        for (int s = 0; s < HW_S; s++) {
            hw->x1[s][idx] = hw->x1[s][hw->active_count];
            hw->x2[s][idx] = hw->x2[s][hw->active_count];
            hw->y1[s][idx] = hw->y1[s][hw->active_count];
            hw->y2[s][idx] = hw->y2[s][hw->active_count];
        }
        hw->active_freqs[idx] = hw->active_freqs[hw->active_count];
    }
}

/* ══════════════════════════════════════════════════════════
   每样本处理
   ══════════════════════════════════════════════════════════ */
void howling_tick(howling_detect_t *hw, float err_sample,
                  float *anti_spk, int S)
{
    if (!hw->enabled) return;

    /* 累积误差样本 */
    hw->buf[hw->buf_pos++] = err_sample;

    /* 未满一帧, 仅做陷波 (如果有激活的) */
    if (hw->buf_pos < HW_FFT_N) {
        for (int s = 0; s < S; s++) {
            float x = anti_spk[s];
            for (int i = 0; i < hw->active_count; i++)
                x = notch_apply(x, hw->b1[i], hw->a1[i], hw->a2[i],
                                &hw->x1[s][i], &hw->x2[s][i],
                                &hw->y1[s][i], &hw->y2[s][i]);
            anti_spk[s] = x;
        }
        return;
    }

    /* 满一帧: DFT 检测 */
    hw->buf_pos = 0;

    /* 递增所有活动陷波器的帧龄 (CR-2: 最小保持时间) */
    for (int i = 0; i < hw->active_count; i++) hw->notch_age[i]++;

    /* 复制并加窗 */
    float frame[HW_FFT_N];
    memcpy(frame, hw->buf, HW_FFT_N * sizeof(float));
    apply_hanning(frame, HW_FFT_N);

    float detected = detect_frame(hw, frame, 16000.0f);

    /* ── 状态机 ── */
    if (detected > 0) {
        /* 检测到窄带峰值 */
        if (fabsf(detected - hw->candidate_freq) < (16000.0f / HW_FFT_N) * 1.5f) {
            /* 与上一帧候选频率相近 → 累加计数 */
            hw->candidate_count++;
        } else {
            /* 新频率 → 重置候选 */
            hw->candidate_freq  = detected;
            hw->candidate_count = 1;
        }
        hw->release_timer = 0;

        /* 达到持续阈值 → 确认啸叫 */
        if (hw->candidate_count >= HW_PERSIST &&
            !freq_in_list(hw->candidate_freq, hw->active_freqs, hw->active_count)) {
            int idx = add_notch(hw, hw->candidate_freq);
            if (idx >= 0) {
                /* 陷波器已添加, 重置候选 */
                hw->candidate_freq  = 0;
                hw->candidate_count = 0;
            }
        }
    } else {
        /* 无显著峰值 → 啸叫可能已消失 */
        if (hw->active_count > 0 && hw->candidate_count == 0) {
            hw->release_timer++;
            if (hw->release_timer >= HW_RELEASE) {
                /* 只释放已超过最小保持时间的陷波器 (CR-2修复: 防释放死循环) */
                int new_count = 0;
                for (int i = 0; i < hw->active_count; i++) {
                    if (hw->notch_age[i] < HW_MIN_HOLD) {
                        /* 未到最小保持时间, 保留陷波器 (压缩到前面) */
                        if (new_count != i) {
                            hw->b1[new_count] = hw->b1[i];
                            hw->a1[new_count] = hw->a1[i];
                            hw->a2[new_count] = hw->a2[i];
                            hw->active_freqs[new_count] = hw->active_freqs[i];
                            hw->notch_age[new_count] = hw->notch_age[i];
                            for (int s = 0; s < HW_S; s++) {
                                hw->x1[s][new_count] = hw->x1[s][i];
                                hw->x2[s][new_count] = hw->x2[s][i];
                                hw->y1[s][new_count] = hw->y1[s][i];
                                hw->y2[s][new_count] = hw->y2[s][i];
                            }
                        }
                        new_count++;
                    }
                }
                hw->active_count = new_count;
                hw->release_timer = 0;
            }
        }
        hw->candidate_count = 0;  /* 重置候选 (中间断了) */
    }

    /* ── 应用陷波器 (每扬声器独立 IIR 状态) ── */
    for (int s = 0; s < S; s++) {
        float x = anti_spk[s];
        for (int i = 0; i < hw->active_count; i++)
            x = notch_apply(x, hw->b1[i], hw->a1[i], hw->a2[i],
                            &hw->x1[s][i], &hw->x2[s][i],
                            &hw->y1[s][i], &hw->y2[s][i]);
        anti_spk[s] = x;
    }
}
