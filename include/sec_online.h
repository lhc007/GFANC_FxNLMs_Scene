/** Online secondary-path identification — NLMS per (error, speaker) pair.
 *
 *  Uses anti_spk (ANC output) as excitation and err_mic as response.
 *  No probe noise required — anti_spk is broadband when environment has noise.
 *  Extremely slow adaptation (μ ~ 5e-6) makes bias from anti↔disturbance
 *  correlation negligible on multi-second timescales.
 *
 *  Updates sec_coeffs in-place; FIR tick reads old coeffs before update in
 *  the same callback frame → no race condition.
 */
#ifndef SEC_ONLINE_H
#define SEC_ONLINE_H

typedef struct {
    int    E, S, sec_len;     /* dimensions: error mics, speakers, Ŝ taps */
    int    dsp_delay;         /* offset to active coeffs in sec_coeffs stride */
    int    sp;                /* sec_coeffs stride = sec_len + dsp_delay */
    float *anti_hist;         /* [S * sec_len] speaker anti ring buffers */
    int    anti_ptr;          /* ring write pointer (next slot) */
    float  mu;                /* NLMS step size (very small, ~5e-6) */
    float  power_floor;       /* regularization floor (1e-6) */
} sec_online_t;

/** Allocate and initialize.
 *  @param sec_len   Ŝ filter length (typically 1024)
 *  @param dsp_delay padding before active coeffs in sec_coeffs stride
 *  @param mu        NLMS step size for Ŝ identification
 *  @return 0 on success, -1 on OOM
 */
int  sec_online_init(sec_online_t *so, int E, int S, int sec_len,
                     int dsp_delay, float mu);

/** Per-sample NLMS update of secondary path coefficients.
 *  Call after fxnlms_tick_rt / fxnlms_forward_rt, before anti clamping.
 *  Only update when system is in normal operating mode (not muted/howling).
 *
 *  @param anti_spk    [S] current ANC anti output (pre-clamp, pre-mute)
 *  @param err_mic     [E] bandpassed error mic signal (same band as ANC)
 *  @param sec_coeffs  [E*S*sp] secondary path coeffs, active at +dsp_delay
 */
void sec_online_update(sec_online_t *so, const float *anti_spk,
                       const float *err_mic, float *sec_coeffs);

void sec_online_free(sec_online_t *so);

#endif
