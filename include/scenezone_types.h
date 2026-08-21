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
    int   reset_hyst;            /* cos<τ 连续秒数才 RESET 迟滞 (env: GFANC_RESET_HYST, 默认 1) */
    float nr_converge_db;        /* 收敛判定 NR 阈值 dB */
    int   freeze_retry_sec;      /* freeze 后尝试解冻的秒数 */
    float diverge_anti_rms;      /* anti_rms 连续3s超此值 → Wc 发散救援 (env: GFANC_DIVERGE_ANTI) */
    float diverge_err_ratio;     /* P0-4: 救援须 err_rms/ref_rms 同时 > 此值 (对消失败) 才触发,
                                    防深对消时 anti 高但 err 已被压住 → 误回滚健康 Wc (env: GFANC_DIVERGE_ERR_RATIO) */
    /* wc_gain 已移除: Wc RMS 始终按 stub_rms×1.0 构造, LMS 自适应收敛到正确增益 */

    /* 啸叫检测 */
    float hw_thresh_db;          /* 峰均值比阈值 */
    int   hw_persist;            /* 确认帧数 */
    int   hw_release;            /* 释放帧数 */
    float hw_notch_r;            /* 陷波带宽 */
    int   hw_min_hold;           /* 陷波最小保持帧数 */

    int   dsp_delay;             /* Ŝ 前补零延迟 (env: GFANC_DSP_DELAY) */
    int   embed_delay_ms;        /* 嵌入式信号链处理延迟 ADC+DSP+DAC (env: GFANC_EMBED_DELAY_MS, 默认0ms — R-58-8).
                                    离线 main.c pad Ŝ 模拟因果缺口; 0=实时PC等效(无处理延迟). */
    float sec_online_mu;         /* 在线Ŝ辨识 NLMS 步长, 0=禁用 (env: GFANC_SEC_MU) */
    float wc_cold_start;         /* 首次场景Wc衰减系数, 0.3=从30%开始收敛防overshoot (env: GFANC_WC_COLD) */

    /* 去场景层 (scenezone-anc): 无场景切换, CNN 只产 Wc.
       模式: 0=continuous (CNN 仅首秒初始化, FxNLMS 永不重置),
             1=reset (多质心 OCG 簇索引闸门 → CrossFader 重置). */
    int   gfanc_mode;          /* env: GFANC_MODE=reset|continuous */

    /* 双模式 (方案C, 2026-08-21, 详见 docs/无误差麦方案_与SFANC对照_路线分析.md):
       anc_mode 0=adapt 闭环 (现状默认: FxLMS + 误差麦),
                 1=fixed 开环 (µ=0 无误差麦 — 生成式 SFANC: CNN 仍每秒产 Wc,
                 派发恒走 fxnlms_forward_rt, 无梯度/无Wc变更/无在线Ŝ).
       全自动标定 (对标 SFANC 就地训滤波器): adapt 收敛后 Ctrl+C 自动保存
       data/wc_fixed.bin; fixed 启动自动加载 (CNN 不覆盖), 无文件则退回 CNN 生成式. */
    int   anc_mode;          /* 0=adapt 闭环(默认), 1=fixed 开环µ=0 (env: GFANC_ANC_MODE=adapt|fixed) */

    /* OCG 在线聚类闸门 (ICASSP 2026, 详见 ocg.h):
       在增益域对 CNN 输出做多质心聚类, 仅簇索引变化才更换滤波器 —
       抑制簇内抖动/慢漂移导致的反复重置, 保护 FxNLMS 收敛.
       τ 独立 (P0-1: 解耦 switch_threshold, 不再复用 GFANC_RESET_THRESH) */
    int   ocg_enable;          /* 1=OCG 闸门, 0=旧 cos(anchor,cur)<τ (env: GFANC_OCG) */
    float ocg_tau;             /* 簇半径阈值: cos(g',c)>=τ 归入, 否则新建簇 (env: GFANC_OCG_TAU, 默认 0.8) */
    float ocg_alpha;           /* 质心 EMA 漂移系数 (env: GFANC_OCG_ALPHA, 默认 0.1) */
    int   ocg_max_clusters;    /* 簇上限 (env: GFANC_OCG_CLUSTERS, 默认 8) */
    int   ocg_hold;            /* 持续性判据: 候选簇需连续命中帧数 (1Hz→帧数≈秒数, 默认 3)
                                  (env: GFANC_OCG_HOLD, 默认 3; <=1 回退立即切换) */

    /* CNN 增益时间平滑 (P0-2, 治纯音带跨秒翻转抖动 — bands 2/6/14/17/19):
       自适应 EMA — 帧间 cos < switch (场景真切换) → β=1 立即跟随 (无延迟);
       否则 β 慢速平滑, 吸收带选择抖动 (OCG/cos 闸门不再被抖动触发). */
    float gain_smooth_beta;    /* EMA 平滑系数 (env: GFANC_GAIN_SMOOTH, 默认 0.5; 1=关闭平滑) */
    float gain_smooth_switch;  /* 旁路阈值: 帧间增益 cos 低于此视为场景切换, β 强制 1 (默认 0.85) */

    /* 环境安静检测 (P0-5, 治"噪声消失后反相声残留/嗡嗡声") 阶段③ 判据 (2026-08-11):
       唯一可靠信号 = 参考麦塌底 (ref<quiet_ref_max = 无真实噪声进参考麦). 深对消/
       1000Hz 天花板时 ref 都停在 0.048, 只有噪声真停才塌到 0.040. anti 仍在输出
       (anti>quiet_anti_rms = 有残留反相要消) 持续 quiet_hold 秒 → 判定噪声消失 →
       冻结梯度 + 逐样本衰减 Wc, 反噪声平滑消退.
       实测证伪 NR/err 门 (不再用于进入): 反噪声还开着时声学上仍在"抵消"底噪,
       NR 保持 8-12dB 不塌 → 用它当门槛安静永远进不去; 反噪声衰减过渡期 err 先
       冲到 0.08 再塌 → err 门把 3s 计数打断. quiet_nr_db/quiet_err_max 字段保留
       仅为 env 兼容, 不再参与判定.
       退出: ref 重回安静基准的 quiet_exit 倍 (路噪回归) 或 err 重回安静期 err
       基准的 quiet_err_exit 倍 (纯音回归 — 纯音 ref 只比底噪高 20%, ref 判据
       够不着, 靠 err 先冲高触发) → 重建 INIT. */
    float quiet_anti_rms;      /* anti 高于此才考虑安静判定 (env: GFANC_QUIET_ANTI, 默认 0.02) */
    float quiet_nr_db;         /* [弃用, 仅 env 兼容] 原 NR 门 (阶段③ 实测 NR 不塌, 已移出判据) */
    int   quiet_hold;          /* 连续秒数 (env: GFANC_QUIET_HOLD, 默认 3) */
    float quiet_exit;          /* ref 重回安静基准×此值 → 退出安静重建 (env: GFANC_QUIET_EXIT, 默认 1.5) */
    float quiet_ref_max;       /* 参考麦残差低于此才算"无噪声进入" (env: GFANC_QUIET_REF,
                                  默认 0.042): 深对消/1000Hz 时 ref=0.048 仍高, 被此门挡住 */
    float quiet_err_ref;       /* 进入安静需 err/ref 高于此 (env: GFANC_QUIET_ERR_REF, 默认 1.5):
                                  阶段④修正 (2026-08-13) — ref 门槛余量窄 (音 ref 0.048
                                  vs 门槛 0.042, 底噪 0.040), 污染 FIR 把 ref 压低即误触发.
                                  err_ref 门提供 ~2× 余量: 深对消纯音 err 被压到 ref 量级以下
                                  (err_ref≈0.7-1.1) → 挡住误判; 噪声真停时 anti 失去对消对象,
                                  err 被自身输出主导 (err≈anti×G≈0.095 vs ref 塌底 0.040 →
                                  err_ref≈2.4) → 通过. 与 ref 塌底 + anti 大 + "曾有噪声" 组合,
                                  "深对消纯音" 与 "噪声真停" 才可分离. */
    float quiet_err_max;       /* [弃用, 仅 env 兼容] 原 err 门 (阶段③ 实测反噪声过渡期 err
                                  先冲高打断计数, 已移出判据) */
    float quiet_err_exit;      /* 安静中 err 重回 err 基准×此值 → 退出重建 (env: GFANC_QUIET_ERR_EXIT,
                                  默认 2.0): 治纯音回归 ref 判据够不着时靠 err 跳变退出 */
    int   quiet_ref_memory;    /* 距 ref 上次高于 quiet_ref_max 的秒数上限: 超过视为"从来就
                                  安静", 不判定噪声消失 — 防宽带弱噪声 (ref 一直低于门槛,
                                  如马路噪音 ref≈0.038) 被绝对阈值误判而砍掉反相 (env:
                                  GFANC_QUIET_MEMORY, 默认 20) */
} gfanc_config_t;

