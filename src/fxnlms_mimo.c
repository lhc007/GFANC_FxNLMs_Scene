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
    { free(fx->wc); free(fx->xd); fx->wc = NULL; fx->xd = NULL; }
