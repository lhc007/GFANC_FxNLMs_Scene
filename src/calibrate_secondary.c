/** calibrate_secondary — 实测次级路径 Ŝ(e,s) + 反馈路径 (v4)
 *
 * 编译: gcc -O2 -Iinclude src/calibrate_secondary.c -lm -o calibrate_secondary.exe
 * 输出: data/secondary_path_measured.bin  [E*S][SEC_TAPS], 已对齐 (peak≈GUARD)
 *       data/sec_bulk_delay.bin           1×float32 = 流启动时 bulk 延迟 @16k
 *       data/feedback_path_s0.bin / _s1.bin
 *
 * v4 要点 (针对 WASAPI 双设备流滑移 ~1000-2000ppm):
 *   - 探测: 17 个 0.25s 子窗各自找峰 → 聚类投票 (真峰在 ±150 内反复复现,
 *     随机噪声峰散布全程). 不再依赖单窗 PNR (滑移涂抹会压低单窗峰).
 *   - 辨识: 每 0.125s 块单独跟踪 lag 并整数对齐 → NLMS 输入涂抹从 ±70 → ±3 样本
 *   - 流参数与运行时一致 (960 帧 + 0.2s), bulk 取流启动时刻的 lag
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

/* ══════════════════════════════════════════════════════════ */
#define FS_HW       48000
#define FS_CAL      16000
#define SEC_TAPS    1024        /* 与 main_realtime 的 SEC_LEN 一致 */
#define CAL_SEC     6           /* 每只扬声器校准时长 (秒) */
#define NOISE_AMP   0.9f        /* 白噪声幅度 (响, 保证 SNR) */
#define NLMS_MU     0.2f
#define NLMS_PASSES 2
#define E           3
#define S           2
#define NMIC        4           /* ch0=ref, ch1-3=err */
#define MAXLAG      8000        /* 探测范围 0.5s (实测环路延迟 ~80ms) */
#define GUARD       64          /* 对齐后 IR peak 位置 (4ms) */
#define SKIP_HEAD   16000       /* 跳过前 1s 流启动瞬态 */
#define SUBWIN      4000        /* 探测子窗 0.25s */
#define CLUSTER_TOL 150         /* 聚类半径 (样本) */
#define CHUNK       2000        /* 辨识重对齐块 0.125s */
#define TRACK_SPAN  400         /* 逐块跟踪搜索半径 */
#define SEC_OUT_FILE "data/secondary_path_measured.bin"
#define DLY_OUT_FILE "data/sec_bulk_delay.bin"

/* PortAudio 最小绑定 */
typedef int PaError;
typedef void PaStream;
#define paFloat32 0x00000001
#define paNoFlag  0

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
typedef struct { double inputBufferAdcTime, currentTime, outputBufferDacTime; } PaCbTimeInfo;
typedef struct { int structVersion; const char *name; int hostApi, maxInputChannels, maxOutputChannels; double defLowInLat, defLowOutLat, defHighInLat, defHighOutLat, defaultSampleRate; } PaDeviceInfo2;

#define PA_LOAD(fn) p_##fn = (void*)GetProcAddress(pa_dll, #fn)

static int pa_init(void) {
    pa_dll = LoadLibraryA("libportaudio64bit-asio.dll");
    if (!pa_dll) { fprintf(stderr, "DLL not found\n"); return -1; }
    PA_LOAD(Pa_Initialize); PA_LOAD(Pa_Terminate);
    PA_LOAD(Pa_OpenStream); PA_LOAD(Pa_StartStream);
    PA_LOAD(Pa_StopStream); PA_LOAD(Pa_CloseStream);
    PA_LOAD(Pa_GetDeviceCount); PA_LOAD(Pa_GetDeviceInfo);
    PA_LOAD(Pa_GetErrorText);
    return 0;
}

/* ══════════════════════════════════════════════════════════
   音频回调
   ══════════════════════════════════════════════════════════ */
