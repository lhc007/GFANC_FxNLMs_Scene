/** SceneController — CNN 决策层 (去场景层 + SFANC 硬选库).
 *
 * 每秒调用一次, 输入 1 秒音频 (16000 样本).
 *
 * 双模式 (GFANC_ANC_MODE 决定, 计划见 docs/无误差麦方案_与SFANC对照_路线分析.md):
 *   calibrate (adapt, 默认): 直接权重回归 — CNN 回归 30 维子带增益 (S×C),
 *     gain[i]=tanh(logit[i]) → Wc[s,l]=Σ_c gain[s,c]·sub[c,s,l] → RMS 标定 + 取反,
 *     作为 FxLMS 收敛起点 (scene_ctrl_process).
 *   deploy (fixed, 开环): SFANC 分类选库 — CNN 输出 N 类 logits → argmax → 防抖 →
 *     返回类索引, 由调用方从 wc_bank 槽 c 取成品 Wc (绝对增益烘焙, 无 RMS 归一化)
 *     (scene_ctrl_classify). 这是"部署无误差麦开环有效降噪"的命门 (研究结论 §1).
 *
 * K (CNN 输出维) 从 cnn_linear_weight.bin 大小推导: calibrate=30 (S*C),
 *   deploy 分类 CNN (Phase 3) = N (库槽数).
 */
#ifndef SCENE_CONTROLLER_H
#define SCENE_CONTROLLER_H

#define SC_S      2
#define SC_C      15
#define SC_DW_MAX 30   /* 直接权重输出维上限 (SC_S*SC_C) */

typedef struct {
    const float *sub_filters;   /* [C, S, L] 子滤波器基 (与标注/导出一致) */
    int    K;                   /* CNN 输出维 (运行时推导) */
    int    classify_mode;       /* 1=分类 (deploy, K=N≠S*C): 走 scene_ctrl_classify;
                                   0=回归 (calibrate, K==S*C): 走 scene_ctrl_process */
    int    L;                   /* filter_len (1024) */
    float  stub_rms;
    float  wc_rms_target;       /* 自动标定: Wc 构造目标 RMS (基于 Ŝ 物理衰减) */
    float  prev_gains[SC_DW_MAX];  /* 上一秒增益 [S*C] (弱信号/CNN失败时保持) */
    int    prev_gains_valid;       /* 是否有可用历史增益 */

    /* P0-2 增益时间平滑参数 (scene_ctrl_set_gain_smoothing 覆盖; 默认 0.5/0.85) */
    float  gain_smooth_beta;       /* EMA 平滑系数 (0~1; 1=不平滑) */
    float  gain_smooth_switch;     /* 旁路阈值 cos: 帧间低于此 → 场景切换, β 强制 1 */

    /* 输入归一化稳定标定 (2026-08-10): 每秒独立 minmax 的 denom 逐秒漂移 → CNN 输入
       逐秒抖 (实机抖动根因). EMA 平滑让缩放基准慢速跟随, 消除跳变. alpha=新值权重
       (0.1 ≈ 10s 时间常数). 与训练侧幅度增强 (0.5~2.0) 自洽. */
    float  norm_denom_smooth;      /* 平滑后的归一化 denom */
    int    norm_denom_valid;       /* 是否有历史基准 */
    float  norm_ema_alpha;         /* EMA 系数 (默认 0.1) */

    /* SFANC 分类选库 (Phase 2, deploy): 分类决策状态.
       决策层输出 = sel_class (防抖后); 调用方据此选库槽. */
    int    bank_n;                 /* 库槽数 N (scene_ctrl_set_bank; 0=未接入) */
    int    sel_class;              /* 当前选定类 (防抖后) */
    int    cand_class;             /* 候选类 (防抖中) */
    int    cand_cnt;               /* 候选连续命中帧数 */
    int    bank_hold_frames;       /* 防抖帧数 (GFANC_BANK_HOLD, 默认 2) */
} scene_ctrl_t;

int  scene_ctrl_init(scene_ctrl_t *sc, const float *sub_filters, int filter_len);
void scene_ctrl_free(scene_ctrl_t *sc);
/** 设置增益时间平滑参数 (P0-2). beta∈[0,1]; beta=1 关闭平滑; switch_cos 为场景切换旁路阈值. */
void scene_ctrl_set_gain_smoothing(scene_ctrl_t *sc, float beta, float switch_cos);
/** 接入 SFANC 库槽数 (deploy 决策层用). n_slots<=0 → 不参与选库 (纯回归). */
void scene_ctrl_set_bank(scene_ctrl_t *sc, int n_slots);
/** 设置分类防抖帧数 (GFANC_BANK_HOLD; <=0 回退默认 2). */
void scene_ctrl_set_bank_hold(scene_ctrl_t *sc, int hold_frames);
/** 直接权重 Wc 生产者 (calibrate): audio_1s → CNN → tanh 增益 → Wc[S*L].
 *  返回诊断用 argmax |gain| 带索引 (0..S*C-1, 弱信号/失败时沿用历史).
 *  输出 wc_out[S*L] (已 RMS 标定 + 取反), gains_out[S*C] (tanh 增益). */
int  scene_ctrl_process(scene_ctrl_t *sc, const float *audio_1s,
                        float *wc_out, float *gains_out);
/** SFANC 分类决策 (deploy): audio_1s → minmax → CNN → argmax → 防抖 → 更新 sel_class.
 *  返回当前选定类索引 (0..K-1). logits_out[K] 可空 (诊断用).
 *  弱信号/CNN 失败 → 保持 sel_class. */
int  scene_ctrl_classify(scene_ctrl_t *sc, const float *audio_1s, float *logits_out);
/** 用给定 30 维增益构造 Wc: wc[s*L+l] = Σ_c gains[s*C+c]·sub[(c*S+s)*L+l], 然后 RMS 标定 + 取反. */
void scene_ctrl_construct_wc(const scene_ctrl_t *sc, const float *gains, float *wc_out);

#endif
