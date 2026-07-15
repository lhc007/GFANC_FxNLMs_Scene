#ifndef BINARY_LOADER_H
#define BINARY_LOADER_H

#include <stddef.h>

/* 从 .bin 文件读取 float32 数组, 返回元素个数, data 由调用方 free */
int   bin_load_float(const char *path, float **data_out);
/* 释放 */
void  bin_free(float *data);

/* 便捷: 从 data/ 目录加载指定名称的 .bin */
float *bin_load(const char *name, int *n_out);

#endif
