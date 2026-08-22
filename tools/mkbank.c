/** mkbank — SFANC 库构建/检视小工具 (运维, 不参与主构建).
 *
 *  用法:
 *    ./mkbank.exe from-sub  <n_slots>  从 data/sub_filters.bin 前 n_slots×S*L 段建库
 *    ./mkbank.exe from-fixed           data/wc_fixed.bin (S*L float) → 库槽 0 (N=1)
 *    ./mkbank.exe info                 打印 data/wc_bank.bin 头 (槽数/槽长)
 *
 *  Phase 1 验证用: 旧 wc_fixed.bin 转 N=1 库 → deploy 与旧 fixed 等价回归.
 *  sub_filters 槽 0 = 离线 FxLMS 收敛成品 (绝对增益烘焙) — 合成 N=1 库的合法来源.
 *
 *  S/L 编译期与主程序一致 (S=2, L=1024, 槽长 = S*L = 2048 float).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "binary_loader.h"
#include "scene_bank.h"

#define S 2
#define L 1024
#define SLOT_FLOATS (S * L)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: mkbank <from-sub n_slots|from-fixed|info>\n"); return 1; }

    if (strcmp(argv[1], "info") == 0) {
        scene_bank_t bank;
        if (scene_bank_load("data/wc_bank.bin", S, L, &bank) != 0) {
            fprintf(stderr, "data/wc_bank.bin: 不存在或头不符\n"); return 1;
        }
        printf("wc_bank.bin: n_slots=%u slot_len=%u (%u float = %u B)\n",
               bank.n_slots, bank.slot_len, bank.slot_len,
               (unsigned)(bank.slot_len * sizeof(float)));
        scene_bank_free(&bank);
        return 0;
    }

    if (strcmp(argv[1], "from-fixed") == 0) {
        float *wc = NULL;
        int n = bin_load_float("data/wc_fixed.bin", &wc);
        if (n != SLOT_FLOATS) { fprintf(stderr, "wc_fixed.bin 尺寸 %d != %d\n", n, SLOT_FLOATS); return 1; }
        int rc = scene_bank_save_slot("data/wc_bank.bin", S, L, 0, wc);
        if (rc == 0) printf("已写 data/wc_bank.bin 槽0 (来自 wc_fixed.bin, N=1)\n");
        else         fprintf(stderr, "写库失败\n");
        bin_free(wc);
        return rc;
    }

    if (strcmp(argv[1], "from-sub") == 0) {
        if (argc < 3) { fprintf(stderr, "需要 n_slots\n"); return 1; }
        int n_slots = atoi(argv[2]);
        if (n_slots < 1 || n_slots > 30) { fprintf(stderr, "n_slots 1..30\n"); return 1; }
        float *sub = NULL;
        int n = bin_load_float("data/sub_filters.bin", &sub);
        if (n < n_slots * SLOT_FLOATS) {
            fprintf(stderr, "sub_filters.bin 尺寸 %d 不足 %d×%d\n", n, n_slots, SLOT_FLOATS);
            bin_free(sub); return 1;
        }
        int rc = scene_bank_write_all("data/wc_bank.bin", S, L, n_slots, sub);
        if (rc == 0) printf("已写 data/wc_bank.bin: %d 槽 × %d float (来自 sub_filters 前段)\n",
                            n_slots, SLOT_FLOATS);
        else         fprintf(stderr, "写库失败\n");
        bin_free(sub);
        return rc;
    }

    fprintf(stderr, "未知命令 %s\n", argv[1]);
    return 1;
}
