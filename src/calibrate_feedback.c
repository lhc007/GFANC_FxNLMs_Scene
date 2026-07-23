/** calibrate_feedback — 离线测量扬声器→参考麦反馈路径
 *
 * 编译: gcc -O2 -Iinclude src/calibrate_feedback.c src/fir_filter.c src/binary_loader.c src/pa_loader.c -lm -o calibrate_feedback.exe
 * 运行: ./calibrate_feedback.exe
 * 输出: data/feedback_path.bin (256 tap float32 FIR)
 *
 * 原理: 扬声器播放白噪声, 参考麦录制, NLMS 辨识 FIR 冲激响应.
 *       Fb_path = speaker_output → acoustic → ref_mic
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#include "fir_filter.h"
#include "binary_loader.h"

/* ══════════════════════════════════════════════════════════ */
#define FS_HW       48000
#define FS_CAL      16000
#define FB_TAPS     256
#define CAL_SEC     4           /* 校准时长 (秒) */
#define NOISE_AMP   0.3f        /* 白噪声幅度 */
#define NLMS_MU     0.2f        /* NLMS 步长 */
#define FB_FILE     "data/feedback_path.bin"

/* ══════════════════════════════════════════════════════════ */
typedef struct {
    float *noise_16k;   /* 预生成白噪声 (16kHz, 用于ZOH×3播放+NLMS辨识) */
    float *ref_hw;      /* 参考麦录制 (48kHz, 仅在回调中写入) */
    int    idx;         /* 48k 样本计数 */
    int    total;       /* 48k 总样本数 */
    int    spk_idx;     /* 校准扬声器: 0=spk0, 1=spk1 */
} cal_data_t;

#include "pa_loader.h"

/* ══════════════════════════════════════════════════════════
   音频回调: 播放白噪声, 录制参考麦
   ══════════════════════════════════════════════════════════ */
static int cal_cb(const void *input, void *output, unsigned long fcount,
                   const PaCbTimeInfo *ti, unsigned long flags, void *user)
{
    cal_data_t *cal = (cal_data_t *)user;
    const float *in = (const float *)input;
    float *out = (float *)output;
    (void)ti; (void)flags;

    for (unsigned long i = 0; i < fcount; i++) {
        if (cal->idx >= cal->total) {
            out[i*2] = out[i*2+1] = 0;
            continue;
        }
        /* 16k白噪声 ZOH×3 → 48k播放 (仅校准扬声器输出, 另一通道静音) */
        float n = cal->noise_16k[cal->idx / 3];
        out[i*2]   = (cal->spk_idx == 0) ? n : 0.0f;
        out[i*2+1] = (cal->spk_idx == 1) ? n : 0.0f;
        /* 录制参考麦 (ch0) */
        cal->ref_hw[cal->idx] = in[i*6 + 0];
        cal->idx++;
    }
    return 0; /* paContinue */
}

/* ══════════════════════════════════════════════════════════
   NLMS 辨识: 已知激励(out) 和响应(ref), 求 FIR 系数
   ══════════════════════════════════════════════════════════ */
static void nlms_identify(const float *noise_16k, const float *ref_16k,
                          int n_samples, float *fb_coeffs, int n_taps)
{
    float *x = (float *)calloc(n_taps, sizeof(float));  /* delay line */
    int ptr = 0;

    memset(fb_coeffs, 0, n_taps * sizeof(float));

    printf("  NLMS identifying %d taps from %d samples...\n", n_taps, n_samples);

    float max_err = 0, max_coeff = 0;
    int check_interval = n_samples / 10;
    for (int n = 0; n < n_samples; n++) {
        if (n % check_interval == 0 && n > 0) {
            float crms = 0;
            for (int k = 0; k < n_taps; k++) crms += fb_coeffs[k] * fb_coeffs[k];
            printf("  NLMS %d%%: max|err|=%.4f coeff_rms=%.6f\n",
                   n * 100 / n_samples, max_err, sqrtf(crms / n_taps));
        }
        /* shift in new input sample */
        x[ptr] = noise_16k[n];

        /* compute filter output estimate */
        float y = 0;
        for (int k = 0; k < n_taps; k++)
            y += fb_coeffs[k] * x[(ptr - k + n_taps) % n_taps];

        /* error = actual - estimate */
        float e = ref_16k[n] - y;

        /* signal power sum (with regularization), NOT averaged */
        float power = 1e-6f;
        for (int k = 0; k < n_taps; k++) {
            float v = x[(ptr - k + n_taps) % n_taps];
            power += v * v;
        }
        /* power = sum(x²), 不除以 n_taps. NLMS_MU 直接对应标准 β (0 < β < 2) */

        /* NLMS update */
        float mu = NLMS_MU / power;
        for (int k = 0; k < n_taps; k++)
            fb_coeffs[k] += mu * e * x[(ptr - k + n_taps) % n_taps];

        if (fabsf(e) > max_err) max_err = fabsf(e);
        for (int k = 0; k < n_taps; k++)
            if (fabsf(fb_coeffs[k]) > max_coeff) max_coeff = fabsf(fb_coeffs[k]);

        ptr = (ptr + 1) % n_taps;
    }

    printf("  NLMS: max|err|=%.4f  max|coeff|=%.4f\n", max_err, max_coeff);
    free(x);

    /* 计算 FIR 的 RMS 和峰值信息 */
    float rms = 0, peak = 0;
    int peak_idx = 0;
    for (int k = 0; k < n_taps; k++) {
        rms += fb_coeffs[k] * fb_coeffs[k];
        if (fabsf(fb_coeffs[k]) > peak) {
            peak = fabsf(fb_coeffs[k]);
            peak_idx = k;
        }
    }
    rms = sqrtf(rms / n_taps);
    printf("  FIR: peak=%.4f @ tap %d (%.2fms), RMS=%.4f\n",
           peak, peak_idx, (float)peak_idx / FS_CAL * 1000.0f, rms);
}

