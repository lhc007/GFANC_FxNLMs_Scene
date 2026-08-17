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
#include <sys/stat.h>   /* BUG-8: Ŝ 文件选择 (stat mtime) */
#include <windows.h>

#include "os_port.h"        /* R-28: gf_sleep_ms + gf_log_timestamp */
#include "fir_filter.h"
#include "binary_loader.h"
#include "cnn_m5_forward.h"
#include "scene_controller.h"
#include "scene_manager.h"
#include "ocg.h"
#include "fxnlms_mimo.h"
#include "howling_detect.h"
#include "sec_online.h"

/* ── ADV-F3: 回调 WCET 监控 (诊断). GFANC_WCET=1 开启 (默认关, 零开销).
   rdtsc 仅 x86/x64; 其他平台编译为恒 0, WCET 显示 0. ── */
#if defined(_MSC_VER)
#  include <intrin.h>
#  define GFANC_RDTSC() ((unsigned long long)__rdtsc())
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#  include <x86intrin.h>
#  define GFANC_RDTSC() ((unsigned long long)__rdtsc())
#else
#  define GFANC_RDTSC() ((unsigned long long)0)
#endif
static int    g_wcet_on = 0;       /* GFANC_WCET=1 开启 */
static double g_wcet_cps = 1.0;    /* 启动校准: cycles/µs */
static volatile unsigned long long g_wcet_min, g_wcet_max, g_wcet_sum; /* 回调耗时(cycles) */
static volatile int g_wcet_cnt;    /* 回调累计计数 (≈375/s @128帧) */

/* 一次性校准 cycles/µs: rdtsc delta / QPC delta 50ms */
static void wcet_calibrate(void)
{
    LARGE_INTEGER freq, t0, t1;
    unsigned long long c0, c1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0); c0 = GFANC_RDTSC();
    gf_sleep_ms(50);
    QueryPerformanceCounter(&t1); c1 = GFANC_RDTSC();
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    g_wcet_cps = (secs > 0.0 && (c1 - c0) > 0) ? (double)(c1 - c0) / secs / 1e6 : 1.0;
}

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
#define BP_ANC_LEN   64     /* R-13: ANC 带通 (64tap 群延迟 2ms vs 8ms — 砍环路延迟, 宽带可消上限 ↑) */
#define SEC_LEN      1024
/* DSP_DELAY 由 cfg.dsp_delay 管理, 默认 0; 无 GFANC_DSP_DELAY 时运行时自动从
   data/sec_bulk_delay.bin 加载实测环路延迟补偿 (dsp_delay = 环路延迟 − Ŝ峰位),
   GFANC_DSP_DELAY 环境变量可手动覆盖 */
/* R-9: 以下参数统一由 cfg (gfanc_config_t) 管理, env 变量可直接生效
   GFANC_RAMP_MS / GFANC_MUTE_MS / GFANC_FADE_LEN / GFANC_MIC_GAIN */
