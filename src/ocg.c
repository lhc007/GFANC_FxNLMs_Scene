/** Online Clustering Gate (OCG) — 在线聚类闸门实现.
 *
 *  替代场景切换滞回检测的决策层 (详见 include/ocg.h). 方案来源:
 *  Luo et al., ICASSP 2026 "Stabilized Hybrid GFANC+FxNLMS with Online Clustering".
 *
 *  结构: 纯函数 + 调用方持有的 ocg_t (无全局, 无动态分配), 1Hz 主线程调用.
 *  相似度复用 scene_manager.h 的 sm_cos_sim.
 */
#include <stdio.h>
#include <string.h>
#include "ocg.h"
#include "scene_manager.h"

int ocg_init(ocg_t *ocg, const gfanc_config_t *cfg, int K)
{
    memset(ocg, 0, sizeof(*ocg));
    ocg->K = K;
    if (K < 1 || K > GFANC_K_MAX) return -1;

    ocg->enabled        = cfg->ocg_enable;
    ocg->alpha          = cfg->ocg_alpha;
    ocg->stay_thresh    = cfg->ocg_stay_thresh;
    ocg->rejoin_thresh  = cfg->ocg_rejoin_thresh;
    ocg->confirm_frames = cfg->ocg_confirm_frames;
    ocg->max_clusters   = cfg->ocg_max_clusters;
    if (ocg->max_clusters < 1) ocg->max_clusters = 1;
    if (ocg->max_clusters > OCG_MAX_CLUSTERS) ocg->max_clusters = OCG_MAX_CLUSTERS;

    ocg->active = -1;
    ocg->cand_scene = -1;
    return 0;
}

void ocg_reset(ocg_t *ocg, const float *probs, int scene)
{
    memset(ocg->cluster, 0, sizeof(ocg->cluster));
    ocg->n_clusters = 1;
    ocg->active = 0;
    ocg->cluster[0].valid = 1;
    memcpy(ocg->cluster[0].center, probs, ocg->K * sizeof(float));
    ocg->cluster[0].scene = scene;
    ocg->cluster[0].last_used = 0;
    ocg->frame = 0;
    ocg->cand_scene = -1; ocg->cand_cnt = 0;
}

/* 只更新活动簇中心 (漂移跟踪). probs 在单纯形内, 凸组合保持 sum=1, 无需归一化. */
static void ocg_drift(ocg_t *ocg, const float *probs)
{
    float alpha = ocg->alpha;
    float *c = ocg->cluster[ocg->active].center;
    for (int k = 0; k < ocg->K; k++) c[k] += alpha * (probs[k] - c[k]);
}

/* 新建簇 (上限 LRU 淘汰最久未命中簇; 活动簇每帧刷新 last_used, 不会被淘汰). */
static int ocg_create_cluster(ocg_t *ocg, const float *probs, int scene)
{
    int idx;
    if (ocg->n_clusters < ocg->max_clusters) {
        idx = ocg->n_clusters++;
    } else {
        idx = 0;
        for (int j = 1; j < ocg->n_clusters; j++)
            if (ocg->cluster[j].last_used < ocg->cluster[idx].last_used) idx = j;
    }
    ocg->cluster[idx].valid = 1;
    memcpy(ocg->cluster[idx].center, probs, ocg->K * sizeof(float));
    ocg->cluster[idx].scene = scene;
    ocg->cluster[idx].last_used = ocg->frame;
    ocg->active = idx;
    return idx;
}

