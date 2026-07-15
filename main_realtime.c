/** GFANC FxNLMS — 实时 WASAPI 降噪.
 *
 * 编译: gcc -O2 -Iinclude main_realtime.c src/*.c -lm -lole32 -o gfanc_realtime.exe
 * 运行: ./gfanc_realtime.exe
 *
 * 硬件拓扑:
 *   输入:  YDM6MIC 麦克风阵列, 48kHz, 6ch (ch0=参考麦, ch1-3=误差麦)
 *   输出:  USB Audio Device 扬声器, 48kHz, 2ch
 *   内部:  重采样到 16kHz 处理, 反噪声重采样回 48kHz 输出
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <windows.h>

#include "fir_filter.h"
#include "binary_loader.h"
#include "wasapi_io.h"

/* CNN forward declare */
extern int cnn_m5_init(void);
extern int cnn_m5_forward(const float *audio, float *logits);

/* ══════════════════════════════════════════════════════════
   常量
   ══════════════════════════════════════════════════════════ */
#define FS_HW      48000    /* 硬件采样率 */
#define FS_ANC     16000    /* ANC 内部采样率 */
#define E          3        /* 误差麦数 */
#define S          2        /* 扬声器数 */
#define C          15       /* 子滤波器数 */
#define K          8        /* 场景数 */
#define BP_LEN     1024     /* 带通 FIR 长度 */
#define PRI_LEN    1024     /* 初级路径长度 */
#define SEC_LEN_RAW 1024    /* 次级路径原始长度 */
#define DSP_DELAY  16       /* DSP 延迟 (样本) */
#define FADE_LEN   16       /* 交叉淡化长度 */
#define LATENCY_MS 10       /* WASAPI 目标延迟 */

/* ══════════════════════════════════════════════════════════
   全局数据
   ══════════════════════════════════════════════════════════ */
static float *g_sec_path   = NULL;
static float *g_pri_path   = NULL;
static float *g_sub_filters = NULL;
static float *g_centroids  = NULL;
static float *g_bp_fir     = NULL;
static int    g_sec_len, g_pri_len, g_sub_len, g_bp_len;

/* ══════════════════════════════════════════════════════════
   流式重采样器 (48kHz ↔ 16kHz, 线性插值)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    double phase;    /* 累加相位 [0, 1) */
    int    ratio_num, ratio_den;
} resampler_t;

static void resampler_init(resampler_t *r, int fs_in, int fs_out)
{
    r->phase = 0.0;
    r->ratio_num = fs_in;
    r->ratio_den = fs_out;
}

/* 下采样: in[in_ch][in_len] → out[out_ch][*out_len], ch 不变 */
static int resample_down(const float *in, int in_len, int in_ch,
                          float *out, resampler_t *r)
{
    int out_len = 0;
    double phase = r->phase;
    int ratio_num = r->ratio_num;
    int ratio_den = r->ratio_den;

    while (1) {
        int pos = (int)phase;
        if (pos >= in_len) break;
        double frac = phase - pos;
        for (int c = 0; c < in_ch; c++) {
            float a = in[in_len * c + pos];
            float b = (pos + 1 < in_len) ? in[in_len * c + pos + 1] : a;
            out[out_len * in_ch + c] = (float)(a * (1.0 - frac) + b * frac);
        }
        out_len++;
        phase += (double)ratio_num / ratio_den;
    }
    r->phase = phase - in_len;
    return out_len;
}

/* 上采样: in[ch][in_len] → out[ch][*out_len] */
static int resample_up(const float *in, int in_len, int in_ch,
                        float *out, resampler_t *r)
{
    int out_len = 0;
    double phase = r->phase;
    int ratio_den = r->ratio_den;
    int ratio_num = r->ratio_num;

    while (1) {
        int pos = (int)phase;
        if (pos >= in_len) break;
        double frac = phase - pos;
        for (int c = 0; c < in_ch; c++) {
            float a = in[in_len * c + pos];
            float b = (pos + 1 < in_len) ? in[in_len * c + pos + 1] : a;
            out[out_len * in_ch + c] = (float)(a * (1.0 - frac) + b * frac);
        }
        out_len++;
        phase += (double)ratio_den / ratio_num;
    }
    r->phase = phase - in_len;
    return out_len;
}