#define MIC_CLIP_MAX  1.0f    /* 输入软限幅 (防止吹气/大声压冲爆 FIR) */
#define FB_LEN       512     /* 反馈路径 FIR 长度 (R-50修订: 256 装不下 USB 设备往返
                                ~238样本 + 响应尾 → 截断 → NLMS 不收敛, FIR 全坏) */
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
    ocg_t         ocg;          /* OCG 多质心聚类闸门 (reset 模式决策) */
    fxnlms_mimo_t fx;
    fir_filter_t  bp_fir;        /* ref 带通 CNN (1024tap, 分类用) */
    fir_filter_t  bp_fir_anc;    /* R-13: ref 带通 ANC (256tap, 群延迟8ms) */
    fir_filter_t  bp_err[E];     /* err 带通 ANC (256tap) */
    fir_filter_t  bp_fx[E*S];    /* R-58-10: 梯度 Fx 带通, 每 (e,s) 独立 FIR (与 err_meas 同路径对齐) */
    fir_filter_t *sec_firs;      /* [E*S] 次级路径 */
    float        *sec_coeffs;
    float        *bp_anc_coeffs; /* R-13: ANC 带通系数 (256tap, 与 CNN 1024tap 独立) */

    /* 反馈抵消 (逐扬声器独立 FIR, F-G修复) */
    fir_filter_t     fb_fir[GFANC_S_MAX];      /* [spk] 扬声器→参考麦反馈路径 FIR */
    float            fb_coeffs_buf[GFANC_S_MAX][FB_LEN];
    int              fb_active;      /* 已加载的扬声器数 (0/1/2) */

    /* 啸叫检测 */
    howling_detect_t hw;           /* DFT 频谱检测 + IIR 陷波 */
    sec_online_t   sec_on;        /* 在线 Ŝ 辨识 (NLMS, 零探测噪声) */

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

    /* 去场景层 (gfanc-direct-weight): 无场景记忆/切换/OCG.
       单一 known-good Wc 备份, 供发散救援 + freeze 重试回滚. */
    float  last_good_wc[S*L];
    int    converged_frames;      /* 连续正常帧数 (判断已收敛) */
    float  anchor_gains[SC_DW_MAX];    /* 上次重置时的 30 维增益锚点 (reset 模式 cos_sim 对比) */
    int    reset_pending;         /* cos<τ 连续秒数 (RESET 迟滞, 达 reset_hyst 才触发) */
    int    freeze_timer;          /* Wc freeze 计时器 (秒), >0=冻结中, 60s后尝试解冻 */
    int    freeze_permanent;      /* 解冻后3s内再次触发 → 永久冻结直到场景切换 */
    int    peak_hold_cnt;         /* anti峰值连续超限计数 (快检测safety_mute, 10样本=0.6ms触发) */
    volatile int peak_mute;       /* 峰值快检测触发静音 */
    int    peak_release_cnt;      /* peak_mute 释放迟滞: 连续低于阈值的样本数 (10ms 防抖) */
    volatile int peak_rollback_cnt; /* peak_mute 上升沿 Wc 减半次数 (主线程显示) */
    float  out_gain;              /* 静音包络 0..1 (slew~4ms, 替代硬切零, 消除开关咔哒声) */
    float  ref_env;               /* ref 包络 (~16ms), AGC 防饱和 */
    volatile LONG cold_hold;     /* cold start anti 硬限幅保护, 2s 后释放 */
    int    diverge_sec;           /* anti_rms 连续超限秒数 (≥2s 触发 Wc 救援) */
    float  prev_err_ref;          /* P0-4: 上一秒 err_rms/ref_rms (上升趋势=反相生长, 健康收敛是下降) */
    /* P0-5: 环境安静检测 (治"噪声消失后嗡嗡声"): anti 大但 NR 低 → 冻结+衰减 Wc */
    int    quiet_sec;             /* anti 无对消效果连续秒数 (>=quiet_hold 进入安静) */
    int    quiet_since_active;    /* 距 ref 上次高于 quiet_ref_max 的秒数 — 防弱噪声误判为"噪声消失" */
    volatile int quiet_active;    /* 1=安静模式: 回调冻结梯度 + 衰减 Wc */
    float  quiet_floor;           /* 安静期 ref_rms 基准 (EMA, 退出判定: ref 重回基准×quiet_exit) */
    float  quiet_err_floor;       /* 安静期 err_rms 基准 (EMA, 退出判定: err 重回基准×quiet_err_exit) */
    int    reinit_needed;         /* 安静退出后请求重建 INIT (下秒 first_sec=1) */
    int    init_skip_agc;         /* 重建 INIT 时跳过自动增益重标定 (保留已标定增益) */
    float  leak_ema;              /* 自适应 leak 连续 EMA (治 anti_rms 跨档 1×↔5×↔10× 跳变) */
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
    volatile float acc_ref, acc_err, acc_fb;
    volatile float acc_ch[4], acc_anti, acc_anti_est;
    volatile float acc_d_est;    /* 扰动估计功率 Σ(err-anti_est)² (诚实NR分子) */
    volatile float acc_err_cross;/* Σ(err × anti_est), Ŝ 校准后重构 d_cal 用 */
    volatile float acc_err_win;  /* BUG-1: 分散采样窗口误差功率 Σerr² (与 pa/cross 同源) */
    /* s_cal 已移除: Ŝ 保持物理尺度, anti_est 无需去归一化校准 */
    int    anti_est_offset;   /* BUG-1: anti_est 分散采样相位 (0..63, 每帧随机化) */
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

    /* ADV-F3: WCET 计时起点 (正常路径; 关闭时零开销) */
    unsigned long long cb_t0 = 0;
    if (g_wcet_on) cb_t0 = GFANC_RDTSC();

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
        /* AGC: 平滑包络追踪 ref 峰值, 突增时自动压低增益防饱和.
           attack ~5ms (快), release ~50ms (慢, 防增益 pumping).
           稳态噪声不受影响 (ref_env 稳定在阈值以下). */
        {   float ra = fabsf(ref_raw - fb_est);
            float tc = (ra > ctx->ref_env) ? 0.003f : 0.0003f;
            ctx->ref_env += (ra - ctx->ref_env) * tc;
            float agc = 1.0f;
            if (ctx->ref_env > 0.06f) agc = 0.06f / ctx->ref_env;
            if (agc < 0.08f) agc = 0.08f;
            ref_raw = (ref_raw - fb_est) * agc;
        }
        float ref_sample  = ref_raw * cfg.mic_pre_gain;
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

        /* CrossFader — 帧边界新旧 Wc 线性混合 (delayless, SFANC 2024):
           a: 1→0, fx.wc = a·wc_old + (1-a)·wc_cur.
           自然结束 (P1-1): 末帧残留 (1/fade_len)·wc_old ≈ 0.06% 由恢复的 LMS
           下一拍微调吸收, 不做硬 memcpy — fade 期间梯度冻结 (下方 R-6),
           硬覆盖无 LMS 状态可救, 只会引入一步硬跳变. */
        if (ctx->fade_cnt > 0) {
            float a = (float)ctx->fade_cnt / cfg.fade_len;
            for (int i = 0; i < S*L; i++)
                ctx->fx.wc[i] = a * ctx->wc_old[i] + (1.0f - a) * ctx->wc_cur[i];
            InterlockedDecrement(&ctx->fade_cnt);
        }

        /* R-13: Fx = Ŝ ⊗ ref_anc (256tap 带通, 群延迟 8ms) */
        float Fx_arr[E*S];
        for (int e = 0; e < E; e++)
            for (int s = 0; s < S; s++)
                Fx_arr[e*S+s] = fir_tick(&ctx->sec_firs[e*S+s], ref_anc);
        /* R-58-10: Fx 过 bp_anc, 与 err_meas 梯度对齐 (修复实时梯度相位失配, 同离线根因).
           每条 (e,s) 路径独立 FIR, 避免扬声器间延迟线交叉污染. */
        for (int e = 0; e < E; e++)
            for (int s = 0; s < S; s++)
                Fx_arr[e*S+s] = fir_tick(&ctx->bp_fx[e*S+s], Fx_arr[e*S+s]);

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

        /* 自适应 leak: anti RMS 偏高时自动加强正则化, 防 Wc 慢性漂移.
           平时 leak 不变, anti>0.06 时逐渐加大 (max 10×). 滞后释放防抖动.
           2026-08-11 (P0-5): 离散分档 (1/2/5/10×) 改连续映射 + EMA — anti_rms
           跨档 (如 0.10↔0.15) 时 leak 每秒硬跳 → Wc 生长率阶跃 → anti 幅度 1Hz
           泵动 (听感: 持续滋滋). 连续化 + 慢 EMA 消除阶跃. */
        {   float ar = ctx->anti_rms;  /* 上一帧平滑值 */
            float mult = 1.0f;
            if      (ar > 0.18f) mult = 10.0f;
            else if (ar > 0.06f) mult = 1.0f + 9.0f * (ar - 0.06f) / 0.12f;  /* 连续 1→10× */
            ctx->leak_ema += 0.001f * (mult - ctx->leak_ema);  /* ~62ms 时间常数 */
            ctx->fx.leak = cfg.leak * ctx->leak_ema;
        }

        /* FxNLMS 实时路径: anti=Wc⊗ref_anc, 梯度用err_meas直接驱动 (不合成err)
           R-6: 静音/peak_mute/fade/啸叫陷波活跃时冻结梯度, 防止反馈环路.
           P1-1 注: fade 期间冻结是有意为之 — 混合公式要求 fx.wc 是确定性线性组合,
           冻结 100ms 的收敛损失可忽略, 这是 delayless 交接的正确代价, 非缺陷.
           BUG-3: cold_hold 冷启动**硬限幅段**(前1s, cold_hold>FS_ANC)冻结梯度 —
           输出被钳到 ±0.12 时若继续用 err 驱动 Wc, 强噪声下 Wc 开环增长;
           软释放段(后1s, cap 0.12→1.0)梯度活跃, Wc 在输出受界内自适应收敛. */
        if (ctx->fade_cnt > 0 || ctx->safety_mute || ctx->peak_mute
            || ctx->quiet_active
            || ctx->cold_hold > FS_ANC
            || (HOWLING_ENABLED && ctx->hw.active_count > 0)) {
            fxnlms_forward_rt(&ctx->fx, ref_anc, Fx_arr, err_meas, anti_spk);
            /* 静音/安静/啸叫期间 Wc 持续衰减 — 反馈事件/噪声消失后 Wc 自行退回到安全区.
               P0-5: quiet_active 时冻结梯度 + 衰减 Wc → anti 平滑消退, 治"噪声消失后
               嗡嗡声" (0.04 底噪不被当有效噪声持续降). */
            if (ctx->safety_mute || ctx->peak_mute || ctx->quiet_active
                || (HOWLING_ENABLED && ctx->hw.active_count > 0)) {
                const float dk = 1.0f - WC_MUTE_DECAY;  /* 半衰期~0.25s */
                for (int i = 0; i < S*L; i++) ctx->fx.wc[i] *= dk;
            }
        } else {
            fxnlms_tick_rt(&ctx->fx, ref_anc, Fx_arr, err_meas, anti_spk);
            /* 在线 Ŝ 辨识: 利用 anti→err 关系跟踪次级路径变化.
               仅正常运行时更新 (非 mute/fade/howling/ramp).
               anti 此时尚未钳位, err_meas 为带通信号, NLMS 无偏. */
            if (cfg.sec_online_mu > 0 && ctx->ramp_cnt == 0)
                sec_online_update(&ctx->sec_on, anti_spk, err_meas, ctx->sec_coeffs);
        }

        /* R-8: NaN/Inf 保护 + 输出钳位 + 看门狗
           驱动毛刺 → NaN 进入 FIR 延迟线 → 永久 NaN 输出 (延迟线无自恢复能力)
           看门狗: 连续 >1s NaN 输出 → 复位全部 FIR 延迟线 (memset+指针归零, <10μs) */
        /* 冷启动 anti 软释放: 新场景首次 2s.
           前 1s (cold_hold>FS_ANC): cap=0.12, 梯度冻结, 输出极小;
           后 1s (0<remain<=FS_ANC): cap 线性 0.12→1.0, 梯度活跃 —
           Wc 在输出受界内自适应收敛, 避免硬释放时未收敛 Wc 的瞬态
           输出激起扬声器→参考麦反馈啸叫 (降噪耳机用固定滤波器无此瞬态). */
        if (ctx->cold_hold > 0) {
            int remain = (int)InterlockedDecrement(&ctx->cold_hold);
            float cap;
            if (remain > FS_ANC)
                cap = 0.12f;
            else
                cap = 0.12f + 0.88f * (1.0f - (float)remain / FS_ANC);
            for (int s = 0; s < S; s++) {
                if      (anti_spk[s] >  cap) anti_spk[s] =  cap;
                else if (anti_spk[s] < -cap) anti_spk[s] = -cap;
            }
        }

        int nan_anti = 0;
        for (int s = 0; s < S; s++) {
            if (!isfinite(anti_spk[s])) { anti_spk[s] = 0.0f; nan_anti = 1; }
            /* 输出软限幅(soft-knee)替代硬钳位 ±1.0 — 鲁棒性提升.
               硬钳位把峰值削平 → 3/5/7 次谐波失真.
               软膝: |x|≤0.9 线性不变, |x|>0.9 用 tanh 圆滑过渡到 ±1.0,
               只去掉尖角高次谐波, 保留基波幅度 (膝点 C¹ 连续, 不引入新谐波).
               (2026-08-17 实测 250Hz 滋滋并非削波所致, 此改动不治滋滋但更稳.) */
            else if (anti_spk[s] > 0.9f)
                anti_spk[s] = 0.9f + 0.1f * tanhf((anti_spk[s] - 0.9f) * 10.0f);
            else if (anti_spk[s] < -0.9f)
                anti_spk[s] = -0.9f - 0.1f * tanhf((-anti_spk[s] - 0.9f) * 10.0f);
        }
        if (nan_anti) {
            ctx->nan_out_hold++;
            if (ctx->nan_out_hold > FS_ANC) {
                fir_reset(&ctx->bp_fir);
                fir_reset(&ctx->bp_fir_anc);  /* R-13 */
                for (int e = 0; e < E; e++) fir_reset(&ctx->bp_err[e]);
                for (int i = 0; i < E*S; i++) { fir_reset(&ctx->bp_fx[i]); fir_reset(&ctx->sec_firs[i]); }
                for (int s = 0; s < S; s++)
                    if (ctx->fb_fir[s].coeffs) fir_reset(&ctx->fb_fir[s]);
                ctx->nan_out_hold = 0;
                ctx->nan_in_cnt = -(ctx->nan_in_cnt + 1);  /* 负哨兵通知主线程 */
            }
        } else {
            ctx->nan_out_hold = 0;
        }

        /* 峰值快检测: 连续10样本|anti|>0.99 → Wc×0.5 (仅真正clipping触发).
           阈值从 0.95→0.99: 正常降噪时瞬时峰值可达 0.95, 频繁误触导致可闻嗡嗡.
           软限幅(tanh→±1.0) + anti-windup(|anti|>1.2→200×leak) 已提供足够保护. */
        {   int peak = 0;
            for (int s = 0; s < S; s++)
                if (fabsf(anti_spk[s]) > 0.99f) peak = 1;
            if (peak) {
                ctx->peak_release_cnt = 0;
                if (++ctx->peak_hold_cnt >= 10 && !ctx->peak_mute) {
                    ctx->peak_mute = 1;
                    for (int i = 0; i < S*L; i++) ctx->fx.wc[i] *= 0.5f;
                    ctx->wc_init_max *= 0.5f;  /* R-52: 同步收紧 freeze 基准, 防止膨胀逃逸 */
                    if (ctx->wc_init_max < 0.001f) ctx->wc_init_max = 0.001f;  /* 防连减到 ~0 → 假 freeze */
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
        }
        /* BUG-1: 分散采样 — 每 64 样本取 1 个 (整帧恰好 250 个), 覆盖整秒.
           替代 R-55 的连续 250 样本窗口: 连续窗口 + int 溢出 LCG 产生负偏移时
           整帧不采样 (NR 恒 0), 且窗口落在局部收敛区时误差功率失真.
           每帧 250 个样本均匀分布, 与 1s 边界无系统对齐, 相位每帧随机化. */
        if (((ctx->acc_cnt + ctx->anti_est_offset) & 63) == 0) {
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
                /* BUG-1: pe/pa/cross 必须来自同一组采样样本, NR 比值才成立 */
                ctx->acc_err_win += err_meas[e] * err_meas[e];
            }
        }
        if ((ctx->acc_cnt += 1) >= FS_ANC) {
            /* BUG-1: pe/pa/cross 全部来自同一组分散采样样本 (250个/帧), 外推因子在
               比值中抵消. 原实现混用全帧 pe 与窗口 pa/cross, 窗口未命中时 pd≈pe →
               NR 恒 0; 窗口命中局部收敛区时 NR 虚高至 40-80dB. */
            float pe = ctx->acc_err_win;
            float pa = ctx->acc_anti_est;
            float cross = ctx->acc_err_cross;
            float pd = pe + pa - 2.0f * cross;               /* d = err - anti_est (Ŝ 物理尺度) */
            if (pd < 0.0f) pd = 0.0f;                        /* 数值保护: 完美对消时 pd 可为负 */
            /* 残差接近数值基底时 pd/pe 无意义, 限制读数范围防噪声基底虚高 */
            if (pe < 1e-10f) {
                ctx->nr_level = 0.0f;
            } else {
                ctx->nr_level = 10.0f * log10f((pd + 1e-12f) / (pe + 1e-12f));
                if (ctx->nr_level > 30.0f) ctx->nr_level = 30.0f;
                if (ctx->nr_level < -30.0f) ctx->nr_level = -30.0f;
            }
            ctx->anti_est_rms = sqrtf(pa / (250.0f * E));
            ctx->ref_rms  = sqrtf(ctx->acc_ref  / FS_ANC);
            ctx->err_rms  = sqrtf(ctx->acc_err  / (FS_ANC * E));
            ctx->dist_rms = sqrtf(pd / (250.0f * E));
            ctx->fb_rms   = sqrtf(ctx->acc_fb   / FS_ANC);
            for (int c = 0; c < 4; c++)
                ctx->ch_rms[c] = sqrtf(ctx->acc_ch[c] / FS_ANC);
            ctx->anti_rms = sqrtf(ctx->acc_anti / (FS_ANC * 2));
            /* R-56: NR<0 确保只有真正恶化时才判定发散 (NR>0 说明仍在降噪,
               pa>>pe 只是小误差导致的高比值, 不是 Wc 膨胀) */
            ctx->diverged = (pa > 9.0f * pe && ctx->anti_rms > 0.05f && ctx->nr_level < 0.0f);
            /* 2026-08-10 实机调试: 2× 阈值对 err/ref≈7-9 的 rig 结构性误杀 —
               anti 仅 0.01 时无反馈可护, err 跳变全是路噪, 该消不该冻.
               改 8× + anti_rms>0.05 门 (anti 极小时物理上不可能啸叫). */
            ctx->safety_mute = (ctx->err_rms > ctx->ref_rms * 8.0f
                                && ctx->anti_rms > 0.05f
                                && ctx->ref_rms > 0.001f
                                && ctx->mute_hold <= 0);
            ctx->acc_ref = ctx->acc_err = ctx->acc_fb = 0;
            ctx->acc_anti = ctx->acc_anti_est = ctx->acc_d_est = ctx->acc_err_cross = 0;
            ctx->acc_err_win = 0;
            ctx->acc_cnt = 0;
            /* BUG-1: 相位随机化 (0..63). 无符号运算避免 int 溢出 (原 LCG 有符号
               溢出产生负偏移, 使连续窗口整帧不采样). */
            ctx->anti_est_offset = (int)((ctx->anti_est_offset * 1103515245u + 12345u) & 63u);
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

    /* ADV-F3: WCET 累计 (cb 线程写; 主线程 1Hz print_diagnostics 读+清零) */
    if (g_wcet_on) {
        unsigned long long dt = GFANC_RDTSC() - cb_t0;
        if (g_wcet_cnt == 0) { g_wcet_min = g_wcet_max = dt; }
        else {
            if (dt < g_wcet_min) g_wcet_min = dt;
            if (dt > g_wcet_max) g_wcet_max = dt;
        }
        g_wcet_sum += dt;
        g_wcet_cnt++;
    }
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
                              const float *gains) {
    char nr_str[20];
    char topbuf[64];
    (void)new_scene;  /* 仍在 CSV 机器日志使用 (见调用点) */
    if (ctx->diverged)
        snprintf(nr_str, sizeof(nr_str), "NR=DIV!(振荡)");
    else
        snprintf(nr_str, sizeof(nr_str), "NR=%.1fdB", ctx->nr_level);
    sm_fmt_top_gains(gains, ctx->sc.K, topbuf, sizeof(topbuf));
    printf("[CNN] top=%s cos=%.2f clu=%d/%d %s anti=%.4f%s%s%s%s gain=%.0fx cb=%d%s\n",
           topbuf, cos_sim,
           ctx->ocg.active, ctx->ocg.n_clusters,
           nr_str, ctx->anti_rms,
           ctx->safety_mute ? " [MUTE]" : "",
           ctx->peak_mute ? " [PMUTE]" : "",
           ctx->quiet_active ? " [QUIET]" : "",
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
    /* ADV-F3: WCET 诊断 (GFANC_WCET=1) — 回调耗时 vs 128帧@48k=2.67ms 预算 */
    if (g_wcet_on && g_wcet_cnt > 0) {
        unsigned long long mn = g_wcet_min, mx = g_wcet_max, sm = g_wcet_sum;
        int cn = g_wcet_cnt;
        printf("       WCET: min=%.0f avg=%.0f max=%.0f us  (%d cb/s, max=%.1f%% of 2.67ms budget)\n",
               (double)mn / g_wcet_cps, (double)sm / cn / g_wcet_cps,
               (double)mx / g_wcet_cps, cn,
               100.0 * (double)mx / g_wcet_cps / 2666.7);
        g_wcet_min = g_wcet_max = g_wcet_sum = 0; g_wcet_cnt = 0;
    }
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

    /* P0-4: 输出能量发散救援 — 三重门控才回滚 last_good_wc:
       (1) anti_rms 连续 2s 超限 (输出能量大);
       (2) err_rms/ref_rms 高于 diverge_err_ratio (对消失败);
       (3) err_ref 比上一秒高 0.1 以上 (err 逐秒恶化 = 反相生长).
       2026-08-10 实测修正:
       - 纯 anti>0.25 判发散 → 健康深对消 (err 已压到 0.018) 被误回滚 → 锯齿循环;
       - 仅加 err_ref>0.6 仍不够: 本硬件 err 麦比 ref 麦热, 收敛中 err_ref 可达 1.3
         (anti=0.2587 时 err_ref=0.97 且 err 仍在下降) → 仍会误杀收敛中的健康 Wc.
       - 决定性判据 = err_ref 上升趋势: 健康收敛 err 逐秒下降, 真发散 (反相正反馈)
         err 逐秒上升. 三重条件连续 2s → 回滚. 阈值随 mic_pre_gain 自适应收缩 (R-54). */
    float div_thresh = cfg.diverge_anti_rms / (cfg.mic_pre_gain > 0.1f ? cfg.mic_pre_gain : 1.0f);
    float err_ref = (ctx->ref_rms > 0.001f) ? (ctx->err_rms / ctx->ref_rms) : 0.0f;
    float err_ref_prev = ctx->prev_err_ref;
    ctx->prev_err_ref = err_ref;
    int err_rising = (err_ref > err_ref_prev + 0.1f);
    if (ctx->anti_rms > div_thresh && err_ref > cfg.diverge_err_ratio && err_rising) {
        if (++ctx->diverge_sec >= 2) {
            memcpy(ctx->wc_shadow, ctx->last_good_wc, S*L*sizeof(float));
            InterlockedExchangeAdd(&ctx->wc_seq, 2);
            InterlockedExchange(&ctx->ramp_cnt, (FS_ANC * cfg.ramp_ms / 1000));
            ctx->diverge_sec = 0;
            ctx->peak_rollback_cnt = 0;
            if (ctx->log_file)
                fprintf(ctx->log_file, "# EVENT: Wc RESCUE anti_rms=%.3f err_ref=%.2f(prev %.2f) x2s\n",
                        ctx->anti_rms, err_ref, err_ref_prev);
            printf("[WARN] anti_rms %.3f err_ref %.2f→%.2f rising x2s — Wc rescued (rollback + ramp)\n",
                   ctx->anti_rms, err_ref_prev, err_ref);
        }
    } else {
        ctx->diverge_sec = 0;
    }

    /* C1: 使用共享 freeze 状态机 */
    int was_watching = (ctx->freeze_timer < 0);  /* 调用前是否处于观察期 (-3..-1) */
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
        /* R-7: 解冻重试 — 回滚到 known-good Wc (去场景层: 单一 last_good_wc) */
        memcpy(ctx->wc_shadow, ctx->last_good_wc, S*L*sizeof(float));
        InterlockedExchangeAdd(&ctx->wc_seq, 2);
        InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 0);
        if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc unfreeze retry (rolled back)\n");
        printf("[INFO] Wc unfrozen, watching 3s...\n");
    } else if (freeze_action == 2) {
        /* 永久冻结 */
        InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 1);
        if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: Wc freeze PERMANENT\n");
        printf("[WARN] Wc diverged again during watch period! "
               "LMS permanently frozen until next reset\n");
    } else if (freeze_action == 0 && !ctx->fx.freeze_lms && !ctx->freeze_permanent) {
        /* 观察期安全度过 (freeze_timer 从 -1 回到 0) 才打印 — 正常态 freeze_timer==0
           满足旧条件导致每秒刷屏 "[INFO] Wc stable", 改为仅在观察期结束时刻打印 */
        if (was_watching && ctx->freeze_timer == 0)
            printf("[INFO] Wc stable after unfreeze, normal operation resumed\n");
    }
}

