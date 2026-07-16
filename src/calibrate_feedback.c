/** calibrate_feedback — 离线测量扬声器→参考麦反馈路径
 *
 * 编译: gcc -O2 -Iinclude src/calibrate_feedback.c src/fir_filter.c src/binary_loader.c -lm -o calibrate_feedback.exe
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
    float *noise_hw;    /* 预生成白噪声 (48kHz) */
    float *ref_hw;      /* 参考麦录制 (48kHz) */
    int    idx;
    int    total;
} cal_data_t;

/* PortAudio 类型 + DLL 函数指针 */
typedef int PaError;
typedef void PaStream;
#define paFloat32 0x00000001
#define paNoFlag  0
#define paNoError 0

static HMODULE pa_dll;
static PaError (*p_Pa_Initialize)(void);
static PaError (*p_Pa_Terminate)(void);
static PaError (*p_Pa_OpenStream)(PaStream **, const void *, const void *, double, unsigned long, unsigned long, void *, void *);
static PaError (*p_Pa_StartStream)(PaStream *);
static PaError (*p_Pa_StopStream)(PaStream *);
static PaError (*p_Pa_CloseStream)(PaStream *);
static int    (*p_Pa_GetDeviceCount)(void);
static const void *(*p_Pa_GetDeviceInfo)(int);
static const void *(*p_Pa_GetHostApiInfo)(int);
static int    (*p_Pa_GetDefaultHostApi)(void);
static int    (*p_Pa_HostApiTypeIdToHostApiIndex)(int);
static const char *(*p_Pa_GetErrorText)(int);

typedef struct { int device, channelCount, sampleFormat; double suggestedLatency; void *hostApiSpecificStreamInfo; } PaStreamParams;
typedef struct { double inputBufferAdcTime, currentTime, outputBufferDacTime; } PaCbTimeInfo;
typedef struct { int structVersion; const char *name; int type, deviceCount, defaultInputDevice, defaultOutputDevice; } PaHostApiInfo2;
typedef struct { int structVersion; const char *name; int hostApi, maxInputChannels, maxOutputChannels; double defLowInLat, defLowOutLat, defHighInLat, defHighOutLat, defaultSampleRate; } PaDeviceInfo2;

#define PA_LOAD(fn) p_##fn = (void*)GetProcAddress(pa_dll, #fn)

/* ══════════════════════════════════════════════════════════ */
static int pa_init(void) {
    pa_dll = LoadLibraryA("libportaudio64bit-asio.dll");
    if (!pa_dll) { fprintf(stderr, "DLL not found\n"); return -1; }
    PA_LOAD(Pa_Initialize); PA_LOAD(Pa_Terminate);
    PA_LOAD(Pa_OpenStream); PA_LOAD(Pa_StartStream);
    PA_LOAD(Pa_StopStream); PA_LOAD(Pa_CloseStream);
    PA_LOAD(Pa_GetDeviceCount); PA_LOAD(Pa_GetDeviceInfo);
    PA_LOAD(Pa_GetHostApiInfo); PA_LOAD(Pa_GetDefaultHostApi);
    PA_LOAD(Pa_HostApiTypeIdToHostApiIndex); PA_LOAD(Pa_GetErrorText);
    return 0;
}

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
        /* 两声道输出同相位白噪声 */
        float n = cal->noise_hw[cal->idx];
        out[i*2]   = n;
        out[i*2+1] = n;
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
    printf("\nInput device ID (YDM6MIC): "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID (USB Speaker): "); fflush(stdout); scanf("%d", &out_dev);

    /* 预生成白噪声 (48kHz) */
    int total_hw = FS_HW * CAL_SEC;
    float *noise_hw = (float *)malloc(total_hw * sizeof(float));
    float *ref_hw   = (float *)malloc(total_hw * sizeof(float));
    srand(42);  /* 固定种子, 可复现 */
    for (int i = 0; i < total_hw; i++)
        noise_hw[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * NOISE_AMP;

    cal_data_t cal = { noise_hw, ref_hw, 0, total_hw };

    /* 打开音频流 */
    PaStreamParams in_p  = { in_dev,  6, paFloat32, 0.01, NULL };
    PaStreamParams out_p = { out_dev, 2, paFloat32, 0.01, NULL };
    PaStream *stream = NULL;
    int err = p_Pa_OpenStream(&stream, &in_p, &out_p, FS_HW, 96, paNoFlag, cal_cb, &cal);
    if (err != 0) {
        fprintf(stderr, "PA open error: %s\n", p_Pa_GetErrorText(err));
        return 1;
    }

    printf("\nPlaying white noise for %d seconds...\n", CAL_SEC);
    printf("  Keep the room quiet - no talking or moving!\n");
    p_Pa_StartStream(stream);

    /* 等待完成 */
    while (cal.idx < total_hw) Sleep(100);

    p_Pa_StopStream(stream);
    p_Pa_CloseStream(stream);
    p_Pa_Terminate();
    printf("  Recording done.\n\n");

    /* 3:1 抽取到 16kHz */
    int n_16k = total_hw / 3;
    float *noise_16k = (float *)malloc(n_16k * sizeof(float));
    float *ref_16k   = (float *)malloc(n_16k * sizeof(float));
    for (int i = 0; i < n_16k; i++) {
        noise_16k[i] = noise_hw[i * 3];
        ref_16k[i]   = ref_hw[i * 3];
    }

    /* 诊断: 检查信号电平 */
    {
        float nrms = 0, rrms = 0;
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

    /* 保存 */
    FILE *f = fopen(FB_FILE, "wb");
    if (!f) { fprintf(stderr, "ERROR: Cannot write %s\n", FB_FILE); return 1; }
    fwrite(fb_coeffs, sizeof(float), FB_TAPS, f);
    fclose(f);
    printf("\n  Saved: %s (%d taps)\n", FB_FILE, FB_TAPS);

    /* 清理 */
    free(noise_hw); free(ref_hw);
    free(noise_16k); free(ref_16k);

    printf("\nDone. Now run gfanc_realtime.exe with feedback cancellation.\n\n");
    return 0;
}
