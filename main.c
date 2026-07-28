/** GFANC FxNLMS — 离线 WAV 降噪 (统一实时路径).
 *
 *  信号路径与 main_realtime.c 完全一致, 仅 I/O 不同:
 *    实时版: PortAudio 硬件 (ADC/DAC)
 *    离线版: WAV 文件读写
 *
 *  编译: gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c
 *              src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c
 *              -lm -o main.exe
 *  运行: ./main.exe <noise.wav>
 *  输出: anti_out.wav (S=2ch 反噪声), error_out.wav (E=3ch 误差麦信号)
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
            short afmt, nch; int sr; short bps;
            fread(&afmt, 2, 1, f); fread(&nch, 2, 1, f);
            fread(&sr, 4, 1, f); fseek(f, 6, SEEK_CUR);
            fread(&bps, 2, 1, f);
            /* R-18: 仅支持 16-bit PCM (24/32-bit 需预先转换) */
            if (afmt != 1 || bps != 16) {
                fprintf(stderr, "ERROR: Only 16-bit PCM WAV supported (got fmt=%d bps=%d)\n", afmt, bps);
                fclose(f); return -1;
            }
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
    /* R-18: 下采样前简易抗混叠 (2样本移动平均, 截止 ~fs/4) */
    float *filt = NULL;
    if (sr_in > sr_out) {
        filt = (float *)malloc(n_in * sizeof(float));
        filt[0] = in[0] * 0.5f;
        for (int i = 1; i < n_in; i++)
            filt[i] = (in[i-1] + in[i]) * 0.5f;
    }
    const float *src = filt ? filt : in;

    *n_out = (int)((long long)n_in * sr_out / sr_in);
    float *out = (float *)malloc(*n_out * sizeof(float));
    for (int i = 0; i < *n_out; i++) {
        double pos = (double)i * sr_in / sr_out;
        int i0 = (int)pos; double frac = pos - i0;
        if (i0 + 1 < n_in) out[i] = (float)(src[i0] * (1.0 - frac) + src[i0 + 1] * frac);
        else if (i0 < n_in) out[i] = src[i0];
        else out[i] = 0.0f;
    }
    free(filt);
    return out;
}

/* ══════════════════════════════════════════════════════════
   参数
   ══════════════════════════════════════════════════════════ */
#define E       3
#define S       2
#define C       15
#define FS      16000
#define BP_LEN  1024
#define PRI_LEN 1024
#define SEC_LEN 1024
#define DSP_DELAY 64  /* ASIO 96帧@48k: ADC 2ms + DAC 2ms ≈ 4ms → 64样本@16kHz */
/* R-9: 增益/渐变参数统一由 cfg 管理, GFANC_MIC_GAIN / GFANC_FADE_LEN 等 env 变量可覆盖 */

