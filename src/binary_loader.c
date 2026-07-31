#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "binary_loader.h"

/* R-16-②: .bin 格式头 — magic + version + n_floats + crc32 = 16B */
#define BIN_MAGIC   0x434E4647u   /* "GFNC" little-endian */
#define BIN_VERSION 1
#define BIN_HDR_SZ  16            /* magic(4) + version(4) + n_floats(4) + crc32(4) */

/* CRC32 (IEEE 802.3 polynomial, 与 Python zlib.crc32 一致) */
static uint32_t bin_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return crc ^ 0xFFFFFFFFu;
}

int bin_load_float(const char *path, float **data_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_sz < 4) { fclose(f); return -1; }

    /* R-16-②: 尝试读取头部 — 检测 magic "GFNC" */
    uint8_t hdr[BIN_HDR_SZ];
    int has_hdr = 0;
    if (file_sz >= BIN_HDR_SZ + 4) {  /* 至少 header + 1 float */
        if (fread(hdr, 1, BIN_HDR_SZ, f) == BIN_HDR_SZ) {
            uint32_t magic; memcpy(&magic, hdr, 4);
            if (magic == BIN_MAGIC) {
                has_hdr = 1;
                uint32_t version; memcpy(&version, hdr + 4, 4);
                uint32_t n_exp;   memcpy(&n_exp,   hdr + 8, 4);
                uint32_t crc_exp; memcpy(&crc_exp, hdr + 12, 4);
                long data_sz = file_sz - BIN_HDR_SZ;
                int n = (int)(data_sz / sizeof(float));

                if (version != BIN_VERSION) {
                    fprintf(stderr, "[WARN] %s: bin version %u != %u\n",
                            path, version, BIN_VERSION);
                }
                if ((uint32_t)n != n_exp) {
                    fprintf(stderr, "[WARN] %s: n_floats=%d header says %u (truncated?)\n",
                            path, n, n_exp);
                }

                float *d = (float *)malloc(data_sz);
                if (!d) { fclose(f); return -1; }
                if (fread(d, 1, data_sz, f) != (size_t)data_sz) {
                    free(d); fclose(f); return -1;
                }

                /* CRC32 校验 */
                uint32_t crc_actual = bin_crc32((uint8_t *)d, data_sz);
                if (crc_actual != crc_exp) {
                    fprintf(stderr, "[WARN] %s: CRC mismatch (got 0x%08X, expected 0x%08X) "
                            "— file may be corrupted!\n",
                            path, crc_actual, crc_exp);
                    /* 不阻止加载: CRC 告警但允许继续 (损坏数据好过拒绝启动) */
                }

                fclose(f);
                *data_out = d;
                return n;
            }
        }
        /* 不是 GFNC magic — 回退到旧格式 (裸 float 流) */
        rewind(f);
    }

    /* ── 旧格式: 裸 float 流 (向后兼容) ── */
    {
        long size = file_sz;
        int n = (int)(size / sizeof(float));
        float *d = (float *)malloc(size);
        if (!d) { fclose(f); return -1; }
        if (fread(d, 1, size, f) != (size_t)size) {
            free(d); fclose(f); return -1;
        }
        fclose(f);
        *data_out = d;
        return n;
    }
}

void bin_free(float *data) { if (data) free(data); }

float *bin_load(const char *name, int *n_out)
{
    char path[512];
    snprintf(path, sizeof(path), "data/%s.bin", name);
    float *d = NULL;
    int n = bin_load_float(path, &d);
    if (n < 0) { *n_out = 0; return NULL; }
    *n_out = n;
    return d;
}
