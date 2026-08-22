/** SceneZone-ANC — 离线 WAV 降噪评估 (SFANC 硬选库, 纯开环).
 *
 *  信号路径与 main_realtime.c deploy 分支完全一致, 仅 I/O 不同:
 *    实时版: PortAudio 硬件 (ADC/DAC)
 *    离线版: WAV 文件读写
 *  开环硬选 (唯一路径): 从 data/wc_bank.bin 选槽 (SIM 轮换 / 分类 CNN argmax /
 *    静态槽), 固定 Wc 纯前向 (µ=0, 无误差麦驱动). 误差麦信号仍合成 (Pri+Ŝ)
 *    用于 NR_true 度量 (离线可测). 无库 → FATAL.
 *
 *  编译: gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c
 *              src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c
 *              src/scene_bank.c -lm -o main.exe
 *  运行: ./main.exe <noise.wav>   (GFANC_BANK_SIM=1 轮换 / GFANC_FORCE_CLASS=k 静态)
 *  输出: anti_out.wav (S=2ch 反噪声), error_out.wav (E=3ch 误差麦信号)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>   /* 仅 Windows: SetConsoleOutputCP 需要 (clock() 来自 <time.h>) */
#endif

#include "fir_filter.h"
#include "binary_loader.h"
#include "cnn_m5_forward.h"
#include "scene_controller.h"
#include "scene_manager.h"
#include "fxnlms_mimo.h"
#include "scene_bank.h"

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

/* R-18: 2阶 Butterworth 低通 biquad — 与实时版 (main_realtime.c R-14) 逐字一致,
   fc=0.40625×sr_out≈6.5kHz@16k 输出. 替换旧"2样本移动平均"(截止仅~fs_in/4,
   折叠镜像压制不足, 且与实时抗混叠链不一致). */
typedef struct { float b0,b1,b2,a1,a2,z1,z2; } biquad_t;