/* ══════════════════════════════════════════════════════════
   主函数
   ══════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    gfanc_config_t cfg = GFANC_CONFIG_DEFAULT;
    gfanc_config_load_env(&cfg);
    if (argc < 2) {
        printf("GFANC FxNLMS — Offline WAV ANC Demo\n\n");
        printf("Usage: %s <noise.wav>\n\n", argv[0]);
        printf("Output: anti_out.wav (%d ch), error_out.wav (%d ch)\n", S, E);
        return 1;
    }

    /* ── 1. 加载权重 (R-3: 逐文件校验长度) ── */
    printf("Loading weights...\n");
    float *sec_path, *pri_path, *sub_filters, *centroids, *bp_coeff;
    int sec_len  = bin_load_float("data/secondary_path.bin", &sec_path);
    int pri_len  = bin_load_float("data/primary_path.bin", &pri_path);
    int sub_len  = bin_load_float("data/sub_filters.bin", &sub_filters);
    int bp_len   = bin_load_float("data/bandpass_fir.bin", &bp_coeff);
    int n_scene  = bin_load_float("data/scene_defs.bin", &centroids);

    if (sec_len < E*S*SEC_LEN) {
        fprintf(stderr, "FATAL: secondary_path.bin too short/load failed (%d<%d)\n", sec_len, E*S*SEC_LEN);
        return 1;
    }
    if (pri_len < E*2*PRI_LEN) {
        fprintf(stderr, "FATAL: primary_path.bin too short/load failed (%d<%d)\n", pri_len, E*2*PRI_LEN);
        return 1;
    }
    if (sub_len < C*S || sub_len % (C*S) != 0) {
        fprintf(stderr, "FATAL: sub_filters.bin invalid size %d (expect multiple of %d)\n", sub_len, C*S);
        return 1;
    }
    if (bp_len < BP_LEN) {
        fprintf(stderr, "FATAL: bandpass_fir.bin too short/load failed (%d<%d)\n", bp_len, BP_LEN);
        return 1;
    }
    if (n_scene < S*C) {
        fprintf(stderr, "FATAL: scene_defs.bin too short (%d<%d)\n", n_scene, S*C);
        return 1;
    }
    int L = sub_len / (C * S); /* 1024 */
    if (L < 64 || L > 4096) {
        fprintf(stderr, "FATAL: filter length L=%d out of range [64,4096]\n", L);
        return 1;
    }
    printf("  OK: sec=%d pri=%d sub=%d bp=%d L=%d\n", sec_len, pri_len, sub_len, bp_len, L);
    (void)pri_len;

    /* ── 2. 初始化组件 ── */
    /* CNN 必须先初始化 (加载权重) */
    extern int cnn_m5_init(void);
    if (cnn_m5_init() != 0) { fprintf(stderr, "CNN init failed\n"); return 1; }
    printf("  CNN loaded.\n");

    /* 2a. 带通 FIR */
    fir_filter_t bp_fir = { bp_coeff, (double *)calloc(BP_LEN, sizeof(double)), BP_LEN, 0 };

    /* 2b. 次级路径 FIR (含 DSP 延迟) */
    /* ── Ŝ 全局归一化: 所有通道统一除全局peak, 保持通道间相对增益 ── */
    {
        float global_peak = 0;
        for (int i = 0; i < E*S*SEC_LEN; i++) {
            float a = fabsf(sec_path[i]);
            if (a > global_peak) global_peak = a;
        }
        if (global_peak > 0.001f) {
            float inv = 1.0f / global_peak;
            for (int i = 0; i < E*S*SEC_LEN; i++)
                sec_path[i] *= inv;
        }
    }
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

    /* 2c. 初级路径 FIR (R=0, 持久延迟线 — 跨 chunk 连续) */
    fir_filter_t pri_firs[E], pri_raw_firs[E];
    for (int e = 0; e < E; e++) {
        pri_firs[e].coeffs = pri_raw_firs[e].coeffs = pri_path + e * 2 * PRI_LEN;
        pri_firs[e].n_taps = pri_raw_firs[e].n_taps = PRI_LEN;
        pri_firs[e].delay_line     = (double *)calloc(PRI_LEN, sizeof(double));
        pri_raw_firs[e].delay_line = (double *)calloc(PRI_LEN, sizeof(double));
        pri_firs[e].ptr = pri_raw_firs[e].ptr = 0;
    }

    /* 2d. Scene Controller */
    scene_ctrl_t sc;
    if (scene_ctrl_init(&sc, centroids, sub_filters, L, n_scene) != 0) {
        fprintf(stderr, "ERROR: scene_ctrl_init OOM\n"); return 1;
    }
    /* R-4: CNN K vs scene_defs K 交叉校验 */
    { extern int cnn_m5_get_K(void);
      if (cnn_m5_get_K() != sc.K) {
        fprintf(stderr, "FATAL: CNN K=%d != scene_defs K=%d (data/ batch mix-up?)\n",
                cnn_m5_get_K(), sc.K);
        return 1;
      }
    }

    /* 2e. FxNLMS */
    fxnlms_mimo_t fx;
    if (fxnlms_init(&fx, E, S, L, cfg.step_size, cfg.leak) != 0) {
        fprintf(stderr, "ERROR: fxnlms_init OOM\n"); return 1;
    }
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

    /* 预处理: ref_filt_all = bandpass(noise × pre_gain)
       匹配实时版 ref_filt 信号链: pre_gain → soft_clip → bandpass
       注意: 不做峰值归一化 (实时版 ADC 输入无归一化) */
    float *ref_filt_all = (float *)malloc(N * sizeof(float));
    {
        fir_filter_t bp_tmp = { bp_fir.coeffs, (double *)calloc(BP_LEN, sizeof(double)), BP_LEN, 0 };
        for (int i = 0; i < N; i++) {
            float rs = ref[i] * cfg.mic_pre_gain;
            if      (rs >  1.0f) rs =  tanhf(rs);
            else if (rs < -1.0f) rs = -tanhf(-rs);
            ref_filt_all[i] = fir_tick(&bp_tmp, rs);
        }
        free(bp_tmp.delay_line);
    }

    /* 次级路径 FIR — 误差合成用 (独立延迟线, 同 sec 系数)
       用于 anti_spk → Ŝ → anti_at_mic (声学尺度, 与 Pri(noise) 可比) */
    fir_filter_t *sec_firs_err = (fir_filter_t *)calloc(E * S, sizeof(fir_filter_t));
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e * S + s;
            sec_firs_err[idx].coeffs = sec_coeffs + idx * sec_padded;
            sec_firs_err[idx].n_taps = sec_padded;
            sec_firs_err[idx].delay_line = (double *)calloc(sec_padded, sizeof(double));
        }

    /* 误差麦带通 FIR (匹配实时版 bp_err[E]) */
    fir_filter_t bp_err[E];
    for (int e = 0; e < E; e++) {
        bp_err[e].coeffs = bp_coeff;
        bp_err[e].n_taps = BP_LEN;
        bp_err[e].delay_line = (double *)calloc(BP_LEN, sizeof(double));
        bp_err[e].ptr = 0;
    }

    /* ── 4. 离线 ANC (统一实时路径) ── */
    int chunk = FS, n_sec = (N + chunk - 1) / chunk;
    float *anti_out = (float *)calloc(S * N, sizeof(float));
    float *err_out  = (float *)calloc(E * N, sizeof(float));

    /* CrossFader 状态 */
    int   fade_cnt = 0;
    float wc_old[2048], wc_cur[2048];

    float sum_nr_db = 0;
    clock_t t0 = clock();

    /* 跨秒持久状态: 避免每秒重置导致音频咔声 */
    float err_meas[E] = {0};      /* LMS 梯度驱动信号 */
    float anti_est_prev[E] = {0}; /* anti_est = Wc⊗Xd, Ŝ域抗噪估计 */

    printf("\n%4s | %5s | %25s | %-55s | %8s | %s\n", "Sec", "Scene", "Top-3", "Full Probs (0~K-1)", "NR(dB)", "Action");
    for (int i = 0; i < 120; i++) printf("-"); printf("\n");

    for (int sec = 0; sec < n_sec; sec++) {
        int start = sec * chunk, len = (start + chunk <= N) ? chunk : (N - start);
        if (len <= 0) break;

        /* 4a. CNN 场景分类 (输入: ref_filt_all, 匹配实时版 CNN 输入) */
        float probs[SC_K_MAX];
        int K = sc.K;
        int new_scene;
        if (len == chunk) {
            new_scene = scene_ctrl_process(&sc, ref_filt_all + start, wc_cur, probs);
        } else {
            memcpy(probs, sc.prev_probs, K * sizeof(float));
            new_scene = sc.cur_scene;
        }

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

        char prob_full[120];
        int off = 0;
        for (int i = 0; i < K && off < (int)sizeof(prob_full) - 4; i++)
            off += snprintf(prob_full + off, sizeof(prob_full) - off, "%d:%.2f ", i, probs[i]);

        /* 4b. 滞回检测 + CrossFader */
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
                fade_cnt = cfg.fade_len;
                snprintf(action, sizeof(action), "RESET");
            }
        }
        memcpy(sc.prev_probs, probs, sc.K * sizeof(float));

        /* 4c. 逐样本 FxNLMS (统一实时路径) */
        float err_pwr = 0, dis_pwr = 0, fx_pwr_diag = 0;
        for (int n = 0; n < len; n++) {
            int idx = start + n;
            float ref_filt = ref_filt_all[idx];

            /* CrossFader */
            if (fade_cnt > 0) {
                float a = (float)fade_cnt / cfg.fade_len;
                for (int i = 0; i < S * L; i++)
                    fx.wc[i] = a * wc_old[i] + (1.0f - a) * wc_cur[i];
                fade_cnt--;
                if (fade_cnt == 0) memcpy(fx.wc, wc_cur, S * L * sizeof(float));
            }

            /* Fx = Ŝ ⊗ ref_filt (梯度用) */
            float Fx_arr[E * S];
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++) {
                    float v = fir_tick(&sec_firs[e * S + s], ref_filt);
                    Fx_arr[e * S + s] = v;
                    if (sec == 0) fx_pwr_diag += v * v;
                }

            /* anti_spk = Wc ⊗ x_hist (匹配实时版) */
            float anti_spk[S];
            if (fade_cnt == 0)
                fxnlms_tick_rt(&fx, ref_filt, Fx_arr, err_meas, anti_spk);
            else
                fxnlms_forward_rt(&fx, ref_filt, Fx_arr, err_meas, anti_spk);

            for (int s = 0; s < S; s++) {
                if (!isfinite(anti_spk[s])) anti_spk[s] = 0.0f;
                if (anti_spk[s] > 1.0f) anti_spk[s] = 1.0f;
                if (anti_spk[s] < -1.0f) anti_spk[s] = -1.0f;
                anti_out[s * N + idx] = anti_spk[s];
            }

            /* 带限扰动 + 残差 (Ŝ域, 驱动 LMS 梯度和 NR) */
            float dis_val[E];
            for (int e = 0; e < E; e++) {
                dis_val[e] = fir_tick(&pri_firs[e], ref_filt);
                dis_pwr += dis_val[e] * dis_val[e];
                err_meas[e] = dis_val[e] + anti_est_prev[e];
                err_pwr += err_meas[e] * err_meas[e];
            }

            /* error_out: 匹配实时版误差麦信号链 —
               anti_spk / pre_gain 还原到声学尺度, 与 Pri(noise) 可比 */
            for (int e = 0; e < E; e++) {
                float pri_raw = fir_tick(&pri_raw_firs[e], ref[idx]);
                float anti_at_mic = 0;
                for (int s = 0; s < S; s++)
                    anti_at_mic += fir_tick(&sec_firs_err[e * S + s], anti_spk[s] / cfg.mic_pre_gain);
                float es = (pri_raw + anti_at_mic) * cfg.mic_pre_gain;
                if      (es >  1.0f) es =  tanhf(es);
                else if (es < -1.0f) es = -tanhf(-es);
                err_out[e * N + idx] = fir_tick(&bp_err[e], es);
            }

            /* 更新 anti_est_prev = Wc ⊗ Xd (供下一样本) */
            fxnlms_get_anti_est(&fx, anti_est_prev);
        }
        err_pwr /= (len * E);
        dis_pwr /= (len * E);

        /* NR = 带内降噪量 (匹配实时版: 仅 20-1500 Hz 范围内的降噪效果) */
        float nr_db = 10.0f * log10f((dis_pwr + 1e-12f) / (err_pwr + 1e-12f));

        printf("%4d | %5d | %22s | %-40s | %8.2f dB | %s", sec + 1, new_scene, scene_str, prob_full, nr_db, action);
        if (sec == 0) printf("  [FxRMS=%.4f]", sqrtf(fx_pwr_diag / (len * E * S)));
        printf("\n");

        sum_nr_db += nr_db;
    }

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

    for (int i = 0; i < 85; i++) printf("-"); printf("\n");
    printf("  Avg |                           | %9.2f dB |\n", sum_nr_db / n_sec);
    printf("\nProcessing: %.1fs for %.1fs audio (%.1fx)\n", elapsed, (double)N / FS, (double)N / FS / elapsed);

    /* ── 5. 输出 ── */
    wav_write("anti_out.wav",  anti_out, N, S, FS);
    wav_write("error_out.wav", err_out,  N, E, FS);
    printf("Output: anti_out.wav (%d ch), error_out.wav (%d ch)\n", S, E);

    /* ── 6. 清理 ── */
    free(anti_out); free(err_out); free(ref_filt_all);
    for (int e = 0; e < E; e++) free(bp_err[e].delay_line);
    fxnlms_free(&fx);
    scene_ctrl_free(&sc);
    for (int i = 0; i < E * S; i++) { free(sec_firs[i].delay_line); free(sec_firs_err[i].delay_line); }
    free(sec_firs); free(sec_firs_err); free(sec_coeffs);
    for (int e = 0; e < E; e++) { free(pri_firs[e].delay_line); free(pri_raw_firs[e].delay_line); }
    free(bp_fir.delay_line);
    free(wav.data); if (ref_resampled) free(ref_resampled);
    bin_free(sec_path); bin_free(pri_path); bin_free(sub_filters);
    bin_free(centroids); bin_free(bp_coeff);
    printf("Done.\n");
    return 0;
}
