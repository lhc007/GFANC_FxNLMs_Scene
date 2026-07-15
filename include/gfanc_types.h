#ifndef GFANC_TYPES_H
#define GFANC_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* ── 系统尺寸 (由 gfanc_config.h 定义) ── */
#include "../data/gfanc_config.h"

/* ── 数据类型 ── */
typedef float gfanc_float_t;

/* ── FIR 滤波器状态 ── */
typedef struct {
    gfanc_float_t *coeffs;      /* 滤波器系数 [N]          */
    double        *delay_line;  /* 延迟线 double 精度 [N]  */
    int             n_taps;     /* 抽头数                   */
    int             ptr;        /* 环形缓冲区写入指针        */
} fir_filter_t;

/* ── SceneController 状态 ── */
typedef struct {
    const gfanc_float_t *centroids;   /* [K, S*C] 预设表               */
    const gfanc_float_t *sub_filters; /* [C, S, FILTER_LEN] 子滤波器   */
    gfanc_float_t        wc[GFANC_S * GFANC_FILTER_LEN];  /* 当前 Wc 系数    */
    gfanc_float_t        wc_old[GFANC_S * GFANC_FILTER_LEN]; /* 旧 Wc (fade) */
    gfanc_float_t        stub_rms;   /* 等权 Wc RMS 参考值             */
    int                  scene_id;   /* 当前硬选择场景                  */
    int                  fade_cnt;   /* 交叉淡化剩余样本               */
} scene_ctrl_t;

/* ── CNN 前向状态 ── */
typedef struct {
    /* 中间 buffer — 固定大小, 编译期分配 */
    gfanc_float_t conv0_out[64 * 500];          /* Conv1d+BN+ReLU+MaxPool 输出 */
    gfanc_float_t res1_out[64 * 250];           /* ResBlock群1 输出 */
    gfanc_float_t res2_out[64 * 62];            /* ResBlock群2 输出 */
    gfanc_float_t pool_out[64 * 15];            /* Pool后 */
    gfanc_float_t fc_in[64];                    /* global_pool → 64D */
    gfanc_float_t logits[GFANC_K];             /* 输出 logits */
} cnn_state_t;

/* ── MIMO FxNLMS 状态 ── */
typedef struct {
    gfanc_float_t  wc[GFANC_S * GFANC_FILTER_LEN]; /* 控制滤波器 Wc */
    gfanc_float_t  xd[GFANC_E * GFANC_S * GFANC_FILTER_LEN]; /* 滤波参考延迟线 */
    int            xd_ptr;
    gfanc_float_t  step_size;
    gfanc_float_t  leak;
    gfanc_float_t  wc_rms;                      /* stub RMS */
} fxnlms_mimo_t;

/* ── 系统总状态 ── */
typedef struct {
    /* 滤波器实例 */
    fir_filter_t    bp_fir;        /* 带通 FIR (可选用) */
    scene_ctrl_t    scene_ctrl;
    cnn_state_t     cnn_state;
    fxnlms_mimo_t   fxnlms;

    /* 次级路径卷积状态 */
    fir_filter_t    sec_fir[GFANC_E * GFANC_S]; /* 每通道一个 FIR */

    /* 信号缓冲 */
    gfanc_float_t  *ref_signal;   /* 1s 参考信号 [INPUT_LEN] */

    /* 配置 */
    int             use_bandpass; /* 1=使用带通FIR, 0=全频 */
    gfanc_float_t   reset_threshold; /* cos_sim阈值 */

    /* 逐秒处理中间值 */
    gfanc_float_t   blend_weights[GFANC_SC_DIM]; /* Blend输出 */
    int             scene_switch_pending;         /* 是否有待切换Wc */
} gfanc_system_t;

#endif /* GFANC_TYPES_H */
