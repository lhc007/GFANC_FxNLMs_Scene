/** MIMO FxNLMS — 逐样本自适应. */
#ifndef FXNLMS_MIMO_H
#define FXNLMS_MIMO_H

#include "fir_filter.h"

typedef struct {
    float       *wc;         /* [S * L] */
    float       *xd;         /* [E * S * L] */
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

void fxnlms_free(fxnlms_mimo_t *fx);

#endif
