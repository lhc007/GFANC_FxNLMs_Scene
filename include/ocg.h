/** Online Clustering Gate (OCG) — 在线聚类闸门.
 *
 *  替代"场景切换滞回检测"(sm_check_scene_switch) 的决策层. 方案来源:
 *  Luo et al., "A Stabilized Hybrid Active Noise Control Algorithm of
 *  GFANC and FxNLMS with Online Clustering", ICASSP 2026.
 *
 *  问题: GFANC/SFANC 场景切换是唯一的重初始化 FxNLMS 事件. CNN 每帧预测
 *  probs 的小幅抖动若每次都触发切换 → 打断自适应 → 不稳定 (现用静态滞回:
 *  冻结 anchor + cos<0.8 + 3 帧一致补丁). 论文用在线聚类闸门: 只在噪声真的
 *  进入新聚类时才更新滤波器, 避免无谓重初始化.
 *
 *  本模块移植思想, 在 K 维 probs 单纯形上维护若干在线聚类中心:
 *    - 活动簇中心以慢速移动平均跟踪当前噪声族的漂移 (解决冻结 anchor 过期
 *      → 噪声缓慢漂移误触切换);
 *    - probs 留在活动簇 (cos ≥ stay)        → 不切;
 *    - probs 离开活动簇且接近已知簇 (rejoin)  → 切回该簇 (复用, 快速识别回归场景);
 *    - probs 离开所有簇                       → 新建簇 (上限 LRU 淘汰), 切换;
 *    - 保留连续 N 帧一致性 (对应现有 3 帧滞回);
 *    - 置信不足帧 (probs[argmax] < OCG_MIN_PEAK) 不判定不漂移 (保守).
 *
 *  只返回"目标场景 id 或 -1"; 切换机制仍由调用方走 sm_scene_switch_execute.
 *  调用方持有 ocg_t (无全局), 1Hz 主线程调用 (无锁需求).
 */
#ifndef OCG_H
#define OCG_H

#include "gfanc_types.h"   /* gfanc_config_t, GFANC_K_MAX */

#define OCG_MAX_CLUSTERS 8     /* 簇数量编译期上限 (栈数组) */
#define OCG_MIN_PEAK     0.5f  /* 最低 argmax 概率: 低于此视为"置信不足", 不判定 (内部常数) */

typedef enum {
    OCG_REASON_NONE   = 0,     /* 无动作 (候选未确认 / 置信不足 / 同场景) */
    OCG_REASON_STAY   = 1,     /* 留在活动簇 (含灰区跟踪), 不切换 */
    OCG_REASON_REJOIN = 2,     /* 离开活动簇, 回归已知簇 (确认后切换) */
    OCG_REASON_NEW    = 3      /* 离开所有簇, 新建簇 (确认后切换) */
} ocg_reason_t;

typedef struct {
    float center[GFANC_K_MAX]; /* 簇中心 (K 维 probs 单纯形, 凸组合保持 sum=1) */
    int   scene;               /* 该簇关联场景 id (创建时 argmax) */
    int   valid;               /* 是否已初始化 */
    int   last_used;           /* 最近命中帧计数 (LRU 淘汰依据) */
} ocg_cluster_t;

typedef struct {
    /* 维度/参数 (init 时从 gfanc_config 拷贝) */
    int   K;                   /* probs 维度 (=场景数) */
    int   enabled;             /* 1=OCG 生效, 0=调用方走旧滞回 */
    float alpha;               /* 活动簇漂移学习率 (默认0.10) */
    float stay_thresh;         /* 留在活动簇 cos 下限 (默认0.90) */
    float rejoin_thresh;       /* 识别已知簇 cos 下限 (默认0.75) */
    int   confirm_frames;      /* 确认切换所需连续帧数 (默认3) */
    int   max_clusters;        /* 簇数量上限 (默认4, 钳到[1,OCG_MAX_CLUSTERS]) */

    /* 状态 (运行时) */
    ocg_cluster_t cluster[OCG_MAX_CLUSTERS];
    int  n_clusters;           /* 当前簇数 */
    int  active;               /* 活动簇索引 (-1=无) */
    int  cand_scene;           /* 候选目标场景 (-1=无) */
    int  cand_cnt;             /* 候选连续帧数 */
    int  frame;                /* 帧计数 (LRU/调试) */
} ocg_t;

/** 初始化. @return 0 成功 (K 非法/超上限时仍置 0 并返回 -1). */
int  ocg_init(ocg_t *ocg, const gfanc_config_t *cfg, int K);

/** 重置: 清空所有簇, 以当前 probs 播种唯一活动簇. 在首次 INIT 时调用. */
void ocg_reset(ocg_t *ocg, const float *probs, int scene);

/** 每帧判定 (1Hz 主线程).
 *  @param probs        当前帧 softmax 概率 [K]
 *  @param new_scene    argmax(probs)
 *  @param cur_scene_id 当前场景
 *  @param reason_out   输出本帧动作 (STAY/REJOIN/NEW; 未确认时 NONE), 可 NULL
 *  @return >=0 = 确认切换的目标场景 id; -1 = 不切换.
 *          确认切换时内部已同步簇状态 (REJOIN→切 active 到该簇;
 *          NEW→创建簇并设 active). 同场景族 (target==cur) 只跟踪不返回. */
int  ocg_step(ocg_t *ocg, const float *probs, int new_scene,
              int cur_scene_id, ocg_reason_t *reason_out);

/** 当前 probs 到活动簇中心的余弦相似度 (诊断显示用, 无簇时返回 -1). */
float ocg_active_cos(const ocg_t *ocg, const float *probs);

#endif /* OCG_H */