/* ══════════════════════════════════════════════════════════
   FxLMS 块处理 (每帧调用, 替代逐样本循环)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    int    running;
    int    filter_len;
    int    wc_total;
    int    xd_total;

    /* FIR 状态 */
    fir_filter_t  bp_fir;       /* 带通 (ref 侧) */
    fir_filter_t  bp_err[E];    /* 带通 (err 侧, 每麦独立) */
    fir_filter_t *sec_firs;     /* [E*S] 次级路径 */
    float        *sec_padded;   /* [E*S*sec_tap_padded] 含 DSP 延迟 */
    int           sec_tap_padded;

    /* Wc */
    float  *wc_fx;              /* [S * filter_len] 自适应控制滤波器 */
    float  *xd_buf;             /* [E * S * filter_len] 滤波参考延迟线 */
    float   step_size;
    float   leak;

    /* 场景切换 */
    float  *wc_old, *wc_cur;    /* [S * filter_len] */
    float   prev_probs[K];
    int     cur_scene;
    int     fade_cnt;
    float   stub_rms;

    /* 逐秒 CNN */
    float  *cnn_buf;            /* [FS_ANC] 累积参考信号 */
    int     cnn_cnt;

    /* 重采样 */
    resampler_t resamp_ref_down;
    resampler_t resamp_err_down;
    resampler_t resamp_anti_up;
} fxlms_rt_t;

static int fxlms_rt_init(fxlms_rt_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    fx->filter_len = g_sub_len / (C * S);  /* 1024 */
    fx->wc_total   = S * fx->filter_len;   /* 2048 */
    fx->xd_total   = E * S * fx->filter_len; /* 6144 */
    fx->step_size  = 0.0001f;
    fx->leak       = 1e-5f;
    fx->cur_scene  = -1;
    fx->running    = 1;

    fx->wc_fx  = (float *)calloc(fx->wc_total, sizeof(float));
    fx->wc_old = (float *)calloc(fx->wc_total, sizeof(float));
    fx->wc_cur = (float *)calloc(fx->wc_total, sizeof(float));
    fx->xd_buf = (float *)calloc(fx->xd_total, sizeof(float));
    fx->cnn_buf = (float *)malloc(FS_ANC * sizeof(float));

    /* 带通 FIR */
    fx->bp_fir.coeffs   = g_bp_fir;
    fx->bp_fir.n_taps   = g_bp_len;
    fx->bp_fir.ptr      = 0;
    fx->bp_fir.delay_line = (double *)calloc(g_bp_len, sizeof(double));

    for (int e = 0; e < E; e++) {
        fx->bp_err[e].coeffs   = g_bp_fir;
        fx->bp_err[e].n_taps   = g_bp_len;
        fx->bp_err[e].ptr      = 0;
        fx->bp_err[e].delay_line = (double *)calloc(g_bp_len, sizeof(double));
    }

    /* 次级路径 FIR (含 16 样本 DSP 延迟) */
    fx->sec_tap_padded = SEC_LEN_RAW + DSP_DELAY;
    fx->sec_firs = (fir_filter_t *)calloc(E * S, sizeof(fir_filter_t));
    fx->sec_padded = (float *)calloc(E * S * fx->sec_tap_padded, sizeof(float));
    for (int e = 0; e < E; e++) {
        for (int s = 0; s < S; s++) {
            int idx = e * S + s;
            float *p = fx->sec_padded + idx * fx->sec_tap_padded;
            memcpy(p + DSP_DELAY, g_sec_path + idx * SEC_LEN_RAW,
                   SEC_LEN_RAW * sizeof(float));
            fx->sec_firs[idx].coeffs = p;
            fx->sec_firs[idx].n_taps = fx->sec_tap_padded;
            fx->sec_firs[idx].ptr = 0;
            fx->sec_firs[idx].delay_line = (double *)calloc(fx->sec_tap_padded, sizeof(double));
        }
    }

    /* stub RMS */
    {
        float *stub = (float *)calloc(S * fx->filter_len, sizeof(float));
        for (int c = 0; c < C; c++)
            for (int s = 0; s < S; s++)
                for (int l = 0; l < fx->filter_len; l++)
                    stub[s * fx->filter_len + l] +=
                        g_sub_filters[(c * S + s) * fx->filter_len + l];
        float ss = 0;
        for (int i = 0; i < S * fx->filter_len; i++) ss += stub[i] * stub[i];
        fx->stub_rms = sqrtf(ss / (S * fx->filter_len));
        free(stub);
    }

    /* 重采样器 */
    resampler_init(&fx->resamp_ref_down,  FS_HW, FS_ANC);
    resampler_init(&fx->resamp_err_down,  FS_HW, FS_ANC);
    resampler_init(&fx->resamp_anti_up,   FS_ANC, FS_HW);

    return 0;
}

