/** GFANC FxNLMS — PortAudio callback realtime ANC.
 *
 * 编译: gcc -O2 -Iinclude main_realtime.c src/pa_io.c src/scene_controller.c
 *       src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c
 *       src/cnn_m5_forward.c -lm -o gfanc_realtime.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#include "fir_filter.h"
#include "binary_loader.h"
#include "scene_controller.h"
#include "fxnlms_mimo.h"

/* ══════════════════════════════════════════════════════════ */
#define FS_HW    48000
#define FS_ANC   16000
#define E        3
#define S        2
#define L        1024
#define BP_LEN   1024
#define SEC_LEN  1024
#define DSP_DELAY 16
#define FADE_LEN 16

/* ══════════════════════════════════════════════════════════ */
typedef struct {
    /* ANC 模块 */
    scene_ctrl_t  sc;
    fxnlms_mimo_t fx;
    fir_filter_t  bp_fir;        /* ref 带通 */
    fir_filter_t  bp_err[E];     /* err 带通 */
    fir_filter_t *sec_firs;      /* [E*S] 次级路径 */
    float        *sec_coeffs;

    /* 跨回调状态 */
    float  wc_old[S*L], wc_cur[S*L];
    int    fade_cnt;
    float  cnn_buf[FS_ANC];
    int    cnn_cnt;
    int    first_sec;

    /* 48k 重采样缓冲 */
    float *ref_48k, *anti_48k, *err_48k; /* 在回调内部分配? 用固定大小 */
    int    dec_phase;

    /* 统计 */
    volatile float db_reduction;
    volatile float acc_dist, acc_err;
    volatile float dist_rms;  /* 扰动 RMS, 用于静音检测 */
    volatile int   acc_cnt;
    volatile int   running;
    volatile int   callback_count;
} rt_ctx_t;

/* PortAudio 类型 + DLL 函数指针 (最小化, 运行时加载) */
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

/* 3:1 抽取 (简单线性) */
static void decimate_3to1(const float *in, int in_len, float *out) {
    for (int i = 0; i < in_len / 3; i++) out[i] = in[i*3];
}
static void interpolate_1to3(const float *in, int in_len, float *out) {
    for (int i = 0; i < in_len; i++)
        out[i*3] = out[i*3+1] = out[i*3+2] = in[i];
}

/* ══════════════════════════════════════════════════════════
   音频回调 (PortAudio 线程)
   ══════════════════════════════════════════════════════════ */
