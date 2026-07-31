/** Online secondary-path NLMS identification.
 *
 *  Identifies Ŝ(e,s) = speaker[s] → error_mic[e] acoustic transfer function.
 *
 *  Uses anti_spk (ANC output driving speakers) as excitation and err_mic
 *  (bandpassed error microphone) as response.  No probe noise required:
 *  anti_spk is broadband whenever the environment has noise in the ANC band,
 *  and disturbance residual acts as dither that averages out over time.
 *
 *  μ ≈ 5e-6 means each Ŝ tap changes by ~5e-8 per sample at typical levels,
 *  i.e. ~0.1% per second — slow enough that anti↔disturbance correlation
 *  (which would bias the Ŝ estimate) decorrelates over minutes of operation.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sec_online.h"

int sec_online_init(sec_online_t *so, int E, int S, int sec_len,
                    int dsp_delay, float mu)
{
    so->E = E; so->S = S; so->sec_len = sec_len;
    so->dsp_delay = dsp_delay;
    so->sp = sec_len + dsp_delay;
    so->anti_ptr = 0;
    so->mu = mu;
    so->power_floor = 1e-6f;

    so->anti_hist = (float *)calloc(S * sec_len, sizeof(float));
    if (!so->anti_hist) return -1;

    return 0;
}

void sec_online_update(sec_online_t *so, const float *anti_spk,
                       const float *err_mic, float *sec_coeffs)
{
    int E = so->E, S = so->S, L = so->sec_len;
    int sp = so->sp, dly = so->dsp_delay;
    float mu = so->mu, pf = so->power_floor;
    int ptr = so->anti_ptr;

    /* 1. Write anti_spk to ring buffers */
    for (int s = 0; s < S; s++) {
        /* NaN guard: poisoned anti poisons the entire Ŝ delay line permanently */
        float a = anti_spk[s];
        if (!isfinite(a)) a = 0.0f;
        so->anti_hist[s * L + ptr] = a;
    }
    ptr = (ptr + 1) % L;
    so->anti_ptr = ptr;

    /* 2. Compute ring read layout: newest → oldest, two linear segments.
     *    segment 1: newest, newest-1, ..., 0
     *    segment 2: L-1, L-2, ..., newest+1 */
    int newest = (ptr == 0) ? L - 1 : ptr - 1;
    int seg1_len = newest + 1;          /* indices newest .. 0 inclusive */
    int seg2_len = L - seg1_len;        /* indices L-1 .. newest+1 */

    /* 3. Power per speaker: Σ anti[s]² over the delay line.
     *    Same for all error mics (same excitation drives all paths). */
    float power[/* GFANC_S_MAX */ 4];   /* max S=4 (GFANC_S_MAX) */
    for (int s = 0; s < S; s++) {
        float pwr = pf;
        float *hist = so->anti_hist + s * L;

        /* seg1: newest .. 0 */
        for (int i = newest; i >= 0; i--)
            pwr += hist[i] * hist[i];
        /* seg2: L-1 .. newest+1 */
        for (int i = L - 1; i > newest; i--)
            pwr += hist[i] * hist[i];

        power[s] = pwr;
    }

    /* 4. Per error mic: predict total Ŝ contribution, then update all Ŝ(e,*) */
    for (int e = 0; e < E; e++) {
        /* 4a. y_pred = Σ_{s,k} Ŝ[e,s,k] * anti[s, delayed_k] */
        float y_pred = 0.0f;
        for (int s = 0; s < S; s++) {
            float *coef = sec_coeffs + (e * S + s) * sp + dly;
            float *hist = so->anti_hist + s * L;

            /* seg1 */
            int k = 0;
            for (int i = newest; i >= 0; i--, k++)
                y_pred += coef[k] * hist[i];
            /* seg2 */
            for (int i = L - 1; i > newest; i--, k++)
                y_pred += coef[k] * hist[i];
        }

        /* 4b. Identification error */
        float em = err_mic[e];
        if (!isfinite(em)) em = 0.0f;
        float e_id = em - y_pred;

        /* 4c. NLMS update: Ŝ[e,s,k] += μ × e_id × anti[s,k] / power[s] */
        for (int s = 0; s < S; s++) {
            float *coef = sec_coeffs + (e * S + s) * sp + dly;
            float *hist = so->anti_hist + s * L;
            float inv_pwr = mu / power[s];

            /* seg1 */
            int k = 0;
            for (int i = newest; i >= 0; i--, k++)
                coef[k] += inv_pwr * e_id * hist[i];
            /* seg2 */
            for (int i = L - 1; i > newest; i--, k++)
                coef[k] += inv_pwr * e_id * hist[i];
        }
    }
}

void sec_online_free(sec_online_t *so)
{
    free(so->anti_hist);
    so->anti_hist = NULL;
}
