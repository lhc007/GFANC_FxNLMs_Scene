#include <stdlib.h>
#include <string.h>
#include "fir_filter.h"

void fir_init(fir_filter_t *f, const gfanc_float_t *coeffs, int n_taps)
{
    f->coeffs = (gfanc_float_t *)coeffs;
    f->n_taps = n_taps;
    f->ptr = 0;
    f->delay_line = (double *)calloc(n_taps, sizeof(double));
}

void fir_reset(fir_filter_t *f)
{
    if (f->delay_line) {
        memset(f->delay_line, 0, f->n_taps * sizeof(double));
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
    double *dl = f->delay_line;
    const gfanc_float_t *c = f->coeffs;
    int N = f->n_taps;
    int p = f->ptr;

    dl[p] = (double)x;

    /* double 精度匹配 Python scipy.signal.lfilter 的 float64 */
    double y = 0.0;
    for (int k = 0; k < N; k++) {
        y += (double)c[k] * dl[(p - k + N) % N];
    }

    f->ptr = (p + 1) % N;
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
