/** GFANC FxNLMS — PortAudio callback realtime ANC.
 *
 * 编译: gcc -O2 -Iinclude main_realtime.c src/scene_controller.c
 *       src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c
 *       src/cnn_m5_forward.c src/pa_loader.c -lm -o gfanc_realtime.exe
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
#include "howling_detect.h"

/* ══════════════════════════════════════════════════════════ */
#define FS_HW    48000
#define FS_ANC   16000
#define E        3
#define S        2
#define L        1024
#define BP_LEN   1024
#define SEC_LEN  1024
#define DSP_DELAY 16
#define FADE_LEN 1600  /* Wc切换CrossFader时长 (100ms @16k, =20Hz×2周期, ≥FIR长度) */
#define MIC_PRE_GAIN  10.0f    /* 输入数字预增益 (噪声源良好耦合时 8-10x 足够) */
#define MIC_CLIP_MAX  1.0f    /* 输入软限幅 (防止吹气/大声压冲爆 FIR) */
#define COLDSTART_MS  400     /* 冷启动 ramp 时长 ms (anti_out 0→1) */
#define RAMP_SAMPLES   ((FS_ANC * COLDSTART_MS) / 1000)
#define MUTE_HOLD_MS  1500    /* safety_mute 抑制时长 ms (需 >1s 覆盖下一次 RMS 评估) */
#define MUTE_HOLD_SAMPLES ((FS_ANC * MUTE_HOLD_MS) / 1000)
#define FB_LEN       256     /* 反馈路径 FIR 长度 */
#define FB_ENABLED   1       /* 反馈抵消开关 (需先运行 calibrate_feedback.exe) */
#define HOWLING_ENABLED 1    /* 啸叫检测 + 陷波 (DFT 频谱峰值检测) */

/* ══════════════════════════════════════════════════════════ */
typedef struct {
    /* ANC 模块 */
    scene_ctrl_t  sc;
    fxnlms_mimo_t fx;
    fir_filter_t  bp_fir;        /* ref 带通 */
    fir_filter_t  bp_err[E];     /* err 带通 */
    fir_filter_t *sec_firs;      /* [E*S] 次级路径 */
    float        *sec_coeffs;

    /* 反馈抵消 */
    fir_filter_t     fb_fir;        /* 扬声器→参考麦反馈路径 FIR */
    float           *fb_coeffs_buf;
    int              fb_active;     /* 1=反馈抵消已加载 */

    /* 啸叫检测 */
    howling_detect_t hw;           /* DFT 频谱检测 + IIR 陷波 */

    /* 跨回调状态 */
    float  wc_old[S*L], wc_cur[S*L];
    int    fade_cnt;
    int    ramp_cnt;        /* 输出渐变: RAMP_SAMPLES → 0, anti_out 0→1 */
    int    mute_hold;       /* safety_mute 抑制: MUTE_HOLD_SAMPLES → 0, 覆盖首次 RMS */
    /* CNN 双缓冲: 回调填一块→原子标记就绪→切到另一块, 无数据竞争零样本丢失 */
    float  cnn_buf[2][FS_ANC];
    int    cnn_fill_idx;      /* 当前填充块 (0/1), 仅回调访问 */
    int    cnn_cnt;           /* 当前填充块已写入样本数, 仅回调访问 */
    volatile LONG cnn_buf_ready; /* -1=无就绪, 0/1=该块已满待主线程处理 */
    int    first_sec;

    /* 每场景记忆: 保存已收敛的 Wc, 下次切回时直接恢复 */
    float  scene_wc[SC_K][S*L];
    int    scene_wc_valid[SC_K];  /* 1=该场景已有收敛好的 Wc */
    int    cur_scene_id;
    int    converged_frames;      /* 连续正常帧数 (判断已收敛) */

    /* 48k 重采样缓冲 */
    float *ref_buf, *anti_buf, *err_buf; /* 堆分配 (main初始化), 存16k速率数据, 名_48k为历史遗留 */
    int    dec_phase;

    /* 统计 */
    volatile float nr_level, ref_rms, err_rms, dist_rms;
    volatile float ch_rms[4];    /* 原始声道 RMS: ch0=ref, ch1-3=err */
    volatile float anti_rms;     /* 反噪声 RMS */
    volatile float acc_ref, acc_err, acc_dist, acc_fb;
    volatile float acc_ch[4], acc_anti, acc_anti_est;
    volatile float fb_rms;       /* 反馈抵消量 RMS */
    volatile float anti_est_rms; /* 模型估计反噪声 RMS (NR计算用) */
    volatile int   acc_cnt;
    volatile int   safety_mute;
    volatile int   running;
    volatile int   callback_count;
} rt_ctx_t;

