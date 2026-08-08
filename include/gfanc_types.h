/** 核心类型 + 分级日志 + 集中参数 — 被所有模块共用. */
#ifndef GFANC_TYPES_H
#define GFANC_TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>  /* gf_log (ADV-D4: LOG 宏去 GCC ##__VA_ARGS__ 扩展) */
#include <string.h>  /* gfanc_config_load_env: GFANC_MODE strcmp */

typedef float gfanc_float_t;

/* ── 维度编译期上限 (R-22: 消除 VLA, R-29: 统一硬编码源头) ── */
#define GFANC_E_MAX  5    /* 误差麦最大数量 (当前 3, 目标 1×5×4=5) */
#define GFANC_S_MAX  4    /* 扬声器最大数量 (当前 2, 目标 1×5×4=4) */
#define GFANC_L_MAX  2048 /* 滤波器最大长度 (当前 1024) */
#define GFANC_C_MAX  15   /* 子滤波器最大数量 (SC_C, 已存在于 scene_controller.h) */

/* FIR 滤波器.
 *
 *  延迟线精度:
 *    - 默认 double (匹配 Python scipy float64, x86/A72 硬件支持).
 *    - 定义 GFANC_FLOAT_DELAY 切换为 float32 (Phase-3 MCU/DSP:
 *      Cortex-M 无双精度 FPU, double 软浮点 10-30× 减速 + RAM 翻倍).
 *    - 建议 x86/A72 保持 double; 仅在 MCU 上启用此开关. */
#ifdef GFANC_FLOAT_DELAY
typedef float  gfanc_delay_t;
#else
typedef double gfanc_delay_t;
#endif

typedef struct {
    gfanc_float_t *coeffs;
    gfanc_delay_t *delay_line;
    int            n_taps;
    int            ptr;
} fir_filter_t;

/* ── 分级日志宏 (CR-20) — C99 标准兼容 (ADV-D4: 去掉 GCC `##__VA_ARGS__` 扩展).
   实现: 变参函数 + 变参宏, 支持任意参数 (含零参数), MSVC/IAR/Keil 兼容.
   用法: LOG_INFO("msg") 或 LOG_INFO("val=%d", x) ── */
static inline void gf_log(FILE *fp, const char *tag, const char *fmt, ...)
{
    va_list ap;
    fputs(tag, fp);
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
}
#define LOG_ERROR(...) gf_log(stderr, "[ERROR] ", __VA_ARGS__)
#define LOG_WARN(...)  gf_log(stderr, "[WARN]  ", __VA_ARGS__)
#define LOG_INFO(...)  gf_log(stdout, "[INFO]  ", __VA_ARGS__)
#define LOG_DEBUG(...) /* disabled in release */ ((void)0)

/* ── 算法常数 (非用户调节, 表达物理/设计约束) ── */
#define TARGET_REF_RMS  0.03f   /* 自动增益标定目标 ref RMS (-30dBFS) */

