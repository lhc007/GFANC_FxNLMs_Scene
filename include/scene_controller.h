/** SceneController — CNN 决策层 (SFANC 硬选库分类).
 *
 * 每秒调用一次, 输入 1 秒音频 (16000 样本).
 *
 * 部署 (GFANC_ANC_MODE=fixed, 开环): 分类 CNN 输出 N 类 logits → argmax → 防抖 →
 *   返回类索引 (scene_ctrl_classify), 由调用方从 wc_bank 槽 c 取成品 Wc.
 *   "部署无误差麦开环有效降噪"的命门 (研究结论 §1).
 * 标定 (adapt) 不初始化 CNN/决策层: 纯闭环 FxLMS 从零收敛, 收敛后写库槽.
 *
 * K (CNN 输出维) 从 cnn_bank_linear_weight.bin 大小推导 (cnn_m5_get_K),
 *   须 == 库槽数 N (调用方校验).
 */
#ifndef SCENE_CONTROLLER_H
#define SCENE_CONTROLLER_H

#define SC_S      2
#define SC_C      15
#define SC_DW_MAX 30   /* CNN 线性层输出维上限 (与 cnn_m5_forward.h 同源约束) */

typedef struct {
    int    K;                   /* CNN 输出维 (=库槽数 N, 运行时推导) */

    /* 输入归一化稳定标定 (2026-08-10): 每秒独立 minmax 的 denom 逐秒漂移 → CNN 输入
       逐秒抖 (实机抖动根因). EMA 平滑让缩放基准慢速跟随, 消除跳变. alpha=新值权重
       (0.1 ≈ 10s 时间常数). 与训练侧幅度增强 (0.5~2.0) 自洽. */
    float  norm_denom_smooth;      /* 平滑后的归一化 denom */
    int    norm_denom_valid;       /* 是否有历史基准 */
    float  norm_ema_alpha;         /* EMA 系数 (默认 0.1) */

    /* SFANC 分类选库 (deploy): 分类决策状态.
       决策层输出 = sel_class (防抖后); 调用方据此选库槽. */
    int    bank_n;                 /* 库槽数 N (scene_ctrl_set_bank; 0=未接入) */
    int    sel_class;              /* 当前选定类 (防抖后) */
    int    cand_class;             /* 候选类 (防抖中) */
    int    cand_cnt;               /* 候选连续命中帧数 */
    int    bank_hold_frames;       /* 防抖帧数 (GFANC_BANK_HOLD, 默认 2) */
} scene_ctrl_t;

int  scene_ctrl_init(scene_ctrl_t *sc);
void scene_ctrl_free(scene_ctrl_t *sc);
/** 接入 SFANC 库槽数 (deploy 决策层用). n_slots<=0 → 不参与选库. */
void scene_ctrl_set_bank(scene_ctrl_t *sc, int n_slots);
/** 设置分类防抖帧数 (GFANC_BANK_HOLD; <=0 回退默认 2). */
void scene_ctrl_set_bank_hold(scene_ctrl_t *sc, int hold_frames);
/** SFANC 分类决策 (deploy): audio_1s → minmax → CNN → argmax → 防抖 → 更新 sel_class.
 *  返回当前选定类索引 (0..K-1). logits_out[K] 可空 (诊断用).
 *  弱信号/CNN 失败 → 保持 sel_class. */
int  scene_ctrl_classify(scene_ctrl_t *sc, const float *audio_1s, float *logits_out);

#endif
