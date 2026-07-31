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

#include "os_port.h"        /* R-28: gf_sleep_ms + gf_log_timestamp */
#include "fir_filter.h"
#include "binary_loader.h"
#include "cnn_m5_forward.h"
#include "scene_controller.h"
#include "scene_manager.h"
#include "fxnlms_mimo.h"
#include "howling_detect.h"

/* R-14: 2阶 Butterworth 低通 biquad (48kHz侧抗混叠, fc≈6kHz) */
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

/* ══════════════════════════════════════════════════════════ */
#define FS_HW    48000
#define FS_ANC   16000
#define E        3
#define S        2
#define L        1024
#define BP_LEN       1024   /* CNN 带通 (分类需频率分辨率) */
#define BP_ANC_LEN   256    /* R-13: ANC 带通 (群延迟 8ms vs 32ms, 宽带NR+3-5dB) */
#define SEC_LEN      1024
/* DSP_DELAY 由 cfg.dsp_delay 管理, 默认 16, 环境变量 GFANC_DSP_DELAY 可覆盖 */
/* R-9: 以下参数统一由 cfg (gfanc_config_t) 管理, env 变量可直接生效
   GFANC_RAMP_MS / GFANC_MUTE_MS / GFANC_FADE_LEN / GFANC_MIC_GAIN */
#define MIC_CLIP_MAX  1.0f    /* 输入软限幅 (防止吹气/大声压冲爆 FIR) */
#define FB_LEN       256     /* 反馈路径 FIR 长度 */
#define FB_ENABLED   1       /* 反馈抵消开关 (需先运行 calibrate_feedback.exe) */
#define HOWLING_ENABLED 1    /* 啸叫检测 + 陷波 (DFT 频谱峰值检测) */

/* ── 集中参数 (A2): 单一配置入口, 环境变量可覆盖 ── */
static gfanc_config_t cfg = GFANC_CONFIG_DEFAULT;

/* ── 算法常数 (非用户调节, 表达物理/设计约束) ── */
#define WC_MUTE_DECAY   4e-5f   /* 静音期间 Wc 逐样本衰减因子 (半衰期~0.25s @16kHz) */
#define OUT_GAIN_SLEW   0.004f  /* 输出增益包络 EMA 系数 (~4ms 时间常数) */

