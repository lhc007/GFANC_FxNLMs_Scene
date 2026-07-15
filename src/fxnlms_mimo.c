/** MIMO FxNLMS C 实现 — 逐样本自适应控制滤波器更新.

对应 Python: gfanc/Combine_GFANC_with_FxNLMS_MIMO.py

算法:
    Xd[:,:,0] = Fx[e][s] (滤波参考)
    y_ff = Σ_s Wc[s,:] · Xd[:,s,:] → anti_est  (已取反, 无额外取反)
    err = Dis + anti_est                        (Wc 已取反 → err = d + anti)
    梯度: grad[s,k] = Σ_e err[e] · Xd[e,s,k]
    avg_power = mean(||Xd[s,:,:]||²)            (SISO式总功率归一化)
    Wc[s,:] -= μ · grad[s,:] / avg_power        (减法更新, 匹配 err 定义)
    Wc[s,:] *= (1 - leak)                       (泄漏防止漂移)
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfanc_types.h"

#include "../data/gfanc_config.h"

static const float LEAK_FACTOR = 1e-5f;

/* ── 初始化 ── */
void fxnlms_init(fxnlms_mimo_t *fx, float step_size)
{
    int total = GFANC_S * GFANC_FILTER_LEN;
    memset(fx->wc, 0, total * sizeof(gfanc_float_t));
    memset(fx->xd, 0, GFANC_E * GFANC_S * GFANC_FILTER_LEN * sizeof(gfanc_float_t));
    fx->xd_ptr     = 0;
    fx->step_size  = step_size;
    fx->leak       = LEAK_FACTOR;
    fx->wc_rms     = 0.0f;
}

/* ── 加载初始 Wc 并缓存 RMS ── */
void fxnlms_set_wc(fxnlms_mimo_t *fx, const gfanc_float_t *wc)
{
    int total = GFANC_S * GFANC_FILTER_LEN;
    memcpy(fx->wc, wc, total * sizeof(gfanc_float_t));
    gfanc_float_t sum_sq = 0.0f;
    for (int i = 0; i < total; i++) sum_sq += wc[i] * wc[i];
    fx->wc_rms = sqrtf(sum_sq / (gfanc_float_t)total);
}

/* ── 逐样本 FxNLMS 更新 ──
   Fx[e][s]: 当前时刻滤波参考 (已通过 Sec 路径卷积)
   Dis[e]:   当前时刻扰动 (由 Pri 路径卷积噪声得到)
   返回: y[s] = anti_ff[s] — 反噪声输出 */
void fxnlms_tick(fxnlms_mimo_t *fx,
                  const gfanc_float_t *Fx,    /* [E*S] row-major */
                  const gfanc_float_t *Dis,   /* [E] */
                  gfanc_float_t *anti_out)    /* [S] */
{
    int E = GFANC_E, S = GFANC_S, L = GFANC_FILTER_LEN;

    /* 更新延迟线: Xd[:,:, ptr] = Fx */
    int p = fx->xd_ptr;
    for (int e = 0; e < E; e++) {
        for (int s_ = 0; s_ < S; s_++) {
            fx->xd[(e * S + s_) * L + p] = Fx[e * S + s_];
        }
    }
    fx->xd_ptr = (p + 1) % L;

    /* 前向: anti[s] = Σ_e Σ_k Wc[s,k] · Xd[e,s,k] (e 维度聚合) */
    /* 每个 e 贡献一份 anti, 取均值 */
    for (int s_ = 0; s_ < S; s_++) {
        gfanc_float_t acc = 0.0f;
        for (int e = 0; e < E; e++) {
            for (int k = 0; k < L; k++) {
                acc += fx->wc[s_ * L + k] * fx->xd[(e * S + s_) * L + k];
            }
        }
        anti_out[s_] = acc;
    }

    /* 误差: err[e] = Dis[e] + anti_est[e] → anti_est = Σ_s Fx[e,s,k]·Wc[s,k] */
    /* 但这里 anti_est 是 Σ_e... 的均值版本，需要更精确的计算 */
    /* 实际: anti_est[e] = Σ_s Σ_k Wc[s,k] · Fx_filtered[e,s,k] */
    /* Fx_filtered 已经在 Xd 中,但每个e不同 */

    /* 梯度更新: Wc[s,k] -= μ · Σ_e(err[e] × Xd[e,s,k]) / power[s] */
    gfanc_float_t power[S];
    for (int s_ = 0; s_ < S; s_++) {
        gfanc_float_t pwr = 0.0f;
        for (int e = 0; e < E; e++) {
            for (int k = 0; k < L; k++) {
                pwr += fx->xd[(e * S + s_) * L + k]
                     * fx->xd[(e * S + s_) * L + k];
            }
        }
        power[s_] = pwr / (gfanc_float_t)(E * L) + 1e-10f;
    }

    for (int s_ = 0; s_ < S; s_++) {
        gfanc_float_t inv_pwr = 1.0f / power[s_];
        for (int e = 0; e < E; e++) {
            /* anti_est[e] = Σ_s2 Σ_k Wc[s2,k] · Xd[e,s2,k] */
            gfanc_float_t anti_est_e = 0.0f;
            for (int s2 = 0; s2 < S; s2++) {
                for (int k = 0; k < L; k++) {
                    anti_est_e += fx->wc[s2 * L + k]
                                * fx->xd[(e * S + s2) * L + k];
                }
            }
            gfanc_float_t err_e = Dis[e] + anti_est_e;

            /* gradient for speaker s_: grad[k] = err_e × Xd[e,s_,k] */
            for (int k = 0; k < L; k++) {
                fx->wc[s_ * L + k] -= fx->step_size * err_e
                                    * fx->xd[(e * S + s_) * L + k]
                                    * inv_pwr;
            }
        }
        /* Leak */
        for (int k = 0; k < L; k++) {
            fx->wc[s_ * L + k] *= (1.0f - fx->leak);
        }
    }
}
