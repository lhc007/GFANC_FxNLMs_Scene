/** 生成确定性测试 WAV 文件 (1s, 16kHz, mono, 16-bit PCM).
 *  固定种子确保跨平台可复现, 用于 GFANC 黄金回归测试。
 *
 *  编译: gcc -O2 gen_test_wav.c -lm -o gen_test_wav.exe
 *  运行: ./gen_test_wav.exe [输出路径, 默认 test_signal.wav]
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

static void write_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void write_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "test_signal.wav";
    int sr = 16000, dur_sec = 1, n = sr * dur_sec;

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "Cannot create %s\n", out); return 1; }

    /* ── RIFF header ── */
    uint32_t data_size = n * 2;          /* 16-bit mono */
    fwrite("RIFF", 1, 4, f);
    write_u32(f, 36 + data_size);
    fwrite("WAVE", 1, 4, f);

    /* ── fmt chunk ── */
    fwrite("fmt ", 1, 4, f);
    write_u32(f, 16);                    /* PCM */
    write_u16(f, 1);                     /* audio format = PCM */
    write_u16(f, 1);                     /* channels */
    write_u32(f, sr);
    write_u32(f, sr * 2);                /* byte rate */
    write_u16(f, 2);                     /* block align */
    write_u16(f, 16);                    /* bits per sample */

    /* ── data chunk ── */
    fwrite("data", 1, 4, f);
    write_u32(f, data_size);

    /* 确定性信号: 440Hz + 1kHz 混合 + 少量噪声 (固定种子) */
    srand(42);
    for (int i = 0; i < n; i++) {
        float t = (float)i / sr;
        float sig = 0.25f * sinf(2.0f * 3.1415926535f * 440.0f * t)
                  + 0.15f * sinf(2.0f * 3.1415926535f * 1000.0f * t)
                  + 0.01f * ((float)rand() / (float)RAND_MAX - 0.5f);
        /* 软限幅 */
        if (sig > 1.0f) sig = 1.0f; else if (sig < -1.0f) sig = -1.0f;
        int16_t s = (int16_t)(sig * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    printf("  GEN %s: %dHz %dch %ds (%d samples)\n", out, sr, 1, dur_sec, n);
    return 0;
}