#include "pa_loader.h"

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
        ctx->ref_buf[n] = in[(n*3)*6 + 0];  /* 最近邻抽取: 每3个取1个 */
        ctx->err_buf[n*3+0] = in[(n*3)*6 + 1];
        ctx->err_buf[n*3+1] = in[(n*3)*6 + 2];
        ctx->err_buf[n*3+2] = in[(n*3)*6 + 3];
    }

    /* ── ANC @ 16kHz ── */
    float anti_spk[S] = {0, 0};   /* 反馈抵消需要上一轮的 anti 值 */
    for (int n = 0; n < c16k; n++) {
        float ref_raw     = ctx->ref_buf[n];             /* 原始电平 (用于 RMS 显示) */

        /* 反馈抵消: 估计扬声器→参考麦的反馈分量并减去 */
        float fb_est = 0;
        if (ctx->fb_active) {
            float anti_mix = (anti_spk[0] + anti_spk[1]) * 0.5f;
            fb_est = fir_tick(&ctx->fb_fir, anti_mix);
        }
        float ref_sample  = (ref_raw - fb_est) * MIC_PRE_GAIN;
        /* 输入软限幅: tanh 防止吹气/冲击导致 FIR 饱和 → 非线性失真 → 发散 */
        if      (ref_sample >  MIC_CLIP_MAX) ref_sample =  tanhf(ref_sample);
        else if (ref_sample < -MIC_CLIP_MAX) ref_sample = -tanhf(-ref_sample);

        /* 带通滤波 (预增益已应用, 不造成反馈) */
        float ref_filt = fir_tick(&ctx->bp_fir, ref_sample);

        /* CNN 累积 */
        /* CNN 双缓冲: 填满一块→原子标记就绪→切到另一块 */
        if (ctx->cnn_cnt < FS_ANC) {
            ctx->cnn_buf[ctx->cnn_fill_idx][ctx->cnn_cnt++] = ref_filt;
            if (ctx->cnn_cnt >= FS_ANC) {
                InterlockedExchange(&ctx->cnn_buf_ready, ctx->cnn_fill_idx);
                ctx->cnn_fill_idx ^= 1;
                ctx->cnn_cnt = 0;
            }
        }

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

        /* 扰动 = bp(mic) × 预增益 (含软限幅) — 实测误差, 直接驱动梯度 */
        float err_meas[E];
        for (int e = 0; e < E; e++) {
            float es = ctx->err_buf[n*3+e] * MIC_PRE_GAIN;
            if      (es >  MIC_CLIP_MAX) es =  tanhf(es);
            else if (es < -MIC_CLIP_MAX) es = -tanhf(-es);
            err_meas[e] = fir_tick(&ctx->bp_err[e], es);
        }

        /* FxNLMS 实时路径: anti=Wc⊗ref, 梯度用err_meas直接驱动 (不合成err) */
        if (ctx->fade_cnt == 0)
            fxnlms_tick_rt(&ctx->fx, ref_filt, Fx_arr, err_meas, anti_spk);
        else
            fxnlms_forward_rt(&ctx->fx, ref_filt, Fx_arr, err_meas, anti_spk);

        /* NaN/Inf 保护 + 输出钳位 (防止 FxNLMS 计算值远超 ±1.0 导致硬截断失真) */
        for (int s = 0; s < S; s++) {
            if (!isfinite(anti_spk[s])) anti_spk[s] = 0.0f;
            if (anti_spk[s] > 1.0f) anti_spk[s] = 1.0f;
            if (anti_spk[s] < -1.0f) anti_spk[s] = -1.0f;
        }

        /* 啸叫检测 + 陷波 (钳位后, 统计前) */
        {
            float err_avg = 0;
            for (int e = 0; e < E; e++) err_avg += err_meas[e];
            err_avg /= (float)E;
            howling_tick(&ctx->hw, err_avg, anti_spk, S);
        }

        /* 累积功率: 原始声道 + 滤波参考 + 误差 + 反噪声 */
        ctx->acc_ch[0] += ref_raw * ref_raw;
        for (int e = 0; e < E; e++)
            ctx->acc_ch[1+e] += ctx->err_buf[n*3+e] * ctx->err_buf[n*3+e];
        ctx->acc_ref += ref_filt * ref_filt;
        ctx->acc_fb  += fb_est * fb_est;
        for (int e = 0; e < E; e++) {
            ctx->acc_err  += err_meas[e] * err_meas[e];
            ctx->acc_dist += err_meas[e] * err_meas[e];  /* 实测误差功率 (用于NR参考) */
        }
        /* anti_est[e] = Σ_s,k Wc[s,k] * Xd[e,s,k] (模型估计反噪声, 用于NR) */
        {   float anti_est[E]; memset(anti_est, 0, sizeof(anti_est));
            for (int e = 0; e < E; e++)
                for (int s = 0; s < S; s++)
                    for (int k = 0; k < L; k++)
                        anti_est[e] += ctx->fx.wc[s*L+k]
                                     * ctx->fx.xd[(e*S+s)*L+k];
            for (int e = 0; e < E; e++)
                ctx->acc_anti_est += anti_est[e] * anti_est[e];
        }
        ctx->acc_anti += anti_spk[0] * anti_spk[0] + anti_spk[1] * anti_spk[1];
        if ((ctx->acc_cnt += 1) >= FS_ANC) {
            float pe = ctx->acc_err, pa = ctx->acc_anti_est;
            /* NR: 模型估计反噪声 vs 实测残差 (proxy for acoustic NR) */
            ctx->nr_level = 10.0f * log10f((pe + pa + 1e-12f) / (pe + 1e-12f));
            ctx->anti_est_rms = sqrtf(pa / (FS_ANC * E));
            ctx->ref_rms  = sqrtf(ctx->acc_ref  / FS_ANC);
            ctx->err_rms  = sqrtf(pe / (FS_ANC * E));
            ctx->dist_rms = sqrtf((pe + pa) / (FS_ANC * E)); /* 估计噪声RMS(err+anti_est) */
            ctx->fb_rms   = sqrtf(ctx->acc_fb   / FS_ANC);
            for (int c = 0; c < 4; c++)
                ctx->ch_rms[c] = sqrtf(ctx->acc_ch[c] / FS_ANC);
            ctx->anti_rms = sqrtf(ctx->acc_anti / (FS_ANC * 2));
            ctx->safety_mute = (ctx->err_rms > ctx->ref_rms && ctx->ref_rms > 0.0001f
                                && ctx->mute_hold == 0);  /* 冷启动期间抑制, 由 ramp 接管 */
            ctx->acc_ref = ctx->acc_err = ctx->acc_dist = ctx->acc_fb = 0;
            ctx->acc_anti = ctx->acc_anti_est = 0; ctx->acc_cnt = 0;
            for (int c = 0; c < 4; c++) ctx->acc_ch[c] = 0;
        }

        /* 安全静音时输出零 (冷启动抑制期内不触发) */
        if (ctx->safety_mute) { anti_spk[0] = 0; anti_spk[1] = 0; }

        /* 冷启动 ramp: INIT/RESET 后 anti_out 从 0 平滑渐入 */
        if (ctx->ramp_cnt > 0) {
            float ramp = 1.0f - (float)ctx->ramp_cnt / RAMP_SAMPLES;
            anti_spk[0] *= ramp;
            anti_spk[1] *= ramp;
            ctx->ramp_cnt--;
        }

        /* safety_mute 抑制计数 (独立于 ramp, 覆盖到下一次有效 RMS 评估) */
        if (ctx->mute_hold > 0) ctx->mute_hold--;

        ctx->anti_buf[n] = anti_spk[0];
        ctx->anti_buf[n + c16k] = anti_spk[1];
    }

    /* 内插 + 输出 (ch0=spk0, ch1=spk1) */
    int oi = 0;
    for (int n = 0; n < c16k; n++) {
        float a0 = ctx->anti_buf[n], a1 = ctx->anti_buf[n + c16k];
        if (!isfinite(a0)) a0 = 0.0f;
        if (!isfinite(a1)) a1 = 0.0f;
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
/* pa_init() → src/pa_loader.c */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    if (pa_init() != 0) return 1;
    p_Pa_Initialize();

    /* 查找 WASAPI host API (Windows 系统->声音 显示的设备) */
    int wasapi_api = -1;
    for (int i = 0; ; i++) {
        const PaHostApiInfo2 *info = (const PaHostApiInfo2 *)p_Pa_GetHostApiInfo(i);
        if (!info) break;
        if (strstr(info->name, "WASAPI")) { wasapi_api = i; break; }
    }
    if (wasapi_api < 0) wasapi_api = p_Pa_GetDefaultHostApi();  /* fallback */
    const PaHostApiInfo2 *api = (const PaHostApiInfo2 *)p_Pa_GetHostApiInfo(wasapi_api);

    /* 列出设备 (仅 WASAPI 端点, 对应 Windows 系统->声音 中显示的设备) */
    int nd = p_Pa_GetDeviceCount();
    printf("\n=== Audio Devices (WASAPI) ===\n");
    for (int i = 0; i < nd; i++) {
        const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
        if (info && info->hostApi == wasapi_api)
            printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
                i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
    }
    printf("Host API: %s\n", api ? api->name : "default");

    int in_dev, out_dev;
    printf("\nInput device ID: "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID: "); fflush(stdout); scanf("%d", &out_dev);

    printf("\n");

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
    ctx.cnn_buf_ready = -1;  /* -1=无就绪块, 0/1=该块已满 */
    g_ctx = &ctx;
    ctx.running = 1; ctx.first_sec = 1;
    ctx.mute_hold = MUTE_HOLD_SAMPLES;  /* 启动抑制 safety_mute, 覆盖第一秒 Wc=0 期 */

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

    /* 反馈抵消: 加载反馈路径 FIR (需先运行 calibrate_feedback.exe) */
#if FB_ENABLED
    {
        float *fb_coeffs_raw = NULL;
        int fb_loaded = bin_load_float("data/feedback_path.bin", &fb_coeffs_raw);
        if (fb_loaded > 0 && fb_coeffs_raw) {
            ctx.fb_coeffs_buf = (float *)calloc(FB_LEN, sizeof(float));
            int copy_len = fb_loaded < FB_LEN ? fb_loaded : FB_LEN;
            memcpy(ctx.fb_coeffs_buf, fb_coeffs_raw, copy_len * sizeof(float));
            ctx.fb_fir.coeffs = ctx.fb_coeffs_buf;
            ctx.fb_fir.n_taps = FB_LEN;
            ctx.fb_fir.delay_line = (double *)calloc(FB_LEN, sizeof(double));
            ctx.fb_fir.ptr = 0;
            ctx.fb_active = 1;
            float fb_rms = 0;
            for (int i = 0; i < FB_LEN; i++) fb_rms += ctx.fb_coeffs_buf[i] * ctx.fb_coeffs_buf[i];
            printf("  Feedback cancel: %d taps loaded, RMS=%.4f\n", FB_LEN, sqrtf(fb_rms / FB_LEN));
            bin_free(fb_coeffs_raw);
        } else {
            ctx.fb_active = 0;
            printf("  Feedback cancel: disabled (run calibrate_feedback.exe first)\n");
        }
    }
#endif

    scene_ctrl_init(&ctx.sc, centroids, sub_filters, L);
    howling_init(&ctx.hw, HOWLING_ENABLED);
    fxnlms_init(&ctx.fx, E, S, L, 0.0001f, 1e-6f);  /* leak 已从 step_size 解耦 */

    /* 缓冲 */
    ctx.ref_buf = (float *)malloc(FS_HW * sizeof(float));
    ctx.anti_buf = (float *)malloc(FS_HW * S * sizeof(float));
    ctx.err_buf = (float *)malloc(FS_HW * E * sizeof(float));
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
        {
            LONG ready = InterlockedExchange(&ctx.cnn_buf_ready, -1);
            if (ready >= 0) {
            float probs[8];
            int new_scene = scene_ctrl_process(&ctx.sc, ctx.cnn_buf[ready], ctx.wc_cur, probs);

            if (ctx.first_sec) {
                /* 首次 INIT: 使用 CNN 通用 Wc, 标记该场景已有记忆 */
                fxnlms_set_wc(&ctx.fx, ctx.wc_cur);
                memcpy(ctx.scene_wc[new_scene], ctx.wc_cur, S*L*sizeof(float));
                ctx.scene_wc_valid[new_scene] = 1;
                ctx.cur_scene_id = new_scene;
                ctx.ramp_cnt  = RAMP_SAMPLES;
                ctx.mute_hold = MUTE_HOLD_SAMPLES;
                printf("[CNN] INIT scene=%d max=%.2f (ramp %dms, mute_hold %dms)\n",
                       new_scene, probs[new_scene], COLDSTART_MS, MUTE_HOLD_MS);
                ctx.first_sec = 0;
            } else {
                float dot = 0, np = 0, nc = 0;
                for (int k = 0; k < 8; k++) {
                    dot += ctx.sc.prev_probs[k] * probs[k];
                    np  += ctx.sc.prev_probs[k] * ctx.sc.prev_probs[k];
                    nc  += probs[k] * probs[k];
                }
                float cos_sim = dot / (sqrtf(np)*sqrtf(nc) + 1e-10f);

                printf("[CNN] s=%d max=%.2f cos=%.2f NR=%.1fdB anti=%.4f%s%s gain=%.0fx cb=%d\n",
                       new_scene, probs[new_scene], cos_sim,
                       ctx.nr_level, ctx.anti_rms,
                       ctx.safety_mute ? " [MUTE]" : "",
                       ctx.ramp_cnt > 0 ? " [RAMP]" : "",
                       MIC_PRE_GAIN, ctx.callback_count);
                printf("       raw: ch0(ref)=%.4f ch1=%.4f ch2=%.4f ch3=%.4f (refFilt=%.4f)\n",
                       ctx.ch_rms[0], ctx.ch_rms[1], ctx.ch_rms[2], ctx.ch_rms[3],
                       ctx.ref_rms);
                printf("       ANC: err=%.4f antiEst=%.4f  (err=实测残差, antiEst=模型估计反噪声)\n",
                       ctx.err_rms, ctx.anti_est_rms);
                if (ctx.fb_active)
                    printf("       FB:  est=%.4f (反馈抵消量 RMS)\n", ctx.fb_rms);
                if (ctx.hw.active_count > 0 || ctx.hw.dominant_db > HW_THRESH_DB * 0.7f)
                    printf("       HW:  f=%.0fHz peak=%.1fdB notches=%d%s\n",
                           ctx.hw.dominant_freq, ctx.hw.dominant_db,
                           ctx.hw.active_count,
                           ctx.hw.active_count > 0 ? " [NOTCH]" : "");

                /* 收敛检测: 连续 3 帧 NR>3dB → 保存当前场景的已收敛 Wc */
                if (ctx.nr_level > 3.0f && !ctx.safety_mute) {
                    ctx.converged_frames++;
                    if (ctx.converged_frames >= 3) {
                        memcpy(ctx.scene_wc[ctx.cur_scene_id], ctx.fx.wc, S*L*sizeof(float));
                        ctx.scene_wc_valid[ctx.cur_scene_id] = 1;
                        ctx.converged_frames = 0;  /* 保存后重置, 避免每帧都写 */
                    }
                } else {
                    ctx.converged_frames = 0;
                }

                /* 场景切换检测 (cos<0.8 且场景确实变了才触发) */
                if (cos_sim < 0.8f && new_scene != ctx.cur_scene_id) {
                    /* 保存旧场景的当前 Wc (最新的收敛状态) */
                    memcpy(ctx.scene_wc[ctx.cur_scene_id], ctx.fx.wc, S*L*sizeof(float));
                    ctx.scene_wc_valid[ctx.cur_scene_id] = 1;

                    /* 新场景: 有记忆就用记忆, 否则用 CNN 通用 Wc */
                    if (ctx.scene_wc_valid[new_scene]) {
                        memcpy(ctx.wc_cur, ctx.scene_wc[new_scene], S*L*sizeof(float));
                        printf("  -> RESET s%d→s%d (restored adapted Wc)\n",
                               ctx.cur_scene_id, new_scene);
                    } else {
                        /* 首次遇到该场景, wc_cur 已是 CNN 通用值 */
                        ctx.scene_wc_valid[new_scene] = 1;
                        printf("  -> RESET s%d→s%d (new scene, CNN preset)\n",
                               ctx.cur_scene_id, new_scene);
                    }

                    memcpy(ctx.wc_old, ctx.fx.wc, S*L*sizeof(float));
                    ctx.fade_cnt   = FADE_LEN;
                    ctx.ramp_cnt   = RAMP_SAMPLES;
                    ctx.mute_hold  = MUTE_HOLD_SAMPLES;
                    ctx.cur_scene_id = new_scene;
                    ctx.converged_frames = 0;
                }
            }
            memcpy(ctx.sc.prev_probs, probs, 8*sizeof(float));
            }
        }
    }

    printf("\nStopping...\n");
    p_Pa_StopStream(stream);
    p_Pa_CloseStream(stream);
    p_Pa_Terminate();
    printf("Done.\n");
    return 0;
}
