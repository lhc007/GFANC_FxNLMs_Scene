/** SceneBank — SFANC 式硬选滤波器库 I/O (Phase 1).
 *
 *  库格式: 单文件 data/wc_bank.bin
 *    头 16B: magic "GFNC" (4B) + version u32 + n_slots u32 + slot_len u32
 *    数据:   n_slots × (slot_len float32)  // slot_len = S*L
 *
 *  C 端 N 由 (n_floats)/(S*L) 推导. 加载期校验 magic/version/slot_len.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scene_bank.h"

/* ── 小端 u32 读写 (库格式固定 LE, 跨 x86/ARM 一致) ── */
static uint32_t rd_u32(const unsigned char *p)
    { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff); p[3] = (unsigned char)((v >> 24) & 0xff);
}

int scene_bank_load(const char *path, int S, int L, scene_bank_t *bank)
{
    unsigned char hdr[SCENE_BANK_HEADER_SIZE];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(hdr, 1, SCENE_BANK_HEADER_SIZE, f) != SCENE_BANK_HEADER_SIZE) { fclose(f); return -1; }
    if (rd_u32(hdr) != SCENE_BANK_MAGIC)        { fclose(f); return -1; }
    if (rd_u32(hdr + 4) != SCENE_BANK_VERSION)  { fclose(f); return -1; }
    uint32_t n_slots  = rd_u32(hdr + 8);
    uint32_t slot_len = rd_u32(hdr + 12);

    /* 槽长必须与当前 S*L 一致; 库槽数>0 */
    if ((int)slot_len != S * L || n_slots == 0) { fclose(f); return -1; }

    long flen;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    flen = ftell(f);
    if (flen != SCENE_BANK_HEADER_SIZE + (long)n_slots * (long)slot_len * (long)sizeof(float)) {
        fclose(f); return -1;   /* 文件截断/多段 */
    }
    /* 跳过 16B 头再读数据 — 原 rewind() 把头部字节读成前 4 个 float (tap0-3 污染) */
    if (fseek(f, SCENE_BANK_HEADER_SIZE, SEEK_SET) != 0) { fclose(f); return -1; }

    float *data = (float *)malloc((size_t)n_slots * slot_len * sizeof(float));
    if (!data) { fclose(f); return -1; }
    if (fread(data, sizeof(float), (size_t)n_slots * slot_len, f)
            != (size_t)n_slots * slot_len) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    bank->n_slots  = n_slots;
    bank->slot_len = slot_len;
    bank->data     = data;
    return 0;
}

/* 已存在库的 header 读取 (写槽用): 返回 0=可写 (n/slot 传出), -1=需新建 */
static int probe_existing(const char *path, uint32_t *n_slots, uint32_t *slot_len)
{
    unsigned char hdr[SCENE_BANK_HEADER_SIZE];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int ok = (fread(hdr, 1, SCENE_BANK_HEADER_SIZE, f) == SCENE_BANK_HEADER_SIZE)
          && (rd_u32(hdr) == SCENE_BANK_MAGIC)
          && (rd_u32(hdr + 4) == SCENE_BANK_VERSION);
    fclose(f);
    if (!ok) return -1;
    *n_slots  = rd_u32(hdr + 8);
    *slot_len = rd_u32(hdr + 12);
    return 0;
}