/* ── 集中参数 (A2): 所有可调参数一处管理 ── */
typedef struct {
    /* 音频链 */
    int   fs_hw, fs_anc;         /* 硬件/处理采样率 */
    float mic_pre_gain;          /* 输入数字预增益 (env: GFANC_MIC_GAIN) */

    /* ANC 自适应 */
    float step_size;             /* LMS 步长 μ */
    float leak;                  /* 泄漏因子 */
    float wc_rms_target;         /* Wc 初始 RMS 目标 (env: GFANC_WC_TARGET) */
    int   fade_len;              /* CrossFader 过渡样本数 */
    int   ramp_ms;               /* 冷启动 ramp 时长 ms */
    int   mute_hold_ms;          /* safety_mute 抑制时长 ms */

    /* 安全保护 */
    float freeze_ratio;          /* max|Wc| > ratio×stub_rms → 冻结 */
    float switch_threshold;      /* cos_sim 场景切换阈值 */
    float nr_converge_db;        /* 收敛判定 NR 阈值 dB */
    int   freeze_retry_sec;      /* freeze 后尝试解冻的秒数 */
    float diverge_anti_rms;      /* anti_rms 连续3s超此值 → Wc 发散救援 (env: GFANC_DIVERGE_ANTI) */
    /* wc_gain 已移除: Wc RMS 始终按 stub_rms×1.0 构造, LMS 自适应收敛到正确增益 */

    /* 啸叫检测 */
    float hw_thresh_db;          /* 峰均值比阈值 */
    int   hw_persist;            /* 确认帧数 */
    int   hw_release;            /* 释放帧数 */
    float hw_notch_r;            /* 陷波带宽 */
    int   hw_min_hold;           /* 陷波最小保持帧数 */

    int   dsp_delay;             /* Ŝ 前补零延迟 (env: GFANC_DSP_DELAY) */
    int   embed_delay_ms;        /* 嵌入式信号链处理延迟 ADC+DSP+DAC (env: GFANC_EMBED_DELAY_MS, 默认3ms).
                                    离线 main.c pad Ŝ 模拟因果缺口; 0=实时PC等效(无处理延迟). */
    float sec_online_mu;         /* 在线Ŝ辨识 NLMS 步长, 0=禁用 (env: GFANC_SEC_MU) */
    float wc_cold_start;         /* 首次场景Wc衰减系数, 0.3=从30%开始收敛防overshoot (env: GFANC_WC_COLD) */

    /* 去场景层 (gfanc-direct-weight): 无场景切换, CNN 只产 Wc.
       模式: 0=continuous (CNN 仅首秒初始化, FxNLMS 永不重置),
             1=reset (多质心 OCG 簇索引闸门 → CrossFader 重置). */
    int   gfanc_mode;          /* env: GFANC_MODE=reset|continuous */

    /* OCG 在线聚类闸门 (ICASSP 2026, 详见 ocg.h):
       在增益域对 CNN 输出做多质心聚类, 仅簇索引变化才更换滤波器 —
       抑制簇内抖动/慢漂移导致的反复重置, 保护 FxNLMS 收敛.
       τ 复用 switch_threshold (GFANC_RESET_THRESH). */
    int   ocg_enable;          /* 1=OCG 闸门 (默认), 0=旧 cos(anchor,cur)<τ (env: GFANC_OCG) */
    float ocg_alpha;           /* 质心 EMA 漂移系数 (env: GFANC_OCG_ALPHA, 默认 0.1) */
    int   ocg_max_clusters;    /* 簇上限 (env: GFANC_OCG_CLUSTERS, 默认 8) */
} gfanc_config_t;

/* 默认配置 (与当前 #define 一致)
   2026-07-28 稳定性批次: gain 3.0→1.0 (降环路增益), step 1e-6→5e-7,
   leak 1e-6→5e-6 (抑制安静期漂移), 新增 diverge_anti_rms=0.25 */
