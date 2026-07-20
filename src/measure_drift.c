/** measure_drift — 测两个 USB 设备的时钟漂移 (基于已验证的 calibrate_feedback.c)
 *
 * 编译: gcc -O2 -Iinclude src/measure_drift.c src/fir_filter.c src/binary_loader.c -lm -o measure_drift.exe
 *
 * 与 calibrate_feedback.exe 使用完全相同的 PortAudio 初始化 — 已验证可跨设备全双工.
 * 区别: 播放脉冲而非白噪声, 互相关找延迟, 重复10轮检测漂移.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

typedef struct { double inputBufferAdcTime, currentTime, outputBufferDacTime; } PaCbTimeInfo;  /* 回调参数类型 */

#define ROUNDS      10
#define REC_SEC     1.5f         /* 每轮录制时长 */
#define PULSE_WID   300          /* 脉冲宽度 */

/* ═══════════ PortAudio DLL (与 calibrate_feedback.c 完全相同) ═══════════ */
typedef int PaError; typedef void PaStream;
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
static const char *(*p_Pa_GetErrorText)(int);
typedef struct { int device, channelCount, sampleFormat; double suggestedLatency; void *hostApiSpecificStreamInfo; } PaStreamParams;
typedef struct { int structVersion; const char *name; int hostApi, maxInputChannels, maxOutputChannels; double defLowInLat, defLowOutLat, defHighInLat, defHighOutLat, defaultSampleRate; } PaDeviceInfo2;
#define PA_LOAD(fn) p_##fn = (void*)GetProcAddress(pa_dll, #fn)
static int pa_init(void) {
    pa_dll = LoadLibraryA("libportaudio64bit-asio.dll");
    if (!pa_dll) return -1;
    PA_LOAD(Pa_Initialize); PA_LOAD(Pa_Terminate);
    PA_LOAD(Pa_OpenStream); PA_LOAD(Pa_StartStream);
    PA_LOAD(Pa_StopStream); PA_LOAD(Pa_CloseStream);
    PA_LOAD(Pa_GetDeviceCount); PA_LOAD(Pa_GetDeviceInfo);
    PA_LOAD(Pa_GetErrorText);
    return 0;
}

/* ═══════════ 数据 ═══════════ */
typedef struct {
    float *pulse, *rec;
    int    idx, total;
} dr_t;

/* 回调 (与 cal_cb 结构完全一致) */
static int dr_cb(const void *input, void *output, unsigned long fcount,
                  const PaCbTimeInfo *ti, unsigned long flags, void *user)
{
    dr_t *d = (dr_t *)user;
    const float *in = (const float *)input;
    float *out = (float *)output;
    (void)ti; (void)flags;
    for (unsigned long i = 0; i < fcount; i++) {
        if (d->idx >= d->total) { out[i*2] = out[i*2+1] = 0; continue; }
        out[i*2] = out[i*2+1] = d->pulse[d->idx];
        d->rec[d->idx] = in[i*6 + 0];
        d->idx++;
    }
    return 0;
}

/* 互相关找脉冲到达时刻 */
static float xcorr_delay(const float *rec, int n, const float *tmpl, int tn,
                         float *peak_val) {
    float best = 0; int best_lag = 0;
    for (int lag = 0; lag < n - tn; lag++) {
        float corr = 0;
        for (int j = 0; j < tn; j++) corr += rec[lag + j] * tmpl[j];
        if (corr > best) { best = corr; best_lag = lag; }
    }
    *peak_val = best;
    return (float)best_lag;
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    printf("\n=== Clock Drift Test ===\n\n");
    if (pa_init()) return 1;
    p_Pa_Initialize();

    int nd = p_Pa_GetDeviceCount();
    printf("Devices:\n");
    for (int i = 0; i < nd; i++) {
        const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
        if (info && (info->maxInputChannels > 0 || info->maxOutputChannels > 0))
            printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
                i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
    }

    int in_dev, out_dev;
    printf("\nInput device (YDM6MIC): "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device (USB Spk): "); fflush(stdout); scanf("%d", &out_dev);

    /* 使用输入设备的实际采样率, 而非硬编码 48000 */
    const PaDeviceInfo2 *ii = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(in_dev);
    const PaDeviceInfo2 *oi = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(out_dev);
    int fs_use = (int)(ii ? ii->defaultSampleRate : 48000);
    int fs_out = (int)(oi ? oi->defaultSampleRate : 48000);
    if (fs_use != fs_out) fs_use = fs_out;  /* 取两者中较小的 */
    if (fs_use < 44100) fs_use = 44100;

    printf("Using %d Hz (in=%d out=%d)\n", fs_use,
           ii ? (int)ii->defaultSampleRate : 0,
           oi ? (int)oi->defaultSampleRate : 0);

    int total = (int)(fs_use * REC_SEC);
    float *pulse = (float *)calloc(total, sizeof(float));
    float *rec   = (float *)calloc(total, sizeof(float));
    int pulse_at = fs_use / 4;  /* 脉冲在 250ms 处 */
    /* 短脉冲 */
    for (int i = 0; i < PULSE_WID; i++) pulse[pulse_at + i] = 0.3f;

    printf("\nPlace speaker close to YDM6MIC ref mic!\n");
    printf("Testing %d rounds...\n\n", ROUNDS);
    printf("%4s | %8s | %8s | %8s\n", "轮", "延迟ms", "漂移ms", "相关峰");
    printf("-----+----------+----------+----------\n");

    float first_delay = 0;

    for (int r = 0; r < ROUNDS; r++) {
        memset(rec, 0, total * sizeof(float));
        dr_t d = { pulse, rec, 0, total };

        PaStreamParams in_p  = { in_dev, 6, paFloat32, 0.01, NULL };
        PaStreamParams out_p = { out_dev, 2, paFloat32, 0.01, NULL };
        PaStream *stream = NULL;

        int err = p_Pa_OpenStream(&stream, &in_p, &out_p, (double)fs_use,
                                  96, paNoFlag, dr_cb, &d);
        if (err) { printf("ERROR: OpenStream=%d\n", err); goto done; }
        p_Pa_StartStream(stream);
        while (d.idx < total) Sleep(5);
        p_Pa_StopStream(stream);
        p_Pa_CloseStream(stream);

        float peak;
        float delay_smp = xcorr_delay(rec, total, pulse + pulse_at,
                                       PULSE_WID, &peak);
        float delay_ms = (delay_smp - pulse_at) * 1000.0f / fs_use;
        if (r == 0) first_delay = delay_ms;

        printf("%4d | %7.2f | %+7.2f | %8.4f", r + 1, delay_ms,
               delay_ms - first_delay, peak);
        if (r > 0 && fabsf(delay_ms - first_delay) > 0.3f)
            printf("  <-- 漂移!");
        printf("\n");

        if (r < ROUNDS - 1) Sleep(2000);
    }

    printf("-----+----------+----------+----------\n");
    free(pulse); free(rec);
done:
    p_Pa_Terminate();
    return 0;
}