static void check_convergence(rt_ctx_t *ctx) {
    /* C1: 使用共享收敛检测. 去场景层: 收敛样本写回单一 known-good Wc
       (last_good_wc), 供发散救援/freeze 回滚; 无场景记忆, offset 恒 0. */
    int saved = sm_check_convergence(
        ctx->nr_level, cfg.nr_converge_db,
        ctx->safety_mute, ctx->diverged,
        &ctx->converged_frames,
        ctx->last_good_wc, 0,
        ctx->wc_snapshot, S * L,
        &ctx->wc_init_max);
    if (saved) {
        /* last_good_wc 已更新为收敛期 Wc, wc_init_max 已更新为收敛期 max|Wc| */
    }
}

/* 去场景层 (gfanc-direct-weight): Reset 模式触发 — 无场景记忆, 直接过渡到
   scene_ctrl_process 本秒产出的新候选 wc_cur. Continuous 模式不调用.
   P0-6 (2026-08-14) 软重锚定: 场景切换只做 crossfade(wc_old→wc_cur) + 重锚定,
   去掉 cold_hold/mute — 那 3.5s 打断本是冷启动保护, 场景切换用 peak_mute 兜底即可.
   否则切换 latency = 2s cold_hold + 1.5s mute, 比 FxLMS 小步长硬爬还慢 (问题1). */