#define GFANC_CONFIG_DEFAULT { \
    48000, 16000,      /* fs_hw, fs_anc */ \
    1.0f,               /* mic_pre_gain */ \
    1e-7f, 5e-7f,       /* step_size, leak. 基准值, 运行时会根据 Ŝ RMS 自动缩放.
                           leak 5e-6→5e-7 (2026-08-05): 弱信号下 leak 压死 Wc 生长
                           (离线 1.4dB→13dB 的收敛杠杆), 降一档让 Wc 长起来 */ \
    0.01f,              /* wc_rms_target (初始Wc幅度, env: GFANC_WC_TARGET).
                           Python Ŝ (RMS≈0.039): ref=0.04→anti≈0.013, 安静且稳定 */ \
    1600, 400, 1500,    /* fade_len, ramp_ms, mute_hold_ms */ \
    30.0f, 0.8f, 3.0f, /* freeze_ratio, switch_threshold, nr_converge_db */ \
    60,                 /* freeze_retry_sec */ \
    0.25f,              /* diverge_anti_rms */ \
    12.0f, 4, 8, 0.96f, /* hw_thresh_db(12), hw_persist, hw_release, hw_notch_r */ \
    32,                 /* hw_min_hold */ \
    0,                  /* dsp_delay (Ŝ peak@tap10 已含声学延迟) */ \
    0,                  /* embed_delay_ms (R-58-8: 默认0 — 训练世界无此延迟! 3ms 加在 Ŝ 上
                           → anti 相位错位 48 样本 → 自适应正反馈发散; 需评估嵌入式目标时
                           GFANC_EMBED_DELAY_MS 显式开启) */ \
    5e-6f,              /* sec_online_mu (在线Ŝ辨识步长, 0=禁用) */ \
    0.3f,               /* wc_cold_start (首次场景Wc衰减, 0.3=30%, 1.0=关闭) */ \
    1,                  /* gfanc_mode: 默认 reset (去场景层后主模式), GFANC_MODE=continuous 切换 */ \
    1, 0.1f, 8,         /* ocg_enable, ocg_alpha, ocg_max_clusters (OCG 聚类闸门) */ \
}

/* 从环境变量覆盖可调参数 (GFANC_MIC_GAIN, GFANC_STEP 等) */
static void gfanc_config_load_env(gfanc_config_t *cfg) {
    const char *s;
    if ((s = getenv("GFANC_MIC_GAIN")))  cfg->mic_pre_gain = (float)atof(s);
    if ((s = getenv("GFANC_STEP")))      cfg->step_size    = (float)atof(s);
    if ((s = getenv("GFANC_RAMP_MS")))   cfg->ramp_ms      = atoi(s);
    if ((s = getenv("GFANC_MUTE_MS")))   cfg->mute_hold_ms = atoi(s);
    if ((s = getenv("GFANC_FADE_LEN")))  cfg->fade_len     = atoi(s);
    if ((s = getenv("GFANC_LEAK")))         cfg->leak         = (float)atof(s);
    if ((s = getenv("GFANC_FREEZE_RATIO"))) cfg->freeze_ratio = (float)atof(s);
    if ((s = getenv("GFANC_DSP_DELAY")))   cfg->dsp_delay    = atoi(s);
    if ((s = getenv("GFANC_EMBED_DELAY_MS"))) cfg->embed_delay_ms = atoi(s);
    if ((s = getenv("GFANC_DIVERGE_ANTI"))) cfg->diverge_anti_rms = (float)atof(s);
    if ((s = getenv("GFANC_WC_TARGET")))  cfg->wc_rms_target = (float)atof(s);
    if ((s = getenv("GFANC_SEC_MU")))     cfg->sec_online_mu  = (float)atof(s);
    if ((s = getenv("GFANC_WC_COLD")))   cfg->wc_cold_start  = (float)atof(s);
    if ((s = getenv("GFANC_HW_THRESH"))) cfg->hw_thresh_db   = (float)atof(s);
    if ((s = getenv("GFANC_MODE"))) {
        if (!strcmp(s, "continuous")) cfg->gfanc_mode = 0;
        else if (!strcmp(s, "reset")) cfg->gfanc_mode = 1;
    }
    if ((s = getenv("GFANC_RESET_THRESH"))) cfg->switch_threshold = (float)atof(s);
    if ((s = getenv("GFANC_OCG"))) {
        if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false")) cfg->ocg_enable = 0;
        else if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true")) cfg->ocg_enable = 1;
    }
    if ((s = getenv("GFANC_OCG_ALPHA")))    cfg->ocg_alpha = (float)atof(s);
    if ((s = getenv("GFANC_OCG_CLUSTERS"))) cfg->ocg_max_clusters = atoi(s);
    /* wc_gain 已移除: Wc RMS 始终按 stub_rms×1.0 构造, LMS 自适应收敛到正确增益 */
    /* if ((s = getenv("GFANC_WC_GAIN"))) cfg->wc_gain = (float)atof(s); */
}

#endif