typedef struct {
    const float *noise_hw;
    float       *mic_hw;     /* [NMIC][total] */
    int          idx, total, spk;
} cal_data_t;

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
        float nz = cal->noise_hw[cal->idx];
        out[i*2+0] = (cal->spk == 0) ? nz : 0.0f;
        out[i*2+1] = (cal->spk == 1) ? nz : 0.0f;
        for (int c = 0; c < NMIC; c++)
            cal->mic_hw[c * cal->total + cal->idx] = in[i*4 + c];  /* UMC 4ch */
        cal->idx++;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════ */
static double xcorr_at(const float *x, const float *y, int n, int lag)
{
    double c = 0;
    for (int i = 0; i < n; i++) c += (double)x[i] * y[i + lag];
    return c;
}

static int argmax_lag(const float *x, const float *y, int n,
                      int lag_lo, int lag_hi)
{
    double best = -1; int bl = lag_lo;
    for (int lag = lag_lo; lag <= lag_hi; lag++) {
        double c = fabs(xcorr_at(x, y, n, lag));
        if (c > best) { best = c; bl = lag; }
    }
    return bl;
}

static float rms_of(const float *x, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
    return (float)sqrt(s / n);
}

/* ══════════════════════════════════════════════════════════
   聚类投票探测: nsub 个子窗各自找全程峰, 最大 ±TOL 聚类 ≥ 1/3 → 真实
   返回聚类中 t 最早子窗的 lag (≈流启动延迟), -1=失败
   ══════════════════════════════════════════════════════════ */
static int probe_vote(const float *noise, const float *mic, int n16,
                      double *slip_out)
{
    int lags[64], offs[64], nsub = 0;
    for (int j = 0; j < 64; j++) {
        long o = SKIP_HEAD + (long)j * SUBWIN;
        if (o + SUBWIN + MAXLAG >= n16) break;
        lags[nsub] = argmax_lag(noise + o, mic + o, SUBWIN, 0, MAXLAG - 1);
        offs[nsub] = (int)o;
        nsub++;
    }

    /* 最大聚类 */
    int best_cnt = 0, best_i = -1;
    for (int i = 0; i < nsub; i++) {
        int cnt = 0;
        for (int j = 0; j < nsub; j++)
            if (abs(lags[j] - lags[i]) <= CLUSTER_TOL) cnt++;
        if (cnt > best_cnt) { best_cnt = cnt; best_i = i; }
    }

    printf("    子窗峰: ");
    for (int j = 0; j < nsub; j++) {
        int in_c = abs(lags[j] - lags[best_i]) <= CLUSTER_TOL;
        printf("%s%d%s ", in_c ? "[" : "", lags[j], in_c ? "]" : "");
    }
    printf("\n    聚类: %d/%d 子窗聚在 %d±%d\n",
           best_cnt, nsub, lags[best_i], CLUSTER_TOL);

    if (best_cnt * 3 < nsub) return -1;   /* 未达 1/3 法定数 */

    /* 聚类内: 最早子窗的 lag = 流启动延迟; 首尾差 → 滑移率 */
    int first_lag = -1, last_lag = 0;
    long first_off = 0, last_off = 0;
    for (int j = 0; j < nsub; j++) {
        if (abs(lags[j] - lags[best_i]) <= CLUSTER_TOL) {
            if (first_lag < 0) { first_lag = lags[j]; first_off = offs[j]; }
            last_lag = lags[j]; last_off = offs[j];
        }
    }
    *slip_out = (last_off > first_off)
        ? (double)(last_lag - first_lag) / (double)(last_off - first_off) * 1e6 : 0;
    return first_lag;
}

/* ══════════════════════════════════════════════════════════
   逐块跟踪 + 整数对齐 NLMS 辨识
   每 CHUNK 样本在 anchor±TRACK_SPAN 内跟踪 lag, 按 (lag−GUARD) 平移
   mic 后拼接, NLMS 输入的响应恒定出现在 GUARD 处.
   ══════════════════════════════════════════════════════════ */
