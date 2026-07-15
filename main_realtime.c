/** GFANC FxNLMS — 实时 WASAPI 降噪.
 *
 * 编译: gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601
 *        main_realtime.c src/scene_controller.c src/fxnlms_mimo.c
 *        src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c
 *        src/wasapi_io.c -lm -lole32 -o gfanc_realtime.exe
 * 运行: ./gfanc_realtime.exe  (Ctrl+C 停止)
 *
 * 硬件:
 *   输入:  YDM6MIC 麦克风阵列, 48kHz, 6ch (ch0=参考, ch1-3=误差麦)
 *   输出:  USB Audio Device 扬声器, 48kHz, 2ch
 *   内部:  重采样到 16kHz → ANC → 重采样到 48kHz 输出
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <windows.h>

#include "fir_filter.h"
#include "binary_loader.h"
#include "scene_controller.h"
#include "fxnlms_mimo.h"
#include "wasapi_io.h"

/* ══════════════════════════════════════════════════════════
   参数
   ══════════════════════════════════════════════════════════ */
#define FS_HW      48000
#define FS_ANC     16000
#define E          3
#define S          2
#define C          15
#define K          8
#define L          1024
#define SEC_LEN    1024
#define PRI_LEN    1024
#define BP_LEN     1024
#define DSP_DELAY  16
#define FADE_LEN   16
#define LATENCY_MS 10

/* ══════════════════════════════════════════════════════════
   流式重采样器
   ══════════════════════════════════════════════════════════ */
typedef struct {
    double phase;
    int    ratio_num, ratio_den;
} resampler_t;

static void resampler_init(resampler_t *r, int fs_in, int fs_out) {
    r->phase = 0.0; r->ratio_num = fs_in; r->ratio_den = fs_out;
}

static int resample_down(const float *in, int in_len, int in_ch,
                          float *out, resampler_t *r) {
    int out_len = 0;
    double phase = r->phase;
    int rn = r->ratio_num, rd = r->ratio_den;
    while (1) {
        int pos = (int)phase;
        if (pos >= in_len) break;
        double frac = phase - pos;
        for (int c = 0; c < in_ch; c++) {
            float a = in[in_len * c + pos];
            float b = (pos + 1 < in_len) ? in[in_len * c + pos + 1] : a;
            out[out_len * in_ch + c] = (float)(a * (1.0 - frac) + b * frac);
        }
        out_len++; phase += (double)rn / rd;
    }
    r->phase = phase - in_len;
    return out_len;
}

static int resample_up(const float *in, int in_len, int in_ch,
                        float *out, resampler_t *r) {
    int out_len = 0;
    double phase = r->phase;
    int rn = r->ratio_num, rd = r->ratio_den;
    while (1) {
        int pos = (int)phase;
        if (pos >= in_len) break;
        double frac = phase - pos;
        for (int c = 0; c < in_ch; c++) {
            float a = in[in_len * c + pos];
            float b = (pos + 1 < in_len) ? in[in_len * c + pos + 1] : a;
            out[out_len * in_ch + c] = (float)(a * (1.0 - frac) + b * frac);
        }
        out_len++; phase += (double)rd / rn;
    }
    r->phase = phase - in_len;
    return out_len;
}

/* ══════════════════════════════════════════════════════════
   Ctrl+C 处理
   ══════════════════════════════════════════════════════════ */
