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
#include <time.h>
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
/* R-9: 以下参数统一由 cfg (gfanc_config_t) 管理, env 变量可直接生效
   GFANC_RAMP_MS / GFANC_MUTE_MS / GFANC_FADE_LEN / GFANC_MIC_GAIN */
#define MIC_CLIP_MAX  1.0f    /* 输入软限幅 (防止吹气/大声压冲爆 FIR) */
#define FB_LEN       256     /* 反馈路径 FIR 长度 */
#define FB_ENABLED   1       /* 反馈抵消开关 (需先运行 calibrate_feedback.exe) */
#define HOWLING_ENABLED 1    /* 啸叫检测 + 陷波 (DFT 频谱峰值检测) */

/* ── 集中参数 (A2): 单一配置入口, 环境变量可覆盖 ── */
static gfanc_config_t cfg = GFANC_CONFIG_DEFAULT;

/* ══════════════════════════════════════════════════════════ */
typedef struct {
    /* ANC 模块 */
    scene_ctrl_t  sc;
    fxnlms_mimo_t fx;
    fir_filter_t  bp_fir;        /* ref 带通 */
    fir_filter_t  bp_err[E];     /* err 带通 */
    fir_filter_t *sec_firs;      /* [E*S] 次级路径 */
    float        *sec_coeffs;

    /* 反馈抵消 (逐扬声器独立 FIR, F-G修复) */
    fir_filter_t     fb_fir[2];      /* [spk] 扬声器→参考麦反馈路径 FIR */
    float            fb_coeffs_buf[2][FB_LEN];
    int              fb_active;      /* 已加载的扬声器数 (0/1/2) */

    /* 啸叫检测 */
    howling_detect_t hw;           /* DFT 频谱检测 + IIR 陷波 */

    /* §6.2 跨线程: 影子缓冲, 主线程不直接写/读 fx.wc */
    float  wc_shadow[S*L];      /* 主线程写→回调应用 (Wc更新) */
    float  wc_snapshot[S*L];    /* 回调每帧写→主线程读 (诊断/快照/切换) */
    volatile LONG wc_seq;       /* 主线程写完递增+2 */
    LONG   wc_seq_last;         /* 回调上次看到的序号 */

    /* 跨回调状态 */
    float  wc_old[S*L], wc_cur[S*L];
    float  anti_spk_prev[S]; /* 上一回调末anti值, 反馈抵消跨回调连续性 */
    volatile LONG fade_cnt;
    volatile LONG ramp_cnt;  /* 输出渐变: RAMP_SAMPLES → 0, anti_out 0→1 */
    volatile LONG mute_hold; /* safety_mute 抑制: MUTE_HOLD_SAMPLES → 0, 覆盖首次 RMS */
    /* CNN 双缓冲: 回调填一块→原子标记就绪→切到另一块, 无数据竞争零样本丢失 */
    float  cnn_buf[2][FS_ANC];
    int    cnn_fill_idx;      /* 当前填充块 (0/1), 仅回调访问 */
    int    cnn_cnt;           /* 当前填充块已写入样本数, 仅回调访问 */
    volatile LONG cnn_buf_ready; /* -1=无就绪, 0/1=该块已满待主线程处理 */
    int    first_sec;

    /* 每场景记忆: 保存已收敛的 Wc, 下次切回时直接恢复 */
    float  scene_wc[SC_K_MAX][S*L];
    int    scene_wc_valid[SC_K_MAX];  /* 1=该场景已有收敛好的 Wc */
    int    cur_scene_id;
    int    converged_frames;      /* 连续正常帧数 (判断已收敛) */
    float  anchor_probs[SC_K_MAX];    /* 进入当前场景时的probs锚点 (S-1修复) */
    int    freeze_timer;          /* Wc freeze 计时器 (秒), >0=冻结中, 60s后尝试解冻 */
    int    freeze_permanent;      /* 解冻后3s内再次触发 → 永久冻结直到场景切换 */
    int    peak_hold_cnt;         /* anti峰值连续超限计数 (快检测safety_mute, 10样本=0.6ms触发) */
    volatile int peak_mute;       /* 峰值快检测触发静音 */
    int      nan_in_cnt;        /* NaN/Inf 输入样本累计 (R-8 看门狗) */
    int      nan_out_hold;      /* 连续 NaN anti 样本计数, >FS_ANC 触发 FIR 复位 */
    FILE  *log_file;              /* 运行时统计日志 (C1: NR/场景切换/发散事件) */

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
    volatile LONG  callback_count;
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
        ctx->ref_buf[n] = in[(n*3)*4 + 0];  /* 最近邻抽取: 每3个取1个 */
        ctx->err_buf[n*3+0] = in[(n*3)*4 + 1];
        ctx->err_buf[n*3+1] = in[(n*3)*4 + 2];
        ctx->err_buf[n*3+2] = in[(n*3)*4 + 3];
    }

    /* ── ANC @ 16kHz ── */
    float anti_spk[S];
    /* §6.2: 检查主线程是否提交了新 Wc → 原子应用 */
    {   LONG seq = ctx->wc_seq;
        if (seq != ctx->wc_seq_last) {
            memcpy(ctx->fx.wc, ctx->wc_shadow, S*L*sizeof(float));
            ctx->wc_seq_last = seq;
        }
    }

    anti_spk[0] = ctx->anti_spk_prev[0];  /* 跨回调连续性, 首个样本用上一轮回调的末值 */
    anti_spk[1] = ctx->anti_spk_prev[1];
    for (int n = 0; n < c16k; n++) {
        float ref_raw     = ctx->ref_buf[n];             /* 原始电平 (用于 RMS 显示) */
        /* R-8: 输入 isfinite 防护 — 驱动毛刺/热插拔 NaN 进入延迟线则永久中毒 */
        if (!isfinite(ref_raw)) { ref_raw = 0.0f; ctx->nan_in_cnt++; }

        /* 反馈抵消: 估计扬声器→参考麦的反馈分量并减去 */
        float fb_est = 0;
        if (ctx->fb_active) {
            fb_est = 0;
            for (int s = 0; s < 2; s++)
                if (ctx->fb_fir[s].coeffs)
                    fb_est += fir_tick(&ctx->fb_fir[s], anti_spk[s]);
        }
        float ref_sample  = (ref_raw - fb_est) * cfg.mic_pre_gain;
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
            float a = (float)ctx->fade_cnt / cfg.fade_len;
            for (int i = 0; i < S*L; i++)
                ctx->fx.wc[i] = a * ctx->wc_old[i] + (1.0f - a) * ctx->wc_cur[i];
            if (InterlockedDecrement(&ctx->fade_cnt) == 0)
                memcpy(ctx->fx.wc, ctx->wc_cur, S*L*sizeof(float));
        }

        /* Fx = Sec ⊗ ref_filt */
        float Fx_arr[E*S];
        for (int e = 0; e < E; e++)
            for (int s = 0; s < S; s++)
                Fx_arr[e*S+s] = fir_tick(&ctx->sec_firs[e*S+s], ref_filt);

        /* 扰动 = bp(mic) × 预增益 (含软限幅) — 实测误差, 直接驱动梯度 */
        float err_meas[E];
        for (int e = 0; e < E; e++) {
            float es = ctx->err_buf[n*3+e];
            if (!isfinite(es)) { es = 0.0f; ctx->nan_in_cnt++; }
            es *= cfg.mic_pre_gain;
            if      (es >  MIC_CLIP_MAX) es =  tanhf(es);
            else if (es < -MIC_CLIP_MAX) es = -tanhf(-es);
            err_meas[e] = fir_tick(&ctx->bp_err[e], es);
        }

        /* FxNLMS 实时路径: anti=Wc⊗ref, 梯度用err_meas直接驱动 (不合成err)
           R-6: 静音/peak_mute/fade 期间冻结梯度, 防止开环空转污染 Wc */
        if (ctx->fade_cnt > 0 || ctx->safety_mute || ctx->peak_mute)
            fxnlms_forward_rt(&ctx->fx, ref_filt, Fx_arr, err_meas, anti_spk);
        else
            fxnlms_tick_rt(&ctx->fx, ref_filt, Fx_arr, err_meas, anti_spk);

        /* R-8: NaN/Inf 保护 + 输出钳位 + 看门狗
           驱动毛刺 → NaN 进入 FIR 延迟线 → 永久 NaN 输出 (延迟线无自恢复能力)
           看门狗: 连续 >1s NaN 输出 → 复位全部 FIR 延迟线 (memset+指针归零, <10μs) */
        int nan_anti = 0;
        for (int s = 0; s < S; s++) {
            if (!isfinite(anti_spk[s])) { anti_spk[s] = 0.0f; nan_anti = 1; }
            if (anti_spk[s] > 1.0f) anti_spk[s] = 1.0f;
            if (anti_spk[s] < -1.0f) anti_spk[s] = -1.0f;
        }
        if (nan_anti) {
            ctx->nan_out_hold++;
            if (ctx->nan_out_hold > FS_ANC) {
                fir_reset(&ctx->bp_fir);
                for (int e = 0; e < E; e++) fir_reset(&ctx->bp_err[e]);
                for (int i = 0; i < E*S; i++) fir_reset(&ctx->sec_firs[i]);
                for (int s = 0; s < 2; s++)
                    if (ctx->fb_fir[s].coeffs) fir_reset(&ctx->fb_fir[s]);
                ctx->nan_out_hold = 0;
                ctx->nan_in_cnt = -(ctx->nan_in_cnt + 1);  /* 负哨兵通知主线程 */
            }
        } else {
            ctx->nan_out_hold = 0;
        }

        /* 峰值快检测: 连续10样本|anti|>0.95 → 立即静音 (CR-12, 0.6ms响应 vs 原1秒) */
        {   int peak = 0;
            for (int s = 0; s < S; s++)
                if (fabsf(anti_spk[s]) > 0.95f) peak = 1;
            if (peak) {
                if (++ctx->peak_hold_cnt >= 10) ctx->peak_mute = 1;
            } else {
                ctx->peak_hold_cnt = 0;
                ctx->peak_mute = 0;  /* 峰值恢复, 解除快检测静音 */
            }
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
                                && ctx->mute_hold <= 0);  /* 冷启动期间抑制, 由 ramp 接管 */
            ctx->acc_ref = ctx->acc_err = ctx->acc_dist = ctx->acc_fb = 0;
            ctx->acc_anti = ctx->acc_anti_est = 0; ctx->acc_cnt = 0;
            for (int c = 0; c < 4; c++) ctx->acc_ch[c] = 0;
        }

        /* 安全静音时输出零 (冷启动抑制期内不触发). peak_mute=快检测, safety_mute=慢检测 */
        if (ctx->safety_mute || ctx->peak_mute) { anti_spk[0] = 0; anti_spk[1] = 0; }

        /* 冷启动 ramp: INIT/RESET 后 anti_out 从 0 平滑渐入 */
        if (ctx->ramp_cnt > 0) {
            float ramp = 1.0f - (float)ctx->ramp_cnt / (FS_ANC * cfg.ramp_ms / 1000);
            anti_spk[0] *= ramp;
            anti_spk[1] *= ramp;
            InterlockedDecrement(&ctx->ramp_cnt);
        }

        /* safety_mute 抑制计数 (独立于 ramp, 覆盖到下一次有效 RMS 评估) */
        if (ctx->mute_hold > 0) InterlockedDecrement(&ctx->mute_hold);

        /* 累积实际输出功率 (mute/ramp之后, 反映真实扬声器输出) */
        ctx->acc_anti += anti_spk[0] * anti_spk[0] + anti_spk[1] * anti_spk[1];

        ctx->anti_buf[n] = anti_spk[0];
        ctx->anti_buf[n + c16k] = anti_spk[1];
    }
    ctx->anti_spk_prev[0] = anti_spk[0];  /* 保存末值, 供下一回调首样本反馈抵消 */
    ctx->anti_spk_prev[1] = anti_spk[1];

    /* 快照 fx.wc → wc_snapshot: 主线程安全读取 (ARM float原子, 无撕裂) */
    memcpy(ctx->wc_snapshot, ctx->fx.wc, S*L*sizeof(float));

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

    InterlockedIncrement(&ctx->callback_count);
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

