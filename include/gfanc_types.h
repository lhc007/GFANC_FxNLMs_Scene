/** 核心类型 + 分级日志 — 被所有模块共用. */
#ifndef GFANC_TYPES_H
#define GFANC_TYPES_H

#include <stdio.h>

typedef float gfanc_float_t;

/* FIR 滤波器 (double 精度延迟线, 匹配 Python scipy float64) */
typedef struct {
    gfanc_float_t *coeffs;
    double        *delay_line;
    int            n_taps;
    int            ptr;
} fir_filter_t;

/* ── 分级日志宏 (CR-20) ── */
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  printf(  "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) /* disabled in release */ ((void)0)

#endif