static void biquad_init_lpf(biquad_t *f, float fc, float fs)
{
    float w0 = 2.0f * 3.14159265f * fc / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / (2.0f * 0.70710678f);  /* Q=1/√2 Butterworth */
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f - c) / (2.0f * a0);
    f->b1 = (1.0f - c) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0f * c / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_tick(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static float *resample_mono(const float *in, int n_in, int sr_in, int sr_out, int *n_out)
{
    /* R-18: 下采样前抗混叠低通 — 同实时版 R-14 (biquad fc≈6.5kHz@16k),
       替代旧 2样本移动平均. 折叠镜像压制 ~25dB+ (移动平均 @fs_in/4 仅 ~6dB). */
    float *filt = NULL;
    if (sr_in > sr_out) {
        filt = (float *)malloc(n_in * sizeof(float));
        biquad_t aa;
        biquad_init_lpf(&aa, 0.40625f * sr_out, (float)sr_in);
        for (int i = 0; i < n_in; i++)
            filt[i] = biquad_tick(&aa, in[i]);
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
#define L       1024  /* 控制滤波器长度 (与库槽长 S*L 对齐, scene_bank_load 校验) */
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

/* Phase 3: 文件存在探针 (deploy 分类 CNN 集选择用) */
static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ══════════════════════════════════════════════════════════
   主函数
   ══════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    gfanc_config_t cfg = GFANC_CONFIG_DEFAULT;
    gfanc_config_load_env(&cfg);
    if (argc < 2) {
        printf("SceneZone-ANC — Offline WAV ANC Eval (SFANC 硬选库, 纯开环)\n\n");
        printf("Usage: %s <noise.wav>\n", argv[0]);
        printf("  GFANC_BANK_SIM=1    定时轮换库槽类 (验证切换无爆音)\n");
        printf("  GFANC_FORCE_CLASS=k 强制静态库槽 k\n\n");
        printf("Output: anti_out.wav (%d ch), error_out.wav (%d ch)\n", S, E);
        return 1;
    }

    /* ── 1. 加载权重 (R-3: 逐文件校验长度) ── */
    printf("Loading weights...\n");
    float *sec_path, *pri_path, *bp_coeff;
    int sec_len  = bin_load_float("data/secondary_path.bin", &sec_path);
    int pri_len  = bin_load_float("data/primary_path.bin", &pri_path);
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
    if (bp_len < BP_LEN) {
        fprintf(stderr, "FATAL: bandpass_fir.bin too short/load failed (%d<%d)\n", bp_len, BP_LEN);
        return 1;
    }
    printf("  OK: sec=%d pri=%d bp=%d L=%d\n", sec_len, pri_len, bp_len, L);
    (void)pri_len;

    /* ── 2. 初始化组件 ── */
    /* R-27: 批次指纹 — 检测 cnn_bank/bandpass 是否跨批混配 (WARN, 不阻断).
       分类 CNN (cnn_bank_*.bin) 只在库 N≥2 且需要分类时初始化, 见下方模式选择. */
    bin_check_batch();

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
    /* 嵌入式处理延迟 (GFANC_EMBED_DELAY_MS, 默认0ms — R-58-8): 模拟 ADC+DSP+DAC 信号链延迟,
       pad 到 Ŝ 模型 (Fx 对齐 + 误差合成共用), 消耗因果裕量 — 离线预测嵌入式目标 NR.
       默认 0 与训练世界一致 (3ms=48样本 pad 会 anti 相位错位→自适应正反馈发散);
       评估嵌入式目标时 GFANC_EMBED_DELAY_MS 显式开启, GFANC_VIRT_DELAY_MS 可覆盖 (实验/扫参用). */
    int virt_delay = cfg.embed_delay_ms * FS / 1000;
    {   const char *vd = getenv("GFANC_VIRT_DELAY_MS");
        if (vd) virt_delay = (int)(atof(vd) * FS / 1000.0f);
        printf("  PROC delay: %d ms (%d samples) added to Ŝ — 嵌入式信号链处理延迟"
               " (默认0ms, GFANC_EMBED_DELAY_MS 显式开启; 可覆盖)\n",
               virt_delay * 1000 / FS, virt_delay); }
    int sec_padded = SEC_LEN + DSP_DELAY + virt_delay;
    float *sec_coeffs = (float *)calloc(E * S * sec_padded, sizeof(float));
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e * S + s;
            memcpy(sec_coeffs + idx * sec_padded + DSP_DELAY + virt_delay,
                   sec_path + idx * SEC_LEN, SEC_LEN * sizeof(float));
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

    /* 2d. (SFANC 分类决策层 在库加载后按需初始化, 见下方模式选择) */

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
    printf("  System ready.\n");

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
    /* ── 4. 离线 ANC (与实时版 main_realtime.c 信号路径一致, 仅 I/O 不同) ── */
    int chunk = FS, n_sec = (N + chunk - 1) / chunk;
    float *anti_out = (float *)calloc(S * N, sizeof(float));
    float *err_out  = (float *)calloc(E * N, sizeof(float));

    /* CrossFader 状态 */
    int   fade_cnt = 0;
    float wc_old[S*L], wc_cur[S*L];

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

    printf("\n%4s | %22s | %6s | %6s | %6s | %7s | %6s | %s\n",
           "Sec", "BankClass", "NR_est", "NR_true", "err", "refFilt", "anti", "Note");
    for (int i = 0; i < 105; i++) printf("-");
    printf("\n");

    /* ── SFANC 硬选库评估 (纯开环, 唯一路径): 加载库 → 三模式选槽 ──
       误差麦信号仍合成 (Pri+Ŝ) 用于 NR_true 度量 (离线可测), 但不驱动任何更新 —
       物理无梯度链 (deploy 与 main_realtime 同一形态). 无库 → FATAL (硬选部署前提). */
    int force_class = getenv("GFANC_FORCE_CLASS") ? atoi(getenv("GFANC_FORCE_CLASS")) : 0;
    int bank_sim     = cfg.bank_sim;
    int bank_sim_sec = cfg.bank_sim_sec;
    float *open_wc = NULL;          /* 初始槽 Wc (fxnlms_set_wc 预载) */
    float *open_bank = NULL;        /* 整库 [N*S*L] (SIM/分类轮换用) */
    int open_bank_n = 0;
    int open_class = 0;             /* 当前库槽索引 */
    int mode_sim = 0;               /* 1=SIM 轮换 (GFANC_BANK_SIM=1, N≥2) */
    int mode_class = 0;             /* 1=真分类选库 (N≥2 + cnn_bank_*.bin) */
    scene_ctrl_t sc;                /* 分类决策层 (mode_class 时初始化) */
    {
        scene_bank_t bank;
        if (scene_bank_load(cfg.bank_file, S, L, &bank) != 0) {
            fprintf(stderr, "FATAL: 无库 %s — SFANC 硬选评估必须 data/wc_bank.bin "
                    "(先 export/generate_bank.py 生成)\n", cfg.bank_file);
            fxnlms_free(&fx); return 1;
        }
        open_bank_n = (int)bank.n_slots;
        open_bank = (float *)malloc((size_t)open_bank_n * bank.slot_len * sizeof(float));
        if (!open_bank) { scene_bank_free(&bank); fxnlms_free(&fx); return 1; }
        memcpy(open_bank, bank.data, (size_t)open_bank_n * bank.slot_len * sizeof(float));
        open_wc = (float *)malloc((size_t)S * L * sizeof(float));
        if (!open_wc) { scene_bank_free(&bank); fxnlms_free(&fx); return 1; }
        scene_bank_free(&bank);

        if (bank_sim && open_bank_n >= 2) {
            /* SIM 轮换: 每 bank_sim_sec 秒换类, 验证切换无爆音 (决策层验证用, 不依赖 CNN) */
            mode_sim = 1;
            memcpy(open_wc, open_bank, (size_t)S * L * sizeof(float));
            printf("  [BANK] SIM 轮换: 整库 %d 槽, 每 %d s 换类 (验证切换无爆音)\n",
                   open_bank_n, bank_sim_sec);
        } else if (open_bank_n >= 2 && file_exists("data/cnn_bank_linear_weight.bin")) {
            /* 真分类选库: SFANC 分类 CNN (cnn_bank_*, K=N), 决策层初始化.
               K==N 对齐由 scene_ctrl_init + set_bank 保证 (argmax 越界已防御). */
            if (cnn_m5_init_base("cnn_bank") != 0) {
                fprintf(stderr, "  [WARN] cnn_bank CNN 加载失败, 回退静态槽 %d\n", force_class);
                int c = force_class % open_bank_n;
                memcpy(open_wc, open_bank + (size_t)c * S * L, (size_t)S * L * sizeof(float));
                open_class = c;
            } else {
                scene_ctrl_init(&sc);
                scene_ctrl_set_bank(&sc, open_bank_n);
                scene_ctrl_set_bank_hold(&sc, cfg.bank_hold_frames);
                mode_class = 1;
                memcpy(open_wc, open_bank, (size_t)S * L * sizeof(float));   /* 初始槽 0 */
                printf("  [BANK] 分类选槽: 整库 %d 槽, CNN argmax+防抖 (K=%d, %d帧) — "
                       "每 s 选槽→crossfade\n", open_bank_n, sc.K, cfg.bank_hold_frames);
            }
        } else {
            /* 静态: N=1 恒播槽 0; N≥2 缺分类 CNN → force_class 选槽 (WARN) */
            int c = (open_bank_n >= 2) ? (force_class % open_bank_n) : 0;
            memcpy(open_wc, open_bank + (size_t)c * S * L, (size_t)S * L * sizeof(float));
            open_class = c;
            if (open_bank_n >= 2)
                fprintf(stderr, "  [WARN] 库 N=%d ≥2 但缺分类 CNN (data/cnn_bank_*.bin) — 静态槽 %d\n",
                        open_bank_n, c);
            else
                printf("  [BANK] 静态槽 0 (N=1)\n");
        }
        fxnlms_set_wc(&fx, open_wc);
    }
    printf("  OPEN_LOOP: 固定库槽 Wc, 纯前向无梯度 (deploy 离线等价)\n");

    for (int sec = 0; sec < n_sec; sec++) {
        int start = sec * chunk, len = (start + chunk <= N) ? chunk : (N - start);
        if (len <= 0) break;

        /* 4a. 每秒决策层选槽 (SIM / 分类): 换槽时启动 delayless crossfade
           (wc_old=当前 fx.wc → wc_cur=新槽), 与实时部署决策层同一机制.
           fade 进行中 (fade_cnt>0) 不打断 — 轮换间隔 >> fade_len, 正常不会重叠.
           静态 (非 SIM/分类): 固定库槽 Wc 恒播 — fxnlms_set_wc 已预载, 无每帧切换. */
        char action[20] = "-";
        if (mode_sim && sec > 0) {
            int c = (sec / bank_sim_sec) % open_bank_n;
            if (c != open_class) {
                memcpy(wc_old, fx.wc, S * L * sizeof(float));
                memcpy(wc_cur, open_bank + (size_t)c * S * L, S * L * sizeof(float));
                fade_cnt = cfg.fade_len;
                snprintf(action, sizeof(action), "SIM%d->%d", open_class, c);
                printf("  [BANK] SIM 类 %d → %d (slot %d, fade %d)\n",
                       open_class, c, c, cfg.fade_len);
                open_class = c;
            }
        } else if (mode_class && len == chunk) {
            /* 分类: 每秒 CNN argmax → 防抖选类 → 换槽 crossfade. 弱信号/CNN 失败 →
               scene_ctrl_classify 保持当前类, 不换槽. 仅整秒 (len==chunk) 分类 —
               末尾不足 1s 片段跳过 (scene_ctrl 固定读 16000 样本, 超尾会 OOB). */
            int c = scene_ctrl_classify(&sc, ref_filt_all + start, NULL);
            if (c < 0) c = 0;
            if (c >= open_bank_n) c = open_bank_n - 1;
            if (c != open_class) {
                memcpy(wc_old, fx.wc, S * L * sizeof(float));
                memcpy(wc_cur, open_bank + (size_t)c * S * L, S * L * sizeof(float));
                fade_cnt = cfg.fade_len;
                snprintf(action, sizeof(action), "C%d->%d", open_class, c);
                printf("  [BANK] 分类 %d→%d (filter %d, slot %d, fade %d)\n",
                       open_class, c, c, c, cfg.fade_len);
                open_class = c;
            }
        }

        /* 4b. 逐样本纯前向 (deploy 硬选: 无梯度链) */
        float err_pwr = 0, dis_pwr = 0;
        acc_err = acc_anti_est = acc_d_est = acc_err_cross = 0;
        acc_err_win = 0;
        acc_anti = acc_ref = 0; acc_cnt = 0;
        for (int n = 0; n < len; n++) {
            int idx = start + n;
            float ref_filt = ref_anc_all[idx];   /* BUG-6: 64tap ANC 带通 (匹配实时) */
            acc_ref += ref_filt_all[idx] * ref_filt_all[idx];  /* 1024tap CNN 带通 (匹配实时 ref_rms 显示) */

            /* CrossFader — 类切换共用 (fade_cnt>0 才激活). 静态 fade_cnt 恒 0, 跳过. */
            if (fade_cnt > 0) {
                float a = (float)fade_cnt / cfg.fade_len;
                for (int i = 0; i < S * L; i++)
                    fx.wc[i] = a * wc_old[i] + (1.0f - a) * wc_cur[i];
                fade_cnt--;
                if (fade_cnt == 0) memcpy(fx.wc, wc_cur, S * L * sizeof(float));
            }

            /* anti_spk = Wc ⊗ x_hist (纯前向, 不写 xd/不读 err_meas).
               物理无梯度链 — Fx/误差合成只为度量 (NR_true), 不驱动任何更新. */
            float anti_spk[S];
            fxnlms_forward_rt_open(&fx, ref_filt, anti_spk);

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
        char bankbuf[32];
        snprintf(bankbuf, sizeof(bankbuf), "[BANK] 类 %d/%d", open_class, open_bank_n);
        printf("%4d | %22s | %6s | %6s | %5.3f | %6.4f | %5.4f | %s",
               sec + 1, bankbuf, nr_est_str, nr_true_str,
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
    for (int e = 0; e < E; e++) { free(bp_err[e].delay_line); free(bp_err_nr[e].delay_line); }
    fxnlms_free(&fx);
    scene_ctrl_free(&sc);
    for (int i = 0; i < E * S; i++) free(sec_firs_err[i].delay_line);
    free(sec_firs_err); free(sec_coeffs);
    for (int e = 0; e < E; e++) { free(pri_firs[e].delay_line); free(pri_raw_firs[e].delay_line); }
    free(bp_fir.delay_line);
    free(wav.data); if (ref_resampled) free(ref_resampled);
    bin_free(sec_path); if (pri_path_used != pri_path) free(pri_path_used); bin_free(pri_path);
    bin_free(bp_coeff);
    if (bp_anc_ok) bin_free(bp_anc_coeff);   /* BUG-6: bandpass_anc.bin 独立所有权 */
    if (open_wc) free(open_wc);              /* 初始库槽副本 */
    if (open_bank) free(open_bank);          /* 整库副本 */
    printf("Done.\n");
    return 0;
}
