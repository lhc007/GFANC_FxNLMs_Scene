/** 核心类型 — 仅 FIR 滤波器, 被所有模块共用. */
#ifndef GFANC_TYPES_H
#define GFANC_TYPES_H

typedef float gfanc_float_t;

/* FIR 滤波器 (double 精度延迟线, 匹配 Python scipy float64) */
typedef struct {
    gfanc_float_t *coeffs;
    double        *delay_line;
    int            n_taps;
    int            ptr;
} fir_filter_t;

#endif