int ocg_step(ocg_t *ocg, const float *probs, int new_scene,
             int cur_scene_id, ocg_reason_t *reason_out)
{
    int K = ocg->K;
    if (reason_out) *reason_out = OCG_REASON_NONE;
    if (K < 1 || !ocg->enabled) return -1;

    /* 首次运行兜底: 调用方未调 ocg_reset 时以当前 probs 建立基线 */
    if (ocg->n_clusters == 0 || ocg->active < 0) {
        ocg_reset(ocg, probs, new_scene);
        if (reason_out) *reason_out = OCG_REASON_STAY;
        return -1;
    }

    /* 置信不足帧 (probs 接近均匀, 如 K=3 时 ≈0.33): 不判定也不漂移,
       避免 CNN 犹豫帧污染簇/误切 (旧滞回在此类帧会因 cos 偏低切到 index 0). */
    if (probs[new_scene] < OCG_MIN_PEAK) {
        ocg->cand_scene = -1; ocg->cand_cnt = 0;
        return -1;
    }

    ocg->frame++;
    const float *ca = ocg->cluster[ocg->active].center;
    float cos_a = sm_cos_sim(ca, probs, K);

    /* ① 留在活动簇 (高置信): 漂移更新, 不切换 */
    if (cos_a >= ocg->stay_thresh) {
        ocg_drift(ocg, probs);
        ocg->cluster[ocg->active].last_used = ocg->frame;
        ocg->cand_scene = -1; ocg->cand_cnt = 0;
        if (reason_out) *reason_out = OCG_REASON_STAY;
        return -1;
    }

    /* ② 已离开活动簇: 找最匹配的已知簇 */
    int best_j = -1; float best_cos = -2.0f;
    for (int j = 0; j < ocg->n_clusters; j++) {
        float c = sm_cos_sim(ocg->cluster[j].center, probs, K);
        if (c > best_cos) { best_cos = c; best_j = j; }
    }

    int reason, target;
    if (best_j == ocg->active) {
        /* 仍最接近活动簇 (灰区 [rejoin, stay)): 保持跟踪 (防边界抖动建簇);
           低于 rejoin → 真离开所有簇 → 新簇 */
        if (best_cos >= ocg->rejoin_thresh) {
            ocg_drift(ocg, probs);
            ocg->cluster[ocg->active].last_used = ocg->frame;
            ocg->cand_scene = -1; ocg->cand_cnt = 0;
            if (reason_out) *reason_out = OCG_REASON_STAY;
            return -1;
        }
        reason = OCG_REASON_NEW;  target = new_scene;
    } else if (best_cos >= ocg->rejoin_thresh) {
        reason = OCG_REASON_REJOIN; target = ocg->cluster[best_j].scene;
    } else {
        reason = OCG_REASON_NEW;  target = new_scene;
    }

    /* 同场景族 (K=3 子簇): 只更新簇状态跟踪, 不触发 RESET.
       例: 场景0 内出现新噪声子族 → 建子簇记下, FxNLMS 不重初始化. */
    if (target == cur_scene_id) {
        if (reason == OCG_REASON_REJOIN) ocg->active = best_j;
        else ocg_create_cluster(ocg, probs, new_scene);
        ocg->cand_scene = -1; ocg->cand_cnt = 0;
        if (reason_out) *reason_out = OCG_REASON_STAY;
        return -1;
    }

    /* ③ 连续 N 帧一致性 (对应现有 3 帧滞回) */
    if (target == ocg->cand_scene) {
        ocg->cand_cnt++;
    } else {
        ocg->cand_scene = target; ocg->cand_cnt = 1;
    }
    if (ocg->cand_cnt < ocg->confirm_frames) return -1;   /* 未确认 */

    /* ④ 确认: 同步簇状态后返回目标场景 */
    if (reason == OCG_REASON_REJOIN) {
        ocg->active = best_j;
        ocg->cluster[best_j].last_used = ocg->frame;
    } else {
        ocg_create_cluster(ocg, probs, new_scene);
    }
    ocg->cand_scene = -1; ocg->cand_cnt = 0;
    if (reason_out) *reason_out = reason;
    return target;
}

float ocg_active_cos(const ocg_t *ocg, const float *probs)
{
    if (ocg->active < 0) return -1.0f;
    return sm_cos_sim(ocg->cluster[ocg->active].center, probs, ocg->K);
}
