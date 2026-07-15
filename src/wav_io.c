/** Minimal WAV reader/writer — 16-bit PCM only. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wav_io.h"

/* ── Little-endian helpers ── */
static inline uint32_t read_u32le(FILE *f) {
    unsigned char b[4]; fread(b, 1, 4, f);
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static inline uint16_t read_u16le(FILE *f) {
    unsigned char b[2]; fread(b, 1, 2, f);
    return (uint16_t)b[0] | ((uint16_t)b[1]<<8);
}
static void write_u32le(FILE *f, uint32_t v) {
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v>>8),
                          (unsigned char)(v>>16), (unsigned char)(v>>24)};
    fwrite(b, 1, 4, f);
}
static void write_u16le(FILE *f, uint16_t v) {
    unsigned char b[2] = {(unsigned char)v, (unsigned char)(v>>8)};
    fwrite(b, 1, 2, f);
}

int wav_read(const char *path, wav_t *w)
{
    memset(w, 0, sizeof(*w));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char riff[5] = {0}; fread(riff, 1, 4, f);
    if (strcmp(riff, "RIFF")) { fclose(f); return -1; }

    uint32_t file_size = read_u32le(f);
    (void)file_size;

    char wave[5] = {0}; fread(wave, 1, 4, f);
    if (strcmp(wave, "WAVE")) { fclose(f); return -1; }

    /* Scan chunks for fmt and data */
    int fmt_found = 0, data_found = 0;
    uint32_t data_size = 0;
    int bits_per_sample = 0;

    while (!fmt_found || !data_found) {
        char id[5] = {0};
        if (fread(id, 1, 4, f) < 4) break;
        uint32_t chunk_size = read_u32le(f);

        if (!strcmp(id, "fmt ")) {
            uint16_t audio_format = read_u16le(f);
            if (audio_format != 1) { fclose(f); return -1; } /* PCM only */
            w->n_channels = read_u16le(f);
            w->sample_rate = (int)read_u32le(f);
            read_u32le(f); /* byte rate */
            read_u16le(f); /* block align */
            bits_per_sample = read_u16le(f);
            /* skip extra fmt bytes */
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
            fmt_found = 1;
        } else if (!strcmp(id, "data")) {
            data_size = chunk_size;
            data_found = 1;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!fmt_found || !data_found) { fclose(f); return -1; }

    int bytes_per_sample = bits_per_sample / 8;
    int frame_size = w->n_channels * bytes_per_sample;
    w->n_samples = data_size / frame_size;

    w->data = (float *)malloc(w->n_samples * w->n_channels * sizeof(float));
    if (!w->data) { fclose(f); return -1; }

    /* Read samples, convert to float [-1,1] */
    if (bits_per_sample == 16) {
        for (int i = 0; i < w->n_samples * w->n_channels; i++) {
            int16_t s = (int16_t)read_u16le(f);
            w->data[i] = (float)s / 32768.0f;
        }
    } else if (bits_per_sample == 24) {
        for (int i = 0; i < w->n_samples * w->n_channels; i++) {
            unsigned char b[3]; fread(b, 1, 3, f);
            int32_t s = (int32_t)(b[0] | (b[1]<<8) | (b[2]<<16));
            if (s & 0x800000) s |= 0xFF000000;
            w->data[i] = (float)s / 8388608.0f;
        }
    } else {
        /* fallback: 32-bit float */
        for (int i = 0; i < w->n_samples * w->n_channels; i++) {
            int32_t s = (int32_t)read_u32le(f);
            w->data[i] = (float)(*(float*)&s); /* reinterpret as float */
        }
    }

    fclose(f);
    return 0;
}

int wav_write(const char *path, const float *data, int n_samples,
              int n_channels, int sample_rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int bits_per_sample = 16;
    int bytes_per_sample = bits_per_sample / 8;
    int frame_size = n_channels * bytes_per_sample;
    int data_size = n_samples * frame_size;
    int file_size = 36 + data_size;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    write_u32le(f, file_size);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    write_u32le(f, 16); /* chunk size */
    write_u16le(f, 1);  /* PCM */
    write_u16le(f, n_channels);
    write_u32le(f, sample_rate);
    write_u32le(f, sample_rate * frame_size);
    write_u16le(f, frame_size);
    write_u16le(f, bits_per_sample);

    /* data chunk */
    fwrite("data", 1, 4, f);
    write_u32le(f, data_size);

    for (int i = 0; i < n_samples * n_channels; i++) {
        float v = data[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)(v * 32767.0f);
        write_u16le(f, (uint16_t)s);
    }

    fclose(f);
    return 0;
}

void wav_free(wav_t *w)
{
    if (w->data) { free(w->data); w->data = NULL; }
}
