#include <stdlib.h>
#include <string.h>
#include "fir_filter.h"

void fir_init(fir_filter_t *f, const gfanc_float_t *coeffs, int n_taps)
{
    f->coeffs = (gfanc_float_t *)coeffs;
    f->n_taps = n_taps;
    f->ptr = 0;
    f->delay_line = (gfanc_delay_t *)calloc(n_taps, sizeof(gfanc_delay_t));
}

void fir_reset(fir_filter_t *f)
{
    if (f->delay_line) {
        memset(f->delay_line, 0, f->n_taps * sizeof(gfanc_delay_t));
    }
    f->ptr = 0;
}

void fir_free(fir_filter_t *f)
{
    if (f->delay_line) {
        free(f->delay_line);
        f->delay_line = NULL;
    }
}

gfanc_float_t fir_tick(fir_filter_t *f, gfanc_float_t x)
{
    gfanc_delay_t *dl = f->delay_line;
    const gfanc_float_t *c = f->coeffs;
    int N = f->n_taps;
    int p = f->ptr;

    dl[p] = (gfanc_delay_t)x;

    /* 双段线性循环 — 从p递减到0, 再从N-1递减到p+1, 零取模
       等价于原 (p - k + N) % N 逆序访问, 消除 ~339k idiv/回调.
       R-23: 累加器保持 gfanc_delay_t 精度 (double on x86, float on MCU). */
    gfanc_delay_t y = 0.0;
    int k = 0;
    for (int i = p; i >= 0; i--)
        y += (gfanc_delay_t)c[k++] * dl[i];
    for (int i = N - 1; i > p; i--)
        y += (gfanc_delay_t)c[k++] * dl[i];

    f->ptr = (p + 1 == N) ? 0 : p + 1;  /* 条件替代取模, 仅 1/N 概率触发 */
    return (gfanc_float_t)y;
}

void fir_process_block(fir_filter_t *f,
                       const gfanc_float_t *x,
                       gfanc_float_t *y, int n)
{
    for (int i = 0; i < n; i++) {
        y[i] = fir_tick(f, x[i]);
    }
}

gfanc_float_t vec_dot(const gfanc_float_t *a, const gfanc_float_t *b, int n)
{
    gfanc_float_t sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