/* 构造 Wc (与 main.c 逻辑完全一致) */
static void construct_wc(fxlms_rt_t *fx, int scene_id, float *wc_out)
{
    int SC = S * C, L = fx->filter_len;
    float *blend = (float *)malloc(SC * sizeof(float));

    for (int i = 0; i < SC; i++) blend[i] = g_centroids[scene_id * SC + i];

    float bmax = blend[0];
    for (int i = 1; i < SC; i++) if (blend[i] > bmax) bmax = blend[i];
    float inv_max = 1.0f / (bmax + 1e-10f);

    for (int s = 0; s < S; s++)
        for (int l = 0; l < L; l++) {
            float v = 0.0f;
            for (int c = 0; c < C; c++) {
                float b = blend[s * C + c] * inv_max;
                if (b < 0) b = 0; if (b > 1) b = 1;
                v += b * g_sub_filters[(c * S + s) * L + l];
            }
            wc_out[s * L + l] = v;
        }

    float rms_sq = 0;
    for (int i = 0; i < S * L; i++) rms_sq += wc_out[i] * wc_out[i];
    float scale = (rms_sq > 1e-10f) ? fx->stub_rms / sqrtf(rms_sq / (S * L)) : 1.0f;
    for (int i = 0; i < S * L; i++) wc_out[i] = -wc_out[i] * scale;

    free(blend);
}

/* 每秒 CNN 处理 */
static void process_second(fxlms_rt_t *fx)
{
    float logits[K], probs[K] = {0};
    /* minmaxscaler */
    float mx = fx->cnn_buf[0], mn = fx->cnn_buf[0];
    for (int i = 1; i < FS_ANC; i++) {
        if (fx->cnn_buf[i] > mx) mx = fx->cnn_buf[i];
        if (fx->cnn_buf[i] < mn) mn = fx->cnn_buf[i];
    }
    float denom = mx - mn;
    float *cnn_in = (float *)malloc(FS_ANC * sizeof(float));
    if (denom > 1e-10f)
        for (int i = 0; i < FS_ANC; i++) cnn_in[i] = fx->cnn_buf[i] / denom;
    else
        memcpy(cnn_in, fx->cnn_buf, FS_ANC * sizeof(float));

    if (cnn_m5_forward(cnn_in, logits) == 0) {
        float logit_mx = logits[0], sum_exp = 0;
        for (int k = 0; k < K; k++) if (logits[k] > logit_mx) logit_mx = logits[k];
        for (int k = 0; k < K; k++) { probs[k] = expf(logits[k] - logit_mx); sum_exp += probs[k]; }
        for (int k = 0; k < K; k++) probs[k] /= sum_exp;
    }
    free(cnn_in);

    int new_scene = 0;
    for (int k = 1; k < K; k++) if (probs[k] > probs[new_scene]) new_scene = k;

    /* 构造 Wc */
    float wc_new[2048];
    construct_wc(fx, new_scene, wc_new);

    /* 滞回检测 */
    if (fx->cur_scene < 0) {
        memcpy(fx->wc_cur, wc_new, fx->wc_total * sizeof(float));
        memcpy(fx->wc_fx,  wc_new, fx->wc_total * sizeof(float));
        printf("[CNN] INIT  scene=%d max_prob=%.2f\n", new_scene, probs[new_scene]);
    } else {
        float dot = 0, np = 0, nc = 0;
        for (int k = 0; k < K; k++) {
            dot += fx->prev_probs[k] * probs[k];
            np  += fx->prev_probs[k] * fx->prev_probs[k];
            nc  += probs[k] * probs[k];
        }
        float cos_sim = dot / (sqrtf(np) * sqrtf(nc) + 1e-10f);
        if (cos_sim < 0.8f) {
            memcpy(fx->wc_old, fx->wc_fx,  fx->wc_total * sizeof(float));
            memcpy(fx->wc_cur, wc_new,     fx->wc_total * sizeof(float));
            fx->fade_cnt = FADE_LEN;
            printf("[CNN] RESET scene=%d cos=%.2f max_prob=%.2f\n",
                   new_scene, cos_sim, probs[new_scene]);
        }
    }
    fx->cur_scene = new_scene;
    memcpy(fx->prev_probs, probs, K * sizeof(float));
    fx->cnn_cnt = 0; /* 清空 CNN 累积缓冲 */
}

