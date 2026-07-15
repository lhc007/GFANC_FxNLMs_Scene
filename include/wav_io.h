#ifndef WAV_IO_H
#define WAV_IO_H

#include <stdint.h>
#include <stdio.h>

/* 最小 WAV 读写 — 仅支持 16-bit PCM mono/stereo, 采样率由调用方指定 */

typedef struct {
    int      sample_rate;
    int      n_channels;
    int      n_samples;      /* per channel */
    float   *data;           /* interleaved float [-1,1] */
} wav_t;

/* 读取 WAV, 自动转 float [-1,1]. 返回 0 成功 */
int  wav_read(const char *path, wav_t *w);

/* 写入 16-bit PCM WAV */
int  wav_write(const char *path, const float *data, int n_samples,
               int n_channels, int sample_rate);

void wav_free(wav_t *w);

#endif
