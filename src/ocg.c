/** Online Clustering Gate (OCG) — 多质心在线聚类闸门实现 (ICASSP 2026).
 *
 *  纯函数 + 调用方持有的 ocg_t (无全局, 无动态分配), 1Hz 主线程调用.
 *  相似度复用 scene_manager.h 的 sm_cos_sim (增益域, 尺度不变).
 */
#include <math.h>
#include <string.h>
#include "ocg.h"
#include "scene_manager.h"

int ocg_init(ocg_t *ocg, int K, float tau_cos, float alpha, int max_clusters)
{
    memset(ocg, 0, sizeof(*ocg));
    if (K < 1 || K > GFANC_C_MAX * GFANC_S_MAX) return -1;

    ocg->K = K;
    ocg->tau_cos = (tau_cos > 0.0f) ? tau_cos : 0.8f;
    ocg->alpha = (alpha > 0.0f) ? alpha : 0.1f;
    ocg->max_clusters = (max_clusters < 1) ? 1 : max_clusters;
    if (ocg->max_clusters > OCG_MAX_CLUSTERS) ocg->max_clusters = OCG_MAX_CLUSTERS;
    ocg->active = -1;
    return 0;
}

/* 增益向量 → 单位方向向量 (存质心) */
static void gains_to_unit(const float *gains, float *out, int K)
{
    float n = 0.0f;
    for (int k = 0; k < K; k++) n += gains[k] * gains[k];
    n = sqrtf(n);
    if (n < 1e-8f) {
        /* 零增益 (弱信号保持) 无方向 — 质心留零, 不参与匹配 */
        memset(out, 0, K * sizeof(float));
        return;
    }
    for (int k = 0; k < K; k++) out[k] = gains[k] / n;
}

void ocg_reset(ocg_t *ocg, const float *gains)
{
    memset(ocg->clusters, 0, sizeof(ocg->clusters));
    ocg->n_clusters = 1;
    ocg->active = 0;
    ocg->frame = 0;

    ocg_cluster_t *c0 = &ocg->clusters[0];
    c0->valid = 1;
    c0->count = 1;
    c0->last_used = 0;
    gains_to_unit(gains, c0->center, ocg->K);
    if (c0->center[0] == 0.0f) c0->center[0] = 1.0f; /* 零增益兜底: 单位 e0 */
}

/* 新建簇 (上限 LRU 淘汰最久未命中簇; 活动簇每帧刷新 last_used, 不会被淘汰) */
static int ocg_create_cluster(ocg_t *ocg, const float *gains)
{
    int idx;
    if (ocg->n_clusters < ocg->max_clusters) {
        idx = ocg->n_clusters++;
    } else {
        idx = 0;
        for (int j = 1; j < ocg->n_clusters; j++)
            if (ocg->clusters[j].last_used < ocg->clusters[idx].last_used) idx = j;
    }
    ocg_cluster_t *c = &ocg->clusters[idx];
    memset(c, 0, sizeof(*c));
    c->valid = 1;
    c->count = 1;
    c->last_used = ocg->frame;
    gains_to_unit(gains, c->center, ocg->K);
    if (c->center[0] == 0.0f) c->center[0] = 1.0f;
    return idx;
}

int ocg_step(ocg_t *ocg, const float *gains)
{
    ocg->frame++;

    /* 零增益 (弱信号保持/CNN 失败) → 无方向, 保持当前滤波器 */
    float n = 0.0f;
    for (int k = 0; k < ocg->K; k++) n += gains[k] * gains[k];
    if (n < 1e-8f) return 0;

    /* 最近簇 (余弦相似度, 尺度不变) */
    int best = -1;
    float best_cos = -2.0f;
    for (int j = 0; j < ocg->n_clusters; j++) {
        if (!ocg->clusters[j].valid) continue;
        float c = sm_cos_sim(ocg->clusters[j].center, gains, ocg->K);
        if (c > best_cos) { best_cos = c; best = j; }
    }

    int kp;
    if (best < 0 || best_cos < ocg->tau_cos) {
        /* 距所有簇 > τ → 新噪声模式, 新建簇 (k' 必然 != active) */
        kp = ocg_create_cluster(ocg, gains);
    } else {
        kp = best;
        /* 归入最近簇: 质心 EMA 漂移 (方向空间) — 吸收慢漂移/簇内抖动,
           使 g' 长期留在簇内而不触发更换 (论文式 (3) 的连续版本) */
        ocg_cluster_t *c = &ocg->clusters[kp];
        c->last_used = ocg->frame;
        c->count++;
        float a = ocg->alpha;
        float *ctr = c->center;
        float acc = 0.0f;
        for (int k = 0; k < ocg->K; k++) {
            ctr[k] += a * (gains[k] - ctr[k]);
            acc += ctr[k] * ctr[k];
        }
        acc = sqrtf(acc);
        if (acc > 1e-12f)
            for (int k = 0; k < ocg->K; k++) ctr[k] /= acc;
    }

    /* 论文式 (4): 仅簇索引变化才更换滤波器 */
    if (ocg->active < 0) {   /* 防御: 未初始化 (INIT 应已 ocg_reset) */
        ocg->active = kp;
        return 0;
    }
    if (kp != ocg->active) {
        ocg->active = kp;
        return 1;
    }
    return 0;
}