/* 逐样本 FxNLMS 处理 (单样本, 实时版本) */
static void fxlms_process_sample(fxlms_rt_t *fx, float ref_sample, const float *err_mic)
{
    int L = fx->filter_len, wc_tot = fx->wc_total;

    /* 淡化: 过渡期间用混合 Wc */
    if (fx->fade_cnt > 0) {
        float a = (float)fx->fade_cnt / (float)FADE_LEN;
        for (int i = 0; i < wc_tot; i++)
            fx->wc_fx[i] = a * fx->wc_old[i] + (1.0f - a) * fx->wc_cur[i];
        fx->fade_cnt--;
        if (fx->fade_cnt == 0)
            memcpy(fx->wc_fx, fx->wc_cur, wc_tot * sizeof(float));
    }

    /* 带通滤波参考信号 */
    float ref_filt = fir_tick(&fx->bp_fir, ref_sample);

    /* 次级路径: Ref → Fx[e][s] */
    float Fx[E * S];
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++)
            Fx[e * S + s] = fir_tick(&fx->sec_firs[e * S + s], ref_filt);

    /* Xd 延迟线: roll + 写入 Fx */
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++)
            for (int k = L - 1; k > 0; k--)
                fx->xd_buf[(e * S + s) * L + k] = fx->xd_buf[(e * S + s) * L + (k - 1)];
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++)
            fx->xd_buf[(e * S + s) * L + 0] = Fx[e * S + s];

    /* 前向: anti_est[e] = Wc · Xd */
    float anti_est[E];
    for (int e = 0; e < E; e++) { anti_est[e] = 0;
        for (int s = 0; s < S; s++) for (int k = 0; k < L; k++)
            anti_est[e] += fx->wc_fx[s * L + k] * fx->xd_buf[(e * S + s) * L + k]; }

    /* 功率归一化 */
    float power[S];
    for (int s = 0; s < S; s++) { power[s] = 1e-10f;
        for (int e = 0; e < E; e++) for (int k = 0; k < L; k++)
            power[s] += fx->xd_buf[(e * S + s) * L + k] * fx->xd_buf[(e * S + s) * L + k];
        power[s] /= (float)(E * L); }

    /* FxNLMS 梯度更新 (淡化期间跳过) */
    if (fx->fade_cnt == 0) {
        for (int s = 0; s < S; s++) {
            float inv_pwr = 1.0f / power[s];
            for (int e = 0; e < E; e++) {
                float err_filt = fir_tick(&fx->bp_err[e], err_mic[e] + anti_est[e]);
                for (int k = 0; k < L; k++)
                    fx->wc_fx[s * L + k] -= fx->step_size * err_filt
                        * fx->xd_buf[(e * S + s) * L + k] * inv_pwr;
            }
            for (int k = 0; k < L; k++) fx->wc_fx[s * L + k] *= (1.0f - fx->step_size * fx->leak);
        }
    }
}