static int build_aligned(const float *noise, const float *mic, int n16,
                         int anchor, float **x_out, float **y_out)
{
    int max_chunks = (n16 - SKIP_HEAD) / CHUNK;
    float *xa = (float *)malloc((size_t)max_chunks * CHUNK * sizeof(float));
    float *ya = (float *)malloc((size_t)max_chunks * CHUNK * sizeof(float));
    int n_out = 0, lag = anchor;
    int lag_min = anchor, lag_max = anchor;

    for (int j = 0; j < max_chunks; j++) {
        long o = SKIP_HEAD + (long)j * CHUNK;
        int lo = lag - TRACK_SPAN; if (lo < 0) lo = 0;
        int hi = lag + TRACK_SPAN; if (hi > MAXLAG - 1) hi = MAXLAG - 1;
        if (o + CHUNK + hi >= n16) break;

        lag = argmax_lag(noise + o, mic + o, CHUNK, lo, hi);  /* 跟踪 (逐块连续) */
        if (lag < lag_min) lag_min = lag;
        if (lag > lag_max) lag_max = lag;

        int shift = lag - GUARD;             /* 对齐: 响应移到 GUARD 处 */
        if (o + CHUNK + shift + GUARD >= n16 || shift < 0) break;
        memcpy(xa + (size_t)n_out, noise + o, CHUNK * sizeof(float));
        memcpy(ya + (size_t)n_out, mic + o + shift, CHUNK * sizeof(float));
        n_out += CHUNK;
    }
    printf("    逐块跟踪: lag %d→[%d,%d] (走差 %d 样本), 有效数据 %.1fs\n",
           anchor, lag_min, lag_max, lag_max - lag_min, (float)n_out / FS_CAL);
    *x_out = xa; *y_out = ya;
    return n_out;
}

static float nlms_identify(const float *x_in, const float *y_ref,
                           int n_samples, float *coeffs, int n_taps)
{
    float *x = (float *)calloc(n_taps, sizeof(float));
    memset(coeffs, 0, n_taps * sizeof(float));
    double acc_y = 0, acc_e = 0;

    for (int pass = 0; pass < NLMS_PASSES; pass++) {
        memset(x, 0, n_taps * sizeof(float));
        int ptr = 0;
        for (int n = 0; n < n_samples; n++) {
            x[ptr] = x_in[n];
            float y = 0;
            for (int k = 0; k < n_taps; k++)
                y += coeffs[k] * x[(ptr - k + n_taps) % n_taps];
            float e = y_ref[n] - y;
            float power = 1e-6f;
            for (int k = 0; k < n_taps; k++) {
                float v = x[(ptr - k + n_taps) % n_taps];
                power += v * v;
            }
            float mu = NLMS_MU / power;
            for (int k = 0; k < n_taps; k++)
                coeffs[k] += mu * e * x[(ptr - k + n_taps) % n_taps];
            if (pass == NLMS_PASSES - 1) {
                acc_y += (double)y_ref[n] * y_ref[n];
                acc_e += (double)e * e;
            }
            ptr = (ptr + 1) % n_taps;
        }
    }
    free(x);
    return 10.0f * (float)log10(acc_y / (acc_e + 1e-20));
}

static void print_ir_info(const char *tag, const float *c, int n_taps,
                          float erle, int bulk)
{
    float rms = 0, peak = 0; int peak_idx = 0;
    for (int k = 0; k < n_taps; k++) {
        rms += c[k] * c[k];
        if (fabsf(c[k]) > peak) { peak = fabsf(c[k]); peak_idx = k; }
    }
    printf("  %-10s peak=%+.4f @ tap %4d (总延迟 %6.2fms)  RMS=%.5f  辨识ERLE=%.1fdB%s\n",
           tag, c[peak_idx], peak_idx,
           (float)(bulk + peak_idx) / FS_CAL * 1000.0f,
           sqrtf(rms / n_taps), erle,
           erle < 6.0f ? "  (滑移残余所限)" : "");
}

