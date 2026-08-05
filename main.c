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
#include "cnn_m5_forward.h"
#include "scene_controller.h"
#include "scene_manager.h"
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
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
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
#define BP_LEN  1024   /* CNN 带通 (分类需频率分辨率) */
#define BP_ANC_LEN 64 /* BUG-6: ANC 带通 (与实时版一致, 64tap 群延迟 2ms vs 8ms — 砍环路延迟) */
#define PRI_LEN 1024
#define SEC_LEN 1024
#define DSP_DELAY 0  /* 与实时版一致: Ŝ 已含声学延迟, 无硬件 I/O 延迟需补偿 */
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
    /* BUG-6: ANC 专用短带通 (256tap, 与实时版一致). 无文件时截取 1024tap 前 256 点. */
    float *bp_anc_coeff = NULL;
    int bp_anc_loaded = bin_load_float("data/bandpass_anc.bin", &bp_anc_coeff);
    int bp_anc_ok = (bp_anc_loaded >= BP_ANC_LEN);
    if (!bp_anc_ok) { bp_anc_coeff = bp_coeff; }  /* 回退: 截取用同一指针 */
    printf("  BP ANC: %s (%dtap, gd=%.1fms)\n",
           bp_anc_ok ? "bandpass_anc.bin" : "fallback(1024tap truncated)",
           BP_ANC_LEN, (BP_ANC_LEN-1)/(2.0f*FS)*1000.0f);

    if (sec_len < E*S*SEC_LEN) {
        fprintf(stderr, "FATAL: secondary_path.bin too short/load failed (%d<%d)\n", sec_len, E*S*SEC_LEN);
        return 1;
    }
    if (pri_len < E*2*PRI_LEN) {
        fprintf(stderr, "FATAL: primary_path.bin too short/load failed (%d<%d)\n", pri_len, E*2*PRI_LEN);
        return 1;
    }
    /* 虚拟预览 (GFANC_VIRT_PREVIEW_MS): 参考→误差声学距离. 加在初级路径上,
       预览越大 → 控制越有时间 → 环路延迟的因果裕量越大. 与 VIRT_DELAY 配合验证. */
    float *pri_path_used = pri_path;
    int preview_delay = 0;
    {   const char *vp = getenv("GFANC_VIRT_PREVIEW_MS");
        if (vp) { preview_delay = (int)(atof(vp) * FS / 1000.0f);
            float *pad = (float *)calloc(E*2*PRI_LEN + preview_delay, sizeof(float));
            memcpy(pad + preview_delay, pri_path, E*2*PRI_LEN*sizeof(float));
            pri_path_used = pad;
            printf("  VIRT preview: %s ms → %d samples added to Pri\n", vp, preview_delay); } }
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
    if (cnn_m5_init() != 0) { fprintf(stderr, "CNN init failed\n"); return 1; }
    printf("  CNN loaded.\n");

    /* 2a. 带通 FIR */
    fir_filter_t bp_fir = { bp_coeff, (gfanc_delay_t *)calloc(BP_LEN, sizeof(gfanc_delay_t)), BP_LEN, 0 };

    /* 2b. 次级路径 FIR (peak→1.0 归一化, 与实时版一致) */
    {
        float s_peak = 0;
        for (int i = 0; i < E*S*SEC_LEN; i++) {
            float a = fabsf(sec_path[i]);
            if (a > s_peak) s_peak = a;
        }
        if (s_peak > 0.001f) {
            float inv = 1.0f / s_peak;
            for (int i = 0; i < E*S*SEC_LEN; i++) sec_path[i] *= inv;
        }
    }
    /* 虚拟延迟 (GFANC_VIRT_DELAY_MS): 模拟 DSP/处理延迟, 验证算法在不同延迟下的表现.
       加到 Ŝ 模型 (Fx 对齐 + 误差合成共用), 模拟环路延迟对因果性的消耗. */
    int virt_delay = 0;
    {   const char *vd = getenv("GFANC_VIRT_DELAY_MS");
        if (vd) { virt_delay = (int)(atof(vd) * FS / 1000.0f); /* ms→16k 样本 */
            printf("  VIRT delay: %s ms → %d samples added to Ŝ\n", vd, virt_delay); } }
    int sec_padded = SEC_LEN + DSP_DELAY + virt_delay;
    fir_filter_t *sec_firs = (fir_filter_t *)calloc(E * S, sizeof(fir_filter_t));
    float *sec_coeffs = (float *)calloc(E * S * sec_padded, sizeof(float));
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e * S + s;
            memcpy(sec_coeffs + idx * sec_padded + DSP_DELAY + virt_delay,
                   sec_path + idx * SEC_LEN, SEC_LEN * sizeof(float));
            sec_firs[idx].coeffs = sec_coeffs + idx * sec_padded;
            sec_firs[idx].n_taps = sec_padded;
            sec_firs[idx].delay_line = (gfanc_delay_t *)calloc(sec_padded, sizeof(gfanc_delay_t));
        }

    /* 2c. 初级路径 FIR (R=0, 持久延迟线 — 跨 chunk 连续) */
    fir_filter_t pri_firs[E], pri_raw_firs[E];
    for (int e = 0; e < E; e++) {
        pri_firs[e].coeffs = pri_raw_firs[e].coeffs = pri_path_used + e * 2 * PRI_LEN;
        pri_firs[e].n_taps = pri_raw_firs[e].n_taps = PRI_LEN;
        pri_firs[e].delay_line     = (gfanc_delay_t *)calloc(PRI_LEN, sizeof(gfanc_delay_t));
        pri_raw_firs[e].delay_line = (gfanc_delay_t *)calloc(PRI_LEN, sizeof(gfanc_delay_t));
        pri_firs[e].ptr = pri_raw_firs[e].ptr = 0;
    }

    /* 2d. Scene Controller */
    scene_ctrl_t sc;
    if (scene_ctrl_init(&sc, centroids, sub_filters, L, n_scene) != 0) {
        fprintf(stderr, "ERROR: scene_ctrl_init OOM\n"); return 1;
    }
    /* R-4: CNN K vs scene_defs K 交叉校验 */
    if (cnn_m5_get_K() != sc.K) {
        fprintf(stderr, "FATAL: CNN K=%d != scene_defs K=%d (data/ batch mix-up?)\n",
                cnn_m5_get_K(), sc.K);
        return 1;
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
    float *ref_filt_all = (float *)malloc(N * sizeof(float));   /* 1024tap: CNN 分类用 */
    float *ref_anc_all  = (float *)malloc(N * sizeof(float));   /* 256tap: FxLMS/anti 用 (BUG-6, 与实时版一致) */
    {
        fir_filter_t bp_tmp = { bp_fir.coeffs, (gfanc_delay_t *)calloc(BP_LEN, sizeof(gfanc_delay_t)), BP_LEN, 0 };
        fir_filter_t bp_anc = { bp_anc_coeff, (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t)), BP_ANC_LEN, 0 };
        for (int i = 0; i < N; i++) {
            float rs = ref[i] * cfg.mic_pre_gain;
            if      (rs >  1.0f) rs =  tanhf(rs);
            else if (rs < -1.0f) rs = -tanhf(-rs);
            ref_filt_all[i] = fir_tick(&bp_tmp, rs);
            ref_anc_all[i]  = fir_tick(&bp_anc, rs);
        }
        free(bp_tmp.delay_line);
        free(bp_anc.delay_line);
    }

    /* 次级路径 FIR — 误差合成用 (独立延迟线, 同 sec 系数)
       用于 anti_spk → Ŝ → anti_at_mic (声学尺度, 与 Pri(noise) 可比) */
    fir_filter_t *sec_firs_err = (fir_filter_t *)calloc(E * S, sizeof(fir_filter_t));
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e * S + s;
            sec_firs_err[idx].coeffs = sec_coeffs + idx * sec_padded;
            sec_firs_err[idx].n_taps = sec_padded;
            sec_firs_err[idx].delay_line = (gfanc_delay_t *)calloc(sec_padded, sizeof(gfanc_delay_t));
        }

    /* 误差麦带通 FIR (匹配实时版 bp_err[E], BUG-6: 256tap ANC 带通) */
    fir_filter_t bp_err[E];
    for (int e = 0; e < E; e++) {
        bp_err[e].coeffs = bp_anc_coeff;
        bp_err[e].n_taps = BP_ANC_LEN;
        bp_err[e].delay_line = (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t));
        bp_err[e].ptr = 0;
    }

    /* ── 4. 离线 ANC (与实时版 main_realtime.c 信号路径一致, 仅 I/O 不同) ── */
    int chunk = FS, n_sec = (N + chunk - 1) / chunk;
    float *anti_out = (float *)calloc(S * N, sizeof(float));
    float *err_out  = (float *)calloc(E * N, sizeof(float));

    /* CrossFader 状态 */
    int   fade_cnt = 0;
    float wc_old[S*L], wc_cur[S*L];

    /* ── 场景管理 (匹配实时版) ── */
    float scene_wc[SC_K_MAX][S*L];
    int   scene_wc_valid[SC_K_MAX] = {0};
    int   cur_scene_id = -1;
    float anchor_probs[SC_K_MAX];
    int   converged_frames = 0;
    int   scene_cand = -1, scene_cand_cnt = 0;
    float wc_init_max = 0.01f;

    /* ── NR 累积 (诚实NR, 匹配实时版) ── */
    float acc_err = 0, acc_anti_est = 0, acc_d_est = 0, acc_err_cross = 0;
    float acc_anti = 0, acc_ref = 0;
    float acc_err_win = 0;   /* BUG-1: 分散采样窗口误差功率 (与 pa/cross 同源) */
    int   acc_cnt = 0;
    int   anti_est_offset = 0;  /* BUG-1: 分散采样相位 (0..63, 每帧随机化) */
    int   diverged = 0;
    float prev_nr_est = 0.0f;   /* BUG-7: 上一秒 NR (场景切换时判断 Wc 是否可信) */

    float sum_nr_db = 0;
    clock_t t0 = clock();

    /* 跨秒持久状态 */
    float err_meas[E] = {0};
    float anti_est_prev[E] = {0};

    printf("\n%4s | %5s | %6s | %6s | %6s | %7s | %6s | %s\n",
           "Sec", "Scene", "NR_est", "NR_true", "err", "refFilt", "anti", "Note");
    for (int i = 0; i < 85; i++) printf("-");
    printf("\n");

    for (int sec = 0; sec < n_sec; sec++) {
        int start = sec * chunk, len = (start + chunk <= N) ? chunk : (N - start);
        if (len <= 0) break;

        /* 4a. CNN 场景分类 */
        float probs[SC_K_MAX];
        int K = sc.K;
        int new_scene;
        if (len == chunk)
            new_scene = scene_ctrl_process(&sc, ref_filt_all + start, wc_cur, probs);
        else {
            memcpy(probs, sc.prev_probs, K * sizeof(float));
            new_scene = sc.cur_scene;
        }

        /* 4b. 场景管理 (首次 INIT / 切换 RESET, 匹配实时版) */
        char action[20] = "-";
        int first_sec = (cur_scene_id < 0);
        if (first_sec) {
            /* C1: 使用共享函数初始化场景记忆 + wc_init_max */
            sm_first_sec_init((float *)scene_wc, scene_wc_valid,
                              &cur_scene_id, new_scene,
                              wc_cur, S * L, &wc_init_max, 1.0f);
            memcpy(anchor_probs, probs, K * sizeof(float));
            fxnlms_set_wc(&fx, wc_cur);
            snprintf(action, sizeof(action), "INIT");
            /* 自动增益标定 (匹配实时版) */
            if (!getenv("GFANC_MIC_GAIN")) {
                float auto_gain = TARGET_REF_RMS / (sqrtf(acc_ref / (len + 1e-10f)) + 1e-10f);
                if (auto_gain < 1.0f) auto_gain = 1.0f;
                if (auto_gain > 20.0f) auto_gain = 20.0f;
                cfg.mic_pre_gain = auto_gain;
            }
        } else {
            /* S-1: cos(anchor, cur) 替代 cos(prev, cur) */
            float cos_sim = sm_cos_sim(anchor_probs, probs, K);

            /* P4: 场景切换滞回 — 候选需连续3帧一致 */
            if (sm_check_scene_switch(cos_sim, cfg.switch_threshold,
                                       new_scene, cur_scene_id,
                                       &scene_cand, &scene_cand_cnt, 3)) {
                /* C1: 使用共享场景切换
                   BUG-7: 发散期的 Wc 快照不保存进场景记忆 (防污染) */
                int restored = sm_scene_switch_execute(
                    (float *)scene_wc, scene_wc_valid,
                    &cur_scene_id, new_scene,
                    fx.wc, wc_cur, wc_old, S * L, 1.0f,
                    !diverged && prev_nr_est >= 0.0f);
                fade_cnt = cfg.fade_len;
                memcpy(anchor_probs, probs, K * sizeof(float));
                converged_frames = 0;
                snprintf(action, sizeof(action), restored ? "RESET(mem)" : "RESET");
            }
        }
        memcpy(sc.prev_probs, probs, K * sizeof(float));

        /* 4c. 逐样本 FxNLMS (匹配实时版 audio_cb) */
        float err_pwr = 0, dis_pwr = 0;
        acc_err = acc_anti_est = acc_d_est = acc_err_cross = 0;
        acc_err_win = 0;
        acc_anti = acc_ref = 0; acc_cnt = 0;
        for (int n = 0; n < len; n++) {
            int idx = start + n;
            float ref_filt = ref_anc_all[idx];   /* BUG-6: 256tap ANC 带通 (匹配实时) */
            acc_ref += ref_filt_all[idx] * ref_filt_all[idx];  /* 1024tap CNN 带通 (匹配实时 ref_rms 显示) */

            /* CrossFader */
            if (fade_cnt > 0) {
                float a = (float)fade_cnt / cfg.fade_len;
                for (int i = 0; i < S * L; i++)
                    fx.wc[i] = a * wc_old[i] + (1.0f - a) * wc_cur[i];
                fade_cnt--;
                if (fade_cnt == 0) memcpy(fx.wc, wc_cur, S * L * sizeof(float));
            }

            /* Fx = Ŝ ⊗ ref_filt */
            float Fx_arr[E * S];
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++)
                    Fx_arr[e * S + s] = fir_tick(&sec_firs[e * S + s], ref_filt);

            /* anti_spk = Wc ⊗ x_hist, err_meas = dis + anti_est (合成误差驱动梯度) */
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
                acc_anti += anti_spk[s] * anti_spk[s];
            }

            /* 合成误差: dis = Pri ⊗ ref_filt, err = dis + anti_est */
            float dis_val[E];
            for (int e = 0; e < E; e++) {
                dis_val[e] = fir_tick(&pri_firs[e], ref_filt);
                err_meas[e] = dis_val[e] + anti_est_prev[e];
                err_pwr += err_meas[e] * err_meas[e];
                dis_pwr += dis_val[e] * dis_val[e];  /* 已知真值扰动 (Pri模型) */
            }

            /* BUG-1: 分散采样 (匹配实时版) — 每 64 样本取 1 个, 整帧 250 个覆盖整秒.
               原 R-55 连续 250 窗口 + int 溢出 LCG 产生负偏移时整帧不采样 (NR 恒 0),
               且窗口落在局部收敛区时误差功率失真. */
            if (((acc_cnt + anti_est_offset) & 63) == 0) {
                float anti_est[E];
                fxnlms_get_anti_est(&fx, anti_est);
                for (int e = 0; e < E; e++) {
                    acc_anti_est += anti_est[e] * anti_est[e];
                    float dv = err_meas[e] - anti_est[e];
                    acc_d_est += dv * dv;
                    acc_err_cross += err_meas[e] * anti_est[e];
                    acc_err_win += err_meas[e] * err_meas[e];
                }
            }
            acc_cnt++;

            /* error_out: 匹配实时版误差麦信号链 */
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

            /* 更新 anti_est_prev */
            fxnlms_get_anti_est(&fx, anti_est_prev);
        }

        /* ── 诚实NR (匹配实时版, 分散采样一致指标) ──
           BUG-1: pe/pa/cross 全部来自同一组分散采样样本, 外推因子在比值中抵消.
           原实现混用全帧 pe 与窗口 pa/cross, 窗口未命中时 pd≈pe → NR 恒 0. */
        float pe = acc_err_win;
        float pa = acc_anti_est;
        float cross = acc_err_cross;
        float pd = pe + pa - 2.0f * cross;
        if (pd < 0.0f) pd = 0.0f;   /* 数值保护: 完美对消时 pd 可为负 */
        float nr_est;
        if (pe < 1e-10f) {
            nr_est = 0.0f;           /* 残差接近数值基底, 比值无意义 */
        } else {
            nr_est = 10.0f * log10f((pd + 1e-12f) / (pe + 1e-12f));
            if (nr_est > 30.0f) nr_est = 30.0f;
            if (nr_est < -30.0f) nr_est = -30.0f;
        }
        prev_nr_est = nr_est;                          /* BUG-7: 供下个场景切换判断 */
        float err_rms = sqrtf(err_pwr / (len * E));   /* 显示用全帧误差 RMS */
        float ref_rms = sqrtf(acc_ref / len);
        float anti_rms = sqrtf(acc_anti / (len * S));

        /* ── 已知真值NR (仅离线可用: 利用 Pri 模型精确计算扰动) ── */
        dis_pwr /= (len * E);
        float nr_true = 10.0f * log10f((dis_pwr + 1e-12f) / (err_pwr / (len * E) + 1e-12f));

        /* 发散检测 (基于诚实NR, 匹配实时版) */
        diverged = (pa > 9.0f * pe && anti_rms > 0.05f && nr_est < 0.0f);

        /* 收敛检测: 基于已知真值NR (离线更可靠) */
        /* C1: 使用共享收敛检测 (离线版用 nr_true, 实时版用 nr_level) */
        sm_check_convergence(nr_true, cfg.nr_converge_db,
                             0/*safety_mute*/, diverged,
                             &converged_frames,
                             (float *)scene_wc, cur_scene_id,
                             fx.wc, S * L, &wc_init_max);

        /* 输出: 诚实NR(匹配实时) + 已知真值NR(Pri模型) */
        char nr_est_str[20], nr_true_str[20];
        if (diverged) snprintf(nr_est_str, sizeof(nr_est_str), "DIV!");
        else          snprintf(nr_est_str, sizeof(nr_est_str), "%.1f", nr_est);
        snprintf(nr_true_str, sizeof(nr_true_str), "%.1f", nr_true);
        printf("%4d | %5d | %6s | %6s | %5.3f | %6.4f | %5.4f | %s",
               sec + 1, new_scene, nr_est_str, nr_true_str,
               err_rms, ref_rms, anti_rms, action);
        if (sec == 0) printf(" [FxRMS=%.4f]", sqrtf(acc_ref / len));
        printf("\n");

        sum_nr_db += nr_true;  /* 平均值用已知真值NR (离线评估标准) */
        /* BUG-1: 分散采样相位随机化 (0..63). 无符号运算避免 int 溢出
           (原 LCG 有符号溢出产生负偏移, 使连续窗口整帧不采样). */
        anti_est_offset = (int)((anti_est_offset * 1103515245u + 12345u) & 63u);
    }

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

    for (int i = 0; i < 85; i++) printf("-");
    printf("\n");
    printf("  Avg |                           | %6s | %6.1f |\n", "", sum_nr_db / n_sec);
    printf("  NR_est = 诚实NR(匹配实时版) | NR_true = 已知真值NR(Pri模型, 仅离线可用)\n");
    printf("\nProcessing: %.1fs for %.1fs audio (%.1fx)\n", elapsed, (double)N / FS, (double)N / FS / elapsed);

    /* ── 5. 输出 ── */
    wav_write("anti_out.wav",  anti_out, N, S, FS);
    wav_write("error_out.wav", err_out,  N, E, FS);
    printf("Output: anti_out.wav (%d ch), error_out.wav (%d ch)\n", S, E);

    /* ── 6. 清理 ── */
    free(anti_out); free(err_out); free(ref_filt_all); free(ref_anc_all);
    for (int e = 0; e < E; e++) free(bp_err[e].delay_line);
    fxnlms_free(&fx);
    scene_ctrl_free(&sc);
    for (int i = 0; i < E * S; i++) { free(sec_firs[i].delay_line); free(sec_firs_err[i].delay_line); }
    free(sec_firs); free(sec_firs_err); free(sec_coeffs);
    for (int e = 0; e < E; e++) { free(pri_firs[e].delay_line); free(pri_raw_firs[e].delay_line); }
    free(bp_fir.delay_line);
    free(wav.data); if (ref_resampled) free(ref_resampled);
    bin_free(sec_path); if (pri_path_used != pri_path) free(pri_path_used); bin_free(pri_path); bin_free(sub_filters);
    bin_free(centroids); bin_free(bp_coeff);
    if (bp_anc_ok) bin_free(bp_anc_coeff);   /* BUG-6: bandpass_anc.bin 独立所有权 */
    printf("Done.\n");
    return 0;
}
