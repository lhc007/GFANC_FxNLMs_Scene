/** GFANC FxNLMS — 离线 WAV 降噪.
 *
 * 编译: gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c
 *              src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c
 *              -lm -o main.exe
 * 运行: ./main.exe <noise.wav>
 * 输出: anti_out.wav (S=2ch 反噪声), error_out.wav (E=3ch 残差)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <windows.h>

#include "fir_filter.h"
#include "binary_loader.h"
#include "scene_controller.h"
#include "fxnlms_mimo.h"

/* ══════════════════════════════════════════════════════════
   类型
   ══════════════════════════════════════════════════════════ */
typedef struct {
    int sr, ch, n;
    float *data;
} wav_t;

/* ══════════════════════════════════════════════════════════
   WAV 读写 (16-bit PCM, 内联实现 — 零依赖)
   ══════════════════════════════════════════════════════════ */
static int wav_read_mono(const char *path, wav_t *w)
{
    memset(w, 0, sizeof(*w));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char riff[5] = {0}; fread(riff, 1, 4, f);
    if (strcmp(riff, "RIFF")) { fclose(f); return -1; }
    fseek(f, 4, SEEK_CUR);
    char wave[5] = {0}; fread(wave, 1, 4, f);
    if (strcmp(wave, "WAVE")) { fclose(f); return -1; }

    int fmt_done = 0, data_done = 0; unsigned int data_bytes = 0;
    while (!fmt_done || !data_done) {
        char id[5] = {0};
        if (fread(id, 1, 4, f) < 4) break;
        unsigned int sz; fread(&sz, 4, 1, f);
        if (!strcmp(id, "fmt ")) {
            short afmt, nch; int sr;
            fread(&afmt, 2, 1, f); fread(&nch, 2, 1, f);
            fread(&sr, 4, 1, f); fseek(f, 6, SEEK_CUR);
            short bps; fread(&bps, 2, 1, f);
            w->sr = sr; w->ch = nch;
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
            fmt_done = 1;
        } else if (!strcmp(id, "data")) {
            data_bytes = sz; data_done = 1;
        } else {
            fseek(f, sz, SEEK_CUR);
        }
    }
    if (!fmt_done || !data_done) { fclose(f); return -1; }

    w->n = data_bytes / (w->ch * 2);
    w->data = (float *)malloc(w->n * sizeof(float));
    short *tmp = (short *)malloc(data_bytes);
    fread(tmp, 1, data_bytes, f); fclose(f);

    for (int i = 0; i < w->n; i++) {
        int sum = 0;
        for (int c = 0; c < w->ch; c++) sum += tmp[i * w->ch + c];
        w->data[i] = (float)(sum / w->ch) / 32768.0f;
    }
    free(tmp); return 0;
}

static void wav_write(const char *path, const float *data, int n, int ch, int sr)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int bps = 16, frame = ch * 2, data_sz = n * frame;
    unsigned int v32; unsigned short v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + data_sz; fwrite(&v32, 4, 1, f);
    fwrite("WAVE", 1, 4, f); fwrite("fmt ", 1, 4, f);
    v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f); v16 = ch; fwrite(&v16, 2, 1, f);
    v32 = sr; fwrite(&v32, 4, 1, f); v32 = sr * frame; fwrite(&v32, 4, 1, f);
    v16 = frame; fwrite(&v16, 2, 1, f); v16 = bps; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_sz, 4, 1, f);
    for (int i = 0; i < n; i++)
        for (int c = 0; c < ch; c++) {
            float v = data[c * n + i];
            if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
            short s = (short)(v * 32767.0f); fwrite(&s, 2, 1, f);
        }
    fclose(f);
}

static float *resample_mono(const float *in, int n_in, int sr_in, int sr_out, int *n_out)
{
    *n_out = (int)((long long)n_in * sr_out / sr_in);
    float *out = (float *)malloc(*n_out * sizeof(float));
    for (int i = 0; i < *n_out; i++) {
        double pos = (double)i * sr_in / sr_out;
        int i0 = (int)pos; double frac = pos - i0;
        if (i0 + 1 < n_in) out[i] = (float)(in[i0] * (1.0 - frac) + in[i0 + 1] * frac);
        else if (i0 < n_in) out[i] = in[i0];
        else out[i] = 0.0f;
    }
    return out;
}

/* ══════════════════════════════════════════════════════════
   参数
   ══════════════════════════════════════════════════════════ */
