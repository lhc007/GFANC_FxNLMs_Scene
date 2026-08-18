#ifndef FIR_FILTER_H
#define FIR_FILTER_H

#include "scenezone_types.h"

/* ── 初始化 ── */
void fir_init(fir_filter_t *f, const gfanc_float_t *coeffs, int n_taps);

/* ── 逐样本 FIR: y = Σ coeffs[k] * x[n-k] ── */
gfanc_float_t fir_tick(fir_filter_t *f, gfanc_float_t x);

/* ── 批量 FIR: 输出到 y, 长度 n. x 和 y 不可重叠 ── */
void fir_process_block(fir_filter_t *f,
                       const gfanc_float_t *x,
                       gfanc_float_t *y, int n);

/* ── 重置延迟线 ── */
void fir_reset(fir_filter_t *f);

/* ── 释放资源 ── */
void fir_free(fir_filter_t *f);

/* ── 工具: 标量点积 (不用 filter 结构) ── */
gfanc_float_t vec_dot(const gfanc_float_t *a, const gfanc_float_t *b, int n);

#endif /* FIR_FILTER_H */