/* 默认配置 (与当前 #define 一致)
   2026-07-28 稳定性批次: gain 3.0→1.0 (降环路增益), step 1e-6→5e-7,
   leak 1e-6→5e-6 (抑制安静期漂移), 新增 diverge_anti_rms=0.25 */
#define GFANC_CONFIG_DEFAULT { \
    48000, 16000,      /* fs_hw, fs_anc */ \
    1.0f,               /* mic_pre_gain */ \
    1e-7f, 5e-7f,       /* step_size, leak. step 运行时按 Ŝ RMS 自动缩放;
                           leak 固定不缩放 (2026-08-13): leak 是泄漏因子, 与 Ŝ 幅度无关,
                           曾随 s_scale×0.489 → 2.4e-7 不足以抑制梯度噪声 → Wc 无界膨胀.
                           leak 5e-6→5e-7 (2026-08-05): 弱信号下 leak 压死 Wc 生长 */ \
    0.01f,              /* wc_rms_target (初始Wc幅度, env: GFANC_WC_TARGET).
                           Python Ŝ (RMS≈0.039): ref=0.04→anti≈0.013, 安静且稳定 */ \
    1600, 400, 1500,    /* fade_len, ramp_ms, mute_hold_ms */ \
    30.0f, 0.7f, 1, 3.0f, /* freeze_ratio, switch_threshold, reset_hyst, nr_converge_db.
                           switch_threshold 0.6→0.7 (2026-08-14): 软重锚定 — 500Hz 切换
                           cos 0.63~0.67 需触发提交 wc_cur; 250Hz 深对消 cos 0.88+ 不误触.
                           0.6 下 500Hz 切换永不触发 → FxLMS 小步长硬爬 2~3s (问题1).
                           reset_hyst=1 (2026-08-14): 软重锚定无 cold_hold/mute 打断,
                           迟滞只需挡 1 秒瞬态; latency = 1s CNN + 1s 迟滞 + crossfade ≈ 2s. */ \
    60,                 /* freeze_retry_sec */ \
    0.25f,              /* diverge_anti_rms */ \
    0.6f,               /* diverge_err_ratio (P0-4: anti 高且 err/ref>0.6 才救援; 深对消 err/ref≈0.4 不误杀) */ \
    12.0f, 4, 8, 0.96f, /* hw_thresh_db(12), hw_persist, hw_release, hw_notch_r */ \
    32,                 /* hw_min_hold */ \
    0,                  /* dsp_delay (Ŝ peak@tap10 已含声学延迟) */ \
    0,                  /* embed_delay_ms (R-58-8: 默认0 — 训练世界无此延迟! 3ms 加在 Ŝ 上
                           → anti 相位错位 48 样本 → 自适应正反馈发散; 需评估嵌入式目标时
                           GFANC_EMBED_DELAY_MS 显式开启) */ \
    5e-6f,              /* sec_online_mu (在线Ŝ辨识步长, 0=禁用) */ \
    0.3f,               /* wc_cold_start (首次场景Wc衰减, 0.3=30%, 1.0=关闭) */ \
    1,                  /* gfanc_mode: 默认 reset (去场景层后主模式), GFANC_MODE=continuous 切换 */ \
    0,                  /* anc_mode (方案C双模式: 默认 adapt 闭环, GFANC_ANC_MODE=fixed 切开环) */ \
    0, 0.8f, 0.1f, 8, 3, /* ocg_enable, ocg_tau, ocg_alpha, ocg_max_clusters, ocg_hold.
                           ocg_enable 默认 0 (2026-08-10 P0-3 证伪): 纯音深对消下增益
                           双模震荡 → 簇翻转 → 每次翻转 RESET, 深对消丢失 (开≈18dB vs 关 27.5dB).
                           2026-08-11 修复: 实机 CNN 稳定 (前提① cos>0.95 达成) +
                           ocg_hold 持续性判据 (前提②: 候选簇连续 3s 命中才切换, 治抖动
                           翻转) → 实机验证 GFANC_OCG=1 后再翻转默认开. */ \
    0.5f, 0.85f,        /* gain_smooth_beta, gain_smooth_switch (P0-2 CNN 增益自适应平滑).
                           帧间 cos<0.85(真场景切换)→β=1 立即跟随; 抖动→β=0.5 慢速平滑 */ \
    0.02f, 8.0f, 3, 1.5f, 0.042f, 1.5f, 0.05f, 2.0f, 20, /* quiet_anti_rms, quiet_nr_db(弃用),
                           quiet_hold, quiet_exit, quiet_ref_max, quiet_err_ref,
                           quiet_err_max(弃用), quiet_err_exit, quiet_ref_memory.
                           P0-5 阶段④: anti>0.02 且 ref<0.042 且 err_ref>1.5 且"ref 曾于
                           quiet_ref_memory 秒内高于门槛" 持续 3s → 判定噪声消失 → 冻结+衰减 Wc.
                           2026-08-14 阶段⑥: quiet_hold 1→3 + quiet_ref_max 0.045→0.042 —
                           实测 Ŝ 下 500→250Hz 切换 anti 失配→refFilt 瞬态 dip 0.0464→0.0445
                           跌破 0.045 门槛 (余量仅 2~7%) + hold=1 秒 → 误判"噪声消失", 反噪声
                           被清后噪声仍在 ~2.4s 全程 0dB. 降门槛拉开余量 + hold 回 3 抗瞬态.
                           quiet_err_ref (阶段⑤修正): 深对消纯音 err_ref≈0.7-1.1 被挡, 只有
                           噪声真停 (err 被 anti 自身输出主导, err_ref≈2.4) 才通过.
                           NR/err 门已移出判据 (实测 NR 噪声停后不塌 8-12dB、err 过渡期
                           先冲高). quiet_ref_memory 守卫治阶段④ 实测: 宽带弱噪声
                           (ref≈0.038 < 门槛) 被绝对阈值误判"噪声消失" → 反相被砍 → 0dB.
                           退出: ref 重回 1.5× 或 err 重回 2.0× 安静基准 → 重建.
                           quiet_anti_rms 0.05→0.02: 残余 anti≈0.03 也能抓到. */ \
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
    if ((s = getenv("GFANC_DIVERGE_ERR_RATIO"))) cfg->diverge_err_ratio = (float)atof(s);
    if ((s = getenv("GFANC_WC_TARGET")))  cfg->wc_rms_target = (float)atof(s);
    if ((s = getenv("GFANC_SEC_MU")))     cfg->sec_online_mu  = (float)atof(s);
    if ((s = getenv("GFANC_WC_COLD")))   cfg->wc_cold_start  = (float)atof(s);
    if ((s = getenv("GFANC_HW_THRESH"))) cfg->hw_thresh_db   = (float)atof(s);
    if ((s = getenv("GFANC_MODE"))) {
        if (!strcmp(s, "continuous")) cfg->gfanc_mode = 0;
        else if (!strcmp(s, "reset")) cfg->gfanc_mode = 1;
    }
    /* 双模式 (方案C): GFANC_ANC_MODE=adapt|fixed.
       标定滤波器 data/wc_fixed.bin 全自动: adapt 收敛后 Ctrl+C 自动保存,
       fixed 启动自动加载 (无 GFANC_FIXED_SOURCE / GFANC_SNAPSHOT_WC). */
    if ((s = getenv("GFANC_ANC_MODE"))) {
        if (!strcmp(s, "fixed"))     cfg->anc_mode = 1;
        else if (!strcmp(s, "adapt")) cfg->anc_mode = 0;
    }
    if ((s = getenv("GFANC_RESET_THRESH"))) cfg->switch_threshold = (float)atof(s);
    if ((s = getenv("GFANC_RESET_HYST")))   cfg->reset_hyst = atoi(s);
    if ((s = getenv("GFANC_OCG"))) {
        if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false")) cfg->ocg_enable = 0;
        else if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true")) cfg->ocg_enable = 1;
    }
    if ((s = getenv("GFANC_OCG_TAU")))     cfg->ocg_tau     = (float)atof(s);
    if ((s = getenv("GFANC_OCG_ALPHA")))    cfg->ocg_alpha = (float)atof(s);
    if ((s = getenv("GFANC_OCG_CLUSTERS"))) cfg->ocg_max_clusters = atoi(s);
    if ((s = getenv("GFANC_OCG_HOLD")))     cfg->ocg_hold    = atoi(s);
    if ((s = getenv("GFANC_GAIN_SMOOTH"))) cfg->gain_smooth_beta = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_ANTI"))) cfg->quiet_anti_rms = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_NR")))   cfg->quiet_nr_db    = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_HOLD"))) cfg->quiet_hold     = atoi(s);
    if ((s = getenv("GFANC_QUIET_EXIT"))) cfg->quiet_exit     = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_REF")))  cfg->quiet_ref_max  = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_ERR")))  cfg->quiet_err_max  = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_ERR_EXIT"))) cfg->quiet_err_exit = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_ERR_REF"))) cfg->quiet_err_ref  = (float)atof(s);
    if ((s = getenv("GFANC_QUIET_MEMORY"))) cfg->quiet_ref_memory = atoi(s);
    /* wc_gain 已移除: Wc RMS 始终按 stub_rms×1.0 构造, LMS 自适应收敛到正确增益 */
    /* if ((s = getenv("GFANC_WC_GAIN"))) cfg->wc_gain = (float)atof(s); */
}

#endif
