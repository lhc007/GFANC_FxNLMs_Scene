#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <io.h>
#else
#include <glob.h>
#endif
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

/* ── R-27: 批次指纹 ── */

/* 链式 crc32 — 匹配 Python zlib.crc32(data, prev) 的续算语义:
 *   zlib.crc32(data, prev) = (寄存器从 prev^0xFFFFFFFF 起处理 data) ^ 0xFFFFFFFF.
 *   prev=0 时即标准单文件 crc32. 保证 C 端重算与 export_bin.py 结果逐位一致. */
static uint32_t bin_crc32_chain(uint32_t prev, const uint8_t *data, size_t len)
{
    uint32_t crc = prev ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return crc ^ 0xFFFFFFFFu;
}

static int cmp_pstr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* 把单个文件整段折入链式 crc (分段读, 避免一次性分配) */
static void crc_fold_file(uint32_t *crc, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;   /* 缺失文件两侧一致 (导出时必存在; 运行时若缺失早被 FATAL 拦) */
    uint8_t buf[65536];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0)
        *crc = bin_crc32_chain(*crc, buf, rd);
    fclose(f);
}

uint32_t bin_batch_crc(void)
{
    uint32_t crc = 0;

    /* 1) data/cnn_*.bin — 排序后逐个折入 (与 Python sorted(glob()) 一致:
        纯 ASCII 名, 字节序 == 码点序; 前缀恒为 "data/cnn_", 按 basename 排序等价) */
    char *names[128];
    int n = 0;
#ifdef _WIN32
    struct _finddata_t fd;
    intptr_t h = _findfirst("data/cnn_*.bin", &fd);   /* intptr_t: 64 位句柄, long 会截断 */
    if (h != -1L) {
        do {
            if (n < 128) names[n++] = strdup(fd.name);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    /* POSIX: glob() 枚举 data/cnn_*.bin (glibc 按字节序排序, 与 Windows 排序语义一致) */
    glob_t g;
    if (glob("data/cnn_*.bin", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && n < 128; i++) {
            const char *p = g.gl_pathv[i];
            const char *base = strrchr(p, '/');
            names[n++] = strdup(base ? base + 1 : p);
        }
        globfree(&g);
    }
#endif
    qsort(names, n, sizeof(char *), cmp_pstr);
    for (int i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "data/%s", names[i]);
        crc_fold_file(&crc, path);
        free(names[i]);
    }

    /* 2) 固定算法文件 (非声学路径) — 与 export_bin.py 顺序一致 */
    static const char *fixed[] = {
        "data/sub_filters.bin",
        "data/bandpass_fir.bin",
        "data/bandpass_anc.bin",
    };
    for (int i = 0; i < 3; i++)
        crc_fold_file(&crc, fixed[i]);

    return crc;
}

int bin_check_batch(void)
{
    FILE *f = fopen("data/batch_id.bin", "r");
    if (!f) {
        /* 旧 data/ 无批次文件 → 跳过 (向后兼容), 提示一次 */
        printf("  [batch] data/batch_id.bin 不存在 — 跳过批次指纹校验 (重跑 export_bin.py 可启用)\n");
        return 0;
    }
    char hex[32] = {0};
    int  got = (fscanf(f, "%31s", hex) == 1);
    fclose(f);
    if (!got) return 0;

    uint32_t expected = (uint32_t)strtoul(hex, NULL, 16);
    uint32_t actual   = bin_batch_crc();

    if (expected == actual) {
        printf("  [batch] 批次指纹一致 0x%08x (cnn/sub_filters/bandpass 同批)\n", actual);
        return 0;
    }
    fprintf(stderr,
            "  [WARN] 批次混配检测: data/ 的 cnn_*.bin / sub_filters / bandpass 来自不同批次!\n"
            "        记录批次=0x%08x, 当前文件=0x%08x — Wc 预设初值可能与训练世界错位\n"
            "        (FxLMS 会自适应纠正, 仅影响暖启动收敛). 修复: 重跑 python export/export_bin.py.\n",
            expected, actual);
    return 1;
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