/* ── 主循环辅助函数 (CR-4: 从 ~110 行 while 块拆分) ── */

static void print_diagnostics(rt_ctx_t *ctx, int new_scene, float cos_sim,
                              const float *probs) {
    printf("[CNN] s=%d max=%.2f cos=%.2f NR=%.1fdB anti=%.4f%s%s gain=%.0fx cb=%d\n",
           new_scene, probs[new_scene], cos_sim,
           ctx->nr_level, ctx->anti_rms,
           ctx->safety_mute ? " [MUTE]" : "",
           ctx->ramp_cnt > 0 ? " [RAMP]" : "",
           cfg.mic_pre_gain, ctx->callback_count);
    printf("       raw: ch0(ref)=%.4f ch1=%.4f ch2=%.4f ch3=%.4f (refFilt=%.4f gain=%.1f)\n",
           ctx->ch_rms[0], ctx->ch_rms[1], ctx->ch_rms[2], ctx->ch_rms[3],
           ctx->ref_rms, cfg.mic_pre_gain);
    printf("       ANC: err=%.4f antiEst=%.4f  (err=实测残差, antiEst=模型估计反噪声)\n",
           ctx->err_rms, ctx->anti_est_rms);
    if (ctx->fb_active)
        printf("       FB:  est=%.4f (反馈抵消量 RMS)\n", ctx->fb_rms);
    if (ctx->hw.active_count > 0 || ctx->hw.dominant_db > HW_THRESH_DB * 0.7f)
        printf("       HW:  f=%.0fHz peak=%.1fdB notches=%d%s\n",
               ctx->hw.dominant_freq, ctx->hw.dominant_db,
               ctx->hw.active_count,
               ctx->hw.active_count > 0 ? " [NOTCH]" : "");
}