static int audio_cb(const void *input, void *output, unsigned long fcount,
                     const PaCbTimeInfo *ti, unsigned long flags, void *user)
{
    rt_ctx_t *ctx = (rt_ctx_t *)user;
    const float *in = (const float *)input;
    float *out = (float *)output;
    int c48k = (int)fcount, c16k = c48k / 3;
    (void)ti; (void)flags;

    if (!ctx->running) { memset(out, 0, fcount * 2 * sizeof(float)); return 1; }

    /* 声道拆分 + 抽取 (ch0=ref, ch1-3=err) */
    for (int n = 0; n < c16k; n++) {
        ctx->ref_48k[n] = in[(n*3)*6 + 0];  /* 最近邻抽取: 每3个取1个 */
        ctx->err_48k[n*3+0] = in[(n*3)*6 + 1];
        ctx->err_48k[n*3+1] = in[(n*3)*6 + 2];
        ctx->err_48k[n*3+2] = in[(n*3)*6 + 3];
    }

    /* ── ANC @ 16kHz ── */
    for (int n = 0; n < c16k; n++) {
        float ref_sample = ctx->ref_48k[n];

        /* 带通滤波 (输入端加 mic 预增益, 不造成反馈) */
        float ref_filt = fir_tick(&ctx->bp_fir, ref_sample);

        /* CNN 累积 */
        if (ctx->cnn_cnt < FS_ANC) ctx->cnn_buf[ctx->cnn_cnt++] = ref_filt;

        /* CrossFader */
        if (ctx->fade_cnt > 0) {
            float a = (float)ctx->fade_cnt / FADE_LEN;
            for (int i = 0; i < S*L; i++)
                ctx->fx.wc[i] = a * ctx->wc_old[i] + (1.0f - a) * ctx->wc_cur[i];
            ctx->fade_cnt--;
            if (ctx->fade_cnt == 0) memcpy(ctx->fx.wc, ctx->wc_cur, S*L*sizeof(float));
        }

        /* Fx = Sec ⊗ ref_filt */
        float Fx_arr[E*S];
        for (int e = 0; e < E; e++)
            for (int s = 0; s < S; s++)
                Fx_arr[e*S+s] = fir_tick(&ctx->sec_firs[e*S+s], ref_filt);

        /* 扰动 = bp(mic) × 预增益 */
        float dist[E];
        for (int e = 0; e < E; e++)
            dist[e] = fir_tick(&ctx->bp_err[e], ctx->err_48k[n*3+e]);

        /* FxNLMS */
        float anti_spk[S], err_sig[E];
        if (ctx->fade_cnt == 0)
            fxnlms_tick(&ctx->fx, Fx_arr, dist, anti_spk, err_sig);
        else
            fxnlms_forward_only(&ctx->fx, Fx_arr, anti_spk, err_sig);

        /* 累积功率用于 dB */
        for (int e = 0; e < E; e++) {
            ctx->acc_dist += dist[e] * dist[e];
            ctx->acc_err  += err_sig[e] * err_sig[e];
        }
        if ((ctx->acc_cnt += 1) >= FS_ANC) {
            float pd = ctx->acc_dist, pe = ctx->acc_err;
            ctx->db_reduction = 10.0f * log10f((pd + 1e-12f) / (pe + 1e-12f));
            ctx->dist_rms = sqrtf(pd / (FS_ANC * E));
            ctx->acc_dist = ctx->acc_err = 0; ctx->acc_cnt = 0;
        }

        ctx->anti_48k[n] = anti_spk[0];
        ctx->anti_48k[n + c16k] = anti_spk[1];
    }

    /* 内插 + 输出 (ch0=spk0, ch1=spk1) */
    int oi = 0;
    for (int n = 0; n < c16k; n++) {
        float a0 = ctx->anti_48k[n], a1 = ctx->anti_48k[n + c16k];
        if (a0 > 1.0f) a0 = 1.0f; if (a0 < -1.0f) a0 = -1.0f;
        if (a1 > 1.0f) a1 = 1.0f; if (a1 < -1.0f) a1 = -1.0f;
        for (int r = 0; r < 3; r++) { out[oi++] = a0; out[oi++] = a1; }
    }

    ctx->callback_count++;
    return 0; /* paContinue */
}

/* ══════════════════════════════════════════════════════════
   Ctrl+C
   ══════════════════════════════════════════════════════════ */
static rt_ctx_t *g_ctx;
static BOOL WINAPI ctrl_handler(DWORD t) {
    if (t == CTRL_C_EVENT) { if (g_ctx) g_ctx->running = 0; return TRUE; }
    return FALSE;
}

