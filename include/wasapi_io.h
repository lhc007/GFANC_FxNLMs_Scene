/** WASAPI 实时音频 I/O — 捕获 + 渲染封装.
 *
 * 用法:
 *   wasapi_t *w = wasapi_open(L"麦克风阵列...", L"扬声器...", 48000, 6, 2, 256);
 *   wasapi_start(w);
 *   while (running) {
 *       wasapi_read_capture(w, capture_buf, frames);
 *       // ... 处理 ...
 *       wasapi_write_render(w, render_buf, frames);
 *   }
 *   wasapi_stop(w);
 *   wasapi_close(w);
 */
#ifndef WASAPI_IO_H
#define WASAPI_IO_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wasapi_t wasapi_t;

/**
 * 打开音频设备.
 * @param in_name  捕获设备名称 (null = 默认)
 * @param out_name 渲染设备名称 (null = 默认)
 * @param fs       采样率 (Hz)
 * @param in_ch    捕获声道数
 * @param out_ch   渲染声道数
 * @param latency_ms 目标延迟 (ms), 建议 5~20
 * @return 实例指针, 失败返回 NULL
 */
wasapi_t *wasapi_open(const wchar_t *in_name, const wchar_t *out_name,
                       int fs, int in_ch, int out_ch, int latency_ms);

/** 启动流 */
int   wasapi_start(wasapi_t *w);

/** 停止流 */
int   wasapi_stop(wasapi_t *w);

/** 读取捕获数据 (阻塞直到请求帧数可用).
 * @param buf   float [in_ch * frames], 交错格式
 * @param frames 请求帧数
 * @return 实际读取帧数, -1 错误
 */
int   wasapi_read_capture(wasapi_t *w, float *buf, int frames);

/** 写入渲染数据 (阻塞直到缓冲区可写入).
 * @param buf   float [out_ch * frames], 交错格式
 * @param frames 帧数
 * @return 实际写入帧数, -1 错误
 */
int   wasapi_write_render(wasapi_t *w, const float *buf, int frames);

/** 获取当前填充/剩余帧数 */
int   wasapi_capture_available(wasapi_t *w);
int   wasapi_render_available(wasapi_t *w);

/** 关闭并释放 */
void  wasapi_close(wasapi_t *w);

/** 列出所有音频设备 (调试用) */
void  wasapi_list_devices(void);

#ifdef __cplusplus
}
#endif
#endif /* WASAPI_IO_H */