static void check_wc_divergence(rt_ctx_t *ctx) {
    float wc_max = 0;
    for (int i = 0; i < S*L; i++) {
        float a = fabsf(ctx->wc_snapshot[i]);
        if (a > wc_max) wc_max = a;
    }
    if (wc_max > ctx->sc.stub_rms * cfg.freeze_ratio) {
        if (!ctx->fx.freeze_lms && !ctx->freeze_permanent) {
            InterlockedExchange(&ctx->fx.freeze_lms, 1);
            ctx->freeze_timer = cfg.freeze_retry_sec;
            if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc diverged max=%.3f stub=%.3f\n",
                                       wc_max, ctx->sc.stub_rms);
            printf("[WARN] Wc diverged! max|Wc|=%.3f "
                   "> %.0f×stub(%.3f), LMS frozen (%ds retry)\n",
                   wc_max, cfg.freeze_ratio, ctx->sc.stub_rms, cfg.freeze_retry_sec);
        }
    } else if (ctx->fx.freeze_lms && ctx->freeze_timer > 0 && !ctx->freeze_permanent) {
        ctx->freeze_timer--;
        if (ctx->freeze_timer <= 0) {
            /* R-7: 解冻前回滚到已知良好 Wc — 以发散 Wc 解冻则注定永久冻结 */
            if (ctx->scene_wc_valid[ctx->cur_scene_id]) {
                memcpy(ctx->wc_shadow, ctx->scene_wc[ctx->cur_scene_id], S*L*sizeof(float));
                printf("[INFO] Wc freeze retry — rollback to scene_wc[%d]\n", ctx->cur_scene_id);
            } else {
                scene_ctrl_construct_wc(&ctx->sc, ctx->cur_scene_id, ctx->wc_shadow);
                printf("[INFO] Wc freeze retry — rollback to CNN preset scene=%d\n", ctx->cur_scene_id);
            }
            InterlockedExchangeAdd(&ctx->wc_seq, 2);
            InterlockedExchange(&ctx->fx.freeze_lms, 0);
            ctx->freeze_timer = -3;
            if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc unfreeze retry (rolled back)\n");
            printf("[INFO] Wc unfrozen, watching 3s...\n");
        }
    } else if (!ctx->fx.freeze_lms && ctx->freeze_timer < 0) {
        ctx->freeze_timer++;
        if (ctx->freeze_timer >= 0)
            printf("[INFO] Wc stable after unfreeze, normal operation resumed\n");
    }
    if (ctx->freeze_timer < 0 && wc_max > ctx->sc.stub_rms * cfg.freeze_ratio) {
        InterlockedExchange(&ctx->fx.freeze_lms, 1);
        ctx->freeze_permanent = 1;
        ctx->freeze_timer = 0;
        if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc freeze PERMANENT\n");
        printf("[WARN] Wc diverged again during watch period! "
               "LMS permanently frozen until scene switch\n");
    }
}