/* 获取当前反噪声输出 */
static void fxlms_get_anti(fxlms_rt_t *fx, float *anti_out)
{
    int L = fx->filter_len;
    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int e = 0; e < E; e++) for (int k = 0; k < L; k++)
            anti_out[s] += fx->wc_fx[s * L + k] * fx->xd_buf[(e * S + s) * L + k]; }
}

static void fxlms_rt_free(fxlms_rt_t *fx)
{
    free(fx->wc_fx); free(fx->wc_old); free(fx->wc_cur);
    free(fx->xd_buf); free(fx->cnn_buf);
    free(fx->bp_fir.delay_line);
    for (int e = 0; e < E; e++) free(fx->bp_err[e].delay_line);
    for (int i = 0; i < E * S; i++) free(fx->sec_firs[i].delay_line);
    free(fx->sec_firs); free(fx->sec_padded);
}

/* ══════════════════════════════════════════════════════════
   信号处理 — Ctrl+C 安全退出
   ══════════════════════════════════════════════════════════ */
static volatile int g_running = 1;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) { g_running = 0; return TRUE; }
    return FALSE;
}

/* ══════════════════════════════════════════════════════════
   主函数
   ══════════════════════════════════════════════════════════ */
int main(void)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    printf("══════════════════════════════════════════\n");
    printf("  GFANC FxNLMS — Real-time ANC (WASAPI)\n");
    printf("══════════════════════════════════════════\n\n");

    /* ── 1. 加载权重 ── */
    printf("Loading weights...\n");
    g_sec_len    = bin_load_float("data/secondary_path.bin", &g_sec_path);
    g_pri_len    = bin_load_float("data/primary_path.bin", &g_pri_path);
    g_sub_len    = bin_load_float("data/sub_filters.bin", &g_sub_filters);
    g_bp_len     = bin_load_float("data/bandpass_fir.bin", &g_bp_fir);
    int n_scene  = bin_load_float("data/scene_defs.bin", &g_centroids);
    (void)n_scene; (void)g_pri_len; (void)g_pri_path;

    if (!g_sec_path || !g_sub_filters || !g_centroids || !g_bp_fir) {
        fprintf(stderr, "ERROR: missing .bin files\n"); return 1;
    }
    printf("  sec=%d sub=%d centroids=%d bp=%d\n", g_sec_len, g_sub_len, n_scene, g_bp_len);

    /* ── 2. CNN 初始化 ── */
    if (cnn_m5_init() != 0) {
        fprintf(stderr, "ERROR: CNN init failed\n"); return 1;
    }

    /* ── 3. FxLMS 初始化 ── */
    fxlms_rt_t fx;
    fxlms_rt_init(&fx);
    printf("  ANC ready: E=%d S=%d C=%d K=%d L=%d\n", E, S, C, K, fx.filter_len);

    /* ── 4. WASAPI 打开 ── */
    wasapi_t *wasapi = wasapi_open(
        L"YDM6MIC",         /* 麦克风阵列 */
        L"USB Audio Device",/* USB 扬声器 */
        FS_HW,              /* 48kHz */
        6,                  /* 6ch 输入: ch0=参考, ch1-3=误差 */
        2,                  /* 2ch 输出: 扬声器 */
        LATENCY_MS);        /* 10ms 延迟 */

    if (!wasapi) {
        fprintf(stderr, "ERROR: WASAPI init failed\n");
        wasapi_list_devices();
        fxlms_rt_free(&fx); return 1;
    }

    /* ── 5. 分配缓冲 ── */
    int chunk_hw = 256;  /* 48kHz 每帧处理量 (5.3ms) */
    float *cap_buf  = (float *)malloc(chunk_hw * 6 * sizeof(float));  /* 6ch 捕获 */
    float *out_buf  = (float *)malloc(chunk_hw * 2 * sizeof(float));  /* 2ch 渲染 */
    float *ref_hw    = (float *)malloc(chunk_hw * sizeof(float));     /* ch0 参考 */
    float *err_hw    = (float *)malloc(chunk_hw * 3 * sizeof(float)); /* ch1-3 误差 */
    float *ref_anc   = (float *)malloc(chunk_hw * sizeof(float));     /* ~85 样 @16k */
    float *err_anc   = (float *)malloc(chunk_hw * 3 * sizeof(float));
    float *anti_anc  = (float *)malloc(chunk_hw * 2 * sizeof(float));
    float *anti_hw   = (float *)malloc(chunk_hw * 2 * sizeof(float));

    printf("\nPress Ctrl+C to stop.\n\n");

    /* ── 6. 启动流 ── */
    if (wasapi_start(wasapi) != 0) {
        fprintf(stderr, "ERROR: WASAPI start failed\n");
        goto cleanup;
    }

    /* ── 7. 主循环 ── */
    while (g_running) {
        /* 读取捕获 */
        int n_hw = wasapi_read_capture(wasapi, cap_buf, chunk_hw);
        if (n_hw <= 0) { Sleep(1); continue; }

        /* 声道拆分: ch0=参考麦, ch1-3=误差麦 */
        for (int i = 0; i < n_hw; i++) {
            ref_hw[i] = cap_buf[i * 6 + 0];
            err_hw[i * 3 + 0] = cap_buf[i * 6 + 1];
            err_hw[i * 3 + 1] = cap_buf[i * 6 + 2];
            err_hw[i * 3 + 2] = cap_buf[i * 6 + 3];
        }

        /* 48kHz → 16kHz 重采样 */
        int n_ref = resample_down(ref_hw, n_hw, 1, ref_anc, &fx.resamp_ref_down);
        int n_err = resample_down(err_hw, n_hw, 3, err_anc, &fx.resamp_err_down);
        int n_proc = (n_ref < n_err) ? n_ref : n_err;

        /* ANC 逐样本处理 @ 16kHz */
        for (int i = 0; i < n_proc; i++) {
            float ref_sample = ref_anc[i];
            float err_mic[3] = { err_anc[i * 3 + 0], err_anc[i * 3 + 1], err_anc[i * 3 + 2] };

            fxlms_process_sample(&fx, ref_sample, err_mic);
            fxlms_get_anti(&fx, anti_anc + i * 2);

            /* CNN 累积 (参考信号, 带通滤波后) */
            if (fx.cnn_cnt < FS_ANC) {
                float r_filt = fir_tick(&fx.bp_fir, ref_sample);
                fx.cnn_buf[fx.cnn_cnt++] = r_filt;  /* 注意: 这是第二遍带通 */
            }
        }

        /* 每秒 CNN 更新 */
        if (fx.cnn_cnt >= FS_ANC)
            process_second(&fx);

        /* 16kHz → 48kHz 上采样反噪声 */
        int n_anti_hw = resample_up(anti_anc, n_proc, 2, anti_hw, &fx.resamp_anti_up);

        /* 混音到输出缓冲并写入 */
        int n_out = (n_anti_hw < n_hw) ? n_anti_hw : n_hw;
        for (int i = 0; i < n_out; i++) {
            out_buf[i * 2 + 0] = anti_hw[i * 2 + 0];
            out_buf[i * 2 + 1] = anti_hw[i * 2 + 1];
        }
        wasapi_write_render(wasapi, out_buf, n_out);
    }

    /* ── 8. 清理 ── */
cleanup:
    wasapi_stop(wasapi);
    wasapi_close(wasapi);

    free(cap_buf); free(out_buf); free(ref_hw); free(err_hw);
    free(ref_anc); free(err_anc); free(anti_anc); free(anti_hw);
    fxlms_rt_free(&fx);

    bin_free(g_sec_path); bin_free(g_pri_path);
    bin_free(g_sub_filters); bin_free(g_centroids); bin_free(g_bp_fir);

    printf("Done.\n");
    return 0;
}
