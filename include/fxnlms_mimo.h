/** MIMO FxNLMS — 逐样本自适应. */
#ifndef FXNLMS_MIMO_H
#define FXNLMS_MIMO_H

#include "fir_filter.h"

typedef struct {
    float       *wc;         /* [S * L] 控制滤波器系数 */
    float       *xd;         /* [E * S * L] 滤波参考 (梯度用, 环形缓冲) */
    float       *x_hist;     /* [L] 原始带通参考历史 (实时输出用, 环形缓冲) */
    int          E, S, L;
    int          xd_ptr;     /* R-12: xd 环形写指针 (下一个写入位置) */
    int          x_hist_ptr; /* R-12: x_hist 环形写指针 */
    float        step_size;
    float        leak;
    volatile long freeze_lms; /* 发散检测: 1=冻结梯度更新, 0=正常 */
} fxnlms_mimo_t;

int  fxnlms_init(fxnlms_mimo_t *fx, int E, int S, int L,
                 float step_size, float leak);  /* 返回0=成功, -1=OOM */
void fxnlms_set_wc(fxnlms_mimo_t *fx, const float *wc);

/* ── 离线仿真 (保留, 互不影响) ── */
/** @param Fx      [E*S] 滤波参考
 *  @param disturbance [E] 扰动 (离线=Pri(ref), 不含反噪声)
 *  @param anti_out [S]  反噪声输出 (Ŝ模型域, 仅用于写WAV)
 *  @param err_out  [E]  误差信号 (=disturbance + anti_est)
 */
void fxnlms_tick(fxnlms_mimo_t *fx, const float *Fx, const float *disturbance,
                 float *anti_out, float *err_out);
void fxnlms_forward_only(fxnlms_mimo_t *fx, const float *Fx,
                         float *anti_out, float *err_out);

/* ── 实时 ANC (独立路径, 不与离线仿真共用) ── */
/** @param x_ref     原始带通参考样本 (输出用, Wc ⊗ x_ref → anti_spk)
 *  @param Fx        [E*S] 滤波参考 (梯度用)
 *  @param err_meas  [E] 实测误差麦信号 (带通, 驱动梯度)
 *  @param anti_out  [S] 反噪声输出 (物理扬声器信号)
 */
void fxnlms_tick_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                    const float *err_meas, float *anti_out);
void fxnlms_forward_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                       const float *err_meas, float *anti_out);

void fxnlms_free(fxnlms_mimo_t *fx);

#endif