/* ══════════════════════════════════════════════════════════ */
int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    printf("\n=== Secondary Path Calibration v4 (F-B) ===\n\n");

    if (pa_init() != 0) return 1;
    p_Pa_Initialize();

    int nd = p_Pa_GetDeviceCount();
    printf("Audio Devices:\n");
    for (int i = 0; i < nd; i++) {
        const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
        if (info && (info->maxInputChannels > 0 || info->maxOutputChannels > 0)
            && info->defaultSampleRate >= 44100)
            printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
                i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
    }

    int in_dev, out_dev;
    printf("\nInput device ID (YDM6MIC): "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID (USB Speaker): "); fflush(stdout); scanf("%d", &out_dev);

    int n_16k = FS_CAL * CAL_SEC;
    int total_hw = n_16k * 3;
    float *noise_16k = (float *)malloc(n_16k * sizeof(float));
    float *noise_hw  = (float *)malloc(total_hw * sizeof(float));
    float *mic_hw    = (float *)malloc((size_t)NMIC * total_hw * sizeof(float));
    srand(42);
    for (int i = 0; i < n_16k; i++)
        noise_16k[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * NOISE_AMP;
    for (int i = 0; i < n_16k; i++)
        noise_hw[i*3] = noise_hw[i*3+1] = noise_hw[i*3+2] = noise_16k[i];

    float *mic16 = (float *)malloc((size_t)S * NMIC * n_16k * sizeof(float));
    #define MIC16(s,c) (mic16 + (((size_t)(s) * NMIC + (c)) * n_16k))

    /* ── 录音 ── */
    for (int s = 0; s < S; s++) {
        printf("\n── Speaker %d: 播放白噪声 %d 秒, 保持安静! ──\n", s, CAL_SEC);
        cal_data_t cal = { noise_hw, mic_hw, 0, total_hw, s };
        /* BUG-2: 流参数与运行时完全一致 (GFANC_BUFFER / 推导延迟), bulk 延迟才能对应.
           旧 960帧/0.2s 测出的 bulk (~112ms) 远大于运行时实际环路.
           旧 96帧/0.01s 的 suggestedLatency 会把 ASIO 驱动顶到 512 样本 → 环路 ~30ms;
           现由缓冲推导延迟, 与运行时同步可调. */
        int buf_frames = 128;
        {   const char *be = getenv("GFANC_BUFFER");
            if (be && atoi(be) >= 32 && atoi(be) <= 1024) buf_frames = atoi(be); }
        double buf_lat = (double)buf_frames / FS_HW;
        PaStreamParams in_p  = { in_dev,  4, paFloat32, buf_lat, NULL };  /* UMC 4ch: ch0=ref, ch1-3=err */
        PaStreamParams out_p = { out_dev, 2, paFloat32, buf_lat, NULL };
        PaStream *stream = NULL;
        int err = p_Pa_OpenStream(&stream, &in_p, &out_p, FS_HW, buf_frames, paNoFlag, cal_cb, &cal);
        if (err != 0) { fprintf(stderr, "PA open error: %s\n", p_Pa_GetErrorText(err)); return 1; }
        p_Pa_StartStream(stream);
        while (cal.idx < total_hw) Sleep(100);
        p_Pa_StopStream(stream);
        p_Pa_CloseStream(stream);

        printf("  录音电平: ");
        for (int c = 0; c < NMIC; c++) {
            float *dst = MIC16(s, c);
            for (int i = 0; i < n_16k; i++)
                dst[i] = mic_hw[(size_t)c * total_hw + i*3];
            printf("ch%d=%.5f ", c, rms_of(dst, n_16k));
        }
        printf("\n");
    }

    /* ── 聚类投票探测 ── */
    int anchor[S];
    for (int s = 0; s < S; s++) {
        int best_c = 1; float best_rms = 0;
        for (int c = 1; c < NMIC; c++) {
            float r = rms_of(MIC16(s, c), n_16k);
            if (r > best_rms) { best_rms = r; best_c = c; }
        }
        printf("\n── Speaker %d 聚类投票探测 (ch%d) ──\n", s, best_c);
        double slip = 0;
        anchor[s] = probe_vote(noise_16k, MIC16(s, best_c), n_16k, &slip);
        if (anchor[s] < 0) {
            printf("\n!! Speaker %d 聚类未达法定数 — 未捕获可靠响应.\n"
                   "   请加大扬声器音量 / 缩短扬声器-麦克风距离后重试.\n", s);
            return 1;
        }
        printf("    → 流启动延迟 ≈ %d 样本 (%.2fms), 滑移 %.0fppm (%.1f 样本/秒)\n",
               anchor[s], (float)anchor[s] * 1000.0f / FS_CAL,
               slip, slip * 1e-6 * FS_CAL);
    }

    int bulk = (anchor[0] < anchor[1] ? anchor[0] : anchor[1]) - GUARD;
    if (bulk < 0) bulk = 0;
    printf("\n  bulk 延迟 = %d 样本 (%.2fms)\n", bulk, bulk * 1000.0f / FS_CAL);

    /* ── 逐块对齐 + NLMS ── */
    float *sec = (float *)calloc((size_t)E * S * SEC_TAPS, sizeof(float));
    float *fb  = (float *)calloc((size_t)S * SEC_TAPS, sizeof(float));

    for (int s = 0; s < S; s++) {
        printf("\n── Speaker %d 逐块对齐 + NLMS 辨识 ──\n", s);
        for (int c = 0; c < NMIC; c++) {
            float *xa, *ya;
            int n_al = build_aligned(noise_16k, MIC16(s, c), n_16k, anchor[s], &xa, &ya);
            if (n_al < CHUNK * 4) { free(xa); free(ya);
                printf("  ch%d 有效数据不足, 跳过\n", c); continue; }

            float *dst = (c == 0) ? fb + (size_t)s * SEC_TAPS
                                  : sec + (size_t)((c-1) * S + s) * SEC_TAPS;
            float erle = nlms_identify(xa, ya, n_al, dst, SEC_TAPS);
            free(xa); free(ya);

            char tag[32];
            if (c == 0) snprintf(tag, sizeof(tag), "FB (s%d)", s);
            else        snprintf(tag, sizeof(tag), "S(e%d,s%d)", c-1, s);
            /* 注意: 对齐后 IR peak 恒在 GUARD 附近, 总延迟 = anchor±声学差 */
            print_ir_info(tag, dst, SEC_TAPS, erle, anchor[s] - GUARD);
        }
    }

    /* ── 保存 ── */
    FILE *f = fopen(SEC_OUT_FILE, "wb");
    if (!f) { fprintf(stderr, "ERROR: cannot write %s\n", SEC_OUT_FILE); return 1; }
    fwrite(sec, sizeof(float), (size_t)E * S * SEC_TAPS, f);
    fclose(f);
    printf("\n  Saved: %s\n", SEC_OUT_FILE);

    f = fopen(DLY_OUT_FILE, "wb");
    if (f) { /* BUG-2: 存总环路延迟 (anchor = bulk + GUARD), 运行时按 Ŝ 峰位补偿 */
             float d = (float)(bulk + GUARD);
             fwrite(&d, sizeof(float), 1, f); fclose(f);
             printf("  Saved: %s (loop_delay=%d = %.2fms @16k)\n",
                    DLY_OUT_FILE, bulk + GUARD, (float)(bulk + GUARD) * 1000.0f / FS_CAL); }

    for (int s = 0; s < S; s++) {
        char path[64];
        snprintf(path, sizeof(path), "data/feedback_path_s%d.bin", s);
        f = fopen(path, "wb");
        if (f) { fwrite(fb + (size_t)s * SEC_TAPS, sizeof(float), SEC_TAPS, f); fclose(f);
                 printf("  Saved: %s\n", path); }
    }

    printf("\n  !! 注意: 环路延迟 ~%.0fms + 流滑移 → 宽带随机噪声无法前馈抵消;\n"
           "     低频周期性噪声 (<150Hz) 可自适应跟踪. 根治需共时钟低延迟声卡.\n",
           (float)(bulk + GUARD) * 1000.0f / FS_CAL);

    free(noise_16k); free(noise_hw); free(mic_hw); free(mic16);
    free(sec); free(fb);
    p_Pa_Terminate();
    printf("\nDone. 运行 gfanc_realtime.exe 验证 (应显示 MEASURED + bulk).\n\n");
    return 0;
}