static void check_convergence(rt_ctx_t *ctx) {
    if (ctx->nr_level > 3.0f && !ctx->safety_mute) {
        ctx->converged_frames++;
        if (ctx->converged_frames >= 3) {
            memcpy(ctx->scene_wc[ctx->cur_scene_id], ctx->wc_snapshot, S*L*sizeof(float));
            ctx->scene_wc_valid[ctx->cur_scene_id] = 1;
            ctx->converged_frames = 0;
        }
    } else {
        ctx->converged_frames = 0;
    }
}

static void check_scene_switch(rt_ctx_t *ctx, int new_scene,
                               float cos_sim, float *probs) {
    if (cos_sim >= 0.8f || new_scene == ctx->cur_scene_id) return;

    if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: scene switch %d→%d cos=%.3f restored=%d\n",
                               ctx->cur_scene_id, new_scene, cos_sim,
                               ctx->scene_wc_valid[new_scene]);

    /* 保存旧场景的当前 Wc */
    memcpy(ctx->scene_wc[ctx->cur_scene_id], ctx->wc_snapshot, S*L*sizeof(float));
    ctx->scene_wc_valid[ctx->cur_scene_id] = 1;

    /* 新场景: 有记忆就用记忆, 否则用 CNN 通用 Wc */
    if (ctx->scene_wc_valid[new_scene]) {
        memcpy(ctx->wc_cur, ctx->scene_wc[new_scene], S*L*sizeof(float));
        printf("  -> RESET s%d→s%d (restored adapted Wc)\n",
               ctx->cur_scene_id, new_scene);
    } else {
        ctx->scene_wc_valid[new_scene] = 1;
        printf("  -> RESET s%d→s%d (new scene, CNN preset)\n",
               ctx->cur_scene_id, new_scene);
    }

    memcpy(ctx->wc_old, ctx->wc_snapshot, S*L*sizeof(float));
    InterlockedExchange(&ctx->fx.freeze_lms, 0);
    ctx->freeze_timer = 0; ctx->freeze_permanent = 0;
    memcpy(ctx->anchor_probs, probs, ctx->sc.K * sizeof(float));
    InterlockedExchange(&ctx->fade_cnt, cfg.fade_len);
    InterlockedExchange(&ctx->mute_hold, (FS_ANC * cfg.mute_hold_ms / 1000));
    ctx->cur_scene_id = new_scene;
    ctx->converged_frames = 0;
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    gfanc_config_load_env(&cfg);
    LOG_INFO("Runtime config: gain=%.1f step=%.6f leak=%.0e ramp=%dms mute=%dms",
             cfg.mic_pre_gain, cfg.step_size, cfg.leak, cfg.ramp_ms, cfg.mute_hold_ms);
    if (pa_init() != 0) return 1;
    p_Pa_Initialize();

    /* 列出设备 (跳过 MME/DirectSound, 只显示 ASIO/WASAPI/WDM-KS) */
    int nd = p_Pa_GetDeviceCount();
    int napi = 0;
    printf("\n=== Audio Devices ===\n");
    for (int api_idx = 0; ; api_idx++) {
        const PaHostApiInfo2 *api = (const PaHostApiInfo2 *)p_Pa_GetHostApiInfo(api_idx);
        if (!api) break;
        if (strstr(api->name, "MME") || strstr(api->name, "DirectSound")) continue;
        napi++;
        int has_dev = 0;
        for (int i = 0; i < nd; i++) {
            const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
            if (info && info->hostApi == api_idx) {
                if (!has_dev) { printf("\n[%s]\n", api->name); has_dev = 1; }
                printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
                    i, info->name, info->maxInputChannels,
                    info->maxOutputChannels, info->defaultSampleRate);
            }
        }
    }
    if (napi == 0) { fprintf(stderr, "PA: no host APIs found\n"); return 1; }

    int in_dev, out_dev;
    printf("\nInput device ID: "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID: "); fflush(stdout); scanf("%d", &out_dev);

    printf("\n");

    /* 加载权重 (R-3: 逐文件校验长度, 缺/截断文件 → FATAL 而非崩溃) */
    int ret = 0;
    printf("Loading weights...\n");
    float *sec_path, *sub_filters, *centroids, *bp_coeff;
    int sec_len = bin_load_float("data/secondary_path.bin", &sec_path);
    int sub_len = bin_load_float("data/sub_filters.bin", &sub_filters);
    int n_scene = bin_load_float("data/scene_defs.bin", &centroids);
    int bp_len  = bin_load_float("data/bandpass_fir.bin", &bp_coeff);

    if (sec_len < E*S*SEC_LEN) {
        fprintf(stderr, "FATAL: secondary_path.bin too short/load failed (%d<%d)\n", sec_len, E*S*SEC_LEN);
        ret = 1; goto cleanup;
    }
    if (sub_len < SC_C*SC_S || sub_len % (SC_C*SC_S) != 0) {
        fprintf(stderr, "FATAL: sub_filters.bin invalid size %d (expect multiple of %d)\n", sub_len, SC_C*SC_S);
        ret = 1; goto cleanup;
    }
    if (n_scene < SC_S*SC_C) {
        fprintf(stderr, "FATAL: scene_defs.bin too short (%d<%d)\n", n_scene, SC_S*SC_C);
        ret = 1; goto cleanup;
    }
    if (bp_len < BP_LEN) {
        fprintf(stderr, "FATAL: bandpass_fir.bin too short/load failed (%d<%d)\n", bp_len, BP_LEN);
        ret = 1; goto cleanup;
    }
    {
        int L_from_sub = sub_len / (SC_C*SC_S);
        if (L_from_sub < 64 || L_from_sub > 4096) {
            fprintf(stderr, "FATAL: filter length L=%d out of range [64,4096]\n", L_from_sub);
            ret = 1; goto cleanup;
        }
        if (L_from_sub != L) {
            fprintf(stderr, "FATAL: sub_filters L=%d mismatches compile-time L=%d\n", L_from_sub, L);
            ret = 1; goto cleanup;
        }
    }
    extern int cnn_m5_init(void); extern int cnn_m5_get_K(void);
    if (cnn_m5_init() != 0) {
        fprintf(stderr, "FATAL: CNN init failed (missing/corrupt cnn_*.bin?)\n");
        ret = 1; goto cleanup;
    }
    printf("  OK K=%d L=%d\n", cnn_m5_get_K(), sub_len / (SC_C*SC_S));

    /* 初始化 ANC 模块 */
    PaStream *stream = NULL;
    rt_ctx_t *ctx = calloc(1, sizeof(rt_ctx_t));  /* 堆分配, 避免 ~211KB 栈压力 */
    if (!ctx) { fprintf(stderr, "OOM: rt_ctx_t\n"); ret = 1; goto cleanup; }
    ctx->cnn_buf_ready = -1;  /* -1=无就绪块, 0/1=该块已满 */
    g_ctx = ctx;
    ctx->running = 1; ctx->first_sec = 1;
    InterlockedExchange(&ctx->mute_hold, (FS_ANC * cfg.mute_hold_ms / 1000));  /* 启动抑制 safety_mute */

    ctx->bp_fir.coeffs = bp_coeff; ctx->bp_fir.n_taps = BP_LEN;
    ctx->bp_fir.delay_line = (double *)calloc(BP_LEN, sizeof(double));
    if (!ctx->bp_fir.delay_line) { fprintf(stderr, "OOM: bp_fir\n"); ret = 1; goto cleanup; }
    for (int e = 0; e < E; e++) {
        ctx->bp_err[e].coeffs = bp_coeff; ctx->bp_err[e].n_taps = BP_LEN;
        ctx->bp_err[e].delay_line = (double *)calloc(BP_LEN, sizeof(double));
        if (!ctx->bp_err[e].delay_line) { fprintf(stderr, "OOM: bp_err[%d]\n", e); ret = 1; goto cleanup; }
    }

    int sp = SEC_LEN + DSP_DELAY;
    ctx->sec_firs = (fir_filter_t *)calloc(E*S, sizeof(fir_filter_t));
    ctx->sec_coeffs = (float *)calloc(E*S*sp, sizeof(float));
    if (!ctx->sec_firs || !ctx->sec_coeffs) {
        fprintf(stderr, "OOM: sec_firs/coeffs\n"); ret = 1; goto cleanup;
    }
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e*S+s;
            memcpy(ctx->sec_coeffs + idx*sp + DSP_DELAY, sec_path + idx*SEC_LEN, SEC_LEN*sizeof(float));
            ctx->sec_firs[idx].coeffs = ctx->sec_coeffs + idx*sp;
            ctx->sec_firs[idx].n_taps = sp;
            ctx->sec_firs[idx].delay_line = (double *)calloc(sp, sizeof(double));
            if (!ctx->sec_firs[idx].delay_line) {
                fprintf(stderr, "OOM: sec_firs[%d]\n", idx); ret = 1; goto cleanup;
            }
        }

    /* 反馈抵消: 逐扬声器加载 FIR (需先运行 calibrate_feedback.exe, F-G修复) */
