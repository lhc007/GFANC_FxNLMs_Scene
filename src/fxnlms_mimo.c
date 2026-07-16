/** MIMO FxNLMS — 逐样本自适应. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fxnlms_mimo.h"

void fxnlms_init(fxnlms_mimo_t *fx, int E, int S, int L,
                 float step_size, float leak)
{
    fx->E = E; fx->S = S; fx->L = L;
    fx->step_size = step_size; fx->leak = leak;
    fx->wc = (float *)calloc(S * L, sizeof(float));
    fx->xd = (float *)calloc(E * S * L, sizeof(float));
    fx->x_hist = (float *)calloc(L, sizeof(float));
}

void fxnlms_set_wc(fxnlms_mimo_t *fx, const float *wc)
    { memcpy(fx->wc, wc, fx->S * fx->L * sizeof(float)); }

void fxnlms_update_wc(fxnlms_mimo_t *fx, const float *wc)
    { memcpy(fx->wc, wc, fx->S * fx->L * sizeof(float)); }

static void xd_roll_write(fxnlms_mimo_t *fx, const float *Fx)
{
    int E = fx->E, S = fx->S, L = fx->L;
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++)
            for (int k = L - 1; k > 0; k--)
                fx->xd[(e * S + s) * L + k] = fx->xd[(e * S + s) * L + (k - 1)];
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++)
            fx->xd[(e * S + s) * L + 0] = Fx[e * S + s];
}

void fxnlms_forward_only(fxnlms_mimo_t *fx, const float *Fx,
                         float *anti_out, float *err_out)
{
    int E = fx->E, S = fx->S, L = fx->L;
    xd_roll_write(fx, Fx);

    for (int e = 0; e < E; e++) { err_out[e] = 0;
        for (int s = 0; s < S; s++)
            for (int k = 0; k < L; k++)
                err_out[e] += fx->wc[s * L + k] * fx->xd[(e * S + s) * L + k]; }

    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                anti_out[s] += fx->wc[s * L + k] * fx->xd[(e * S + s) * L + k]; }
}

void fxnlms_tick(fxnlms_mimo_t *fx, const float *Fx, const float *disturbance,
                 float *anti_out, float *err_out)
{
    int E = fx->E, S = fx->S, L = fx->L;

    /* 1. Xd roll + write Fx */
    xd_roll_write(fx, Fx);

    /* 2. anti_est[e] = Σ_s,k Wc[s,k] * Xd[e,s,k] (梯度更新前) */
    float anti_est[E];
    for (int e = 0; e < E; e++) { anti_est[e] = 0;
        for (int s = 0; s < S; s++)
            for (int k = 0; k < L; k++)
                anti_est[e] += fx->wc[s * L + k] * fx->xd[(e * S + s) * L + k]; }

    /* 3. err = disturbance + anti_est (梯度更新前的值, 用于 dB) */
    for (int e = 0; e < E; e++)
        err_out[e] = disturbance[e] + anti_est[e];

    /* 4. anti_out[s] = Σ_e,k Wc[s,k] * Xd[e,s,k] */
    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                anti_out[s] += fx->wc[s * L + k] * fx->xd[(e * S + s) * L + k]; }

    /* 5. 功率归一化 */
    float power[S];
    for (int s = 0; s < S; s++) {
        power[s] = 1e-10f;
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                power[s] += fx->xd[(e * S + s) * L + k] * fx->xd[(e * S + s) * L + k];
        power[s] /= (float)(E * L);
    }

    /* 6. 梯度更新 */
    for (int s = 0; s < S; s++) {
        float inv_pwr = 1.0f / power[s];
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                fx->wc[s * L + k] -= fx->step_size * err_out[e]
                                   * fx->xd[(e * S + s) * L + k] * inv_pwr;
        for (int k = 0; k < L; k++)
            fx->wc[s * L + k] *= (1.0f - fx->step_size * fx->leak);
    }
}

void fxnlms_free(fxnlms_mimo_t *fx)
    { free(fx->wc); free(fx->xd); free(fx->x_hist);
      fx->wc = NULL; fx->xd = NULL; fx->x_hist = NULL; }

/* ══════════════════════════════════════════════════════════
   实时版 (F-A 修复): 输出 = Wc ⊗ x, 误差 = 实测麦信号
   ══════════════════════════════════════════════════════════ */

static void x_hist_roll_write(fxnlms_mimo_t *fx, float x)
{
    memmove(fx->x_hist + 1, fx->x_hist, (fx->L - 1) * sizeof(float));
    fx->x_hist[0] = x;
}

/* anti_est[e] = Σ_s,k Wc[s,k]·Xd[e,s,k] — Ŝ 模型预测的误差麦处反噪声.
   仅用于 NR 统计 (d̂ = e_meas − anti_est), 不参与自适应. */
static void anti_est_calc(const fxnlms_mimo_t *fx, float *anti_est_out)
{
    int E = fx->E, S = fx->S, L = fx->L;
    for (int e = 0; e < E; e++) { anti_est_out[e] = 0;
        for (int s = 0; s < S; s++)
            for (int k = 0; k < L; k++)
                anti_est_out[e] += fx->wc[s * L + k] * fx->xd[(e * S + s) * L + k]; }
}

void fxnlms_forward_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                       float *anti_out, float *anti_est_out)
{
    int S = fx->S, L = fx->L;
    xd_roll_write(fx, Fx);
    x_hist_roll_write(fx, x_ref);

    /* anti[s] = Wc[s] ⊗ x — 物理扬声器驱动信号 (不经 Ŝ, 不对 e 求和) */
    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int k = 0; k < L; k++)
            anti_out[s] += fx->wc[s * L + k] * fx->x_hist[k]; }

    if (anti_est_out) anti_est_calc(fx, anti_est_out);
}

void fxnlms_tick_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                    const float *err_meas, float *anti_out, float *anti_est_out)
{
    int E = fx->E, S = fx->S, L = fx->L;

    /* 前向: roll Xd/x_hist + anti_out (+ anti_est) */
    fxnlms_forward_rt(fx, x_ref, Fx, anti_out, anti_est_out);

    /* 功率归一化 (与 fxnlms_tick 相同) */
    float power[S];
    for (int s = 0; s < S; s++) {
        power[s] = 1e-10f;
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                power[s] += fx->xd[(e * S + s) * L + k] * fx->xd[(e * S + s) * L + k];
        power[s] /= (float)(E * L);
    }

    /* 梯度更新: e(n) 直接用实测误差 (误差麦已含真实反噪声, 不再合成) */
    for (int s = 0; s < S; s++) {
        float inv_pwr = 1.0f / power[s];
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                fx->wc[s * L + k] -= fx->step_size * err_meas[e]
                                   * fx->xd[(e * S + s) * L + k] * inv_pwr;
        for (int k = 0; k < L; k++)
            fx->wc[s * L + k] *= (1.0f - fx->step_size * fx->leak);
    }
}
