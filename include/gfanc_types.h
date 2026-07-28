/** 核心类型 + 分级日志 + 集中参数 — 被所有模块共用. */
#ifndef GFANC_TYPES_H
#define GFANC_TYPES_H

#include <stdio.h>
#include <stdlib.h>

typedef float gfanc_float_t;

/* FIR 滤波器 (double 精度延迟线, 匹配 Python scipy float64) */
typedef struct {
    gfanc_float_t *coeffs;
    double        *delay_line;
    int            n_taps;
    int            ptr;
} fir_filter_t;

/* ── 分级日志宏 (CR-20) ── */
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  printf(  "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) /* disabled in release */ ((void)0)

/* ── 集中参数 (A2): 所有可调参数一处管理 ── */
typedef struct {
    /* 音频链 */
    int   fs_hw, fs_anc;         /* 硬件/处理采样率 */
    float mic_pre_gain;          /* 输入数字预增益 (env: GFANC_MIC_GAIN) */

    /* ANC 自适应 */
    float step_size;             /* LMS 步长 μ */
    float leak;                  /* 泄漏因子 */
    int   fade_len;              /* CrossFader 过渡样本数 */
    int   ramp_ms;               /* 冷启动 ramp 时长 ms */
    int   mute_hold_ms;          /* safety_mute 抑制时长 ms */

    /* 安全保护 */
    float freeze_ratio;          /* max|Wc| > ratio×stub_rms → 冻结 */
    float switch_threshold;      /* cos_sim 场景切换阈值 */
    float nr_converge_db;        /* 收敛判定 NR 阈值 dB */
    int   freeze_retry_sec;      /* freeze 后尝试解冻的秒数 */
    float diverge_anti_rms;      /* anti_rms 连续3s超此值 → Wc 发散救援 (env: GFANC_DIVERGE_ANTI) */

    /* 啸叫检测 */
    float hw_thresh_db;          /* 峰均值比阈值 */
    int   hw_persist;            /* 确认帧数 */
    int   hw_release;            /* 释放帧数 */
    float hw_notch_r;            /* 陷波带宽 */
    int   hw_min_hold;           /* 陷波最小保持帧数 */

    int   dsp_delay;             /* Ŝ 前补零延迟 (env: GFANC_DSP_DELAY) */
} gfanc_config_t;

/* 默认配置 (与当前 #define 一致)
   2026-07-28 稳定性批次: gain 3.0→1.0 (降环路增益), step 1e-6→5e-7,
   leak 1e-6→5e-6 (抑制安静期漂移), 新增 diverge_anti_rms=0.25 */
#define GFANC_CONFIG_DEFAULT { \
    48000, 16000,      /* fs_hw, fs_anc */ \
    1.0f,               /* mic_pre_gain */ \
    0.0000005f, 5e-6f,  /* step_size, leak */ \
    1600, 400, 1500,    /* fade_len, ramp_ms, mute_hold_ms */ \
    30.0f, 0.8f, 3.0f, /* freeze_ratio, switch_threshold, nr_converge_db */ \
    60,                 /* freeze_retry_sec */ \
    0.25f,              /* diverge_anti_rms */ \
    15.0f, 4, 8, 0.96f, /* hw_thresh_db, hw_persist, hw_release, hw_notch_r */ \
    32,                 /* hw_min_hold */ \
    16                  /* dsp_delay */ \
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
    if ((s = getenv("GFANC_DIVERGE_ANTI"))) cfg->diverge_anti_rms = (float)atof(s);
}

#endif
