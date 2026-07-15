/** GFANC FxNLMS Scene — WAV 离线降噪 demo.

编译: make  (或 gcc -O2 -Iinclude main.c src/*.c -lm -o main.exe)
运行: ./main.exe <noise.wav>

输出: anti_out.wav (S通道反噪声), error_out.wav (E通道残差)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "gfanc_system.h"
#include "fir_filter.h"
#include "binary_loader.h"

/* CNN forward declare */
extern int cnn_m5_init(void);
extern int cnn_m5_forward(const float *audio, float *logits);

#ifdef _WIN32
#include <windows.h>
#endif

/* ══════════════════════════════════════════════════════════
   全局数据 (从 .bin 文件加载)
   ══════════════════════════════════════════════════════════ */
static float *g_sec_path   = NULL;  /* [E*S*L] */
static float *g_pri_path   = NULL;  /* [E*R*L] */
static float *g_sub_filters = NULL; /* [C*S*L] */
static float *g_centroids  = NULL;  /* [K*S*C] */
static float *g_bp_fir     = NULL;  /* [BP_LEN] */
static int    g_sec_len, g_pri_len, g_sub_len, g_bp_len;

/* ══════════════════════════════════════════════════════════
   WAV 读取 (16-bit PCM)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    int sr, ch, n;
    float *data; /* mono, normalized [-1,1] */
} wav_t;

static int wav_read_mono(const char *path, wav_t *w)
{
    memset(w, 0, sizeof(*w));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char riff[5] = {0}; fread(riff, 1, 4, f);
    if (strcmp(riff, "RIFF")) { fclose(f); return -1; }
    fseek(f, 4, SEEK_CUR); /* file size */
    char wave[5] = {0}; fread(wave, 1, 4, f);
    if (strcmp(wave, "WAVE")) { fclose(f); return -1; }

    /* scan for fmt and data */
    int fmt_done = 0, data_done = 0;
    unsigned int data_bytes = 0;
    while (!fmt_done || !data_done) {
        char id[5] = {0};
        if (fread(id, 1, 4, f) < 4) break;
        unsigned int sz;
        fread(&sz, 4, 1, f);
        if (!strcmp(id, "fmt ")) {
            short afmt, nch; int sr; fread(&afmt, 2, 1, f); fread(&nch, 2, 1, f);
            fread(&sr, 4, 1, f); fseek(f, 6, SEEK_CUR); /* byte rate + block align */
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

    w->n = data_bytes / (w->ch * 2); /* 16-bit */
    w->data = (float *)malloc(w->n * sizeof(float));
    if (!w->data) { fclose(f); return -1; }

    short *tmp = (short *)malloc(data_bytes);
    fread(tmp, 1, data_bytes, f);
    fclose(f);

    /* mono-ify + normalize */
    for (int i = 0; i < w->n; i++) {
        int sum = 0;
        for (int c = 0; c < w->ch; c++) sum += tmp[i * w->ch + c];
        w->data[i] = (float)(sum / w->ch) / 32768.0f;
    }
    free(tmp);
    return 0;
}

/* ══════════════════════════════════════════════════════════
   WAV 写入 (16-bit PCM, multi-channel interleaved)
   ══════════════════════════════════════════════════════════ */
static void wav_write(const char *path, const float *data, int n, int ch, int sr)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int bps = 16, frame = ch * (bps / 8);
    int data_sz = n * frame;

    unsigned int v32;
    unsigned short v16;
    fwrite("RIFF", 1, 4, f);
    v32 = 36 + data_sz; fwrite(&v32, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f);    /* PCM */
    v16 = ch; fwrite(&v16, 2, 1, f);
    v32 = sr; fwrite(&v32, 4, 1, f);
    v32 = sr * frame; fwrite(&v32, 4, 1, f);
    v16 = frame; fwrite(&v16, 2, 1, f);
    v16 = bps; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_sz, 4, 1, f);

    for (int i = 0; i < n; i++) {
        for (int c = 0; c < ch; c++) {
            float v = data[c * n + i]; /* column-major: [ch][n] */
            if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
            short s = (short)(v * 32767.0f);
            fwrite(&s, 2, 1, f);
        }
    }
    fclose(f);
}

