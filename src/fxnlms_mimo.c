/** MIMO FxNLMS — 逐样本自适应.
 *
 *  R-12: xd/x_hist 改为环形缓冲, 消除每样本 7168 次 memmove 拷贝.
 *        热循环 (功率/梯度) 用双段线性访问, 零取模 + 编译器可 NEON 向量化.
 */
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
    fx->xd_ptr = 0; fx->x_hist_ptr = 0;
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

/* ── R-12: 环形缓冲写入 (替代 memmove) ── */

static void xd_roll_write(fxnlms_mimo_t *fx, const float *Fx)
{
    int E = fx->E, S = fx->S, L = fx->L, p = fx->xd_ptr;
    for (int e = 0; e < E; e++)
        for (int s = 0; s < S; s++)
            fx->xd[((e * S + s) * L) + p] = Fx[e * S + s];
    fx->xd_ptr = (p + 1) % L;
}

static void x_hist_push(fxnlms_mimo_t *fx, float x)
{
    int L = fx->L, p = fx->x_hist_ptr;
    fx->x_hist[p] = x;
    fx->x_hist_ptr = (p + 1) % L;
}

/* R-12: 环形索引 — k=0=最新, k=L-1=最旧. 仅离线轻量路径使用 */
static inline float xd_ring(const fxnlms_mimo_t *fx, int es, int k)
{
    int L = fx->L;
    return fx->xd[es * L + ((unsigned)(fx->xd_ptr - 1 - k) % (unsigned)L)];
}

/* ══════════════════════════════════════════════════════════
   离线仿真路径 (保留, 兼容 — xd 访问改用环形)
   ══════════════════════════════════════════════════════════ */

void fxnlms_forward_only(fxnlms_mimo_t *fx, const float *Fx,
                         float *anti_out, float *err_out)
{
    int E = fx->E, S = fx->S, L = fx->L;
    xd_roll_write(fx, Fx);

    for (int e = 0; e < E; e++) { err_out[e] = 0;
        for (int s = 0; s < S; s++)
            for (int k = 0; k < L; k++)
                err_out[e] += fx->wc[s * L + k] * xd_ring(fx, e * S + s, k); }

    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                anti_out[s] += fx->wc[s * L + k] * xd_ring(fx, e * S + s, k); }
}

void fxnlms_tick(fxnlms_mimo_t *fx, const float *Fx, const float *disturbance,
                 float *anti_out, float *err_out)
{
    int E = fx->E, S = fx->S, L = fx->L;

    xd_roll_write(fx, Fx);

    /* anti_est[e] = Σ_s,k Wc[s,k] * Xd[e,s,k] */
    float anti_est[E];
    for (int e = 0; e < E; e++) { anti_est[e] = 0;
        for (int s = 0; s < S; s++)
            for (int k = 0; k < L; k++)
                anti_est[e] += fx->wc[s * L + k] * xd_ring(fx, e * S + s, k); }

    for (int e = 0; e < E; e++)
        err_out[e] = disturbance[e] + anti_est[e];

    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        for (int e = 0; e < E; e++)
            for (int k = 0; k < L; k++)
                anti_out[s] += fx->wc[s * L + k] * xd_ring(fx, e * S + s, k); }

    /* ── R-12: 功率/梯度用双段线性访问 (零取模, 编译器可向量化) ── */
    int p = fx->xd_ptr;
    int seg1 = (p == 0) ? L - 1 : p - 1;  /* 最新样本物理位置 */

    float power[S];
    for (int s = 0; s < S; s++) {
        power[s] = 1e-6f;
        for (int e = 0; e < E; e++) {
            float *base = fx->xd + (e * S + s) * L;
            for (int idx = seg1; idx >= 0; idx--)
                power[s] += base[idx] * base[idx];
            if (p > 0)  /* p==0 时段1已覆盖全部 L 个样本 */
                for (int idx = L - 1; idx >= p; idx--)
                    power[s] += base[idx] * base[idx];
        }
        power[s] /= (float)(E * L);
    }

    /* 梯度更新 */
    for (int s = 0; s < S; s++) {
        float inv_pwr = 1.0f / power[s];
        for (int e = 0; e < E; e++) {
            float *base = fx->xd + (e * S + s) * L;
            float *wc_s = fx->wc + s * L;
            int k = 0;
            for (int idx = seg1; idx >= 0; idx--, k++)
                wc_s[k] -= fx->step_size * err_out[e] * base[idx] * inv_pwr;
            if (p > 0)
                for (int idx = L - 1; idx >= p; idx--, k++)
                    wc_s[k] -= fx->step_size * err_out[e] * base[idx] * inv_pwr;
        }
        for (int k = 0; k < L; k++)
            fx->wc[s * L + k] *= (1.0f - fx->leak);
    }
}