/* ══════════════════════════════════════════════════════════ */
int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    printf("\n=== Feedback Path Calibration ===\n\n");

    if (pa_init() != 0) return 1;
    p_Pa_Initialize();

    /* 列出设备 */
    int nd = p_Pa_GetDeviceCount();
    printf("Audio Devices:\n");
    for (int i = 0; i < nd; i++) {
        const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
        if (info && (info->maxInputChannels > 0 || info->maxOutputChannels > 0)
            && info->defaultSampleRate >= 48000)
            printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
                i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
    }

    int in_dev, out_dev;
    printf("\nInput device ID (ASIO MIC): "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID (ASIO Speaker): "); fflush(stdout); scanf("%d", &out_dev);

    /* 预生成 16kHz 白噪声 — ZOH×3 播放, 与运行时输出路径一致 (F-F修复) */
    int total_hw = FS_HW * CAL_SEC;
    int n_16k     = total_hw / 3;
    float *noise_16k = (float *)malloc(n_16k * sizeof(float));
    float *ref_hw    = (float *)malloc(total_hw * sizeof(float));
    float *ref_16k   = (float *)malloc(n_16k * sizeof(float));
    srand(42);  /* 固定种子, 可复现 */
    for (int i = 0; i < n_16k; i++)
        noise_16k[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * NOISE_AMP;

    PaStreamParams in_p  = { in_dev,  6, paFloat32, 0.01, NULL };
    PaStreamParams out_p = { out_dev, 2, paFloat32, 0.01, NULL };

    /* ── 逐扬声器校准 (F-G修复) ── */
    for (int spk = 0; spk < 2; spk++) {
        printf("\n--- Calibrating Speaker %d ---\n", spk);
        printf("  Playing white noise on speaker %d only for %d seconds...\n", spk, CAL_SEC);
        printf("  Keep the room quiet - no talking or moving!\n");

        memset(ref_hw, 0, total_hw * sizeof(float));
        cal_data_t cal = { noise_16k, ref_hw, 0, total_hw, spk };

        PaStream *stream = NULL;
        int err = p_Pa_OpenStream(&stream, &in_p, &out_p, FS_HW, 96, paNoFlag, cal_cb, &cal);
        if (err) { fprintf(stderr, "PA open error: %s\n", p_Pa_GetErrorText(err)); return 1; }
        p_Pa_StartStream(stream);
        while (cal.idx < total_hw) Sleep(100);
        p_Pa_StopStream(stream);
        p_Pa_CloseStream(stream);

        /* ref_hw 3:1 抽取 → ref_16k */
        for (int i = 0; i < n_16k; i++)
            ref_16k[i] = ref_hw[i * 3];

        /* 诊断 */
        {   float nrms = 0, rrms = 0;
            for (int i = 0; i < n_16k; i++) {
                nrms += noise_16k[i] * noise_16k[i];
                rrms += ref_16k[i] * ref_16k[i];
            }
            printf("  Signal RMS: noise=%.4f  ref=%.4f\n",
                   sqrtf(nrms / n_16k), sqrtf(rrms / n_16k));
        }

        /* NLMS 辨识 */
        float fb_coeffs[FB_TAPS];
        nlms_identify(noise_16k, ref_16k, n_16k, fb_coeffs, FB_TAPS);

        /* 保存 (feedback_path_0.bin / feedback_path_1.bin) */
        char fname[64];
        snprintf(fname, sizeof(fname), "data/feedback_path_%d.bin", spk);
        FILE *f = fopen(fname, "wb");
        if (!f) { fprintf(stderr, "ERROR: Cannot write %s\n", fname); return 1; }
        fwrite(fb_coeffs, sizeof(float), FB_TAPS, f);
        fclose(f);
        printf("  Saved: %s (%d taps)\n", fname, FB_TAPS);
    }

    p_Pa_Terminate();

    /* 清理 */
    free(noise_16k); free(ref_hw); free(ref_16k);

    printf("\nDone. Now run gfanc_realtime.exe with feedback cancellation.\n\n");
    return 0;
}