/* ══════════════════════════════════════════════════════════
   工具: 重采样
   ══════════════════════════════════════════════════════════ */
static float *resample_mono(const float *in, int n_in, int sr_in,
                             int sr_out, int *n_out)
{
    *n_out = (int)((long long)n_in * sr_out / sr_in);
    float *out = (float *)malloc(*n_out * sizeof(float));
    for (int i = 0; i < *n_out; i++) {
        double pos = (double)i * sr_in / sr_out;
        int i0 = (int)pos;
        double frac = pos - i0;
        if (i0 + 1 < n_in)
            out[i] = (float)(in[i0] * (1.0 - frac) + in[i0 + 1] * frac);
        else if (i0 < n_in)
            out[i] = in[i0];
        else out[i] = 0.0f;
    }
    return out;
}

/* ══════════════════════════════════════════════════════════
   主流程
   ══════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) {
        printf("GFANC FxNLMS Scene — WAV Offline Demo\n\n");
        printf("Usage: %s <noise.wav>\n\n", argv[0]);
        printf("Output: anti_out.wav, error_out.wav\n");
        return 1;
    }

    /* ── 1. 加载权重 ── */
    printf("Loading weights...\n");
    int n_scene;
    g_sec_len   = bin_load_float("data/secondary_path.bin", &g_sec_path);
    g_pri_len   = bin_load_float("data/primary_path.bin", &g_pri_path);
    g_sub_len   = bin_load_float("data/sub_filters.bin", &g_sub_filters);
    g_bp_len    = bin_load_float("data/bandpass_fir.bin", &g_bp_fir);
    n_scene     = bin_load_float("data/scene_defs.bin", &g_centroids);
    (void)n_scene;

    if (!g_sec_path || !g_sub_filters || !g_centroids) {
        fprintf(stderr, "ERROR: missing .bin files. Run: python export/export_bin.py\n");
        return 1;
    }
    printf("  OK: sec=%d pri=%d sub=%d bp=%d\n", g_sec_len, g_pri_len, g_sub_len, g_bp_len);

    /* ── 2. 系统初始化 (手动, 绕过 gfanc_system 的 header includes) ── */
    int E = 3, S = 2, C = 15, K = 8;
    int filter_len = g_sub_len / (C * S);
    printf("  E=%d S=%d C=%d K=%d L=%d\n", E, S, C, K, filter_len);

    int wc_total = S * filter_len;
    int xd_total = E * S * filter_len;
    float *wc_fx  = (float *)calloc(wc_total, sizeof(float));
    float *xd_buf = (float *)calloc(xd_total, sizeof(float));
    float  step_size = 0.0001f;
    float  leak      = 1e-5f;

    /* Scene controller — 简化版 (只用 blend + construct_wc) */
    /* 不再用 scene_ctrl_t 结构, 直接在 main 中处理 */

    /* Sec 路径 FIR (含 16 样本 DSP 延迟, 与 Python sys_delay=16 一致) */
    int dsp_delay = 16;
    int sec_tap_raw = g_sec_len / (E * S);           /* 1024 */
    int sec_tap_padded = sec_tap_raw + dsp_delay;     /* 1040 */
    fir_filter_t *sec_firs = (fir_filter_t *)calloc(E * S, sizeof(fir_filter_t));
    float **sec_padded_coeffs = (float **)calloc(E * S, sizeof(float *));
    for (int e = 0; e < E; e++) {
        for (int s = 0; s < S; s++) {
            int idx = e * S + s;
            float *padded = (float *)calloc(sec_tap_padded, sizeof(float));
            memcpy(padded + dsp_delay, g_sec_path + idx * sec_tap_raw,
                   sec_tap_raw * sizeof(float));
            sec_padded_coeffs[idx] = padded;
            sec_firs[idx].coeffs = padded;
            sec_firs[idx].n_taps = sec_tap_padded;
            sec_firs[idx].ptr = 0;
            sec_firs[idx].delay_line = (double *)calloc(sec_tap_padded, sizeof(double));
        }
    }

    /* 带通 FIR */
    fir_filter_t bp_fir;
    bp_fir.coeffs = g_bp_fir;
    bp_fir.n_taps = g_bp_len;
    bp_fir.ptr = 0;
    bp_fir.delay_line = (double *)calloc(g_bp_len, sizeof(double));

    /* Pri 路径 FIR (3ch × R=0) */
    fir_filter_t *pri_firs = (fir_filter_t *)calloc(E, sizeof(fir_filter_t));
    int pri_len = g_pri_len / (E * 2); /* [E,R=2,Len], R=0 only */
    for (int e = 0; e < E; e++) {
        pri_firs[e].coeffs = g_pri_path + e * 2 * pri_len; /* R=0 */
        pri_firs[e].n_taps = pri_len;
        pri_firs[e].ptr = 0;
        pri_firs[e].delay_line = (double *)calloc(pri_len, sizeof(double));
    }

    /* 子滤波器 stub RMS: stub[s,l] = Σ_c sub[c,s,l], rms = sqrt(mean(stub^2)) */
    float stub_rms = 0;
    {
        float *stub = (float *)calloc(S * filter_len, sizeof(float));
        for (int c = 0; c < C; c++)
            for (int s = 0; s < S; s++)
                for (int l = 0; l < filter_len; l++)
                    stub[s * filter_len + l] += g_sub_filters[(c * S + s) * filter_len + l];
        float ss = 0;
        for (int i = 0; i < S * filter_len; i++) ss += stub[i] * stub[i];
        stub_rms = sqrtf(ss / (S * filter_len));
        free(stub);
    }

    /* CNN 初始化 */
    if (cnn_m5_init() != 0) {
        fprintf(stderr, "ERROR: CNN init failed\n"); return 1;
    }
    printf("  System ready (CNN loaded).\n");

    /* ── 3. 读取输入 WAV ── */
    wav_t wav;
    if (wav_read_mono(argv[1], &wav) != 0) {
        fprintf(stderr, "ERROR: Cannot read %s\n", argv[1]); return 1;
    }
    printf("\nInput: %d Hz, %d ch, %d samples (%.1fs)\n",
           wav.sr, wav.ch, wav.n, (double)wav.n / wav.sr);

    int N;
    float *ref = wav.data;
    float *ref_resampled = NULL;
    if (wav.sr != 16000) {
        ref_resampled = resample_mono(wav.data, wav.n, wav.sr, 16000, &N);
        ref = ref_resampled;
        printf("Resampled: %d Hz -> 16000 Hz (%d samples)\n", wav.sr, N);
    } else {
        N = wav.n;
    }

    /* ── 3b. 峰值归一化 + 初始带通 [20,1500] (匹配 Python noise_loader 行为) ── */
    {
        float peak_val = 0;
        for (int i = 0; i < N; i++) {
            float a = fabsf(ref[i]);
            if (a > peak_val) peak_val = a;
        }
        if (peak_val > 1e-12f) {
            for (int i = 0; i < N; i++) ref[i] /= peak_val;
        }
        printf("Peak normalized: peak=%.4f\n", peak_val);
    }

    /* 初始带通 [20,1500] → noise_bp (Python 的 noise = lfilter(bandpass, peak_norm(raw))) */
    float *noise_bp = (float *)malloc(N * sizeof(float));
    {
        fir_filter_t bp_noise;
        bp_noise.coeffs   = g_bp_fir;
        bp_noise.n_taps   = g_bp_len;
        bp_noise.ptr      = 0;
        bp_noise.delay_line = (double *)calloc(g_bp_len, sizeof(double));
        for (int i = 0; i < N; i++)
            noise_bp[i] = fir_tick(&bp_noise, ref[i]);
        free(bp_noise.delay_line);
        printf("Initial bandpass done (noise_bp created).\n");
    }

    /* ── 4. 离线 ANC 处理 ── */
    int chunk = 16000;
    int n_sec = (N + chunk - 1) / chunk;
    float *anti_out = (float *)calloc(S * N, sizeof(float));
    float *err_out  = (float *)calloc(E * N, sizeof(float));

    int cur_scene = -1, fade_cnt = 0, n_switches = 0;
    float wc_old[2048] = {0}, wc_cur[2048] = {0};
    float prev_probs[8] = {0};  /* 上一秒 softmax 概率, 用于余弦相似度检测 */
    const float reset_threshold = 0.8f;  /* 余弦相似度阈值 (<0.8 触发切换) */
    float sum_db_band = 0, sum_db_full = 0;
    int    n_db = 0;

    clock_t t0 = clock();

    /* ── 打印表头 ── */
    printf("\n%4s | %25s | %10s | %10s | %s\n", "Sec", "Top-3 Scenes", "dB(Band)", "dB(Full)", "Action");
    for (int i = 0; i < 85; i++) printf("-");
    printf("\n");

    for (int sec = 0; sec < n_sec; sec++) {
        int start = sec * chunk;
        int len = (start + chunk <= N) ? chunk : (N - start);
        if (len <= 0) break;

        /* ── 4a. 带通滤波 (第二遍, Python: ref_filt = fir.process(noise_sec)) ── */
        float *ref_filt = (float *)malloc(chunk * sizeof(float));
        for (int i = 0; i < len; i++)
            ref_filt[i] = fir_tick(&bp_fir, noise_bp[start + i]);

        /* ── 4b. CNN 输入: noise_bp + minmaxscaler (与 Python noise_sec 一致) ── */
        float *cnn_input = (float *)malloc(chunk * sizeof(float));
        {
            float cnn_min = noise_bp[start], cnn_max = noise_bp[start];
            for (int i = 0; i < len; i++) {
                float v = noise_bp[start + i];
                cnn_input[i] = v;
                if (v < cnn_min) cnn_min = v;
                if (v > cnn_max) cnn_max = v;
            }
            float cnn_denom = cnn_max - cnn_min;
            if (cnn_denom > 1e-10f) {
                for (int i = 0; i < len; i++)
                    cnn_input[i] /= cnn_denom;
            }
        }

        /* ── 4c. Pri 路径 Dis_band = Pri ⊗ 带通(ref) (与 Python 一致) ── */
        float *dis_buf = (float *)calloc(E * len, sizeof(float));
        fir_filter_t pri_tmp[3];
        for (int e = 0; e < E; e++) {
            pri_tmp[e] = pri_firs[e];  /* 结构体拷贝 (delay_line 指针共享) */
            for (int n = 0; n < len; n++)
                dis_buf[e * len + n] = fir_tick(&pri_tmp[e], ref_filt[n]);
            pri_firs[e].ptr = pri_tmp[e].ptr;  /* ⚠️ 关键: 同步 ptr 回原结构体! */
        }

        /* ── 4d. 全频参考 Dis_full = Pri ⊗ noise_bp (单遍带通, 与 Python Dis_full_sec 一致) ── */
        float *dis_full_buf = (float *)calloc(E * len, sizeof(float));
        for (int e = 0; e < E; e++) {
            fir_filter_t pri_tmp2 = pri_firs[e];  /* 独立副本, 新 delay_line */
            pri_tmp2.delay_line = (double *)calloc(pri_len, sizeof(double));
            pri_tmp2.ptr = 0;
            for (int n = 0; n < len; n++)
                dis_full_buf[e * len + n] = fir_tick(&pri_tmp2, noise_bp[start + n]);
            free(pri_tmp2.delay_line);
        }

        /* ── 4e. 参考能量 (Dis = 无ANC时的扰动, 所有 Mic 取平均, 与 Python 一致) ── */
        float dis_pwr_band = 0, dis_pwr_full = 0;
        for (int e = 0; e < E; e++)
            for (int n = 0; n < len; n++) {
                dis_pwr_band += dis_buf[e * len + n] * dis_buf[e * len + n];
                dis_pwr_full += dis_full_buf[e * len + n] * dis_full_buf[e * len + n];
            }
        dis_pwr_band /= (len * E); dis_pwr_full /= (len * E);

        /* CNN forward (全频 raw + minmaxscaled) */
        float logits[8];
        int new_scene = 0;
        float probs[8] = {0};
        if (cnn_m5_forward(cnn_input, logits) == 0) {
            float mx = logits[0], sum_exp = 0;
            for (int k = 0; k < 8; k++) if (logits[k] > mx) mx = logits[k];
            for (int k = 0; k < 8; k++) { probs[k] = expf(logits[k] - mx); sum_exp += probs[k]; }
            for (int k = 0; k < 8; k++) probs[k] /= sum_exp;
            new_scene = 0;
            for (int k = 1; k < 8; k++) if (probs[k] > probs[new_scene]) new_scene = k;

            /* 诊断: 首秒打印 logits */
            if (sec == 0) {
                printf("[Diag] CNN logits: ");
                for (int k = 0; k < 8; k++) printf("%.4f ", logits[k]);
                printf("| max_prob=%.4f\n", probs[new_scene]);
            }
        }
        free(cnn_input);

        /* Top-3 场景字符串 */
        int top3[3] = {-1, -1, -1};
        for (int i = 0; i < 8; i++) {
            int already = 0;
            for (int k = 0; k < 3; k++) if (top3[k] == i) { already = 1; break; }
            if (already) continue;
            for (int j = 0; j < 3; j++) {
                if (top3[j] < 0 || probs[i] > probs[top3[j]]) {
                    for (int k = 2; k > j; k--) top3[k] = top3[k-1];
                    top3[j] = i; break;
                }
            }
        }
        char scene_str[40];
        snprintf(scene_str, sizeof(scene_str), "%d:%.2f,%d:%.2f,%d:%.2f",
                 top3[0], probs[top3[0]], top3[1], probs[top3[1]], top3[2], probs[top3[2]]);

        /* Blend → Wc */
        int SC = S * C;
        float blend[30], wc_new[2048];
        for (int i = 0; i < SC; i++) blend[i] = g_centroids[new_scene * SC + i];
        float bmax = blend[0];
        for (int i = 1; i < SC; i++) if (blend[i] > bmax) bmax = blend[i];
        float inv_max = 1.0f / (bmax + 1e-10f);
        for (int s = 0; s < S; s++)
            for (int l = 0; l < filter_len; l++) {
                float v = 0.0f;
                for (int c = 0; c < C; c++) {
                    float b = blend[s * C + c] * inv_max;
                    if (b < 0) b = 0; if (b > 1) b = 1;
                    v += b * g_sub_filters[(c * S + s) * filter_len + l];
                }
                wc_new[s * filter_len + l] = v;
            }
        float rms_sq = 0;
        for (int i = 0; i < S * filter_len; i++) rms_sq += wc_new[i] * wc_new[i];
        float scale = (rms_sq > 1e-10f) ? stub_rms / sqrtf(rms_sq / (S * filter_len)) : 1.0f;
        for (int i = 0; i < S * filter_len; i++) wc_new[i] = -wc_new[i] * scale;

        /* Scene switch (基于 softmax 余弦相似度, 与 Python 一致) */
        char action[20] = "-";
        if (cur_scene < 0) {
            /* 首秒: 直接初始化 */
            memcpy(wc_cur, wc_new, wc_total * sizeof(float));
            memcpy(wc_fx, wc_new, wc_total * sizeof(float));
            snprintf(action, sizeof(action), "INIT");
        } else {
            /* 余弦相似度: cos = dot(prev,curr) / (|prev|*|curr|) */
            float dot = 0, np = 0, nc = 0;
            for (int k = 0; k < 8; k++) {
                dot += prev_probs[k] * probs[k];
                np  += prev_probs[k] * prev_probs[k];
                nc  += probs[k] * probs[k];
            }
            float cos_sim = dot / (sqrtf(np) * sqrtf(nc) + 1e-10f);
            if (cos_sim < reset_threshold) {
                /* 场景变化: 从 FxNLMS 自适应后的 wc_fx 淡化到新的 wc_new */
                memcpy(wc_old, wc_fx, wc_total * sizeof(float));
                memcpy(wc_cur, wc_new, wc_total * sizeof(float));
                fade_cnt = 16;
                n_switches++;
                snprintf(action, sizeof(action), "RESET");
            }
        }
        cur_scene = new_scene;
        memcpy(prev_probs, probs, 8 * sizeof(float));

        /* ── 逐样本 FxNLMS ── */
        float err_pwr = 0, fx_pwr_diag = 0;
        for (int n = 0; n < len; n++) {
            int idx = start + n;
            float rn_bp = ref_filt[n];  /* 带通滤波后的参考信号 (与 Python 一致) */

            if (fade_cnt > 0) {
                float a = (float)fade_cnt / 16.0f;
                for (int i = 0; i < wc_total; i++) wc_fx[i] = a * wc_old[i] + (1.0f - a) * wc_cur[i];
                fade_cnt--;
                if (fade_cnt == 0) {
                    /* 淡化结束: 设为纯 new_wc (匹配 Python fader.new_wc.clone()) */
                    memcpy(wc_fx, wc_cur, wc_total * sizeof(float));
                }
            }

            float Fx[E * S];
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++) {
                    float v = fir_tick(&sec_firs[e * S + s], rn_bp);
                    Fx[e * S + s] = v;
                    if (sec == 0) fx_pwr_diag += v * v;
                }

            float Dis[E];
            for (int e = 0; e < E; e++) Dis[e] = dis_buf[e * len + n];

            /* Roll Xd: shift all by 1, newest at index 0 (matches Python) */
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++)
                    for (int k = filter_len - 1; k > 0; k--)
                        xd_buf[(e * S + s) * filter_len + k]
                            = xd_buf[(e * S + s) * filter_len + (k - 1)];
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++)
                    xd_buf[(e * S + s) * filter_len + 0] = Fx[e * S + s];

            /* anti_est[e] = Σ_s Σ_k Wc[s,k] * Xd[e,s,k] (per error mic, matches Python y_ff) */
            float anti_est[E];
            for (int e = 0; e < E; e++) { anti_est[e] = 0;
                for (int s = 0; s < S; s++) for (int k = 0; k < filter_len; k++)
                    anti_est[e] += wc_fx[s * filter_len + k] * xd_buf[(e * S + s) * filter_len + k]; }

            /* 功率归一化 */
            float power[S];
            for (int s = 0; s < S; s++) { power[s] = 1e-10f;
                for (int e = 0; e < E; e++) for (int k = 0; k < filter_len; k++)
                    power[s] += xd_buf[(e * S + s) * filter_len + k] * xd_buf[(e * S + s) * filter_len + k];
                power[s] /= (float)(E * filter_len); }

            /* 梯度更新: 淡化期间跳过 FxNLMS (与 Python 一致, 16样本对收敛可忽略) */
            if (fade_cnt == 0) {
                for (int s = 0; s < S; s++) {
                    float inv_pwr = 1.0f / power[s];
                    for (int e = 0; e < E; e++) {
                        float err_e = Dis[e] + anti_est[e];
                        for (int k = 0; k < filter_len; k++)
                            wc_fx[s * filter_len + k] -= step_size * err_e
                                * xd_buf[(e * S + s) * filter_len + k] * inv_pwr;
                    }
                    for (int k = 0; k < filter_len; k++) wc_fx[s * filter_len + k] *= (1.0f - step_size * leak);
                }
            }

            /* anti_out per speaker: Σ_e Σ_k Wc[s,k] * Xd[e,s,k] */
            float anti_spk[S];
            for (int s = 0; s < S; s++) { anti_spk[s] = 0;
                for (int e = 0; e < E; e++) for (int k = 0; k < filter_len; k++)
                    anti_spk[s] += wc_fx[s * filter_len + k] * xd_buf[(e * S + s) * filter_len + k]; }
            for (int s = 0; s < S; s++) anti_out[s * N + idx] = anti_spk[s];

            /* 误差: err[e] = Dis[e] + anti_est[e], 所有 Mic 平均 (与 Python 一致) */
            for (int e = 0; e < E; e++) {
                float err_val = Dis[e] + anti_est[e];
                err_out[e * N + idx] = err_val;
                err_pwr += err_val * err_val;
            }
        }

        err_pwr /= (len * E);

        /* dB = 10*log10(Dis_power / Err_power), band 和 full 用相同的 err_pwr */
        float db_band = 10.0f * log10f(dis_pwr_band / (err_pwr + 1e-12f));
        float db_full = 10.0f * log10f(dis_pwr_full / (err_pwr + 1e-12f));

        printf("%4d | %25s | %9.2f dB | %9.2f dB | %s",
               sec + 1, scene_str, db_band, db_full, action);

        /* 首秒诊断 + Wc 追踪 */
        if (sec == 0) {
            float wc_rms_diag = sqrtf(rms_sq / (S * filter_len));
            float dis_band_rms = sqrtf(dis_pwr_band);
            float dis_full_rms = sqrtf(dis_pwr_full);
            float fx_rms_diag = sqrtf(fx_pwr_diag / (len * E * S));
            printf("  [WcRMS=%.4f]", sqrtf(rms_sq / (S * filter_len)));
            printf("\n[Diag] Wc_new RMS: %.4f\n", wc_rms_diag);
            printf("[Diag] Dis (带内) RMS: %.4f\n", dis_band_rms);
            printf("[Diag] Dis (全频) RMS: %.4f\n", dis_full_rms);
            printf("[Diag] Fx RMS:        %.4f\n", fx_rms_diag);
        } else {
            /* 追踪 FxNLMS 自适应后的 Wc RMS */
            float wc_adapted_rms = 0;
            for (int i = 0; i < wc_total; i++) wc_adapted_rms += wc_fx[i] * wc_fx[i];
            wc_adapted_rms = sqrtf(wc_adapted_rms / wc_total);
            printf("  [WcRMS=%.4f]", wc_adapted_rms);
        }
        printf("\n");

        sum_db_band += db_band; sum_db_full += db_full; n_db++;

        free(ref_filt); free(dis_buf); free(dis_full_buf);
    }

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

    for (int i = 0; i < 85; i++) printf("-");
    printf("\n");
    printf("  Avg | %25s | %9.2f dB | %9.2f dB | %d switches\n",
           "", sum_db_band / n_db, sum_db_full / n_db, n_switches);
    printf("\nProcessing: %.1fs for %.1fs audio (%.1fx)\n",
           elapsed, (double)N / 16000.0, (double)N / 16000.0 / elapsed);

    /* ── 5. 输出 WAV ── */
    wav_write("anti_out.wav", anti_out, N, S, 16000);
    wav_write("error_out.wav", err_out, N, E, 16000);
    printf("Output: anti_out.wav (%d ch), error_out.wav (%d ch)\n", S, E);

    /* ── 6. 清理 ── */
    free(anti_out); free(err_out);
    free(wc_fx); free(xd_buf);
    for (int i = 0; i < E * S; i++) {
        free(sec_firs[i].delay_line);
        free(sec_padded_coeffs[i]);
    }
    free(sec_padded_coeffs);
    free(sec_firs);
    for (int i = 0; i < E; i++) free(pri_firs[i].delay_line);
    free(pri_firs);
    free(bp_fir.delay_line);
    free(wav.data);
    if (ref_resampled) free(ref_resampled);
    free(noise_bp);
    bin_free(g_sec_path); bin_free(g_pri_path);
    bin_free(g_sub_filters); bin_free(g_centroids); bin_free(g_bp_fir);

    printf("Done.\n");
    return 0;
}