#if FB_ENABLED
    {
        int loaded = 0;
        for (int spk = 0; spk < 2; spk++) {
            char fname[64];
            snprintf(fname, sizeof(fname), "data/feedback_path_%d.bin", spk);
            float *fb_raw = NULL;
            int fb_len = bin_load_float(fname, &fb_raw);
            if (fb_len > 0 && fb_raw) {
                int n = fb_len < FB_LEN ? fb_len : FB_LEN;
                memcpy(ctx->fb_coeffs_buf[spk], fb_raw, n * sizeof(float));
                ctx->fb_fir[spk].coeffs    = ctx->fb_coeffs_buf[spk];
                ctx->fb_fir[spk].n_taps    = FB_LEN;
                ctx->fb_fir[spk].delay_line = (double *)calloc(FB_LEN, sizeof(double));
                ctx->fb_fir[spk].ptr       = 0;
                float fb_rms = 0;
                for (int i = 0; i < FB_LEN; i++) fb_rms += ctx->fb_coeffs_buf[spk][i] * ctx->fb_coeffs_buf[spk][i];
                printf("  Feedback spk%d: %d taps, RMS=%.4f\n", spk, FB_LEN, sqrtf(fb_rms / FB_LEN));
                bin_free(fb_raw); loaded++;
            }
        }
        ctx->fb_active = loaded;
        if (loaded == 0)
            printf("  Feedback cancel: disabled (run calibrate_feedback.exe first)\n");
    }
