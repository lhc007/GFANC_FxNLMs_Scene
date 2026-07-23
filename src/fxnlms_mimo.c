/** MIMO FxNLMS — 逐样本自适应. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fxnlms_mimo.h"

int fxnlms_init(fxnlms_mimo_t *fx, int E, int S, int L,
                 float step_size, float leak)
{
    fx->E = E; fx->S = S; fx->L = L;
    fx->step_size = step_size; fx->leak = leak;
    fx->wc     = (float *)calloc(S * L,     sizeof(float));
    fx->xd     = (float *)calloc(E * S * L, sizeof(float));
    fx->x_hist = (float *)calloc(L,         sizeof(float));
    fx->freeze_lms = 0;
    if (!fx->wc || !fx->xd || !fx->x_hist) {
        free(fx->wc); free(fx->xd); free(fx->x_hist);
        fx->wc = NULL; fx->xd = NULL; fx->x_hist = NULL;
        return -1;
    }
    return 0;
}

void fxnlms_set_wc(fxnlms_mimo_t *fx, const float *wc)
    { memcpy(fx->wc, wc, fx->S * fx->L * sizeof(float)); }

/* ── 内部辅助 ── */

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

static void x_hist_push(fxnlms_mimo_t *fx, float x)
{
    int L = fx->L;
    for (int k = L - 1; k > 0; k--)
        fx->x_hist[k] = fx->x_hist[k - 1];
    fx->x_hist[0] = x;
}

/* ══════════════════════════════════════════════════════════
   离线仿真路径 (保留, 不修改)
   ══════════════════════════════════════════════════════════ */

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

    /* 2. anti_est[e] = Σ_s,k Wc[s,k] * Xd[e,s,k] */
    float anti_est[E];
    for (int e = 0; e < E; e++) { anti_est[e] = 0;
        for (int s = 0; s < S; s++)
            for (int k = 0; k < L; k++)
                anti_est[e] += fx->wc[s * L + k] * fx->xd[(e * S + s) * L + k]; }

    /* 3. err = disturbance + anti_est (离线: disturbance=Pri(ref), 不含反噪声) */
    for (int e = 0; e < E; e++)
        err_out[e] = disturbance[e] + anti_est[e];

    /* 4. anti_out[s] = Σ_e,k Wc[s,k] * Xd[e,s,k] (离线: Ŝ域输出, 仅写WAV) */
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
            fx->wc[s * L + k] *= (1.0f - fx->leak);
    }
}

/* ══════════════════════════════════════════════════════════
   实时 ANC 路径 (独立, 与离线仿真互不影响)
   - 输出: anti[s] = Σ_k Wc[s,k] * x_ref[n-k]  (直接卷积, 不经Ŝ)
   - 梯度: 实测误差麦信号err_meas直接驱动, 不合成
   ══════════════════════════════════════════════════════════ */

void fxnlms_forward_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                       const float *err_meas, float *anti_out)
{
    int S = fx->S, L = fx->L;

    xd_roll_write(fx, Fx);
    x_hist_push(fx, x_ref);

    /* 输出: anti[s] = Σ_k Wc[s,k] * x_ref[n-k] */
    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int k = 0; k < L; k++)
            anti_out[s] += fx->wc[s * L + k] * fx->x_hist[k]; }

    (void)err_meas; /* 前向模式不更新梯度, err_meas仅保留接口一致性 */
}

void fxnlms_tick_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                    const float *err_meas, float *anti_out)
{
    int E = fx->E, S = fx->S, L = fx->L;

    /* 1. Xd roll + write Fx, x_hist push */
    xd_roll_write(fx, Fx);
    x_hist_push(fx, x_ref);

    /* 2. 物理输出: anti[s] = Σ_k Wc[s,k] * x_ref[n-k] */
    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int k = 0; k < L; k++)
            anti_out[s] += fx->wc[s * L + k] * fx->x_hist[k]; }

    /* 3. 功率归一化 (基于滤波参考 Xd) */
    float power[S];
    for (int s = 0; s < S; s++) {
        power[s] = 1e-10f;
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                power[s] += fx->xd[(e * S + s) * L + k] * fx->xd[(e * S + s) * L + k];
        power[s] /= (float)(E * L);
    }

    /* 4. 梯度: Wc[s,k] -= μ/power[s] * Σ_e err_meas[e] * Xd[e,s,k] */
    if (!fx->freeze_lms) {
        for (int s = 0; s < S; s++) {
            float inv_pwr = 1.0f / power[s];
            for (int e = 0; e < E; e++)
                for (int k = 0; k < L; k++)
                    fx->wc[s * L + k] -= fx->step_size * err_meas[e]
                                       * fx->xd[(e * S + s) * L + k] * inv_pwr;
            for (int k = 0; k < L; k++)
                fx->wc[s * L + k] *= (1.0f - fx->leak);
        }
    }
}

void fxnlms_free(fxnlms_mimo_t *fx)
{
    free(fx->wc); free(fx->xd); free(fx->x_hist);
    fx->wc = NULL; fx->xd = NULL; fx->x_hist = NULL;
}