/* ══════════════════════════════════════════════════════════
   实时 ANC 路径 (独立, 与离线仿真互不影响)
   ══════════════════════════════════════════════════════════ */

void fxnlms_forward_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                       const float *err_meas, float *anti_out)
{
    int S = fx->S, L = fx->L, hp = fx->x_hist_ptr;

    xd_roll_write(fx, Fx);
    x_hist_push(fx, x_ref);

    /* R-12: anti = Wc ⊗ x_hist — 双段环形访问 (hp=写入位置=最新样本) */
    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        float *wc_s = fx->wc + s * L;
        int k = 0;
        for (int idx = hp; idx >= 0; idx--, k++)
            anti_out[s] += wc_s[k] * fx->x_hist[idx];
        for (int idx = L - 1; idx > hp; idx--, k++)
            anti_out[s] += wc_s[k] * fx->x_hist[idx];
    }

    (void)err_meas;
}

void fxnlms_tick_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                    const float *err_meas, float *anti_out)
{
    int E = fx->E, S = fx->S, L = fx->L, hp = fx->x_hist_ptr;

    xd_roll_write(fx, Fx);
    x_hist_push(fx, x_ref);

    /* anti = Wc ⊗ x_hist (双段环形, hp=写入位置=最新) */
    for (int s = 0; s < S; s++) { anti_out[s] = 0;
        float *wc_s = fx->wc + s * L;
        int k = 0;
        for (int idx = hp; idx >= 0; idx--, k++)
            anti_out[s] += wc_s[k] * fx->x_hist[idx];
        for (int idx = L - 1; idx > hp; idx--, k++)
            anti_out[s] += wc_s[k] * fx->x_hist[idx];
    }

    /* ── 功率 + 梯度: 双段线性访问 (R-12 优化) ── */
    int p = fx->xd_ptr;
    int seg1 = (p == 0) ? L - 1 : p - 1;

    float power[S];
    for (int s = 0; s < S; s++) {
        power[s] = 1e-6f;
        for (int e = 0; e < E; e++) {
            float *base = fx->xd + (e * S + s) * L;
            for (int idx = seg1; idx >= 0; idx--)
                power[s] += base[idx] * base[idx];
            if (p > 0)
                for (int idx = L - 1; idx >= p; idx--)
                    power[s] += base[idx] * base[idx];
        }
        power[s] /= (float)(E * L);
    }

    /* anti-windup: 输出超出钳位阈值(±1.2)时冻结梯度 + 快速衰减(100×leak),
       将 Wc 迅速拉回线性区, 避免"饱和后锁死". */
    int saturated = 0;
    for (int s = 0; s < S; s++)
        if (fabsf(anti_out[s]) > 1.2f) saturated = 1;

    if (!fx->freeze_lms) {
        if (!saturated) {
            for (int s = 0; s < S; s++) {
                float inv_pwr = 1.0f / power[s];
                for (int e = 0; e < E; e++) {
                    float *base = fx->xd + (e * S + s) * L;
                    float *wc_s = fx->wc + s * L;
                    int k = 0;
                    for (int idx = seg1; idx >= 0; idx--, k++)
                        wc_s[k] -= fx->step_size * err_meas[e] * base[idx] * inv_pwr;
                    if (p > 0)
                        for (int idx = L - 1; idx >= p; idx--, k++)
                            wc_s[k] -= fx->step_size * err_meas[e] * base[idx] * inv_pwr;
                }
            }
        }
        /* 泄漏始终运行; 饱和时 100× 快速衰减 → 0.16%/样本 → ~1s 退出饱和 */
        float lk = saturated ? (fx->leak * 50.0f) : fx->leak;
        for (int s = 0; s < S; s++)
            for (int k = 0; k < L; k++)
                fx->wc[s * L + k] *= (1.0f - lk);
    }
}

void fxnlms_get_anti_est(const fxnlms_mimo_t *fx, float *anti_est)
{
    int E = fx->E, S = fx->S, L = fx->L;
    int p = fx->xd_ptr;
    int seg1 = (p == 0) ? L - 1 : p - 1;

    for (int e = 0; e < E; e++) {
        anti_est[e] = 0;
        for (int s = 0; s < S; s++) {
            float *base = fx->xd + (e * S + s) * L;
            float *wc_s = fx->wc  + s * L;
            int k = 0;
            for (int idx = seg1; idx >= 0; idx--, k++)
                anti_est[e] += wc_s[k] * base[idx];
            if (p > 0)
                for (int idx = L - 1; idx >= p; idx--, k++)
                    anti_est[e] += wc_s[k] * base[idx];
        }
    }
}

void fxnlms_free(fxnlms_mimo_t *fx)
{
    free(fx->wc); free(fx->xd); free(fx->x_hist);
    fx->wc = NULL; fx->xd = NULL; fx->x_hist = NULL;
}