#endif

    if (scene_ctrl_init(&ctx->sc, centroids, sub_filters, L, n_scene) != 0) {
        fprintf(stderr, "ERROR: scene_ctrl_init OOM\n"); ret = 1; goto cleanup;
    }
    /* R-4: CNN K vs scene_defs K 交叉校验 — 防止不同批次 data/ 混配 */
    if (cnn_m5_get_K() != ctx->sc.K) {
        fprintf(stderr, "FATAL: CNN K=%d != scene_defs K=%d (data/ batch mix-up?)\n",
                cnn_m5_get_K(), ctx->sc.K);
        ret = 1; goto cleanup;
    }
    howling_init(&ctx->hw, HOWLING_ENABLED);
    if (fxnlms_init(&ctx->fx, E, S, L, cfg.step_size, cfg.leak) != 0) {
        fprintf(stderr, "ERROR: fxnlms_init OOM\n"); ret = 1; goto cleanup;
    }

    /* 缓冲 */
    ctx->ref_buf = (float *)malloc(FS_HW * sizeof(float));
    ctx->anti_buf = (float *)malloc(FS_HW * S * sizeof(float));
    ctx->err_buf = (float *)malloc(FS_HW * E * sizeof(float));
    if (!ctx->ref_buf || !ctx->anti_buf || !ctx->err_buf) {
        fprintf(stderr, "OOM: buffers\n"); ret = 1; goto cleanup;
    }
    printf("  ANC ready: E=%d S=%d L=%d\n", E, S, L);

    /* 打开 PortAudio 流 */
    PaStreamParams in_p = { in_dev, 4, 0x00000001, 0.01, NULL };  /* paFloat32 */
    PaStreamParams out_p = { out_dev, 2, 0x00000001, 0.01, NULL };
    stream = NULL;
    int err = p_Pa_OpenStream(&stream, &in_p, &out_p, 48000, 96, 0, (void*)audio_cb, ctx);
    if (err != 0) {
        fprintf(stderr, "PA open error: %s\n", p_Pa_GetErrorText(err));
        ret = 1; goto cleanup;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    printf("\n══════════════════════════════════════════\n");
    printf("  GFANC FxNLMS — Realtime ANC\n");
    printf("  Ctrl+C to stop\n");
    printf("══════════════════════════════════════════\n\n");

    p_Pa_StartStream(stream);
    printf("Running...\n");

    /* 运行时统计日志 (C1) */
    ctx->log_file = fopen("gfanc_log.csv", "a");
    if (ctx->log_file) {
        time_t now = time(NULL);
        fprintf(ctx->log_file, "# GFANC session start: %s", ctime(&now));
        fprintf(ctx->log_file, "# sec,scene,max_prob,cos_sim,NR_dB,err_rms,anti_rms,ref_rms,event\n");
        fflush(ctx->log_file);
    }

    /* 主循环: CNN 场景分类 1Hz, 驱动 Wc 更新和场景切换 */
    int log_sec = 0;
    while (ctx->running) {
        Sleep(100);
        LONG ready = InterlockedExchange(&ctx->cnn_buf_ready, -1);
        if (ready < 0) continue;

        float probs[SC_K_MAX] = {0};
        int new_scene;
        const int K = ctx->sc.K;

        /* CrossFader期间跳过CNN: 回调正在读wc_cur做混合, 不能覆盖 */
        if (ctx->fade_cnt > 0) {
            memcpy(probs, ctx->sc.prev_probs, K * sizeof(float));
            new_scene = ctx->cur_scene_id;
        } else {
            new_scene = scene_ctrl_process(&ctx->sc, ctx->cnn_buf[ready], ctx->wc_cur, probs);
        }

        if (ctx->first_sec) {
            /* 首次 INIT: CNN 通用 Wc → 标记场景 → 冷启动 ramp */
            /* 通过影子缓冲提交 Wc (主线程→回调, 零数据竞争) */
            memcpy(ctx->wc_shadow, ctx->wc_cur, S*L*sizeof(float));
            InterlockedExchangeAdd(&ctx->wc_seq, 2);
            InterlockedExchange(&ctx->fx.freeze_lms, 0);
            ctx->freeze_timer = 0; ctx->freeze_permanent = 0;
            memcpy(ctx->scene_wc[new_scene], ctx->wc_cur, S*L*sizeof(float));
            ctx->scene_wc_valid[new_scene] = 1;
            ctx->cur_scene_id = new_scene;
            memcpy(ctx->anchor_probs, probs, K * sizeof(float));
            InterlockedExchange(&ctx->ramp_cnt, (FS_ANC * cfg.ramp_ms / 1000));
            InterlockedExchange(&ctx->mute_hold, (FS_ANC * cfg.mute_hold_ms / 1000));
            printf("[CNN] INIT scene=%d max=%.2f (ramp %dms, mute_hold %dms)\n",
                   new_scene, probs[new_scene], cfg.ramp_ms, cfg.mute_hold_ms);
            ctx->first_sec = 0;
        } else {
            /* S-1修复: cos(anchor, cur) 替代 cos(prev, cur) */
            float dot = 0, np = 0, nc = 0;
            for (int k = 0; k < K; k++) {
                dot += ctx->anchor_probs[k] * probs[k];
                np  += ctx->anchor_probs[k] * ctx->anchor_probs[k];
                nc  += probs[k] * probs[k];
            }
            float cos_sim = dot / (sqrtf(np)*sqrtf(nc) + 1e-10f);

            print_diagnostics(ctx, new_scene, cos_sim, probs);

            /* 运行时统计日志 (C1) */
            if (ctx->log_file) {
                fprintf(ctx->log_file, "%d,%d,%.3f,%.3f,%.1f,%.4f,%.4f,%.4f,%s%s\n",
                        log_sec++, new_scene, probs[new_scene], cos_sim,
                        ctx->nr_level, ctx->err_rms, ctx->anti_rms, ctx->ref_rms,
                        ctx->safety_mute ? "MUTE" : "",
                        ctx->fx.freeze_lms ? (ctx->freeze_permanent ? "FREEZE_PERM" : "FREEZE") : "");
                fflush(ctx->log_file);
            }

            /* R-8: NaN 看门狗日志 — 负哨兵值表示回调已触发 FIR 复位 */
            if (ctx->nan_in_cnt < 0) {
                int total = -(ctx->nan_in_cnt + 1);
                fprintf(stderr, "[WATCHDOG] t=%ds NaN persist >1s → FIR delay lines reset, nan_in=%d\n",
                        log_sec, total);
                if (ctx->log_file)
                    fprintf(ctx->log_file, "# EVENT: WATCHDOG NaN FIR reset nan_in=%d\n", total);
                ctx->nan_in_cnt = 0;
            }

            check_wc_divergence(ctx);
            check_convergence(ctx);
            check_scene_switch(ctx, new_scene, cos_sim, probs);
        }
        memcpy(ctx->sc.prev_probs, probs, K * sizeof(float));
    }

    printf("\nStopping...\n");
    p_Pa_StopStream(stream);
    p_Pa_CloseStream(stream);

cleanup:
    if (ctx) {
        FILE *lf = ctx->log_file;  /* R-10: free(ctx) 前取出, 避免 use-after-free */
        free(ctx->bp_fir.delay_line);
        for (int e = 0; e < E; e++) free(ctx->bp_err[e].delay_line);
        if (ctx->sec_firs) {
            for (int i = 0; i < E*S; i++) free(ctx->sec_firs[i].delay_line);
            free(ctx->sec_firs);
        }
        free(ctx->sec_coeffs);
        for (int s = 0; s < 2; s++) free(ctx->fb_fir[s].delay_line);
        fxnlms_free(&ctx->fx);
        free(ctx->ref_buf); free(ctx->anti_buf); free(ctx->err_buf);
        free(ctx);
        ctx = NULL;
        if (lf) {
            time_t now = time(NULL);
            fprintf(lf, "# GFANC session end: %s\n", ctime(&now));
            fclose(lf);
        }
    }
    p_Pa_Terminate();
    if (ret == 0) printf("Done.\n");
    return ret;
}