static void apply_reset(rt_ctx_t *ctx, float cos_sim, const float *gains, int by_ocg) {
    memcpy(ctx->wc_old, ctx->wc_snapshot, S * L * sizeof(float)); /* 过渡起点 */
    /* wc_cur 已是 scene_ctrl_process 算出的新候选 */
    InterlockedExchange(&ctx->fade_cnt, cfg.fade_len);            /* crossfade 平滑过渡 */
    /* 软重锚定: 不设 cold_hold/mute (冷启动保护仅 INIT 用, 场景切换不需要) */
    InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 0);
    ctx->freeze_timer = 0; ctx->freeze_permanent = 0;
    memcpy(ctx->anchor_gains, gains, ctx->sc.K * sizeof(float));
    ctx->converged_frames = 0;
    if (ctx->log_file) fprintf(ctx->log_file, "# EVENT: reset %s cos=%.3f clu=%d/%d\n",
                               by_ocg ? "ocg" : "cos",
                               cos_sim, ctx->ocg.active, ctx->ocg.n_clusters);
    /* 诚实标注触发源: cos 门打 cos<τ; OCG 打簇索引变化 (cos 仅为诊断, 可能 ≥τ) */
    if (by_ocg)
        printf("  -> RESET (OCG clu=%d/%d, cos=%.2f diag)\n",
               ctx->ocg.active, ctx->ocg.n_clusters, cos_sim);
    else
        printf("  -> RESET (cos=%.2f < %.2f)\n", cos_sim, cfg.switch_threshold);
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    gfanc_config_load_env(&cfg);
    LOG_INFO("Runtime config: gain=%.1f step=%.2e leak=%.0e ramp=%dms mute=%dms dsp=%d",
             cfg.mic_pre_gain, cfg.step_size, cfg.leak, cfg.ramp_ms, cfg.mute_hold_ms, cfg.dsp_delay);
    /* ADV-F3: WCET 监控开关 (GFANC_WCET=1) + 一次性 cycles/µs 校准 */
    {   const char *w = getenv("GFANC_WCET");
        if (w && w[0] == '1') { g_wcet_on = 1; wcet_calibrate(); } }
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
    rt_ctx_t *ctx = NULL;   /* BUG-4: 提前声明置 NULL — 权重校验失败的 goto cleanup
                                在 ctx=calloc 之前跳转, 未初始化则 cleanup 解引用野指针 */
    printf("Loading weights...\n");
    /* BUG-8: Ŝ 文件选择.
       默认 data/secondary_path.bin = 扫频法 (measure_secondary.py → export_bin.py, 用户主测量法).
       GFANC_SEC_FILE 可强制指定其它文件 (如 data/secondary_path_measured.bin = C 校准 calibrate_secondary.exe).
       不用 mtime 比较: export_bin.py 每次导出都会刷新 secondary_path.bin 的 mtime,
       无法区分"新测量"与"仅重导出" (2026-08-07 教训). */
    const char *sec_file = getenv("GFANC_SEC_FILE");
    if (!sec_file || !sec_file[0])
        sec_file = "data/secondary_path.bin";
    float *sec_path, *sub_filters, *bp_coeff;
    int sec_len = bin_load_float(sec_file, &sec_path);
    printf("  Ŝ file: %s\n", sec_file);
    int sub_len = bin_load_float("data/sub_filters.bin", &sub_filters);
    int bp_len  = bin_load_float("data/bandpass_fir.bin", &bp_coeff);
    /* R-13: 尝试加载 ANC 专用短带通 (256tap). 无文件时截取 1024tap 前 256 点作为近似. */
    float *bp_anc_coeff = NULL;
    int bp_anc_loaded = bin_load_float("data/bandpass_anc.bin", &bp_anc_coeff);
    int bp_anc_ok = (bp_anc_loaded >= BP_ANC_LEN);

    if (sec_len < E*S*SEC_LEN) {
        fprintf(stderr, "FATAL: %s too short/load failed (%d<%d)\n", sec_file, sec_len, E*S*SEC_LEN);
        ret = 1; goto cleanup;
    }
    if (sub_len < SC_C*SC_S || sub_len % (SC_C*SC_S) != 0) {
        fprintf(stderr, "FATAL: sub_filters.bin invalid size %d (expect multiple of %d)\n", sub_len, SC_C*SC_S);
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

    /* R-27: 批次指纹 — 检测 cnn/sub_filters/bandpass 是否跨批混配 (WARN, 不阻断) */
    bin_check_batch();

    /* 初始化 ANC 模块 */
    PaStream *stream = NULL;
    ctx = calloc(1, sizeof(rt_ctx_t));  /* 堆分配, 避免 ~211KB 栈压力 */
    if (!ctx) { fprintf(stderr, "OOM: rt_ctx_t\n"); ret = 1; goto cleanup; }
    ctx->cnn_buf_ready = -1;  /* -1=无就绪块, 0/1=该块已满 */
    ctx->anti_est_offset = 0;  /* BUG-1: 分散采样相位, 每帧随机化 (0..63) */
    ctx->leak_ema = 1.0f;      /* P0-5: 自适应 leak 连续 EMA 初值 (1×) */
    ctx->quiet_since_active = 0x7fffffff; /* 哨兵: 启动不算"刚有大噪声", 须 ref 曾高于门槛才允许安静判定 */
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

    /* ── R-58-10: 梯度 Fx 也过 bp_anc (与 err_meas 同路径) — 修复梯度相位失配.
       err_meas = bp_err(es) 带 31.5 样本群延迟 (64tap), 若 Fx = Ŝ⊗ref_anc 不过 bp →
       梯度与误差错位 31.5 样本 → FxLMS 临界稳定 → Wc 相位慢漂移 → 降噪被压 (与离线同构根因,
       实时被 cold_hold/adaptive-leak/safety_mute 掩盖). 修复: Fx 过同一 bp_anc 系数.
       注意: 必须 E×S 每条路径一个独立 FIR — 若共享, s=1 的 tick 会用 s=0 污染的延迟线
       → 第二扬声器滤波参考被交叉污染 → Wc[1] 梯度错位 (离线 bug ② 教训). */
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++) {
            int idx = e*S+s;
            float *ec = bp_anc_ok ? bp_anc_coeff : bp_coeff;
            ctx->bp_fx[idx].coeffs = ec; ctx->bp_fx[idx].n_taps = BP_ANC_LEN;
            ctx->bp_fx[idx].delay_line = (gfanc_delay_t *)calloc(BP_ANC_LEN, sizeof(gfanc_delay_t));
            if (!ctx->bp_fx[idx].delay_line) { fprintf(stderr, "OOM: bp_fx[%d]\n", idx); ret = 1; goto cleanup; }
        }

    /* ── BUG-2: Ŝ 健康检查 + 环路延迟自动补偿 ──
       FxLMS 对齐要求: 模型延迟(dsp_delay + Ŝ峰位) = 真实环路延迟.
       若 Ŝ 在测量时被对齐抹掉了环路延迟 (管道测量 / 校准对齐), 模型延迟偏小,
       FxLMS 滤波参考 xd 早于真实误差到达 → 收敛偏移 + 高频稳定域压缩.
       这是离线 15dB → 实时 4-9dB 的未消除根因之一. ── */
    {
        /* 每条路径的峰值位置 (模型延迟 = dsp_delay + min峰位) */
        int peak_min = SEC_LEN;
        for (int e = 0; e < E; e++)
            for (int s = 0; s < S; s++) {
                const float *p = sec_path + (e*S+s)*SEC_LEN;
                int bp = 0; float bm = 0.0f;
                for (int k = 0; k < SEC_LEN; k++)
                    if (fabsf(p[k]) > bm) { bm = fabsf(p[k]); bp = k; }
                if (bp < peak_min) peak_min = bp;
                printf("    S(e%d,s%d): peak@tap %d (%.2fms)\n",
                       e, s, bp, (float)bp / FS_ANC * 1000.0f);
            }

        /* 环路延迟: GFANC_DSP_DELAY 手动覆盖优先, 否则读 sec_bulk_delay.bin
           (calibrate_secondary 以与运行时一致的 96帧@48k 流参数实测). */
        if (!getenv("GFANC_DSP_DELAY")) {
            float *ld = NULL;
            int n = bin_load_float("data/sec_bulk_delay.bin", &ld);
            if (n >= 1 && ld) {
                int loop = (int)ld[0];   /* 总环路延迟 @16k (样本) */
                bin_free(ld);
                if (loop > 512) {   /* 陈旧/大缓冲测量值 (>32ms): 钳制并警告 */
                    fprintf(stderr, "[WARN] sec_bulk_delay=%d (%dms) 超合理范围 — 请用\n"
                            "      与运行时一致的流参数 (96帧@48k) 重新运行 calibrate_secondary\n",
                            loop, loop * 1000 / FS_ANC);
                    loop = 0;
                }
                /* dsp_delay = 环路延迟 - Ŝ自带延迟(峰位), 使模型延迟 = 真实环路 */
                cfg.dsp_delay = loop - peak_min;
                if (cfg.dsp_delay < 0) cfg.dsp_delay = 0;
                printf("  Loop delay auto-loaded: %d (%dms), dsp_delay=%d\n",
                       loop, loop * 1000 / FS_ANC, cfg.dsp_delay);
            }
        }

        /* 诊断: 模型延迟过小 → Ŝ 疑似缺少环路延迟 */
        float model_ms = ((float)cfg.dsp_delay + peak_min) / FS_ANC * 1000.0f;
        printf("  Ŝ model delay = %.2fms (dsp_delay=%d + peak@%d), 期望环路≈4-6ms\n",
               model_ms, cfg.dsp_delay, peak_min);
        if (model_ms < 2.5f) {
            fprintf(stderr, "[WARN] Ŝ 模型延迟仅 %.1fms — 疑似未包含环路延迟!\n"
                    "      当前 Ŝ 在管道/对齐测量下可能无法代表真实声学路径.\n"
                    "      建议: ① 安装到窗户后重新测量 (measure_secondary.py);\n"
                    "      ② 运行 calibrate_secondary.exe 生成 sec_bulk_delay.bin 自动补偿;\n"
                    "      ③ 或手动设 GFANC_DSP_DELAY=环路延迟ms×16-%.1f.\n",
                    model_ms, (float)peak_min / FS_ANC * 1000.0f);
        }
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
        /* ── 自适应 step: Ŝ RMS 越大 → Xd 越大 → power 越大 → μ_eff 越小.
           补偿: step ∝ s_rms², 使得 μ_eff 在不同 Ŝ 间保持一致.
           (2026-08-17 修正: 原式 s_target²/s_rms² 方向反了 — power∝s_rms², μ_eff=step/power,
            要保持 μ_eff 一致应 step∝s_rms². 旧式在 s_rms>s_target 时反而缩步,
            是 500Hz 压不动的根因之一.)
           设计目标 s_rms≈0.02 (根据 C 实测 Ŝ 标定). GFANC_STEP 可覆盖. */
        if (!getenv("GFANC_STEP")) {
            float s_target = 0.02f;  /* 设计参考 Ŝ_RMS */
            float s_scale = (s_rms * s_rms) / (s_target * s_target + 1e-10f);
            if (s_scale > 4.0f) s_scale = 4.0f;    /* 上限防过冲 */
            if (s_scale < 0.1f) s_scale = 0.1f;    /* 下限保收敛 */
            cfg.step_size *= s_scale;
            /* leak 不再随 Ŝ RMS 缩放 (2026-08-13): leak 是泄漏因子(遗忘率), 与 Ŝ 幅度无关.
               曾随 s_scale×0.489 → 2.4e-7, 不足以抑制梯度噪声 → Wc 无界膨胀
               (250Hz 饱和削波=滋滋 + 500Hz 压不动). leak 固定 5e-7 后两症状同时消失. */
        }
    }
    if (getenv("GFANC_STEP")) cfg.step_size = (float)atof(getenv("GFANC_STEP"));
    printf("  step=%.2e leak=%.1e (μ_eff≈%.1f @ floor, auto-scaled by Ŝ RMS)\n",
           cfg.step_size, cfg.leak, cfg.step_size * 1e6f);
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

    /* 在线 Ŝ 辨识: 从 anti_spk→err_mic 关系持续跟踪次级路径.
       零探测噪声, 利用 ANC 自身输出作为激励. μ≈5e-6 极慢, 抗扰动偏差. */
    if (cfg.sec_online_mu > 0) {
        if (sec_online_init(&ctx->sec_on, E, S, SEC_LEN, dsp_delay,
                             cfg.sec_online_mu) != 0) {
            fprintf(stderr, "OOM: sec_online\n"); ret = 1; goto cleanup;
        }
        printf("  Online Ŝ: μ=%.0e (adaptive, zero probe noise)\n",
               (double)cfg.sec_online_mu);
    } else {
        printf("  Online Ŝ: disabled (GFANC_SEC_MU=0)\n");
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
                for (int i = 0; i < n; i++) fb_rms += ctx->fb_coeffs_buf[spk][i] * ctx->fb_coeffs_buf[spk][i];
                fb_rms = sqrtf(fb_rms / n);
                /* R-57: FIR RMS < 0.00005 时反馈抵消形同虚设, 加载无效 FIR
                   会在高增益下产生虚假 fb_est, 干扰 ref 信号. */
                if (fb_rms < 0.00005f) {
                    printf("  Feedback spk%d: RMS=%.6f too weak, skipping (re-run calibrate)\n", spk, fb_rms);
                    bin_free(fb_raw);
                    continue;
                }
                /* R-50(修订): 峰位边沿门禁 — 峰贴窗口尾 = 响应被截断, NLMS 未收敛,
                   FIR 不可信 (加载会制造虚假 fb_est 干扰 ref). 实测 USB-ASIO 真实反馈峰
                   ~238/512 (46%, 正常); 旧 256 截断文件峰 238/256 (93%, 拒收). */
                {   float fb_peak = 0; int peak_idx = 0;
                    for (int i = 0; i < n; i++)
                        if (fabsf(ctx->fb_coeffs_buf[spk][i]) > fb_peak) {
                            fb_peak = fabsf(ctx->fb_coeffs_buf[spk][i]); peak_idx = i; }
                    if (peak_idx > n - n / 10) {
                        printf("  Feedback spk%d: peak@tap %d/%d (%.0f%%) near window edge — "
                               "truncated response, skipping (re-run calibrate)\n",
                               spk, peak_idx, n, 100.0f * peak_idx / n);
                        bin_free(fb_raw);
                        continue;
                    }
                }
                ctx->fb_fir[spk].coeffs    = ctx->fb_coeffs_buf[spk];
                ctx->fb_fir[spk].n_taps    = n;   /* 实际加载长度, 与 memcpy 一致 (R-50修订:
                                                      短文件尾部不读, 防脏数据当系数) */
                ctx->fb_fir[spk].delay_line = (gfanc_delay_t *)calloc(n, sizeof(gfanc_delay_t));
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

    if (scene_ctrl_init(&ctx->sc, sub_filters, L) != 0) {
        fprintf(stderr, "ERROR: scene_ctrl_init failed\n"); ret = 1; goto cleanup;
    }
    /* P0-2: 增益时间平滑参数 (默认已在 scene_ctrl_init 设好, env 覆盖) */
    scene_ctrl_set_gain_smoothing(&ctx->sc, cfg.gain_smooth_beta, cfg.gain_smooth_switch);
    /* OCG 聚类闸门初始化: τ 独立 (P0-1, ocg_tau 不再复用 switch_threshold);
       持续性命中帧数 ocg_hold (P0-3 修复, 前提②) */
    if (ocg_init(&ctx->ocg, ctx->sc.K, cfg.ocg_tau,
                 cfg.ocg_alpha, cfg.ocg_max_clusters, cfg.ocg_hold) != 0) {
        fprintf(stderr, "ERROR: ocg_init failed\n"); ret = 1; goto cleanup;
    }
    /* ── Wc 增益自动标定: 极保守起始, LMS 在有真实噪声时从零缓慢收敛.
       anti ≈ Wc_RMS × ref_filt × √L. 默认 0.01: ref=0.025→anti≈0.008 (−42dBFS).
       几乎无声, 确保噪声突增时不会饱和; LMS 在 10-30s 内自行收敛到工作点.
       收敛后 last_good_wc 保存正确幅值, 切回直接恢复 (不经过此保守值).
       通过 GFANC_WC_TARGET 环境变量覆盖. ── */
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
    /* 缓冲大小: GFANC_BUFFER 可调 (样本), 默认 128 — 与 UMC ASIO 面板匹配.
       suggestedLatency 由缓冲推导 (128/48k≈2.7ms), 避免旧 0.01s 把 ASIO 驱动
       顶到 512 样本大缓冲 (往返延迟 ~30ms). 改小缓冲后环路延迟下降,
       但需重跑 calibrate_secondary 更新 sec_bulk_delay.bin. */
    int buf_frames = 128;
    {   const char *s = getenv("GFANC_BUFFER");
        if (s && atoi(s) >= 32 && atoi(s) <= 1024) buf_frames = atoi(s); }
    double buf_lat = (double)buf_frames / 48000.0;
    PaStreamParams in_p = { in_dev, 4, 0x00000001, buf_lat, NULL };  /* paFloat32 */
    PaStreamParams out_p = { out_dev, 2, 0x00000001, buf_lat, NULL };
    stream = NULL;
    int err = p_Pa_OpenStream(&stream, &in_p, &out_p, 48000, buf_frames, 0, (void*)audio_cb, ctx);
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
        fprintf(ctx->log_file, "# sec,scene,max_prob,cos_sim,NR_dB,err_rms,anti_rms,ref_rms,event,k_cluster,n_clusters\n");
        fflush(ctx->log_file);
    }

    /* 主循环: CNN 1Hz 产 Wc, 驱动 Reset/Continuous 双模式 (去场景层) */
    int log_sec = 0;
    while (ctx->running) {
        gf_sleep_ms(100);  /* R-28: 可移植睡眠 */
        LONG ready = InterlockedExchange(&ctx->cnn_buf_ready, -1);
        if (ready < 0) continue;

        /* P0-5: 安静退出 (噪声回归) → 下秒走 first_sec INIT 重建 Wc.
           跳过自动增益重标定 — 保留已标定增益, 防噪声回归瞬间重标定.
           清 quiet_active (恢复派发) + 取消挂起的 CrossFader (避免与 INIT 冲突). */
        if (ctx->reinit_needed) {
            ctx->quiet_active = 0;
            InterlockedExchange(&ctx->fade_cnt, 0);
            ctx->first_sec = 1;
            ctx->reinit_needed = 0;
            ctx->init_skip_agc = 1;
        }

        float gains[SC_DW_MAX] = {0};
        int new_scene;
        const int K = ctx->sc.K;

        /* CrossFader期间跳过CNN: 回调正在读wc_cur做混合, 不能覆盖 */
        if (ctx->fade_cnt > 0) {
            memcpy(gains, ctx->sc.prev_gains, K * sizeof(float));
            new_scene = 0;
            for (int i = 1; i < K; i++)
                if (fabsf(gains[i]) > fabsf(gains[new_scene])) new_scene = i;
        } else {
            new_scene = scene_ctrl_process(&ctx->sc, ctx->cnn_buf[ready], ctx->wc_cur, gains);
        }

        if (ctx->first_sec) {
            /* 首次 INIT (两模式一致): CNN Wc → 影子缓冲 → 冷启动 ramp.
               去场景层: 无场景记忆; wc_init_max 由 wc_cur 推导,
               last_good_wc = INIT 值 (后续由 check_convergence 刷新). */
            if (cfg.wc_cold_start < 1.0f && cfg.wc_cold_start > 0.0f) {
                for (int i = 0; i < S*L; i++) ctx->wc_cur[i] *= cfg.wc_cold_start;
            }
            float mx = sm_wc_max_abs(ctx->wc_cur, S*L);
            ctx->wc_init_max = (mx > 0.001f) ? mx : 0.01f;
            /* 通过影子缓冲提交 Wc (主线程→回调, 零数据竞争) */
            memcpy(ctx->wc_shadow, ctx->wc_cur, S*L*sizeof(float));
            memcpy(ctx->last_good_wc, ctx->wc_cur, S*L*sizeof(float)); /* known-good 基线 */
            InterlockedExchangeAdd(&ctx->wc_seq, 2);
            InterlockedExchange((LONG volatile *)&ctx->fx.freeze_lms, 0);
            ctx->freeze_timer = 0; ctx->freeze_permanent = 0;
            memcpy(ctx->anchor_gains, gains, K * sizeof(float));
            ctx->reset_pending = 0;  /* RESET 迟滞计数清零 (重建锚点) */
            ocg_reset(&ctx->ocg, gains);  /* OCG: 首个增益建立簇 0 */
            /* INIT 用 2× ramp: Wc 从零开始, LMS 需更长时间收敛.
               RESET 用 1× ramp: CrossFader 已平滑过渡, 无需延长. */
            int init_ramp_ms = cfg.ramp_ms * 2;
            InterlockedExchange(&ctx->ramp_cnt, (FS_ANC * init_ramp_ms / 1000));
            InterlockedExchange(&ctx->mute_hold, (FS_ANC * cfg.mute_hold_ms / 1000));
            InterlockedExchange(&ctx->cold_hold, 2 * FS_ANC);  /* 冷启动 anti 限幅 */
            char topbuf[64];
            sm_fmt_top_gains(gains, K, topbuf, sizeof(topbuf));
            printf("[CNN] INIT top=%s (ramp %dms, mute_hold %dms)\n",
                   topbuf, init_ramp_ms, cfg.mute_hold_ms);
            /* ── 自动增益标定: 如用户未设 GFANC_MIC_GAIN, 根据实测 ref 电平一次标定 ──
               P0-5: 安静重建时跳过 (init_skip_agc) — 保留已标定增益 */
            if (!getenv("GFANC_MIC_GAIN") && !ctx->init_skip_agc) {
                /* 自动增益标定: 目标 ref≈0.03 (-30dBFS), 上限 5×.
                   超过 5× 的部分需通过 UMC 物理旋钮提升 — 数字增益同步放大反馈残余. */
                float auto_gain = TARGET_REF_RMS / (ctx->ch_rms[0] + 1e-10f);
                if (auto_gain < 1.0f) auto_gain = 1.0f;
                int capped = (auto_gain > 20.0f);
                if (capped) auto_gain = 20.0f;
                cfg.mic_pre_gain = auto_gain;
                printf("       Auto gain=%.1fx from ref_rms=%.4f%s\n",
                       auto_gain, ctx->ch_rms[0],
                       capped ? " (capped@20x — 提高 UMC 物理旋钮)" : "");
            }
            ctx->first_sec = 0;
            ctx->init_skip_agc = 0;
        } else {
            /* S-1修复: cos(anchor, cur) 替代 cos(prev, cur).
               Reset 模式以该值作触发判据; Continuous 模式仅供诊断. */
            float cos_sim = sm_cos_sim(ctx->anchor_gains, gains, K);

            print_diagnostics(ctx, new_scene, cos_sim, gains);

            /* 运行时统计日志 (C1) */
            if (ctx->log_file) {
                fprintf(ctx->log_file, "%d,%d,%.3f,%.3f,%.1f,%.4f,%.4f,%.4f,%s%s,%d,%d\n",
                        log_sec++, new_scene, gains[new_scene], cos_sim,
                        ctx->nr_level, ctx->err_rms, ctx->anti_rms, ctx->ref_rms,
                        ctx->safety_mute ? "MUTE" : "",
                        ctx->fx.freeze_lms ? (ctx->freeze_permanent ? "FREEZE_PERM" : "FREEZE") : "",
                        ctx->ocg.active, ctx->ocg.n_clusters);
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

            /* ── P0-5: 环境安静检测 (治"噪声消失后反相声残留/嗡嗡声") ──
               判据 (2026-08-11 阶段④ + 2026-08-13 阶段⑤修正): 参考麦塌底
               (ref < quiet_ref_max = 无真实噪声进入参考麦) 且"曾经有大噪声最近才停"
               (quiet_since_active <= quiet_ref_memory)。深对消/1000Hz 天花板时
               ref 都停在 0.048 (音仍进参考麦), 只有噪声真停才塌到 0.040.
               阶段⑤加 err_ref>quiet_err_ref (默认 1.5): ref 门槛余量仅 7% (0.048 vs 0.045),
               污染 FIR 把 ref 压低即误触发 (实机 cb≈12100 两次确定性误判). err_ref 分离
               "深对消纯音" (0.7-1.1, err 被压) 与 "噪声真停" (≈2.4, err 被 anti 自身输出
               主导) — 只有后者通过. anti 仍在输出 (anti > quiet_anti_rms = 有残留要消)
               → 持续 quiet_hold 秒 → 进入安静模式: 回调冻结梯度 + 逐样本衰减 Wc, 反噪声平滑消退.
               不用 NR 判据 (阶段③ 实测证伪): 反噪声还开着时它声学上仍在"抵消"
               底噪, NR 保持 8-12dB 不塌, 用它当门槛 → 安静永远进不去, 残留不消.
               不用 err 判据 (同实测): 反噪声衰减过渡期 err 会先冲到 0.08 再塌,
               用它当门槛会把 3s 计数打断, 安静迟触发.
               quiet_since_active 守卫 (阶段④ 实测加): 马路噪音宽带频谱下同样响度
               ref 只有 0.038 (低于 0.045 门槛), 被绝对阈值误判为"噪声停了" → anti
               被砍到 0 → 全程 0dB。守卫要求 ref 曾在 quiet_ref_memory 秒内高于门槛
               (真有大噪声), 才允许判定"噪声消失"。弱噪声 (ref 一直低于门槛) 不再
               触发安静, 反相继续生长。
               退出: ref_rms 重回安静基准的 quiet_exit 倍 (路噪回归) 或 err_rms 重回
               安静期 err 基准的 quiet_err_exit 倍 (纯音回归 — 纯音 ref 只比底噪高
               20%, ref 判据够不着, 靠 err 先冲高 0.026→0.07+ 触发) → 重建 INIT. */
            /* ref 活跃追踪: 每 1s 更新. ref 高于门槛=有真噪声, 清零; 否则累计秒数. */
            if (ctx->ref_rms > cfg.quiet_ref_max)
                ctx->quiet_since_active = 0;
            else if (ctx->quiet_since_active < 0x7fffffff)
                ctx->quiet_since_active++;
            if (ctx->quiet_active) {
                /* 安静中: 持续追踪 ref/err 基准 (anti 衰减后 ref/err 回落至真底噪), 检测噪声回归.
                   quiet_floor 慢 EMA: 反噪声消退期间 ref 仍含污染, 慢跟随防误判回归 */
                ctx->quiet_floor = 0.9f * ctx->quiet_floor + 0.1f * ctx->ref_rms;
                ctx->quiet_err_floor = 0.9f * ctx->quiet_err_floor + 0.1f * ctx->err_rms;
                if (ctx->ref_rms > ctx->quiet_floor * cfg.quiet_exit
                    || ctx->err_rms > ctx->quiet_err_floor * cfg.quiet_err_exit) {
                    /* 保持 quiet_active=1 到重建块统一清除: 本秒派发块被抑制,
                       避免用旧的 quiet 期 CNN 方向误触发一次 RESET */
                    ctx->reinit_needed = 1;
                    if (ctx->log_file)
                        fprintf(ctx->log_file, "# EVENT: quiet exit ref=%.4f>%.4fx%.0f err=%.4f>%.4fx%.0f\n",
                                ctx->ref_rms, ctx->quiet_floor, cfg.quiet_exit,
                                ctx->err_rms, ctx->quiet_err_floor, cfg.quiet_err_exit);
                    printf("[QUIET] exit ref=%.4f>%.4fx%.0f err=%.4f>%.4fx%.0f — 噪声回归, 重建降噪\n",
                           ctx->ref_rms, ctx->quiet_floor, cfg.quiet_exit,
                           ctx->err_rms, ctx->quiet_err_floor, cfg.quiet_err_exit);
                }
            } else if (ctx->anti_rms > cfg.quiet_anti_rms
                       && ctx->ref_rms < cfg.quiet_ref_max
                       && (ctx->err_rms / (ctx->ref_rms + 1e-6f)) > cfg.quiet_err_ref
                       && ctx->quiet_since_active <= cfg.quiet_ref_memory
                       && ctx->ramp_cnt == 0 && ctx->cold_hold == 0
                       && !ctx->safety_mute && !ctx->peak_mute) {
                if (++ctx->quiet_sec >= cfg.quiet_hold) {
                    ctx->quiet_sec = 0;
                    ctx->quiet_active = 1;
                    ctx->quiet_floor = ctx->ref_rms;
                    ctx->quiet_err_floor = ctx->err_rms;
                    if (ctx->log_file)
                        fprintf(ctx->log_file, "# EVENT: QUIET anti=%.3f NR=%.1f ref=%.4f err=%.4f x%ds\n",
                                ctx->anti_rms, ctx->nr_level, ctx->ref_rms, ctx->err_rms, cfg.quiet_hold);
                    printf("[QUIET] 噪声消失: anti=%.3f NR=%.1fdB ref=%.4f err=%.4f (无对消效果) — 反噪声消退\n",
                           ctx->anti_rms, ctx->nr_level, ctx->ref_rms, ctx->err_rms);
                }
            } else {
                ctx->quiet_sec = 0;
            }

            /* 去场景层模式派发: reset=OCG 簇索引变化 → CrossFader 过渡到新 Wc;
               continuous=永不重置 (CNN 仅首秒 INIT 一次).
               P0-5: quiet_active 期间跳过 — 反噪声在消退, 不允许重置重新拉高 Wc.
               OCG (默认关闭 — 2026-08-10 P0-3 实测证伪): 纯音深对消下增益在两
               个模式间震荡 (帧间 cos 0.65-0.71), OCG 簇分配随震荡翻转 (0↔1↔2),
               每次翻转都 RESET → 深对消被反复打断 (最高仅 ~18dB, 而 OCG 关可达
               27.5dB), err 锯齿 0.05↔0.18. OCG 关 (GFANC_OCG=0) 回退旧闸门
               cos(anchor,cur)<τ, 已验证稳定. OCG 代码保留, 待簇判据更鲁棒后再评估. */
            if (!ctx->quiet_active && cfg.gfanc_mode == 1) {   /* reset (安静期不派发) */
                if (cfg.ocg_enable) {
                    /* OCG 已有 ocg_hold 持续性判据, 簇索引变化才切换 */
                    if (ocg_step(&ctx->ocg, gains))
                        apply_reset(ctx, cos_sim, gains, 1);
                } else {
                    /* cos 闸门 + RESET 迟滞: cos 连续 reset_hyst 秒 < τ 才触发软重锚定.
                       场景切换提交 wc_cur (CNN 已算好的新滤波器), FxLMS 从正确起点微调.
                       迟滞挡 1 秒瞬态; 软重锚定无 cold_hold/mute 打断. */
                    if (cos_sim < cfg.switch_threshold) {
                        if (++ctx->reset_pending >= cfg.reset_hyst) {
                            apply_reset(ctx, cos_sim, gains, 0);
                            ctx->reset_pending = 0;
                        }
                    } else {
                        ctx->reset_pending = 0;
                    }
                }
            }                                   /* continuous: 不动作 */
        }
        memcpy(ctx->sc.prev_gains, gains, K * sizeof(float));
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
        for (int i = 0; i < E*S; i++) free(ctx->bp_fx[i].delay_line);  /* R-58-10 */
        if (ctx->sec_firs) {
            for (int i = 0; i < E*S; i++) free(ctx->sec_firs[i].delay_line);
            free(ctx->sec_firs);
        }
        free(ctx->sec_coeffs);
        if (cfg.sec_online_mu > 0) sec_online_free(&ctx->sec_on);
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
