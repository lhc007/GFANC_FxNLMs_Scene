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
#include "os_port.h"        /* R-28: gf_sleep_ms 可移植睡眠 */

#include "fir_filter.h"
#include "binary_loader.h"

/* 2阶 Butterworth 低通 biquad — 与 main_realtime.c R-14 抗混叠一致.
   旧代码直接 3:1 抽取, >8kHz 分量折叠入通带污染 FIR 辨识. */
typedef struct { float b0,b1,b2,a1,a2,z1,z2; } biquad_t;

static void biquad_init_lpf(biquad_t *f, float fc, float fs)
{
    float w0 = 2.0f * 3.14159265f * fc / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / (2.0f * 0.70710678f);  /* Q=1/√2 Butterworth */
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f - c) / (2.0f * a0);
    f->b1 = (1.0f - c) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0f * c / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_tick(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ══════════════════════════════════════════════════════════ */
#define FS_HW       48000
#define FS_CAL      16000
#define FB_TAPS     256
#define CAL_SEC     4           /* 校准时长 (秒) */
#define NOISE_AMP   0.9f        /* 白噪声幅度 (env: GFANC_CAL_NOISE 可覆盖) */
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
        cal->ref_hw[cal->idx] = in[i*4 + 0];
        cal->idx++;
    }
    return 0; /* paContinue */
}

/* ══════════════════════════════════════════════════════════
   NLMS 辨识: 已知激励(out) 和响应(ref), 求 FIR 系数
   ══════════════════════════════════════════════════════════ */
static int nlms_identify(const float *noise_16k, const float *ref_16k,
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

        /* signal power sum (with regularization), NOT averaged.
           R-48 note: 此处 power 是 Σx² 不除以 n_taps, NLMS_MU=0.2 对应标准 β∈(0,2).
           floor=1e-6 合理 (power 预期 ~7.7 for noise_amp=0.3, n_taps=256),
           与 fxnlms_mimo.c 的归一化 power 不同, 无需修改. */
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
    /* R-57: 弱路径质量门禁 — RMS<0.0005 时 FIR 基本是噪声, 反馈抵消形同虚设.
       此时不保存文件, 避免运行时加载无效 FIR 产生虚假 fb_est.
       实测 RMS=0.0001 的 FIR 装载后 fb_est≈0, 扬声器满幅反馈直进参考麦. */
    if (rms < 0.0005f) {
        printf("  ❌ 反馈路径过弱 (FIR RMS=%.6f < 0.0005) — 标定失败, 不保存文件!\n", rms);
        printf("     请提高扬声器音量 / 确认参考麦能听到扬声器后重新标定.\n");
        printf("     提示: 设置 GFANC_CAL_NOISE 环境变量可调整噪声幅度 (默认=%.2f).\n",
               (double)NOISE_AMP);
        return -1;  /* x 已在 L149 释放, 直接返回错误码 */
    }
    if (rms < 0.001f)
        printf("  ⚠ 反馈路径偏弱 (FIR RMS=%.6f < 0.001) — 建议提高扬声器音量后重新标定!\n", rms);
    return 0;  /* 标定成功 */
}

/* ══════════════════════════════════════════════════════════ */
int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    printf("\n=== Feedback Path Calibration ===\n\n");

    if (pa_init() != 0) return 1;
    p_Pa_Initialize();

    /* 列出设备 (跳过 MME/DirectSound, 只显示 ASIO/WASAPI/WDM-KS) */
    int nd = p_Pa_GetDeviceCount();
    int napi = 0;
    printf("\n=== Audio Devices ===\n");
    for (int api_idx = 0; ; api_idx++) {
        const PaHostApiInfo2 *api = (const PaHostApiInfo2 *)p_Pa_GetHostApiInfo(api_idx);
        if (!api) break;
        if (strstr(api->name, "MME") || strstr(api->name, "DirectSound")) continue;
        napi++;
        int has_dev = 0;
        for (int i = 0; i < nd; i++) {
            const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
            if (info && info->hostApi == api_idx
                && info->maxInputChannels > 0 && info->maxOutputChannels > 0) {
                if (!has_dev) { printf("\n[%s]\n", api->name); has_dev = 1; }
                printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
                    i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
            }
        }
    }
    if (napi == 0) { fprintf(stderr, "PA: no host APIs found\n"); return 1; }

    int in_dev, out_dev;
    printf("\nInput device ID (ASIO MIC): "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID (ASIO Speaker): "); fflush(stdout); scanf("%d", &out_dev);

    /* 预生成 16kHz 白噪声 — ZOH×3 播放, 与运行时输出路径一致 (F-F修复) */
    float noise_amp = NOISE_AMP;
    {   const char *s = getenv("GFANC_CAL_NOISE");
        if (s) noise_amp = (float)atof(s); }
    printf("Noise amplitude: %.2f (set GFANC_CAL_NOISE to override)\n", noise_amp);
    int total_hw = FS_HW * CAL_SEC;
    int n_16k     = total_hw / 3;
    float *noise_16k = (float *)malloc(n_16k * sizeof(float));
    float *ref_hw    = (float *)malloc(total_hw * sizeof(float));
    float *ref_16k   = (float *)malloc(n_16k * sizeof(float));
    srand(42);  /* 固定种子, 可复现 */
    for (int i = 0; i < n_16k; i++)
        noise_16k[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * noise_amp;

    PaStreamParams in_p  = { in_dev,  4, paFloat32, 0.01, NULL };
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
        while (cal.idx < total_hw) gf_sleep_ms(100);  /* R-28 */
        p_Pa_StopStream(stream);
        p_Pa_CloseStream(stream);

        /* ref_hw 抗混叠低通 → 3:1 抽取 → ref_16k (与运行时 R-14 链路一致) */
        {
            biquad_t aa;
            biquad_init_lpf(&aa, 6500.0f, (float)FS_HW);
            for (int i = 0; i < total_hw; i++)
                ref_hw[i] = biquad_tick(&aa, ref_hw[i]);
            for (int i = 0; i < n_16k; i++)
                ref_16k[i] = ref_hw[i * 3];
        }

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
        int nlms_ret = nlms_identify(noise_16k, ref_16k, n_16k, fb_coeffs, FB_TAPS);
        if (nlms_ret != 0) {
            printf("  ⚠ Speaker %d calibration failed, skipping file save.\n", spk);
            continue;  /* R-57: 标定质量不达标, 不保存无效 FIR */
        }

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