int scene_bank_save_slot(const char *path, int S, int L, int k, const float *wc)
{
    uint32_t n_slots, slot_len;
    long offset;

    if (probe_existing(path, &n_slots, &slot_len) == 0) {
        /* 已存在: 校验槽长 */
        if ((int)slot_len != S * L) return -1;
        if (k < 0) return -1;
        if ((uint32_t)k < n_slots) {
            /* 槽 k 已存在: 直接覆写 */
            offset = SCENE_BANK_HEADER_SIZE + (long)k * (long)slot_len * (long)sizeof(float);
            FILE *f = fopen(path, "r+b");
            if (!f) return -1;
            if (fseek(f, offset, SEEK_SET) != 0) { fclose(f); return -1; }
            if (fwrite(wc, sizeof(float), (size_t)slot_len, f) != (size_t)slot_len) {
                fclose(f); return -1;
            }
            fclose(f);
            return 0;
        }
        /* 槽 k >= n_slots: 扩展库 (保留旧槽, 新槽置零, 写 wc 到槽 k) — Phase 3c 多槽标定 */
        uint32_t total = (uint32_t)(k + 1);
        float *data = (float *)calloc((size_t)total * S * L, sizeof(float));
        if (!data) return -1;
        FILE *fr = fopen(path, "rb");
        if (!fr) { free(data); return -1; }
        int rdok = fseek(fr, SCENE_BANK_HEADER_SIZE, SEEK_SET) == 0
                && fread(data, sizeof(float), (size_t)n_slots * S * L, fr)
                   == (size_t)n_slots * S * L;
        fclose(fr);
        if (!rdok) { free(data); return -1; }
        memcpy(data + (size_t)k * S * L, wc, (size_t)S * L * sizeof(float));
        FILE *gw = fopen(path, "wb");
        if (!gw) { free(data); return -1; }
        unsigned char hdr[SCENE_BANK_HEADER_SIZE];
        wr_u32(hdr, SCENE_BANK_MAGIC);
        wr_u32(hdr + 4, SCENE_BANK_VERSION);
        wr_u32(hdr + 8, total);
        wr_u32(hdr + 12, (uint32_t)(S * L));
        int ok = fwrite(hdr, 1, SCENE_BANK_HEADER_SIZE, gw) == SCENE_BANK_HEADER_SIZE
              && fwrite(data, sizeof(float), (size_t)total * S * L, gw)
                 == (size_t)total * S * L;
        fclose(gw);
        free(data);
        return ok ? 0 : -1;
    }

    /* 新建: 头 + k 槽写 wc, 其余槽置零 */
    if (k < 0) return -1;
    uint32_t total = (uint32_t)(k + 1);
    float *data = (float *)calloc((size_t)total * S * L, sizeof(float));
    if (!data) return -1;
    memcpy(data + (size_t)k * S * L, wc, (size_t)S * L * sizeof(float));

    FILE *f = fopen(path, "wb");
    if (!f) { free(data); return -1; }
    unsigned char hdr[SCENE_BANK_HEADER_SIZE];
    wr_u32(hdr, SCENE_BANK_MAGIC);
    wr_u32(hdr + 4, SCENE_BANK_VERSION);
    wr_u32(hdr + 8, total);
    wr_u32(hdr + 12, (uint32_t)(S * L));
    if (fwrite(hdr, 1, SCENE_BANK_HEADER_SIZE, f) != SCENE_BANK_HEADER_SIZE
        || fwrite(data, sizeof(float), (size_t)total * S * L, f) != (size_t)total * S * L) {
        fclose(f); free(data); return -1;
    }
    fclose(f);
    free(data);
    return 0;
}

int scene_bank_write_all(const char *path, int S, int L, int n_slots, const float *data)
{
    if (n_slots <= 0 || S <= 0 || L <= 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned char hdr[SCENE_BANK_HEADER_SIZE];
    wr_u32(hdr, SCENE_BANK_MAGIC);
    wr_u32(hdr + 4, SCENE_BANK_VERSION);
    wr_u32(hdr + 8, (uint32_t)n_slots);
    wr_u32(hdr + 12, (uint32_t)(S * L));
    int ok = fwrite(hdr, 1, SCENE_BANK_HEADER_SIZE, f) == SCENE_BANK_HEADER_SIZE;
    if (ok)
        ok = fwrite(data, sizeof(float), (size_t)n_slots * S * L, f) == (size_t)n_slots * S * L;
    fclose(f);
    return ok ? 0 : -1;
}

void scene_bank_free(scene_bank_t *bank)
{
    if (bank) { free(bank->data); bank->data = NULL; bank->n_slots = 0; bank->slot_len = 0; }
}

const float *scene_bank_slot(const scene_bank_t *bank, int k)
{
    if (!bank || !bank->data || k < 0 || (uint32_t)k >= bank->n_slots) return NULL;
    return bank->data + (size_t)k * bank->slot_len;
}