static volatile int g_running = 1;
static BOOL WINAPI ctrl_handler(DWORD type) {
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
    printf("  GFANC FxNLMS — Realtime ANC (WASAPI)\n");
    printf("══════════════════════════════════════════\n\n");

    /* ── 1. 加载权重 ── */
    printf("Loading weights...\n");
    float *sec_path, *pri_path, *sub_filters, *centroids, *bp_coeff;
    int sec_len = bin_load_float("data/secondary_path.bin", &sec_path);
    int pri_len = bin_load_float("data/primary_path.bin", &pri_path);
    int sub_len = bin_load_float("data/sub_filters.bin", &sub_filters);
    int bp_len  = bin_load_float("data/bandpass_fir.bin", &bp_coeff);
    int n_scene = bin_load_float("data/scene_defs.bin", &centroids);
    printf("  OK: sec=%d sub=%d bp=%d scenes=%d, L=%d\n",
           sec_len, sub_len, bp_len, n_scene, sub_len / (C * S));
    (void)pri_len; (void)n_scene; (void)pri_path;

    /* ── 2. 初始化组件 ── */
    /* 带通 FIR */
    fir_filter_t bp_fir = { bp_coeff, (double *)calloc(BP_LEN, sizeof(double)), BP_LEN, 0 };

    /* 误差麦带通 (每个误差麦独立) */
    fir_filter_t bp_err[E];
    for (int e = 0; e < E; e++) {
        bp_err[e].coeffs = bp_coeff;
        bp_err[e].n_taps = BP_LEN;
        bp_err[e].ptr = 0;
        bp_err[e].delay_line = (double *)calloc(BP_LEN, sizeof(double));
    }

    /* 次级路径 FIR (含 DSP 延迟) */
    int sec_padded = SEC_LEN + DSP_DELAY;
    fir_filter_t *sec_firs = (fir_filter_t *)calloc(E * S, sizeof(fir_filter_t));
    float *sec_coeffs = (float *)calloc(E * S * sec_padded, sizeof(float));
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e * S + s;
            memcpy(sec_coeffs + idx * sec_padded + DSP_DELAY,
                   sec_path + idx * SEC_LEN, SEC_LEN * sizeof(float));
            sec_firs[idx].coeffs = sec_coeffs + idx * sec_padded;
            sec_firs[idx].n_taps = sec_padded;
            sec_firs[idx].delay_line = (double *)calloc(sec_padded, sizeof(double));
        }

    /* Scene Controller */
    scene_ctrl_t sc;
    scene_ctrl_init(&sc, centroids, sub_filters, L);

    /* FxNLMS (淡化期间手动管理 Wc, 所以用 NULL sec_firs 占位) */
    fxnlms_mimo_t fx;
    fxnlms_init(&fx, E, S, L, 0.0001f, 1e-5f);

    printf("  ANC ready: E=%d S=%d C=%d K=%d L=%d\n", E, S, C, K, L);

    /* ── 3. WASAPI ── */
    wasapi_t *wasapi = wasapi_open(
        L"YDM6MIC", L"USB Audio Device",
        FS_HW, 6, 2, LATENCY_MS);
    if (!wasapi) {
        fprintf(stderr, "ERROR: WASAPI init failed\n");
        wasapi_list_devices();
        return 1;
    }

    /* ── 4. 缓冲分配 ── */
    int chunk_hw = 256;
    float *cap_buf  = (float *)malloc(chunk_hw * 6 * sizeof(float));
    float *out_buf  = (float *)malloc(chunk_hw * 2 * sizeof(float));
    float *ref_hw    = (float *)malloc(chunk_hw * sizeof(float));
    float *err_hw    = (float *)malloc(chunk_hw * 3 * sizeof(float));
    float *ref_anc   = (float *)malloc(chunk_hw * sizeof(float));
    float *err_anc   = (float *)malloc(chunk_hw * 3 * sizeof(float));
    float *anti_anc  = (float *)malloc(chunk_hw * 2 * sizeof(float));
    float *anti_hw   = (float *)malloc(chunk_hw * 2 * sizeof(float));

    resampler_t r_ref_down, r_err_down, r_anti_up;
    resampler_init(&r_ref_down, FS_HW, FS_ANC);
    resampler_init(&r_err_down, FS_HW, FS_ANC);
    resampler_init(&r_anti_up,  FS_ANC, FS_HW);

    /* CNN 缓冲 */
    float *cnn_buf = (float *)malloc(FS_ANC * sizeof(float));
    int    cnn_cnt = 0;

    /* CrossFader 状态 */
    int   fade_cnt = 0;
    float wc_old[2048], wc_cur[2048];
    int   first_scene = 1;

    printf("\nPress Ctrl+C to stop.\n\n");
    if (wasapi_start(wasapi) != 0) {
        fprintf(stderr, "ERROR: WASAPI start failed\n");
        return 1;
    }

    /* ── 5. 主循环 ── */
    while (g_running) {
        int n_hw = wasapi_read_capture(wasapi, cap_buf, chunk_hw);
        if (n_hw <= 0) { Sleep(1); continue; }

        /* 声道拆分 */
        for (int i = 0; i < n_hw; i++) {
            ref_hw[i] = cap_buf[i * 6 + 0];
            err_hw[i * 3 + 0] = cap_buf[i * 6 + 1];
            err_hw[i * 3 + 1] = cap_buf[i * 6 + 2];
            err_hw[i * 3 + 2] = cap_buf[i * 6 + 3];
        }

        /* 48kHz → 16kHz */
        int n_ref = resample_down(ref_hw, n_hw, 1, ref_anc, &r_ref_down);
        int n_err = resample_down(err_hw, n_hw, 3, err_anc, &r_err_down);
        int n_proc = (n_ref < n_err) ? n_ref : n_err;

        /* ANC @ 16kHz */
        for (int i = 0; i < n_proc; i++) {
            float ref_filt = fir_tick(&bp_fir, ref_anc[i]);

            /* CrossFader */
            if (fade_cnt > 0) {
                float a = (float)fade_cnt / FADE_LEN;
                for (int j = 0; j < S * L; j++)
                    fx.wc[j] = a * wc_old[j] + (1.0f - a) * wc_cur[j];
                fade_cnt--;
                if (fade_cnt == 0) memcpy(fx.wc, wc_cur, S * L * sizeof(float));
            }

            /* Fx = Sec ⊗ ref_filt */
            float Fx_arr[E * S];
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++)
                    Fx_arr[e * S + s] = fir_tick(&sec_firs[e * S + s], ref_filt);

            /* 扰动 = 带通滤波后的误差麦信号 */
            float dist[E];
            for (int e = 0; e < E; e++)
                dist[e] = fir_tick(&bp_err[e], err_anc[i * 3 + e]);

            /* FANLMS: 内部做 Xd roll + anti + err + 梯度 */
            float anti_spk[S], err_dummy[E];
            if (fade_cnt == 0)
                fxnlms_tick(&fx, Fx_arr, dist, anti_spk, err_dummy);
            else
                fxnlms_forward_only(&fx, Fx_arr, anti_spk, err_dummy);

            anti_anc[i * 2 + 0] = anti_spk[0];
            anti_anc[i * 2 + 1] = anti_spk[1];

            /* CNN 累积 */
            if (cnn_cnt < FS_ANC)
                cnn_buf[cnn_cnt++] = ref_filt;
        }

        /* 每秒 CNN */
        if (cnn_cnt >= FS_ANC) {
            float probs[K];
            int new_scene = scene_ctrl_process(&sc, cnn_buf, wc_cur, probs);

            if (first_scene) {
                fxnlms_set_wc(&fx, wc_cur);
                printf("[CNN] INIT  scene=%d max_prob=%.2f\n", new_scene, probs[new_scene]);
                first_scene = 0;
            } else {
                float dot = 0, np = 0, nc = 0;
                for (int k = 0; k < K; k++) {
                    dot += sc.prev_probs[k] * probs[k];
                    np  += sc.prev_probs[k] * sc.prev_probs[k];
                    nc  += probs[k] * probs[k];
                }
                float cos_sim = dot / (sqrtf(np) * sqrtf(nc) + 1e-10f);
                if (cos_sim < 0.8f) {
                    memcpy(wc_old, fx.wc, S * L * sizeof(float));
                    fade_cnt = FADE_LEN;
                    printf("[CNN] RESET scene=%d cos=%.2f max_prob=%.2f\n",
                           new_scene, cos_sim, probs[new_scene]);
                }
            }
            memcpy(sc.prev_probs, probs, K * sizeof(float));
            cnn_cnt = 0;
        }

        /* 16kHz → 48kHz + 输出 */
        int n_anti_hw = resample_up(anti_anc, n_proc, 2, anti_hw, &r_anti_up);
        int n_out = (n_anti_hw < n_hw) ? n_anti_hw : n_hw;
        for (int i = 0; i < n_out; i++) {
            out_buf[i * 2 + 0] = anti_hw[i * 2 + 0];
            out_buf[i * 2 + 1] = anti_hw[i * 2 + 1];
        }
        wasapi_write_render(wasapi, out_buf, n_out);
    }

    /* ── 6. 清理 ── */
    wasapi_stop(wasapi);
    wasapi_close(wasapi);

    free(cap_buf); free(out_buf); free(ref_hw); free(err_hw);
    free(ref_anc); free(err_anc); free(anti_anc); free(anti_hw); free(cnn_buf);
    fxnlms_free(&fx);
    free(bp_fir.delay_line);
    for (int e = 0; e < E; e++) free(bp_err[e].delay_line);
    for (int i = 0; i < E * S; i++) free(sec_firs[i].delay_line);
    free(sec_firs); free(sec_coeffs);
    bin_free(sec_path); bin_free(pri_path); bin_free(sub_filters);
    bin_free(centroids); bin_free(bp_coeff);
    printf("Done.\n");
    return 0;
}