/* ══════════════════════════════════════════════════════════ */
typedef struct {
    /* ANC 模块 */
    scene_ctrl_t  sc;
    fxnlms_mimo_t fx;
    fir_filter_t  bp_fir;        /* ref 带通 CNN (1024tap, 分类用) */
    fir_filter_t  bp_fir_anc;    /* R-13: ref 带通 ANC (256tap, 群延迟8ms) */
    fir_filter_t  bp_err[E];     /* err 带通 ANC (256tap) */
    fir_filter_t *sec_firs;      /* [E*S] 次级路径 */
    float        *sec_coeffs;
    float        *bp_anc_coeffs; /* R-13: ANC 带通系数 (256tap, 与 CNN 1024tap 独立) */

    /* 反馈抵消 (逐扬声器独立 FIR, F-G修复) */
    fir_filter_t     fb_fir[GFANC_S_MAX];      /* [spk] 扬声器→参考麦反馈路径 FIR */
    float            fb_coeffs_buf[GFANC_S_MAX][FB_LEN];
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
    int    peak_release_cnt;      /* peak_mute 释放迟滞: 连续低于阈值的样本数 (10ms 防抖) */
    volatile int peak_rollback_cnt; /* peak_mute 上升沿 Wc 减半次数 (主线程显示) */
    float  out_gain;              /* 静音包络 0..1 (slew~4ms, 替代硬切零, 消除开关咔哒声) */
    int    diverge_sec;           /* anti_rms 连续超限秒数 (≥3s 触发 Wc 救援) */
    volatile int diverged;        /* 1=当前处于发散态 (pa>>pe 且 anti 大), NR 显示 DIV */
    int      nan_in_cnt;        /* NaN/Inf 输入样本累计 (R-8 看门狗) */
    int      nan_out_hold;      /* 连续 NaN anti 样本计数, >FS_ANC 触发 FIR 复位 */
    int      cnn_drop_cnt;      /* R-20: CNN 缓冲覆盖计数 (主线程超1s未消费) */
    biquad_t aa_filt[4];        /* R-14: 48k输入抗混叠低通 (ch0=ref, ch1-3=err, fc≈6kHz) */
    FILE  *log_file;              /* 运行时统计日志 (C1: NR/场景切换/发散事件) */

    /* 48k 重采样缓冲 */
    float *ref_buf, *anti_buf, *err_buf; /* 堆分配 (main初始化), 存16k速率数据, 名_48k为历史遗留 */
    int    dec_phase;       /* 输入 3:1 抽取相位 (0/1/2) */
    int    out_phase;       /* R-15: 输出 1:3 内插相位 (0/1/2) */
    volatile float nr_level, ref_rms, err_rms, dist_rms;
    volatile float ch_rms[4];    /* 原始声道 RMS: ch0=ref, ch1-3=err */
    volatile float anti_rms;     /* 反噪声 RMS */
    volatile float acc_ref, acc_err, acc_dist, acc_fb;
    volatile float acc_ch[4], acc_anti, acc_anti_est;
    volatile float acc_d_est;    /* 扰动估计功率 Σ(err-anti_est)² (诚实NR分子) */
    volatile float acc_err_cross;/* Σ(err × anti_est), Ŝ 校准后重构 d_cal 用 */
    /* s_cal 已移除: Ŝ 保持物理尺度, anti_est 无需去归一化校准 */
    int    anti_est_offset;   /* R-55: anti_est 采样窗口起始偏移 (每帧随机化) */
    float  wc_init_max;           /* INIT 后 Wc 的 max|系数| (freeze 阈值基准) */
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
    int c48k = (int)fcount;
    (void)ti; (void)flags;

    if (!ctx->running) { memset(out, 0, c48k * 2 * sizeof(float)); return 1; }

    /* R-5: 3:1 抽取 + 跨回调相位保持 — WASAPI/WDM-KS 可变帧长不再破坏相位连续性
       R-14: 抽取前 2阶 Butterworth 低通抗混叠 (fc≈6kHz, 防止>8kHz折叠入通带) */
    int c16k = 0;
    {
        int p = ctx->dec_phase;
        for (int i = 0; i < c48k; i++) {
            /* 抗混叠: 全 48k 样本滤波 (每通道 ~5 MAC, 4通道总计 ~1920 MAC/回调 << 0.5% 预算) */
            float ch0 = biquad_tick(&ctx->aa_filt[0], in[i*4 + 0]);
            float ch1 = biquad_tick(&ctx->aa_filt[1], in[i*4 + 1]);
            float ch2 = biquad_tick(&ctx->aa_filt[2], in[i*4 + 2]);
            float ch3 = biquad_tick(&ctx->aa_filt[3], in[i*4 + 3]);
            if (p == 0) {
                ctx->ref_buf[c16k]    = ch0;
                ctx->err_buf[c16k*3+0] = ch1;
                ctx->err_buf[c16k*3+1] = ch2;
                ctx->err_buf[c16k*3+2] = ch3;
                c16k++;
            }
            p = (p == 2) ? 0 : p + 1;
        }
        ctx->dec_phase = p;
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
    float prev_anti_spk[2] = { ctx->anti_spk_prev[0], ctx->anti_spk_prev[1] }; /* R-15: 保存上回调末值供输出内插 */
    for (int n = 0; n < c16k; n++) {
        float ref_raw     = ctx->ref_buf[n];             /* 原始电平 (用于 RMS 显示) */
        /* R-8: 输入 isfinite 防护 — 驱动毛刺/热插拔 NaN 进入延迟线则永久中毒 */
        if (!isfinite(ref_raw)) { ref_raw = 0.0f; ctx->nan_in_cnt++; }

        /* 反馈抵消: 估计扬声器→参考麦的反馈分量并减去 */
        float fb_est = 0;
        if (ctx->fb_active) {
            fb_est = 0;
            for (int s = 0; s < S; s++)
                if (ctx->fb_fir[s].coeffs)
                    fb_est += fir_tick(&ctx->fb_fir[s], anti_spk[s]);
        }
        float ref_sample  = (ref_raw - fb_est) * cfg.mic_pre_gain;
        /* 输入软限幅: tanh 防止吹气/冲击导致 FIR 饱和 → 非线性失真 → 发散 */
        if      (ref_sample >  MIC_CLIP_MAX) ref_sample =  tanhf(ref_sample);
        else if (ref_sample < -MIC_CLIP_MAX) ref_sample = -tanhf(-ref_sample);

        /* 带通滤波 (预增益已应用, 不造成反馈).
           R-13: CNN 和 ANC 使用不同长度的带通滤波器.
           CNN 保留 1024tap (分类需要频率分辨率),
           ANC 使用 256tap (群延迟 32→8ms, 宽带 NR+3-5dB). */
        float ref_cnn = fir_tick(&ctx->bp_fir, ref_sample);
        float ref_anc = fir_tick(&ctx->bp_fir_anc, ref_sample);

        /* CNN 累积 */
        /* CNN 双缓冲: 填满一块→原子标记就绪→切到另一块 */
        if (ctx->cnn_cnt < FS_ANC) {
            ctx->cnn_buf[ctx->cnn_fill_idx][ctx->cnn_cnt++] = ref_cnn;
            if (ctx->cnn_cnt >= FS_ANC) {
                /* R-20: 检测主线程超1s未消费 → 记录丢帧 */
                if (ctx->cnn_buf_ready >= 0) ctx->cnn_drop_cnt++;
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

        /* R-13: Fx = Ŝ ⊗ ref_anc (256tap 带通, 群延迟 8ms) */
        float Fx_arr[E*S];
        for (int e = 0; e < E; e++)
            for (int s = 0; s < S; s++)
                Fx_arr[e*S+s] = fir_tick(&ctx->sec_firs[e*S+s], ref_anc);

        /* 扰动 = bp(mic) × 预增益 (含软限幅) — 实测误差, 直接驱动梯度 */
        float err_meas[E];
        for (int e = 0; e < E; e++) {
            float es = ctx->err_buf[n*E+e];  /* R-29: E hardcoded → E */
            if (!isfinite(es)) { es = 0.0f; ctx->nan_in_cnt++; }
            es *= cfg.mic_pre_gain;
            if      (es >  MIC_CLIP_MAX) es =  tanhf(es);
            else if (es < -MIC_CLIP_MAX) es = -tanhf(-es);
            err_meas[e] = fir_tick(&ctx->bp_err[e], es);
        }

        /* FxNLMS 实时路径: anti=Wc⊗ref_anc, 梯度用err_meas直接驱动 (不合成err)
           R-6: 静音/peak_mute/fade/啸叫陷波活跃时冻结梯度, 防止反馈环路. */
        if (ctx->fade_cnt > 0 || ctx->safety_mute || ctx->peak_mute
            || (HOWLING_ENABLED && ctx->hw.active_count > 0)) {
            fxnlms_forward_rt(&ctx->fx, ref_anc, Fx_arr, err_meas, anti_spk);
            /* 静音/啸叫期间 Wc 持续衰减 — 反馈事件后 Wc 自行退回到安全区 */
            if (ctx->safety_mute || ctx->peak_mute
                || (HOWLING_ENABLED && ctx->hw.active_count > 0)) {
                const float dk = 1.0f - WC_MUTE_DECAY;  /* 半衰期~0.25s */
                for (int i = 0; i < S*L; i++) ctx->fx.wc[i] *= dk;
            }
        } else {
            fxnlms_tick_rt(&ctx->fx, ref_anc, Fx_arr, err_meas, anti_spk);
        }

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
                fir_reset(&ctx->bp_fir_anc);  /* R-13 */
                for (int e = 0; e < E; e++) fir_reset(&ctx->bp_err[e]);
                for (int i = 0; i < E*S; i++) fir_reset(&ctx->sec_firs[i]);
                for (int s = 0; s < S; s++)
                    if (ctx->fb_fir[s].coeffs) fir_reset(&ctx->fb_fir[s]);
                ctx->nan_out_hold = 0;
                ctx->nan_in_cnt = -(ctx->nan_in_cnt + 1);  /* 负哨兵通知主线程 */
            }
        } else {
            ctx->nan_out_hold = 0;
        }

        /* 峰值快检测: 连续10样本|anti|>0.95 → 立即静音 (CR-12, 0.6ms响应 vs 原1秒)
           P0-1 极限环修复:
           a) 触发上升沿 Wc×0.5 — 旧代码冻结 Wc, 解除静音瞬间必然再次饱和,
              形成 0.5~2Hz 硬门控 ("嘟嘟嘟" 的直接来源); 减半后逐次指数收敛
           b) 释放迟滞 160样本(10ms) — 旧代码单样本低于阈值即释放, 防抖 */
        {   int peak = 0;
            for (int s = 0; s < S; s++)
                if (fabsf(anti_spk[s]) > 0.95f) peak = 1;
            if (peak) {
                ctx->peak_release_cnt = 0;
                if (++ctx->peak_hold_cnt >= 10 && !ctx->peak_mute) {
                    ctx->peak_mute = 1;
                    for (int i = 0; i < S*L; i++) ctx->fx.wc[i] *= 0.5f;
                    ctx->wc_init_max *= 0.5f;  /* R-52: 同步收紧 freeze 基准, 防止膨胀逃逸 */
                    ctx->peak_rollback_cnt++;
                }
            } else {
                ctx->peak_hold_cnt = 0;
                if (ctx->peak_mute && ++ctx->peak_release_cnt >= 160)
                    ctx->peak_mute = 0;
            }
        }

        /* R-17: 啸叫检测取三通道瞬时功率最大者, 避免单通道啸叫被平均稀释 ~5dB */
        {
            float err_sel = err_meas[0];
            float best = fabsf(err_meas[0]);
            for (int e = 1; e < E; e++)
                if (fabsf(err_meas[e]) > best) { best = fabsf(err_meas[e]); err_sel = err_meas[e]; }
            howling_tick(&ctx->hw, err_sel, anti_spk, S);
        }

        /* 累积功率: 原始声道 + 滤波参考 + 误差 + 反噪声 */
        ctx->acc_ch[0] += ref_raw * ref_raw;
        for (int e = 0; e < E; e++)
            ctx->acc_ch[1+e] += ctx->err_buf[n*E+e] * ctx->err_buf[n*E+e];
        ctx->acc_ref += ref_cnn * ref_cnn;  /* CNN 带通参考 (诊断用) */
        ctx->acc_fb  += fb_est * fb_est;
        for (int e = 0; e < E; e++) {
            ctx->acc_err  += err_meas[e] * err_meas[e];
            ctx->acc_dist += err_meas[e] * err_meas[e];  /* 实测误差功率 (用于NR参考) */
        }
        /* R-11+R-55: anti_est 每帧随机窗口 250 样本计算 (消除系统性偏差).
           使用 LCG 伪随机偏移, 避免与 1s 边界对齐引入的周期性采样偏差. */
        {   int aoffs = ctx->anti_est_offset;
            if (ctx->acc_cnt >= aoffs && ctx->acc_cnt < aoffs + 250) {
            float anti_est[E]; memset(anti_est, 0, sizeof(anti_est));
            int xp = ctx->fx.xd_ptr;
            int seg1 = (xp == 0) ? L - 1 : xp - 1;
            for (int e = 0; e < E; e++) {
                for (int s = 0; s < S; s++) {
                    float *base = ctx->fx.xd + (e*S+s)*L;
                    float *wc_s = ctx->fx.wc + s*L;
                    float sum = 0; int k = 0;
                    for (int idx = seg1; idx >= 0; idx--, k++)
                        sum += wc_s[k] * base[idx];
                    if (xp > 0)
                        for (int idx = L-1; idx >= xp; idx--, k++)
                            sum += wc_s[k] * base[idx];
                    anti_est[e] += sum;
                }
            }
            for (int e = 0; e < E; e++) {
                ctx->acc_anti_est += anti_est[e] * anti_est[e];
                /* P0-3: 扰动估计 d = err - anti_est (逐样本), 诚实 NR 的分子 */
                float dv = err_meas[e] - anti_est[e];
                ctx->acc_d_est += dv * dv;
                ctx->acc_err_cross += err_meas[e] * anti_est[e];
            }
            } /* R-55: end if(acc_cnt in window) */
        } /* R-55: end outer scope (aoffs) */
        if ((ctx->acc_cnt += 1) >= FS_ANC) {
            float pe = ctx->acc_err;
            float pa = ctx->acc_anti_est * (float)FS_ANC / 250.0f;
            float cross = ctx->acc_err_cross * (float)FS_ANC / 250.0f;
            float pd = pe + pa - 2.0f * cross;               /* d = err - anti_est (Ŝ 物理尺度) */
            ctx->nr_level = 10.0f * log10f((pd + 1e-12f) / (pe + 1e-12f));
            ctx->anti_est_rms = sqrtf(pa / (FS_ANC * E));
            ctx->ref_rms  = sqrtf(ctx->acc_ref  / FS_ANC);
            ctx->err_rms  = sqrtf(pe / (FS_ANC * E));
            ctx->dist_rms = sqrtf(pd / (FS_ANC * E));
            ctx->fb_rms   = sqrtf(ctx->acc_fb   / FS_ANC);
            for (int c = 0; c < 4; c++)
                ctx->ch_rms[c] = sqrtf(ctx->acc_ch[c] / FS_ANC);
            ctx->anti_rms = sqrtf(ctx->acc_anti / (FS_ANC * 2));
            /* R-56: NR<0 确保只有真正恶化时才判定发散 (NR>0 说明仍在降噪,
               pa>>pe 只是小误差导致的高比值, 不是 Wc 膨胀) */
            ctx->diverged = (pa > 9.0f * pe && ctx->anti_rms > 0.05f && ctx->nr_level < 0.0f);
            ctx->safety_mute = (ctx->err_rms > ctx->ref_rms * 2.0f
                                && ctx->ref_rms > 0.001f
                                && ctx->mute_hold <= 0);
            ctx->acc_ref = ctx->acc_err = ctx->acc_dist = ctx->acc_fb = 0;
            ctx->acc_anti = ctx->acc_anti_est = ctx->acc_d_est = ctx->acc_err_cross = 0;
            ctx->acc_cnt = 0;
            /* R-55: LCG 伪随机偏移, 每帧随机化 anti_est 采样窗口位置 [0, 15750] */
            ctx->anti_est_offset = (ctx->anti_est_offset * 1103515245 + 12345) % 15751;
            for (int c = 0; c < 4; c++) ctx->acc_ch[c] = 0;
        }

        /* 安全静音输出包络 (冷启动抑制期内不触发). peak_mute=快检测, safety_mute=慢检测
           P0-1: slew~4ms 平滑替代硬切零 — 消除门控咔哒声; fb 抵消输入=实际播放值 ✓ */
        {
            float target = (ctx->safety_mute || ctx->peak_mute) ? 0.0f : 1.0f;
            ctx->out_gain += (target - ctx->out_gain) * OUT_GAIN_SLEW;
            if (target == 0.0f && ctx->out_gain < 0.002f) ctx->out_gain = 0.0f;
            if (target == 1.0f && ctx->out_gain > 0.998f) ctx->out_gain = 1.0f;
            anti_spk[0] *= ctx->out_gain;
            anti_spk[1] *= ctx->out_gain;
        }

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

    /* R-5+R-15: 相位追踪线性内插 ×3 → 48k — 替代 ZOH, 镜像压制 ~25dB
       状态机保证恰好写入 c48k*2 floats, 跨回调相位连续 */
    {
        int oi = 0, anti_idx = 0;
        int p = ctx->out_phase;  /* R-15: 跨回调输出相位 */
        float a0p = prev_anti_spk[0], a1p = prev_anti_spk[1];  /* 上回调末值 */
        for (; oi < c48k * 2; p = (p == 2) ? 0 : p + 1) {
            float a0, a1;
            if (anti_idx < c16k) {
                float a0c = ctx->anti_buf[anti_idx];
                float a1c = ctx->anti_buf[anti_idx + c16k];
                if (!isfinite(a0c)) a0c = 0.0f;
                if (!isfinite(a1c)) a1c = 0.0f;
                if (a0c > 1.0f) a0c = 1.0f; if (a0c < -1.0f) a0c = -1.0f;
                if (a1c > 1.0f) a1c = 1.0f; if (a1c < -1.0f) a1c = -1.0f;
                float t = (float)p / 3.0f;
                a0 = a0p + t * (a0c - a0p);
                a1 = a1p + t * (a1c - a1p);
                if (p == 2) { a0p = a0c; a1p = a1c; anti_idx++; }
            } else {
                a0 = a0p; a1 = a1p;  /* hold last */
            }
            out[oi++] = a0; out[oi++] = a1;
        }
        ctx->out_phase = p;  /* 保存相位供下个回调 */
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
    char nr_str[20];
    if (ctx->diverged)
        snprintf(nr_str, sizeof(nr_str), "NR=DIV!(振荡)");
    else
        snprintf(nr_str, sizeof(nr_str), "NR=%.1fdB", ctx->nr_level);
    printf("[CNN] s=%d max=%.2f cos=%.2f %s anti=%.4f%s%s%s gain=%.0fx cb=%d%s\n",
           new_scene, probs[new_scene], cos_sim,
           nr_str, ctx->anti_rms,
           ctx->safety_mute ? " [MUTE]" : "",
           ctx->peak_mute ? " [PMUTE]" : "",
           ctx->ramp_cnt > 0 ? " [RAMP]" : "",
           cfg.mic_pre_gain, ctx->callback_count,
           ctx->cnn_drop_cnt > 0 ? " [DROPS]" : "");
    if (ctx->peak_rollback_cnt > 0) {
        printf("       ⚠ peak_mute 触发 %d 次 — Wc 已减半 (输出曾饱和)\n",
               ctx->peak_rollback_cnt);
        ctx->peak_rollback_cnt = 0;
    }
    if (ctx->cnn_drop_cnt > 0) {
        printf("       ⚠ CNN buffer dropped %d frame(s) (main thread >1s blocked?)\n",
               ctx->cnn_drop_cnt);
        ctx->cnn_drop_cnt = 0;
    }
    printf("       raw: ch0(ref)=%.4f ch1=%.4f ch2=%.4f ch3=%.4f (refFilt=%.4f gain=%.1f)\n",
           ctx->ch_rms[0], ctx->ch_rms[1], ctx->ch_rms[2], ctx->ch_rms[3],
           ctx->ref_rms, cfg.mic_pre_gain);
    /* P1: 输入电平诊断 — ref RMS 过低则 ANC 环路增益不足, 受限于 ADC 量化噪声.
       目标 ref_rms ∈ [0.01, 0.1] (-40~-20dBFS). 通过 GFANC_MIC_GAIN 环境变量调节.
       注意: 提高增益前需确保反馈抵消已标定, 否则可能触发啸叫. */
    {
        float ref_dbfs = 20.0f * log10f(ctx->ch_rms[0] + 1e-10f);
        if (ctx->ch_rms[0] < 0.005f && ctx->ch_rms[0] > 1e-8f) {
            float suggested = TARGET_REF_RMS / (ctx->ch_rms[0] + 1e-10f);
            if (suggested > 50.0f) suggested = 50.0f;
            printf("       ⚠ 输入电平过低 ref=%.0fdBFS (目标>-40dBFS), 建议 GFANC_MIC_GAIN=%.0f\n",
                   ref_dbfs, suggested);
        } else if (ctx->ch_rms[0] > 0.3f) {
            printf("       ⚠ 输入电平过高 ref=%.0fdBFS (目标<-10dBFS), 建议降低 GFANC_MIC_GAIN\n",
                   ref_dbfs);
        }
    }
    printf("       ANC: err=%.4f antiEst=%.4f  (err=实测残差, antiEst=模型估计反噪声)\n",
           ctx->err_rms, ctx->anti_est_rms);
    /* R-49: antiEst/anti_rms 比值超 20× 时警告 — Wc 可能膨胀或 Ŝ 模型增益失配,
       此时 NR 读数可能虚高 (pd = Σ(err-anti_est)² 被大 anti_est 主导).
       R-47 时间反转修复 + R-48 功率 floor 修复后该比值应大幅下降. */
    if (ctx->anti_est_rms > ctx->anti_rms * 20.0f && ctx->anti_rms > 0.0005f) {
        printf("       ⚠ antiEst/anti=%.0fx — Wc膨胀或Ŝ增益失配, NR可能虚高\n",
               ctx->anti_est_rms / (ctx->anti_rms + 1e-10f));
    }
    if (ctx->fb_active)
        printf("       FB:  est=%.4f (反馈抵消量 RMS)\n", ctx->fb_rms);
    if (ctx->hw.active_count > 0 || ctx->hw.dominant_db > HW_THRESH_DB * 0.7f)
        printf("       HW:  f=%.0fHz peak=%.1fdB notches=%d%s\n",
               ctx->hw.dominant_freq, ctx->hw.dominant_db,
               ctx->hw.active_count,
               ctx->hw.active_count > 0 ? " [NOTCH]" : "");
}

static void check_wc_divergence(rt_ctx_t *ctx) {
    float wc_max = sm_wc_max_abs(ctx->wc_snapshot, S * L);

    /* P0-2: 输出能量发散救援 — anti_rms 连续 3s 超限 → 回滚 Wc + 冷启动 ramp.
       阈值随 mic_pre_gain 自适应收缩 (R-54). */
    float div_thresh = cfg.diverge_anti_rms / (cfg.mic_pre_gain > 0.1f ? cfg.mic_pre_gain : 1.0f);
    if (ctx->anti_rms > div_thresh) {
        if (++ctx->diverge_sec >= 3) {
            if (ctx->scene_wc_valid[ctx->cur_scene_id])
                memcpy(ctx->wc_shadow, ctx->scene_wc[ctx->cur_scene_id], S*L*sizeof(float));
            else
                scene_ctrl_construct_wc(&ctx->sc, ctx->cur_scene_id, ctx->wc_shadow);
            InterlockedExchangeAdd(&ctx->wc_seq, 2);
            InterlockedExchange(&ctx->ramp_cnt, (FS_ANC * cfg.ramp_ms / 1000));
            ctx->diverge_sec = 0;
            ctx->peak_rollback_cnt = 0;
            if (ctx->log_file)
                fprintf(ctx->log_file, "# EVENT: Wc RESCUE anti_rms=%.3f > %.2f x3s\n",
                        ctx->anti_rms, div_thresh);
            printf("[WARN] anti_rms %.3f > %.2f for 3s — Wc rescued (rollback + ramp)\n",
                   ctx->anti_rms, div_thresh);
        }
    } else {
        ctx->diverge_sec = 0;
    }

    /* C1: 使用共享 freeze 状态机 */
    int freeze_action = sm_check_divergence(wc_max, ctx->wc_init_max, cfg.freeze_ratio,
                                            cfg.freeze_retry_sec,
                                            &ctx->freeze_timer, &ctx->freeze_permanent);
    if (freeze_action == 1) {
        /* 刚触发 freeze — 设置 LMS 冻结 */
        InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 1);
        if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc diverged max=%.3f init=%.3f\n",
                                   wc_max, ctx->wc_init_max);
        printf("[WARN] Wc diverged! max|Wc|=%.3f "
               "> %.0f×init_max(%.3f), LMS frozen (%ds retry)\n",
               wc_max, cfg.freeze_ratio, ctx->wc_init_max, cfg.freeze_retry_sec);
    } else if (freeze_action == 3) {
        /* R-7: 解冻重试 — 回滚到已知良好 Wc */
        if (ctx->scene_wc_valid[ctx->cur_scene_id]) {
            memcpy(ctx->wc_shadow, ctx->scene_wc[ctx->cur_scene_id], S*L*sizeof(float));
            printf("[INFO] Wc freeze retry — rollback to scene_wc[%d]\n", ctx->cur_scene_id);
        } else {
            scene_ctrl_construct_wc(&ctx->sc, ctx->cur_scene_id, ctx->wc_shadow);
            printf("[INFO] Wc freeze retry — rollback to CNN preset scene=%d\n", ctx->cur_scene_id);
        }
        InterlockedExchangeAdd(&ctx->wc_seq, 2);
        InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 0);
        if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc unfreeze retry (rolled back)\n");
        printf("[INFO] Wc unfrozen, watching 3s...\n");
    } else if (freeze_action == 2) {
        /* 永久冻结 */
        InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 1);
        if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc freeze PERMANENT\n");
        printf("[WARN] Wc diverged again during watch period! "
               "LMS permanently frozen until scene switch\n");
    } else if (freeze_action == 0 && ctx->freeze_timer == 0
               && !ctx->fx.freeze_lms && !ctx->freeze_permanent) {
        /* 观察期安全度过 — sm_check_divergence 已将 freeze_timer 归零 */
        if (ctx->freeze_timer >= 0 && ctx->freeze_timer > -3)
            printf("[INFO] Wc stable after unfreeze, normal operation resumed\n");
    }
}