/* ══════════════════════════════════════════════════════════
   初始化 PortAudio DLL
   ══════════════════════════════════════════════════════════ */
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

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    if (pa_init() != 0) return 1;
    p_Pa_Initialize();

    /* 列出设备 */
    int nd = p_Pa_GetDeviceCount();
    printf("\n=== Audio Devices ===\n");
    for (int i = 0; i < nd; i++) {
        const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
        if (info) printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
            i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
    }

    int in_dev, out_dev;
    printf("\nInput device ID: "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID: "); fflush(stdout); scanf("%d", &out_dev);

    /* 查找 WASAPI host API */
    int wasapi_api = p_Pa_HostApiTypeIdToHostApiIndex(6); /* paWASAPI=6 */
    if (wasapi_api < 0) wasapi_api = p_Pa_GetDefaultHostApi();
    const PaHostApiInfo2 *api = (const PaHostApiInfo2 *)p_Pa_GetHostApiInfo(wasapi_api);
    printf("Using host API: %s\n\n", api ? api->name : "default");

    /* 加载权重 */
    printf("Loading weights...\n");
    float *sec_path, *sub_filters, *centroids, *bp_coeff;
    bin_load_float("data/secondary_path.bin", &sec_path);
    int sub_len = bin_load_float("data/sub_filters.bin", &sub_filters);
    bin_load_float("data/scene_defs.bin", &centroids);
    bin_load_float("data/bandpass_fir.bin", &bp_coeff);
    extern int cnn_m5_init(void); cnn_m5_init();
    printf("  OK L=%d\n", sub_len / (15*2));

    /* 初始化 ANC 模块 */
    rt_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    g_ctx = &ctx;
    ctx.running = 1; ctx.first_sec = 1;

    ctx.bp_fir.coeffs = bp_coeff; ctx.bp_fir.n_taps = BP_LEN;
    ctx.bp_fir.delay_line = (double *)calloc(BP_LEN, sizeof(double));
    for (int e = 0; e < E; e++) {
        ctx.bp_err[e].coeffs = bp_coeff; ctx.bp_err[e].n_taps = BP_LEN;
        ctx.bp_err[e].delay_line = (double *)calloc(BP_LEN, sizeof(double));
    }

    int sp = SEC_LEN + DSP_DELAY;
    ctx.sec_firs = (fir_filter_t *)calloc(E*S, sizeof(fir_filter_t));
    ctx.sec_coeffs = (float *)calloc(E*S*sp, sizeof(float));
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e*S+s;
            memcpy(ctx.sec_coeffs + idx*sp + DSP_DELAY, sec_path + idx*SEC_LEN, SEC_LEN*sizeof(float));
            ctx.sec_firs[idx].coeffs = ctx.sec_coeffs + idx*sp;
            ctx.sec_firs[idx].n_taps = sp;
            ctx.sec_firs[idx].delay_line = (double *)calloc(sp, sizeof(double));
        }

    scene_ctrl_init(&ctx.sc, centroids, sub_filters, L);
    fxnlms_init(&ctx.fx, E, S, L, 0.0001f, 1e-5f);

    /* 缓冲 */
    ctx.ref_48k = (float *)malloc(FS_HW * sizeof(float));
    ctx.anti_48k = (float *)malloc(FS_HW * S * sizeof(float));
    ctx.err_48k = (float *)malloc(FS_HW * E * sizeof(float));
    printf("  ANC ready: E=%d S=%d L=%d\n", E, S, L);

    /* 打开 PortAudio 流 */
    PaStreamParams in_p = { in_dev, 6, 0x00000001, 0.01, NULL };  /* paFloat32 */
    PaStreamParams out_p = { out_dev, 2, 0x00000001, 0.01, NULL };
    PaStream *stream = NULL;
    int err = p_Pa_OpenStream(&stream, &in_p, &out_p, 48000, 96, 0, (void*)audio_cb, &ctx);
    if (err != 0) {
        fprintf(stderr, "PA open error: %s\n", p_Pa_GetErrorText(err));
        return 1;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    printf("\n══════════════════════════════════════════\n");
    printf("  GFANC FxNLMS — Realtime ANC\n");
    printf("  Ctrl+C to stop\n");
    printf("══════════════════════════════════════════\n\n");

    p_Pa_StartStream(stream);
    printf("Running...\n");

    /* 主循环: 只做 CNN (每秒一次) */
    while (ctx.running) {
        Sleep(100);
        if (ctx.cnn_cnt >= FS_ANC) {
            float probs[8];
            scene_ctrl_process(&ctx.sc, ctx.cnn_buf, ctx.wc_cur, probs);
            if (ctx.first_sec) {
                fxnlms_set_wc(&ctx.fx, ctx.wc_cur);
                printf("[CNN] INIT scene=%d max=%.2f\n", ctx.sc.cur_scene, probs[ctx.sc.cur_scene]);
                ctx.first_sec = 0;
            } else {
                float dot = 0, np = 0, nc = 0;
                for (int k = 0; k < 8; k++) {
                    dot += ctx.sc.prev_probs[k] * probs[k];
                    np += ctx.sc.prev_probs[k] * ctx.sc.prev_probs[k];
                    nc += probs[k] * probs[k];
                }
                float cos_sim = dot / (sqrtf(np)*sqrtf(nc) + 1e-10f);
                /* 计算反噪声 RMS */
                /* 信号太弱时 dB 不可靠 */
                float db_show = ctx.db_reduction;
                if (ctx.dist_rms < 0.0005f) db_show = 0;

                printf("[CNN] scene=%d max=%.2f cos=%.2f dB=%.1f rms=%.4f cb=%d\n",
                       ctx.sc.cur_scene, probs[ctx.sc.cur_scene], cos_sim,
                       db_show, ctx.dist_rms, ctx.callback_count);
                if (cos_sim < 0.8f) {
                    memcpy(ctx.wc_old, ctx.fx.wc, S*L*sizeof(float));
                    ctx.fade_cnt = FADE_LEN;
                    printf("  -> RESET\n");
                }
            }
            memcpy(ctx.sc.prev_probs, probs, 8*sizeof(float));
            ctx.cnn_cnt = 0;
        }
    }

    printf("\nStopping...\n");
    p_Pa_StopStream(stream);
    p_Pa_CloseStream(stream);
    p_Pa_Terminate();
    printf("Done.\n");
    return 0;
}
