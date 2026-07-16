/** MIMO FxNLMS — 逐样本自适应. */
#ifndef FXNLMS_MIMO_H
#define FXNLMS_MIMO_H

#include "fir_filter.h"

typedef struct {
    float       *wc;         /* [S * L] */
    float       *xd;         /* [E * S * L] */
    float       *x_hist;     /* [L] 原始带通参考历史 (实时输出 anti = Wc ⊗ x 用) */
    int          E, S, L;
    float        step_size;
    float        leak;
} fxnlms_mimo_t;

void fxnlms_init(fxnlms_mimo_t *fx, int E, int S, int L,
                 float step_size, float leak);
void fxnlms_set_wc(fxnlms_mimo_t *fx, const float *wc);
void fxnlms_update_wc(fxnlms_mimo_t *fx, const float *wc);

/** 逐样本完整处理.
 * @param Fx      [E*S] 滤波参考
 * @param disturbance [E] 扰动 (离线=Dis, 实时=bp(mic))
 * @param anti_out [S]  反噪声输出
 * @param err_out  [E]  误差信号 (=disturbance + anti_est, 梯度更新前的值)
 */
void fxnlms_tick(fxnlms_mimo_t *fx, const float *Fx, const float *disturbance,
                 float *anti_out, float *err_out);

/** 仅前向 (淡化期间使用). 不更新 Wc. */
void fxnlms_forward_only(fxnlms_mimo_t *fx, const float *Fx,
                         float *anti_out, float *err_out);

/** 实时逐样本处理 (F-A 修复版, 供 main_realtime 使用).
 *
 * 与 fxnlms_tick (离线仿真语义) 的区别:
 *   1. anti_out[s] = Wc[s] ⊗ x_ref — 物理扬声器驱动信号.
 *      不再经 Ŝ 模型二次滤波、不再对误差通道求和.
 *   2. 自适应直接使用实测误差 err_meas (误差麦物理上已含真实反噪声),
 *      不再合成 err = disturbance + anti_est.
 *
 * @param x_ref        当前带通参考样本
 * @param Fx           [E*S] 滤波参考 (Ŝ ⊗ x_ref)
 * @param err_meas     [E]  实测误差麦样本 (带通)
 * @param anti_out     [S]  反噪声输出 (直接送扬声器)
 * @param anti_est_out [E]  Ŝ 模型预测的误差麦处反噪声 (供 NR 统计, 可为 NULL)
 */
void fxnlms_tick_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                    const float *err_meas, float *anti_out, float *anti_est_out);

/** 实时仅前向 (淡化期间使用). 不更新 Wc. */
void fxnlms_forward_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                       float *anti_out, float *anti_est_out);

void fxnlms_free(fxnlms_mimo_t *fx);

#endif
