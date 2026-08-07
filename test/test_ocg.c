/** OCG (在线聚类闸门) 单元测试 (R-31).
 *
 *  编译: gcc -O2 -I../include test_ocg.c ../src/ocg.c -lm -o test_ocg.exe
 *  运行: ./test_ocg.exe
 *
 *  用合成 probs 序列直接验证 ocg_step 决策逻辑 (方案: Luo et al., ICASSP 2026):
 *  测试 1  首帧播种
 *  测试 2  同场景微扰 → 不切 (STAY)
 *  测试 3  缓慢漂移 10 帧 → 不切且全 STAY (漂移跟踪防误切)
 *  测试 4  跳变到新场景持续 3 帧 → NEW 切换 (第 3 帧)
 *  测试 5  回归已知场景 → REJOIN 切换 (复用)
 *  测试 6  均匀 probs (置信不足) → 不切且中心不变
 *  测试 7  单帧闪烁 → 不切 (一致性防抖)
 *  测试 8  同场景子簇 → 不切 (只跟踪, 不打断 FxNLMS)
 */
#include <stdio.h>
#include <string.h>

#include "ocg.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s\n", msg); failures++; } \
    else { printf("  PASS: %s\n", msg); } \
} while(0)

int main(void)
{
    printf("── OCG 单元测试 ──\n");

    gfanc_config_t cfg = GFANC_CONFIG_DEFAULT;
    cfg.ocg_enable = 1;
    cfg.ocg_alpha = 0.1f;
    cfg.ocg_stay_thresh = 0.90f;
    cfg.ocg_rejoin_thresh = 0.75f;
    cfg.ocg_confirm_frames = 3;
    cfg.ocg_max_clusters = 4;
    const int K = 3;

    ocg_t g; ocg_init(&g, &cfg, K);

    /* 三个场景原型 */
    float A[3] = {0.85f, 0.10f, 0.05f};   /* scene 0 */
    float B[3] = {0.05f, 0.85f, 0.10f};   /* scene 1 */
    float C[3] = {0.08f, 0.12f, 0.80f};   /* scene 2 */

    /* 测试 1: 首次播种 */
    ocg_reset(&g, A, 0);
    CHECK(g.n_clusters == 1 && g.active == 0, "测试1: 首帧播种 (1簇, active=0)");

    /* 测试 2: 同场景微扰 → 不切 (STAY) */
    {
        ocg_reason_t r; int t;
        float p[3] = {0.83f, 0.11f, 0.06f};
        t = ocg_step(&g, p, 0, 0, &r);
        CHECK(t == -1 && r == OCG_REASON_STAY, "测试2: 同场景微扰→不切 (STAY)");
    }

    /* 测试 3: 缓慢漂移 (仍属活动簇) 连续 10 帧 → 不切且全 STAY */
    {
        ocg_reason_t r; int t = -1, n = 0; float p[3];
        for (int i = 1; i <= 10; i++) {
            p[0] = 0.85f - 0.025f * i; p[1] = 0.10f + 0.025f * i; p[2] = 0.05f;
            if (ocg_step(&g, p, 0, 0, &r) != -1) { t = 1; break; }
            if (r == OCG_REASON_STAY) n++;
        }
        CHECK(t == -1 && n == 10, "测试3: 缓慢漂移10帧→不切且全STAY (漂移跟踪)");
    }

    /* 测试 4: 跳变到场景 B 持续 3 帧 → NEW 切换 (第 3 帧) */
    {
        ocg_reason_t r; int t, fired = -1, at = -1; ocg_reason_t rr = OCG_REASON_NONE;
        for (int i = 0; i < 5; i++) {
            t = ocg_step(&g, B, 1, 0, &r);
            if (t >= 0) { fired = t; at = i + 1; rr = r; break; }
        }
        CHECK(fired == 1 && at == 3 && rr == OCG_REASON_NEW,
              "测试4: 场景B跳变3帧→NEW切换(第3帧)");
    }

    /* 测试 5: 当前活动簇=B, 回归场景 A (已知簇) → REJOIN 切换 */
    {
        ocg_reason_t r; int t, fired = -1, at = -1; ocg_reason_t rr = OCG_REASON_NONE;
        for (int i = 0; i < 5; i++) {
            t = ocg_step(&g, A, 0, 1, &r);
            if (t >= 0) { fired = t; at = i + 1; rr = r; break; }
        }
        CHECK(fired == 0 && at == 3 && rr == OCG_REASON_REJOIN,
              "测试5: 回归场景A→REJOIN切换(第3帧)");
    }

    /* 测试 6: 置信不足帧 (均匀 probs) → 不切且活动簇中心不变 */
    {
        ocg_reason_t r; int t;
        float uni[3] = {0.34f, 0.33f, 0.33f};
        float c0_before; c0_before = g.cluster[g.active].center[0];
        t = ocg_step(&g, uni, 0, 0, &r);
        CHECK(t == -1 && r == OCG_REASON_NONE &&
              g.cluster[g.active].center[0] == c0_before,
              "测试6: 均匀probs→不切且中心不变");
    }

    /* 测试 7: 单帧闪烁到 B 再回 A → 不切 (一致性防抖) */
    {
        ocg_reason_t r; int t;
        ocg_step(&g, B, 1, 0, &r);   /* 闪烁帧 */
        ocg_step(&g, A, 0, 0, &r);   /* 立即回 A */
        t = ocg_step(&g, A, 0, 0, &r);
        CHECK(t == -1, "测试7: 单帧闪烁→不切 (防抖)");
    }

    /* 测试 8: 同场景子簇 (漂离原簇但 argmax 仍 0) → 不切 (只跟踪) */
    {
        ocg_reason_t r; int t;
        float sub[3] = {0.55f, 0.05f, 0.40f};   /* 仍 scene 0, 但离原簇远 */
        t = -1;
        for (int i = 0; i < 5; i++) t = ocg_step(&g, sub, 0, 0, &r);
        CHECK(t == -1, "测试8: 同场景子簇→不切 (只跟踪)");
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