static void check_convergence(rt_ctx_t *ctx) {
    /* C1: 使用共享收敛检测 */
    int saved = sm_check_convergence(
        ctx->nr_level, cfg.nr_converge_db,
        ctx->safety_mute, ctx->diverged,
        &ctx->converged_frames,
        (float *)ctx->scene_wc, ctx->cur_scene_id,
        ctx->wc_snapshot, S * L,
        &ctx->wc_init_max);
    if (saved) {
        /* scene_wc 已保存, wc_init_max 已更新为收敛期 max|Wc| */
    }
}

static void check_scene_switch(rt_ctx_t *ctx, int new_scene,
                               float cos_sim, float *probs) {
    if (cos_sim >= 0.8f || new_scene == ctx->cur_scene_id) return;

    int restored = sm_scene_switch_execute(
        (float *)ctx->scene_wc, ctx->scene_wc_valid,
        &ctx->cur_scene_id, new_scene,
        ctx->wc_snapshot, ctx->wc_cur, ctx->wc_old, S * L);

    if (ctx->log_file)
        fprintf(ctx->log_file, "# EVENT: scene switch %d→%d cos=%.3f restored=%d\n",
                ctx->cur_scene_id, new_scene, cos_sim, restored);

    printf("  -> RESET s%d→s%d (%s)\n",
           ctx->cur_scene_id, new_scene,
           restored ? "restored adapted Wc" : "new scene, CNN preset");

    /* CrossFader + 保护重置 (实时版特有: Interlocked 跨线程) */
    InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 0);
    ctx->freeze_timer = 0; ctx->freeze_permanent = 0;
    memcpy(ctx->anchor_probs, probs, ctx->sc.K * sizeof(float));
    InterlockedExchange(&ctx->fade_cnt, cfg.fade_len);
    InterlockedExchange(&ctx->mute_hold, (FS_ANC * cfg.mute_hold_ms / 1000));
    ctx->converged_frames = 0;
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    gfanc_config_load_env(&cfg);
    LOG_INFO("Runtime config: gain=%.1f step=%.2e leak=%.0e ramp=%dms mute=%dms dsp=%d",
             cfg.mic_pre_gain, cfg.step_size, cfg.leak, cfg.ramp_ms, cfg.mute_hold_ms, cfg.dsp_delay);
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
    /* R-13: 尝试加载 ANC 专用短带通 (256tap). 无文件时截取 1024tap 前 256 点作为近似. */
    float *bp_anc_coeff = NULL;
    int bp_anc_loaded = bin_load_float("data/bandpass_anc.bin", &bp_anc_coeff);
    int bp_anc_ok = (bp_anc_loaded >= BP_ANC_LEN);

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
    ctx->anti_est_offset = 0;  /* R-55: 首次从 0 开始, 每帧 LCG 随机化 */
    /* R-14: 初始化 4 通道抗混叠低通 (fc=6.5kHz @48k, 2阶 Butterworth) */
    for (int c = 0; c < 4; c++) biquad_init_lpf(&ctx->aa_filt[c], 6500.0f, 48000.0f);
    g_ctx = ctx;
    ctx->running = 1; ctx->first_sec = 1;
    InterlockedExchange(&ctx->mute_hold, (FS_ANC * cfg.mute_hold_ms / 1000));  /* 启动抑制 safety_mute */

    /* ── R-13: CNN 带通 1024tap (分类用) ── */
    ctx->bp_fir.coeffs = bp_coeff; ctx->bp_fir.n_taps = BP_LEN;
    ctx->bp_fir.delay_line = (gfanc_delay_t *)calloc(BP_LEN, sizeof(gfanc_delay_t));
    if (!ctx->bp_fir.delay_line) { fprintf(stderr, "OOM: bp_fir\n"); ret = 1; goto cleanup; }

    /* ── R-13: ANC 带通 256tap (群延迟 32→8ms) ── */
    {
        float *anc_coeff = bp_anc_ok ? bp_anc_coeff : bp_coeff;  /* 回退: 1024tap 截断 */
        ctx->bp_fir_anc.coeffs = anc_coeff; ctx->bp_fir_anc.n_taps = BP_ANC_LEN;
        ctx->bp_fir_anc.delay_line = (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t));
        if (!ctx->bp_fir_anc.delay_line) { fprintf(stderr, "OOM: bp_fir_anc\n"); ret = 1; goto cleanup; }
        ctx->bp_anc_coeffs = bp_anc_ok ? bp_anc_coeff : NULL;  /* 仅当独立文件时持有所有权 */
        printf("  BP ANC: %s (%dtap, gd=%.1fms vs 1024tap gd=32ms)\n",
               bp_anc_ok ? "bandpass_anc.bin" : "fallback(1024tap truncated)",
               BP_ANC_LEN, (BP_ANC_LEN-1)/(2.0f*FS_ANC)*1000.0f);
    }

    /* ── R-13: 误差带通 256tap (ANC 通路, 与 ref_anc 一致) ── */
    for (int e = 0; e < E; e++) {
        float *ec = bp_anc_ok ? bp_anc_coeff : bp_coeff;
        ctx->bp_err[e].coeffs = ec; ctx->bp_err[e].n_taps = BP_ANC_LEN;
        ctx->bp_err[e].delay_line = (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t));
        if (!ctx->bp_err[e].delay_line) { fprintf(stderr, "OOM: bp_err[%d]\n", e); ret = 1; goto cleanup; }
    }

    int dsp_delay = cfg.dsp_delay;
    int sp = SEC_LEN + dsp_delay;
    /* ── Ŝ peak→1.0 归一化: 消除训练/实测间的尺度差异, 使 power 在可预测范围.
       归一化后 Xd 尺度一致, μ_eff=step_size/power 不再与 Ŝ 源耦合.
       物理实测 Ŝ (peak<1) 归一化后 s_rms 仍由声学特性决定;
       仿真 Ŝ (peak>1) 归一化后 s_rms 回归合理范围.
       GFANC_STEP 可覆盖 step_size. ── */
    {
        float s_peak = 0, s_rms = 0;
        for (int i = 0; i < E*S*SEC_LEN; i++) {
            float a = fabsf(sec_path[i]);
            s_rms += sec_path[i] * sec_path[i];
            if (a > s_peak) s_peak = a;
        }
        s_rms = sqrtf(s_rms / (E*S*SEC_LEN));
        if (s_peak > 0.001f) {
            float inv = 1.0f / s_peak;          /* peak→1.0 */
            for (int i = 0; i < E*S*SEC_LEN; i++) sec_path[i] *= inv;
            s_rms *= inv;
        }
        printf("  Ŝ: peak=%.4f RMS=%.4f → norm peak=1.00 RMS=%.4f\n", s_peak,
               s_peak > 0.001f ? s_rms * s_peak : s_rms, s_rms);
    }
    if (getenv("GFANC_STEP")) cfg.step_size = (float)atof(getenv("GFANC_STEP"));
    printf("  step=%.2e (μ_eff≈%.1f @ epsilon floor)\n", cfg.step_size, cfg.step_size * 1e6f);
    ctx->sec_firs = (fir_filter_t *)calloc(E*S, sizeof(fir_filter_t));
    ctx->sec_coeffs = (float *)calloc(E*S*sp, sizeof(float));
    if (!ctx->sec_firs || !ctx->sec_coeffs) {
        fprintf(stderr, "OOM: sec_firs/coeffs\n"); ret = 1; goto cleanup;
    }
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e*S+s;
            memcpy(ctx->sec_coeffs + idx*sp + dsp_delay, sec_path + idx*SEC_LEN, SEC_LEN*sizeof(float));
            ctx->sec_firs[idx].coeffs = ctx->sec_coeffs + idx*sp;
            ctx->sec_firs[idx].n_taps = sp;
            ctx->sec_firs[idx].delay_line = (gfanc_delay_t *)calloc(sp, sizeof(gfanc_delay_t));
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
                float fb_rms = 0;
                for (int i = 0; i < FB_LEN; i++) fb_rms += ctx->fb_coeffs_buf[spk][i] * ctx->fb_coeffs_buf[spk][i];
                fb_rms = sqrtf(fb_rms / FB_LEN);
                /* R-57: FIR RMS < 0.00005 时反馈抵消形同虚设, 加载无效 FIR
                   会在高增益下产生虚假 fb_est, 干扰 ref 信号. */
                if (fb_rms < 0.00005f) {
                    printf("  Feedback spk%d: RMS=%.6f too weak, skipping (re-run calibrate)\n", spk, fb_rms);
                    bin_free(fb_raw);
                    continue;
                }
                ctx->fb_fir[spk].coeffs    = ctx->fb_coeffs_buf[spk];
                ctx->fb_fir[spk].n_taps    = FB_LEN;
                ctx->fb_fir[spk].delay_line = (gfanc_delay_t *)calloc(FB_LEN, sizeof(gfanc_delay_t));
                ctx->fb_fir[spk].ptr       = 0;
                printf("  Feedback spk%d: %d taps, RMS=%.4f\n", spk, FB_LEN, fb_rms);
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
    /* ── Wc 增益自动标定: 极保守起始, LMS 在有真实噪声时从零缓慢收敛.
       anti ≈ Wc_RMS × ref_filt × √L. Wc_RMS=0.03: ref=0.05→anti≈0.05 (几乎无声).
       安静房间无外部噪声时 Wc 不会自行增长; 有噪声后 LMS 在 10-30s 内收敛到工作点.
       收敛后 scene_wc 记忆保存正确幅值, 下次切回直接恢复 (不经过此保守初始值).
       可通过 GFANC_WC_TARGET 环境变量覆盖 (网格搜索标定结果). ── */
    {
        float wc_target = cfg.wc_rms_target;
        if (wc_target < 0.005f) wc_target = 0.005f;
        if (wc_target > 0.05f) wc_target = 0.05f;
        printf("  Wc RMS target=%.3f (stub_rms=%.4f, gain=%.1fx)\n",
               wc_target, ctx->sc.stub_rms,
               ctx->sc.stub_rms > 1e-6f ? wc_target / ctx->sc.stub_rms : 0.0f);
        ctx->sc.wc_rms_target = wc_target;
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
        gf_log_timestamp(ctx->log_file, "start");  /* R-28: 可移植时间戳 */
        fprintf(ctx->log_file, "# sec,scene,max_prob,cos_sim,NR_dB,err_rms,anti_rms,ref_rms,event\n");
        fflush(ctx->log_file);
    }

    /* 主循环: CNN 场景分类 1Hz, 驱动 Wc 更新和场景切换 */
    int log_sec = 0;
    int scene_cand = -1, scene_cand_cnt = 0;  /* P4: 场景切换滞回候选状态 */
    while (ctx->running) {
        gf_sleep_ms(100);  /* R-28: 可移植睡眠 */
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
            /* C1: 使用共享函数初始化场景记忆 + wc_init_max */
            sm_first_sec_init((float *)ctx->scene_wc, ctx->scene_wc_valid,
                              &ctx->cur_scene_id, new_scene,
                              ctx->wc_cur, S * L, &ctx->wc_init_max);
            /* 通过影子缓冲提交 Wc (主线程→回调, 零数据竞争) */
            memcpy(ctx->wc_shadow, ctx->wc_cur, S*L*sizeof(float));
            InterlockedExchangeAdd(&ctx->wc_seq, 2);
            InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 0);
            ctx->freeze_timer = 0; ctx->freeze_permanent = 0;
            memcpy(ctx->anchor_probs, probs, K * sizeof(float));
            InterlockedExchange(&ctx->ramp_cnt, (FS_ANC * cfg.ramp_ms / 1000));
            InterlockedExchange(&ctx->mute_hold, (FS_ANC * cfg.mute_hold_ms / 1000));
            printf("[CNN] INIT scene=%d max=%.2f (ramp %dms, mute_hold %dms)\n",
                   new_scene, probs[new_scene], cfg.ramp_ms, cfg.mute_hold_ms);
            /* ── 自动增益标定: 如用户未设 GFANC_MIC_GAIN, 根据实测 ref 电平一次标定 ── */
            if (!getenv("GFANC_MIC_GAIN")) {
                /* 自动增益标定: 目标 ref≈0.03 (-30dBFS), 上限 5×.
                   超过 5× 的部分需通过 UMC 物理旋钮提升 — 数字增益同步放大反馈残余. */
                float auto_gain = TARGET_REF_RMS / (ctx->ch_rms[0] + 1e-10f);
                if (auto_gain < 1.0f) auto_gain = 1.0f;
                int capped = (auto_gain > 5.0f);
                if (capped) auto_gain = 5.0f;
                cfg.mic_pre_gain = auto_gain;
                printf("       Auto gain=%.1fx from ref_rms=%.4f%s\n",
                       auto_gain, ctx->ch_rms[0],
                       capped ? " (capped@5x — 提高 UMC 物理旋钮)" : "");
            }
            ctx->first_sec = 0;
        } else {
            /* S-1修复: cos(anchor, cur) 替代 cos(prev, cur) */
            float cos_sim = sm_cos_sim(ctx->anchor_probs, probs, K);

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
            /* P4: 场景切换滞回 — 候选场景需连续 3 帧一致才 RESET.
               CNN 在真实噪声边界上 probs 翻转 (实测 max=0.48~0.63 低置信跳变),
               旧逻辑单帧即切 → fade+mute_hold+Wc重载 循环泵浦 */
            if (sm_check_scene_switch(cos_sim, cfg.switch_threshold,
                                       new_scene, ctx->cur_scene_id,
                                       &scene_cand, &scene_cand_cnt, 3)) {
                check_scene_switch(ctx, new_scene, cos_sim, probs);
            }
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
        free(ctx->bp_fir_anc.delay_line);  /* R-13 */
        free(ctx->bp_anc_coeffs);          /* R-13: bandpass_anc.bin 所有权 */
        for (int e = 0; e < E; e++) free(ctx->bp_err[e].delay_line);
        if (ctx->sec_firs) {
            for (int i = 0; i < E*S; i++) free(ctx->sec_firs[i].delay_line);
            free(ctx->sec_firs);
        }
        free(ctx->sec_coeffs);
        for (int s = 0; s < S; s++) free(ctx->fb_fir[s].delay_line);
        fxnlms_free(&ctx->fx);
        free(ctx->ref_buf); free(ctx->anti_buf); free(ctx->err_buf);
        free(ctx);
        ctx = NULL;
        if (lf) {
            gf_log_timestamp(lf, "end");  /* R-28: 可移植时间戳 */
            fclose(lf);
        }
    }
    cnn_m5_free();  /* C2: 释放 CNN 实例 (权重+激活缓冲) */
    p_Pa_Terminate();
    if (ret == 0) printf("Done.\n");
    return ret;
}