#define E       3
#define S       2
#define C       15
#define K       8
#define FS      16000
#define BP_LEN  1024
#define PRI_LEN 1024
#define SEC_LEN 1024
#define DSP_DELAY 16
#define MIC_PRE_GAIN  1.0f    /* 输入预增益 (离线已做峰值归一化, 默认1.0; >1.0模拟硬件前放) */
#define FADE_LEN  1600  /* Wc切换CrossFader时长 (100ms @16k, =20Hz×2周期) */

/* ══════════════════════════════════════════════════════════
   主函数
   ══════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        printf("GFANC FxNLMS — Offline WAV ANC Demo\n\n");
        printf("Usage: %s <noise.wav>\n\n", argv[0]);
        printf("Output: anti_out.wav (%d ch), error_out.wav (%d ch)\n", S, E);
        return 1;
    }

    /* ── 1. 加载权重 ── */
    printf("Loading weights...\n");
    float *sec_path, *pri_path, *sub_filters, *centroids, *bp_coeff;
    int sec_len  = bin_load_float("data/secondary_path.bin", &sec_path);
    int pri_len  = bin_load_float("data/primary_path.bin", &pri_path);
    int sub_len  = bin_load_float("data/sub_filters.bin", &sub_filters);
    int bp_len   = bin_load_float("data/bandpass_fir.bin", &bp_coeff);
    int n_scene  = bin_load_float("data/scene_defs.bin", &centroids);
    int L = sub_len / (C * S); /* 1024 */
    printf("  OK: sec=%d pri=%d sub=%d bp=%d L=%d\n", sec_len, pri_len, sub_len, bp_len, L);
    (void)pri_len; (void)n_scene;

    /* ── 2. 初始化组件 ── */
    /* CNN 必须先初始化 (加载权重) */
    extern int cnn_m5_init(void);
    if (cnn_m5_init() != 0) { fprintf(stderr, "CNN init failed\n"); return 1; }
    printf("  CNN loaded.\n");

    /* 2a. 带通 FIR */
    fir_filter_t bp_fir = { bp_coeff, (double *)calloc(BP_LEN, sizeof(double)), BP_LEN, 0 };

    /* 2b. 次级路径 FIR (含 DSP 延迟) */
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

    /* 2c. 初级路径 FIR (仅 R=0, 无延迟) */
    fir_filter_t pri_firs[E];
    for (int e = 0; e < E; e++) {
        pri_firs[e].coeffs = pri_path + e * 2 * PRI_LEN; /* R=0 */
        pri_firs[e].n_taps = PRI_LEN;
        pri_firs[e].delay_line = (double *)calloc(PRI_LEN, sizeof(double));
        pri_firs[e].ptr = 0;
    }

    /* 2d. Scene Controller */
    scene_ctrl_t sc;
    scene_ctrl_init(&sc, centroids, sub_filters, L);

    /* 2e. FxNLMS */
    fxnlms_mimo_t fx;
    fxnlms_init(&fx, E, S, L, 0.0001f, 1e-6f);  /* leak 已从 step_size 解耦 */
    printf("  System ready (CNN loaded).\n");

    /* ── 3. 读取 WAV ── */
    wav_t wav;
    if (wav_read_mono(argv[1], &wav) != 0) {
        fprintf(stderr, "ERROR: Cannot read %s\n", argv[1]); return 1;
    }
    printf("\nInput: %d Hz, %d ch, %d samples (%.1fs)\n",
           wav.sr, wav.ch, wav.n, (double)wav.n / wav.sr);

    int N;
    float *ref = wav.data, *ref_resampled = NULL;
    if (wav.sr != FS) {
        ref_resampled = resample_mono(wav.data, wav.n, wav.sr, FS, &N);
        ref = ref_resampled;
        printf("Resampled: %d Hz -> %d Hz (%d samples)\n", wav.sr, FS, N);
    } else N = wav.n;

    /* 峰值归一化 + 初始带通 → noise_bp (匹配 Python noise_loader) */
    {
        float peak = 0;
        for (int i = 0; i < N; i++) { float a = fabsf(ref[i]); if (a > peak) peak = a; }
        if (peak > 1e-12f) for (int i = 0; i < N; i++) ref[i] /= peak;
    }
    float *noise_bp = (float *)malloc(N * sizeof(float));
    {
        fir_filter_t bp_tmp = { bp_fir.coeffs, (double *)calloc(BP_LEN, sizeof(double)), BP_LEN, 0 };
        for (int i = 0; i < N; i++) noise_bp[i] = fir_tick(&bp_tmp, ref[i]);
        free(bp_tmp.delay_line);
    }

    /* ── 4. 离线 ANC ── */
    int chunk = FS, n_sec = (N + chunk - 1) / chunk;
    float *anti_out = (float *)calloc(S * N, sizeof(float));
    float *err_out  = (float *)calloc(E * N, sizeof(float));
    float *wc_fx = NULL; /* 指向 fx.wc (FANLMS 内部) */

    /* CrossFader 状态 */
    int   fade_cnt = 0;
    float wc_old[2048], wc_cur[2048];

    float sum_db_band = 0, sum_db_full = 0;
    clock_t t0 = clock();

    printf("\n%4s | %25s | %10s | %10s | %s\n", "Sec", "Top-3 Scenes", "dB(Band)", "dB(Full)", "Action");
    for (int i = 0; i < 85; i++) printf("-"); printf("\n");

    for (int sec = 0; sec < n_sec; sec++) {
        int start = sec * chunk, len = (start + chunk <= N) ? chunk : (N - start);
        if (len <= 0) break;

        /* 4a. ref_filt = bandpass(noise_bp) × pre_gain */
        float *ref_filt = (float *)malloc(chunk * sizeof(float));
        for (int i = 0; i < len; i++)
            ref_filt[i] = fir_tick(&bp_fir, noise_bp[start + i]) * MIC_PRE_GAIN;

        /* 4b. Dis_band = Pri(ref_filt), Dis_full = Pri(noise_bp) */
        float *dis_buf  = (float *)calloc(E * len, sizeof(float));
        float *full_buf = (float *)calloc(E * len, sizeof(float));
        for (int e = 0; e < E; e++) {
            fir_filter_t pri_tmp = pri_firs[e];
            for (int n = 0; n < len; n++)
                dis_buf[e * len + n] = fir_tick(&pri_tmp, ref_filt[n]);
            pri_firs[e].ptr = pri_tmp.ptr; /* sync ptr back */

            fir_filter_t pri_tmp2 = pri_firs[e];
            pri_tmp2.delay_line = (double *)calloc(PRI_LEN, sizeof(double));
            pri_tmp2.ptr = 0;
            for (int n = 0; n < len; n++)
                full_buf[e * len + n] = fir_tick(&pri_tmp2, noise_bp[start + n]);
            free(pri_tmp2.delay_line);
        }

        /* 4c. 参考能量 (所有 Mic 平均) */
        float dis_pwr_band = 0, dis_pwr_full = 0, fx_pwr_diag = 0;
        for (int e = 0; e < E; e++)
            for (int n = 0; n < len; n++) {
                dis_pwr_band += dis_buf[e * len + n] * dis_buf[e * len + n];
                dis_pwr_full += full_buf[e * len + n] * full_buf[e * len + n];
            }
        dis_pwr_band /= (len * E); dis_pwr_full /= (len * E);

        /* 4d. CNN 场景分类 (输入: noise_bp + minmaxscaler) */
        float probs[K];
        int old_scene = sc.cur_scene;
        int new_scene = scene_ctrl_process(&sc, noise_bp + start, wc_cur, probs);

        /* Top-3 */
        int top3[3] = {-1, -1, -1};
        for (int i = 0; i < K; i++) {
            int dup = 0;
            for (int j = 0; j < 3; j++) if (top3[j] == i) { dup = 1; break; }
            if (dup) continue;
            for (int j = 0; j < 3; j++)
                if (top3[j] < 0 || probs[i] > probs[top3[j]]) {
                    for (int k = 2; k > j; k--) top3[k] = top3[k - 1];
                    top3[j] = i; break;
                }
        }
        char scene_str[40];
        snprintf(scene_str, sizeof(scene_str), "%d:%.2f,%d:%.2f,%d:%.2f",
                 top3[0], probs[top3[0]], top3[1], probs[top3[1]], top3[2], probs[top3[2]]);

        /* 4e. 滞回检测 + CrossFader */
        static int first_sec = 1;
        char action[20] = "-";
        if (first_sec) {
            fxnlms_set_wc(&fx, wc_cur);
            snprintf(action, sizeof(action), "INIT");
            first_sec = 0;
        } else {
            float dot = 0, np = 0, nc = 0;
            for (int k = 0; k < K; k++) {
                dot += sc.prev_probs[k] * probs[k];
                np  += sc.prev_probs[k] * sc.prev_probs[k];
                nc  += probs[k] * probs[k];
            }
            if (dot / (sqrtf(np) * sqrtf(nc) + 1e-10f) < 0.8f) {
                memcpy(wc_old, fx.wc, S * L * sizeof(float));
                fade_cnt = FADE_LEN;
                snprintf(action, sizeof(action), "RESET");
            }
        }
        memcpy(sc.prev_probs, probs, K * sizeof(float));
        wc_fx = fx.wc; /* 指向 FxNLMS 内部 Wc */

        /* 首秒诊断 */
        if (sec == 0) {
            float wc_new_rms = 0;
            for (int i = 0; i < S * L; i++) wc_new_rms += wc_cur[i] * wc_cur[i];
            wc_new_rms = sqrtf(wc_new_rms / (S * L));
            printf("[Diag] CNN logits: (scene=%d max_prob=%.2f)\n", new_scene, probs[new_scene]);
            printf("[Diag] Wc_new RMS: %.4f\n", wc_new_rms);
            printf("[Diag] Dis (带内) RMS: %.4f\n", sqrtf(dis_pwr_band));
            printf("[Diag] Dis (全频) RMS: %.4f\n", sqrtf(dis_pwr_full));
        }

        /* 4f. 逐样本 FxNLMS */
        float err_pwr = 0;
        for (int n = 0; n < len; n++) {
            int idx = start + n;
            float rn_bp = ref_filt[n];

            /* CrossFader: 混合 Wc */
            if (fade_cnt > 0) {
                float a = (float)fade_cnt / FADE_LEN;
                for (int i = 0; i < S * L; i++)
                    fx.wc[i] = a * wc_old[i] + (1.0f - a) * wc_cur[i];
                fade_cnt--;
                if (fade_cnt == 0) memcpy(fx.wc, wc_cur, S * L * sizeof(float));
            }

            /* Fx = Sec ⊗ rn_bp */
            float Fx_arr[E * S];
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++) {
                    float v = fir_tick(&sec_firs[e * S + s], rn_bp);
                    Fx_arr[e * S + s] = v;
                    if (sec == 0) fx_pwr_diag += v * v;
                }

            /* 扰动 = Dis (带限) */
            float dist[E];
            for (int e = 0; e < E; e++) dist[e] = dis_buf[e * len + n];

            /* FxNLMS: 内部做 Xd roll + anti_est + err + 梯度 */
            float anti_spk[S], err_sig[E];
            if (fade_cnt == 0)
                fxnlms_tick(&fx, Fx_arr, dist, anti_spk, err_sig);
            else
                fxnlms_forward_only(&fx, Fx_arr, anti_spk, err_sig);

            /* NaN/Inf 保护: 防止异常值进入输出文件/扬声器 */
            for (int s = 0; s < S; s++) {
                if (!isfinite(anti_spk[s])) anti_spk[s] = 0.0f;
                anti_out[s * N + idx] = anti_spk[s];
            }
            for (int e = 0; e < E; e++) {
                err_out[e * N + idx] = err_sig[e];
                err_pwr += err_sig[e] * err_sig[e];
            }
        }
        err_pwr /= (len * E);

        float db_band = 10.0f * log10f(dis_pwr_band / (err_pwr + 1e-12f));
        float db_full = 10.0f * log10f(dis_pwr_full / (err_pwr + 1e-12f));

        printf("%4d | %25s | %9.2f dB | %9.2f dB | %s", sec + 1, scene_str, db_band, db_full, action);
        if (sec == 0) printf("  [FxRMS=%.4f]", sqrtf(fx_pwr_diag / (len * E * S)));
        printf("\n");

        sum_db_band += db_band; sum_db_full += db_full;
        free(ref_filt); free(dis_buf); free(full_buf);
    }

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

    for (int i = 0; i < 85; i++) printf("-"); printf("\n");
    printf("  Avg |                           | %9.2f dB | %9.2f dB |\n", sum_db_band / n_sec, sum_db_full / n_sec);
    printf("\nProcessing: %.1fs for %.1fs audio (%.1fx)\n", elapsed, (double)N / FS, (double)N / FS / elapsed);

    /* ── 5. 输出 ── */
    wav_write("anti_out.wav",  anti_out, N, S, FS);
    wav_write("error_out.wav", err_out,  N, E, FS);
    printf("Output: anti_out.wav (%d ch), error_out.wav (%d ch)\n", S, E);

    /* ── 6. 清理 ── */
    free(anti_out); free(err_out); free(noise_bp);
    fxnlms_free(&fx);
    for (int i = 0; i < E * S; i++) free(sec_firs[i].delay_line);
    free(sec_firs); free(sec_coeffs);
    for (int e = 0; e < E; e++) free(pri_firs[e].delay_line);
    free(bp_fir.delay_line);
    free(wav.data); if (ref_resampled) free(ref_resampled);
    bin_free(sec_path); bin_free(pri_path); bin_free(sub_filters);
    bin_free(centroids); bin_free(bp_coeff);
    printf("Done.\n");
    return 0;
}
