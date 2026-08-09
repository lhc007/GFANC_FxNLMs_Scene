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
#include "ocg.h"
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

/* 因果性报告: 路径峰值延迟 (argmax tap → ms). Pri=参考→误差麦声学,
   Ŝ=扬声器→误差麦声学. 净预览 = τ_pri − τ_spk − τ_proc (处理延迟).
   >0 宽带可因果对消; <0 因果缺口 → 随机宽带受限 (windows-ANC 因果限制). */
static float path_peak_delay_ms(const float *coeff, int n, float fs)
{
    int peak = 0; float mx = -1.0f;
    for (int i = 0; i < n; i++) {
        float a = fabsf(coeff[i]);
        if (a > mx) { mx = a; peak = i; }
    }
    return (float)peak / fs * 1000.0f;
}

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
    float *sec_path, *pri_path, *sub_filters, *bp_coeff;
    int sec_len  = bin_load_float("data/secondary_path.bin", &sec_path);
    int pri_len  = bin_load_float("data/primary_path.bin", &pri_path);
    int sub_len  = bin_load_float("data/sub_filters.bin", &sub_filters);
    int bp_len   = bin_load_float("data/bandpass_fir.bin", &bp_coeff);
    /* BUG-6: ANC 专用短带通 (64tap, 与实时版一致). 无文件时截取 1024tap 前 64 点. */
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
    if (pri_len < E*PRI_LEN) {
        fprintf(stderr, "FATAL: primary_path.bin too short/load failed (%d<%d)\n", pri_len, E*PRI_LEN);
        return 1;
    }
    /* 虚拟预览 (GFANC_VIRT_PREVIEW_MS): 参考→误差声学距离. 加在初级路径上,
       预览越大 → 控制越有时间 → 环路延迟的因果裕量越大. 与 VIRT_DELAY 配合验证. */
    float *pri_path_used = pri_path;
    int preview_delay = 0;
    {   const char *vp = getenv("GFANC_VIRT_PREVIEW_MS");
        if (vp) { preview_delay = (int)(atof(vp) * FS / 1000.0f);
            float *pad = (float *)calloc(E*PRI_LEN + preview_delay, sizeof(float));
            memcpy(pad + preview_delay, pri_path, E*PRI_LEN*sizeof(float));
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

    /* 2b. 次级路径 FIR — R-58-6: Ŝ 增益标定必须与训练世界一致!
       训练 (Disturbance_generation + FxNLMS_MIMO) 用原始 secondary_path 增益,
       若此处 peak→1.0 归一化 → Ŝ 整体缩小 (真实路径 ÷25.5) → anti 环路增益低 25.5 倍,
       Wc 必须膨胀才能抵消 pri → 自适应路径上 G×tanh 饱和 → 发散 (road-15 根因).
       默认不归一化 (与训练一致); GFANC_SEC_NORM=1 恢复峰值归一化 (实时硬件标定兼容). */
    {
        const char *sn = getenv("GFANC_SEC_NORM");
        if (sn && sn[0] == '1') {
            float s_peak = 0;
            for (int i = 0; i < E*S*SEC_LEN; i++) {
                float a = fabsf(sec_path[i]);
                if (a > s_peak) s_peak = a;
            }
            if (s_peak > 0.001f) {
                float inv = 1.0f / s_peak;
                for (int i = 0; i < E*S*SEC_LEN; i++) sec_path[i] *= inv;
            }
            printf("  Ŝ: peak→1.0 归一化 (GFANC_SEC_NORM=1 显式开启)\n");
        } else {
            printf("  Ŝ: 原始增益 (默认, 与训练世界一致)\n");
        }
    }
    /* 嵌入式处理延迟 (GFANC_EMBED_DELAY_MS, 默认3ms): 模拟 ADC+DSP+DAC 信号链延迟,
       pad 到 Ŝ 模型 (Fx 对齐 + 误差合成共用), 消耗因果裕量 — 离线预测嵌入式目标 NR.
       GFANC_VIRT_DELAY_MS 可覆盖 (实验/扫参用). */
    int virt_delay = cfg.embed_delay_ms * FS / 1000;
    {   const char *vd = getenv("GFANC_VIRT_DELAY_MS");
        if (vd) virt_delay = (int)(atof(vd) * FS / 1000.0f);
        printf("  PROC delay: %d ms (%d samples) added to Ŝ — 嵌入式信号链处理延迟"
               " (GFANC_EMBED_DELAY_MS=3ms 默认, 可覆盖)\n",
               virt_delay * 1000 / FS, virt_delay); }
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

    /* 因果性报告: 净预览时间 = τ_pri − τ_spk − τ_proc.
       τ_proc = ANC带通群延迟 + 处理延迟 (GFANC_VIRT_DELAY_MS, 嵌入式 DSP/编解码预算). */
    {
        float tau_pri = path_peak_delay_ms(pri_path_used + 0*PRI_LEN, PRI_LEN, FS);
        float tau_spk = path_peak_delay_ms(sec_path, SEC_LEN, FS);
        float tau_bp  = (BP_ANC_LEN-1)/(2.0f*FS)*1000.0f;  /* 64tap 线性相位群延迟 */
        float tau_proc = tau_bp + (DSP_DELAY + virt_delay)/(float)FS*1000.0f;
        float preview = tau_pri - tau_spk - tau_proc;
        printf("  Causality: τ_pri=%.2fms τ_spk=%.2fms τ_proc(bp+emb)=%.2fms "
               "→ 净预览=%.2fms %s\n",
               tau_pri, tau_spk, tau_proc, preview,
               preview > 0 ? "(>0 宽带可因果对消)"
                           : "(<0 因果缺口 — 随机宽带对消受限, 只能消窄带/低频)");
    }

    /* 2c. 初级路径 FIR (R=0, 持久延迟线 — 跨 chunk 连续) */
    fir_filter_t pri_firs[E], pri_raw_firs[E];
    for (int e = 0; e < E; e++) {
        pri_firs[e].coeffs = pri_raw_firs[e].coeffs = pri_path_used + e * PRI_LEN;   /* R-58-6: R=1 (硬件 1 参考) */
        pri_firs[e].n_taps = pri_raw_firs[e].n_taps = PRI_LEN;
        pri_firs[e].delay_line     = (gfanc_delay_t *)calloc(PRI_LEN, sizeof(gfanc_delay_t));
        pri_raw_firs[e].delay_line = (gfanc_delay_t *)calloc(PRI_LEN, sizeof(gfanc_delay_t));
        pri_firs[e].ptr = pri_raw_firs[e].ptr = 0;
    }

    /* 2d. Scene Controller (直接权重 Wc 生产者) */
    scene_ctrl_t sc;
    if (scene_ctrl_init(&sc, sub_filters, L) != 0) {
        fprintf(stderr, "ERROR: scene_ctrl_init failed\n"); return 1;
    }
    /* OCG 聚类闸门 (与实时版一致): τ 复用 switch_threshold */
    ocg_t ocg;
    if (ocg_init(&ocg, sc.K, cfg.switch_threshold,
                 cfg.ocg_alpha, cfg.ocg_max_clusters) != 0) {
        fprintf(stderr, "ERROR: ocg_init failed\n"); return 1;
    }
    /* 直接权重模式要求 CNN 输出 = S*C = 30 (已在 scene_ctrl_init 校验) */

    /* 2e. FxNLMS — R-58-10: 步长默认值与修复后链路重新标定.
       R-58-8 曾在旧链路 (含 es∝G² 双增益 + 梯度相位失配) 下扫描, 0.005 发散、
       0.0005 最优 — 那是在 bug 链路上的伪标定 (es 被 G² 推到 tanh 饱和, 梯度还错位).
       R-58-10 修复 (Fx 过 bp_err + es 去双 G) 后重新扫描 (三文件, sum 归一化):
         step:  0.00005  0.0001  0.0005  0.001  0.002  0.005  0.01
         mixed: 7.9     8.4     9.2     9.5    9.6    9.8    9.5
         road-15/0-34: 0.005 稳定且最优 (9.2/9.6)
       平台区 0.001-0.005, 0.01 回落 — 默认取 0.005 (收敛快且稳定, 裕量 2×).
       GFANC_STEP / GFANC_LEAK 环境变量可覆盖. */
    if (!getenv("GFANC_STEP")) {
        cfg.step_size = 0.005f; /* R-58-10: 修复后链路稳定步长 (0.0005 旧标定已过时) */
    }
    if (!getenv("GFANC_LEAK")) {
        cfg.leak = 5e-7f;        /* 保持弱泄漏 */
    }
    fxnlms_mimo_t fx;
    if (fxnlms_init(&fx, E, S, L, cfg.step_size, cfg.leak) != 0) {
        fprintf(stderr, "ERROR: fxnlms_init OOM\n"); return 1;
    }
    /* R-58-9: 离线仿真走 fxnlms_tick_rt 路径, 显式切到 sum 归一化 (与训练世界一致).
       实时版 (main_realtime.c) 不调用此函数 → 保持默认 mean+cap 硬件标定语义. */
    fxnlms_set_norm(&fx, 1);
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

    /* 自动增益标定 — 预处理前预扫描 (修复 R-58: 原 auto_gain 在 INIT 秒才计算,
       但预处理早已完成 → gain 从未生效. 修复 R-58-2: 目标电平 0.15 → TARGET_REF_RMS 0.03,
       与实时版 auto-gain 工作点一致: es_init = 0.03×Pri带内增益(36×) ≈ 1.1 RMS (轻饱和,
       硬件实测 12-15dB 的工作点). 目标 0.15 使 es 饱和 5 倍 → tanh 截断污染梯度 → anti
       被推到 ±1 钳位, NR_true 假阳性 12.9dB 实为发散).
       离线版可整段预扫 1024tap 带通 RMS; 与实时版 [1,20] 钳位不同, 允许 G<1
       (wav 已是满数字量程, 无物理旋钮约束; 响亮输入需衰减避免饱和). */
    if (!getenv("GFANC_MIC_GAIN")) {
        float acc_scan = 0;
        fir_filter_t bp_scan = { bp_fir.coeffs,
            (gfanc_delay_t *)calloc(BP_LEN, sizeof(gfanc_delay_t)), BP_LEN, 0 };
        for (int i = 0; i < N; i++) {
            float y = fir_tick(&bp_scan, ref[i]);
            acc_scan += y * y;
        }
        free(bp_scan.delay_line);
        float ref_rms = sqrtf(acc_scan / (N + 1e-10f));
        float auto_gain = TARGET_REF_RMS / (ref_rms + 1e-10f);
        if (auto_gain < 0.01f) auto_gain = 0.01f;   /* 响亮输入允许衰减 (实时版为硬件旋钮钳 [1,20]) */
        if (auto_gain > 20.0f) auto_gain = 20.0f;
        cfg.mic_pre_gain = auto_gain;
        printf("Auto-gain: bandpass ref RMS %.4f -> mic_pre_gain %.2f (目标 %.3f, 实时版工作点)\n",
               ref_rms, auto_gain, TARGET_REF_RMS);
    }

    /* 预处理: ref_filt_all = bandpass(noise × pre_gain)
       匹配实时版 ref_filt 信号链: pre_gain → soft_clip → bandpass
       注意: 不做峰值归一化 (实时版 ADC 输入无归一化) */
    float *ref_filt_all = (float *)malloc(N * sizeof(float));   /* 1024tap: CNN 分类用 */
    float *ref_anc_all  = (float *)malloc(N * sizeof(float));   /* 64tap: FxLMS/anti 用 (BUG-6, 与实时版一致) */
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

    /* 误差麦带通 FIR (匹配实时版 bp_err[E], BUG-6: 64tap ANC 带通) */
    fir_filter_t bp_err[E], bp_err_nr[E];   /* bp_err_nr: NR_true 统计口径 (未截断, R-58) */
    for (int e = 0; e < E; e++) {
        bp_err[e].coeffs = bp_anc_coeff;
        bp_err[e].n_taps = BP_ANC_LEN;
        bp_err[e].delay_line = (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t));
        bp_err[e].ptr = 0;
        bp_err_nr[e].coeffs = bp_anc_coeff;
        bp_err_nr[e].n_taps = BP_ANC_LEN;
        bp_err_nr[e].delay_line = (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t));
        bp_err_nr[e].ptr = 0;
    }
    /* R-58-10: 梯度 Fx 也过 64tap 带通 (与 err_meas 同路径) — 修复梯度相位失配.
       err_meas = bp_err(es) 带 31.5 样本群延迟, 若 Fx = Ŝ⊗ref_anc 不过 bp → 梯度与
       误差错位 31.5 样本 → FxLMS 临界稳定 → Wc 相位慢漂移 → 降噪随时间衰减 (root cause).
       修复: Fx 过同一 bp_anc → ∂err_meas/∂Wc = bp(Ŝ⊗x) 与 eg 逐样本对齐.
       注意: 必须 E×S 每条路径一个独立 FIR — 若每 e 共享一个, s=1 的 tick 会用
       s=0 污染的延迟线 → 第二扬声器的滤波参考被交叉污染 → Wc[1] 梯度错位 → 慢漂移. */
    fir_filter_t bp_fx[E * S];
    memset(bp_fx, 0, sizeof(bp_fx));
    for (int i = 0; i < E * S; i++) {
        bp_fx[i].coeffs = bp_anc_coeff;
        bp_fx[i].n_taps = BP_ANC_LEN;
        bp_fx[i].delay_line = (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t));
        bp_fx[i].ptr = 0;
    }

    /* ── 4. 离线 ANC (与实时版 main_realtime.c 信号路径一致, 仅 I/O 不同) ── */
    int chunk = FS, n_sec = (N + chunk - 1) / chunk;
    float *anti_out = (float *)calloc(S * N, sizeof(float));
    float *err_out  = (float *)calloc(E * N, sizeof(float));

    /* CrossFader 状态 */
    int   fade_cnt = 0;
    float wc_old[S*L], wc_cur[S*L];

    /* ── Reset 模式锚点 (去场景层, 匹配实时版) ──
       CNN 仍是每秒 Wc 生产者 (scene_ctrl_process, 直接权重); 场景切换/记忆/滞回/OCG 已移除.
       reset 模式: cos_sim(anchor_gains, cur_gains) < switch_threshold → CrossFader 重置到新 Wc.
       continuous 模式: 仅首秒初始化, 之后 FxNLMS 永不重置. */
    float anchor_gains[SC_DW_MAX];  /* 上次重置时的 30 维增益锚点 */
    int   first_sec = 1;            /* 首秒 INIT: CNN Wc → FxNLMS 初始化 */

    /* ── NR 累积 (诚实NR, 匹配实时版) ── */
    float acc_err = 0, acc_anti_est = 0, acc_d_est = 0, acc_err_cross = 0;
    float acc_anti = 0, acc_ref = 0;
    float acc_err_win = 0;   /* BUG-1: 分散采样窗口误差功率 (与 pa/cross 同源) */
    int   acc_cnt = 0;
    int   anti_est_offset = 0;  /* BUG-1: 分散采样相位 (0..63, 每帧随机化) */
    int   diverged = 0;

    float sum_nr_db = 0;
    clock_t t0 = clock();

    /* 跨秒持久状态 */
    float err_meas[E] = {0};

    printf("\n%4s | %5s | %6s | %6s | %6s | %7s | %6s | %s\n",
           "Sec", "Band", "NR_est", "NR_true", "err", "refFilt", "anti", "Note");
    for (int i = 0; i < 85; i++) printf("-");
    printf("\n");

    for (int sec = 0; sec < n_sec; sec++) {
        int start = sec * chunk, len = (start + chunk <= N) ? chunk : (N - start);
        if (len <= 0) break;

        /* 4a. CNN 直接权重 Wc 构造 (每秒) */
        float gains[SC_DW_MAX];
        int K = sc.K;
        int new_scene;
        if (len == chunk)
            new_scene = scene_ctrl_process(&sc, ref_filt_all + start, wc_cur, gains);
        else {
            memcpy(gains, sc.prev_gains, K * sizeof(float));
            new_scene = 0;
        }

        /* 4b. 去场景层双模式 (INIT / RESET, 匹配实时版) */
        char action[20] = "-";
        if (first_sec) {
            /* 首秒 INIT: CNN Wc → FxNLMS 初始化 (两模式一致) */
            memcpy(anchor_gains, gains, K * sizeof(float));
            ocg_reset(&ocg, gains);  /* OCG: 首个增益建立簇 0 */
            fxnlms_set_wc(&fx, wc_cur);
            snprintf(action, sizeof(action), "INIT");
            /* auto_gain 已由预处理前预扫描设定 (R-58: 原此处计算但预处理已完成 → 从未生效) */
            first_sec = 0;
        } else {
            /* S-1: cos(anchor_gains, cur_gains) — 30 维直接权重增益 */
            float cos_sim = sm_cos_sim(anchor_gains, gains, K);

            /* reset 模式: OCG 簇索引变化 (默认) 或 cos(anchor,cur)<τ (GFANC_OCG=0)
               → CrossFader 重置到新 Wc; continuous 模式: CNN 不参与后续 Wc 构造.
               OCG: 多质心聚类抑制簇内抖动/慢漂移导致的反复重置 (ICASSP 2026). */
            int do_reset = cfg.ocg_enable ? ocg_step(&ocg, gains)
                                          : (cos_sim < cfg.switch_threshold);
            if (cfg.gfanc_mode == 1 && do_reset) {
                memcpy(wc_old, fx.wc, S * L * sizeof(float));  /* 过渡起点 (当前收敛 Wc) */
                /* wc_cur 已是 scene_ctrl_process 算出的新候选 */
                fade_cnt = cfg.fade_len;
                memcpy(anchor_gains, gains, K * sizeof(float));
                snprintf(action, sizeof(action), "RESET");
            }
        }
        memcpy(sc.prev_gains, gains, K * sizeof(float));

        /* 4c. 逐样本 FxNLMS (匹配实时版 audio_cb) */
        float err_pwr = 0, dis_pwr = 0;
        acc_err = acc_anti_est = acc_d_est = acc_err_cross = 0;
        acc_err_win = 0;
        acc_anti = acc_ref = 0; acc_cnt = 0;
        for (int n = 0; n < len; n++) {
            int idx = start + n;
            float ref_filt = ref_anc_all[idx];   /* BUG-6: 64tap ANC 带通 (匹配实时) */
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
            /* R-58-10: Fx 过 bp_anc, 与 err_meas 梯度对齐 (修复时间衰减根因).
               每条 (e,s) 路径独立 FIR, 避免扬声器间延迟线交叉污染. */
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++)
                    Fx_arr[e * S + s] = fir_tick(&bp_fx[e * S + s], Fx_arr[e * S + s]);

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

            /* 误差麦信号 (每样本真实, 匹配实时版 ADC 链): es = (Pri⊗ref + Ŝ⊗anti)·G → 64tap 带通.
               anti 传播延迟由 Ŝ FIR 延迟线体现; 修复 R-58: 原 anti_est_prev 每秒更新 → 误差
               信号滞后 1s, 时变场景下梯度失效 (离线 NR 1.5dB 根因之一, 实时版无此滞后).
               修复 R-58-2: anti_spk 直接经 Ŝ 到误差麦 (与实时版 DAC 一致, 无 /gain 除法 —
               原除法使 anti 环路增益 ∝ 1/G, G=1 时反馈最强 → anti 被推到 ±1 钳位,
               截断后带内能量被压扁 → NR_true 假阳性 12.9dB). */
            float dis_val[E];
            for (int e = 0; e < E; e++) {
                dis_val[e] = fir_tick(&pri_firs[e], ref_filt);         /* 带通扰动 (NR_true 口径) */
                /* R-58-4: es 的 pri 分支改用 64tap 带通参考 (ref_anc), 与 anti 分支延迟对齐.
                   原 ref[idx] 全带直通 → pri 群延迟仅 11 样本, anti 分支 (bp_anc 31 + Ŝ 10) ≈ 41 样本
                   → 梯度 Fx(41) 与 es 主导成分 pri(11) 错位 30 样本 → 带限信号互相关显著,
                   LMS 试图让 anti 分支"超前" → 因果 FIR 做不到 → 持续正反馈推大 anti 到钳位.
                   训练世界 d = Pri⊗x_band(带通, 延迟 42) 与 Fx(41) 对齐 → 收敛.
                   修复: pri 分支用 ref_anc (bp_anc 31 + pri 11 = 42) → 与 anti 分支/Fx 对齐. */
                float pri_raw = fir_tick(&pri_raw_firs[e], ref_anc_all[idx]); /* R-58-4: 带通参考 (对齐 anti 分支) */
                float anti_at_mic = 0;
                for (int s = 0; s < S; s++)
                    anti_at_mic += fir_tick(&sec_firs_err[e * S + s], anti_spk[s]);
                /* R-58-10: es 不再二次乘 G — pri_raw/anti_at_mic 已含 G (经 ref_anc=bp(G·x)).
                   原 es=(pri+anti)×G → es∝G² → 有效步长∝G + NR_true 口径 ±20log10(G) 伪影
                   (G=2.72 road-15: tanh 饱和梯度死亡 → 负 NR; G=0.27 road_0-34: +11.4dB 虚高).
                   修复后步长/指标与 G 无关, 弱/强文件行为一致. */
                float es = pri_raw + anti_at_mic;
                float es_nr = es;   /* R-58 口径修复: NR_true 统计用未截断信号.
                                       tanh 是 ADC 饱和模拟, 非声学失真 — 截断把全带能量折叠进带内,
                                       Wc≈0 (弱信号) 时也报假负 NR. 梯度仍用截断 err_meas (匹配实时版). */
                if      (es >  1.0f) es =  tanhf(es);
                else if (es < -1.0f) es = -tanhf(-es);
                err_meas[e] = fir_tick(&bp_err[e], es);                /* 带通残差: 驱动梯度 (匹配实时版) */
                float err_nr = fir_tick(&bp_err_nr[e], es_nr);         /* 未截断带通残差: NR_true 统计 */
                err_pwr += err_nr * err_nr;
                dis_pwr += dis_val[e] * dis_val[e];                    /* 已知真值扰动 (Pri模型) */
                err_out[e * N + idx] = err_meas[e];
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
        float err_rms = sqrtf(err_pwr / (len * E));   /* 显示用全帧误差 RMS */
        float ref_rms = sqrtf(acc_ref / len);
        float anti_rms = sqrtf(acc_anti / (len * S));

        /* ── 已知真值NR (仅离线可用: 利用 Pri 模型精确计算扰动) ── */
        dis_pwr /= (len * E);
        float nr_true = 10.0f * log10f((dis_pwr + 1e-12f) / (err_pwr / (len * E) + 1e-12f));

        /* 发散检测 (基于诚实NR, 匹配实时版) */
        diverged = (pa > 9.0f * pe && anti_rms > 0.05f && nr_est < 0.0f);

        /* 输出: 诚实NR(匹配实时) + 已知真值NR(Pri模型) */
        char nr_est_str[20], nr_true_str[20];
        if (diverged) snprintf(nr_est_str, sizeof(nr_est_str), "DIV!");
        else          snprintf(nr_est_str, sizeof(nr_est_str), "%.1f", nr_est);
        snprintf(nr_true_str, sizeof(nr_true_str), "%.1f", nr_true);
        printf("%4d | %5d | %6s | %6s | %5.3f | %6.4f | %5.4f | %s",
               sec + 1, new_scene, nr_est_str, nr_true_str,
               err_rms, ref_rms, anti_rms, action);
        if (cfg.ocg_enable) printf(" [C%d/%d]", ocg.active, ocg.n_clusters);
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
    for (int e = 0; e < E; e++) { free(bp_err[e].delay_line); free(bp_err_nr[e].delay_line); }
    for (int i = 0; i < E * S; i++) free(bp_fx[i].delay_line);
    fxnlms_free(&fx);
    scene_ctrl_free(&sc);
    for (int i = 0; i < E * S; i++) { free(sec_firs[i].delay_line); free(sec_firs_err[i].delay_line); }
    free(sec_firs); free(sec_firs_err); free(sec_coeffs);
    for (int e = 0; e < E; e++) { free(pri_firs[e].delay_line); free(pri_raw_firs[e].delay_line); }
    free(bp_fir.delay_line);
    free(wav.data); if (ref_resampled) free(ref_resampled);
    bin_free(sec_path); if (pri_path_used != pri_path) free(pri_path_used); bin_free(pri_path); bin_free(sub_filters);
    bin_free(bp_coeff);
    if (bp_anc_ok) bin_free(bp_anc_coeff);   /* BUG-6: bandpass_anc.bin 独立所有权 */
    printf("Done.\n");
    return 0;
}
