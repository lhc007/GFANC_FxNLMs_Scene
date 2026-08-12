#ifndef BINARY_LOADER_H
#define BINARY_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* 从 .bin 文件读取 float32 数组, 返回元素个数, data 由调用方 free */
int   bin_load_float(const char *path, float **data_out);
/* 释放 */
void  bin_free(float *data);

/* 便捷: 从 data/ 目录加载指定名称的 .bin */
float *bin_load(const char *name, int *n_out);

/* ── R-27: 批次指纹 (batch fingerprint) ──
 * 指纹 = 对 [排序后的 data/cnn_*.bin + sub_filters.bin + bandpass_fir.bin
 *           + bandpass_anc.bin] 原始字节做链式 crc32
 *         (与 Python zlib.crc32(data, prev) 语义一致, 见 export_bin.py).
 * 声学路径 (secondary/primary/feedback/…) 是安装态可替换的测量值, 不入指纹
 * (换 Ŝ 属设计行为, 见 R-16-①/BUG-8).
 * bin_batch_crc(): 重算当前 data/ 的批次指纹.
 * bin_check_batch(): 读 data/batch_id.bin (export_bin.py 写的 hex) 并比对.
 *   返回 0 = 一致或文件缺失(旧 data/ 跳过, 向后兼容); 1 = 批次混配 (已打 WARN). */
uint32_t bin_batch_crc(void);
int      bin_check_batch(void);

#endif
