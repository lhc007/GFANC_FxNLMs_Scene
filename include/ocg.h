/** Online Clustering Gate (OCG) — 多质心在线聚类闸门 (ICASSP 2026).
 *
 *  方案来源: Luo et al., ICASSP 2026 "A Stabilized Hybrid Active Noise
 *  Control Algorithm of GFANC and FxNLMS with Online Clustering".
 *
 *  问题背景 (双速率混合): CNN 每秒预测增益向量 g' → 生成控制滤波器 Wc,
 *  FxNLMS 在采样率持续自适应. 若 g' 的微小抖动反复触发滤波器更换,
 *  FxNLMS 的收敛过程被反复打断 → 误差波动/系统不稳定 (论文 Fig.4).
 *
 *  机制 (论文 §2.3, 从 [0,1]^8 权重域适配到 tanh[-1,1]^K 增益域):
 *    - 对 g' 按余弦相似度归入最近质心簇 (质心 = 单位方向向量)
 *    - max cos < τ → 新建簇 (满员 LRU 淘汰最久未命中); 否则归入最近簇,
 *      质心沿 g' 方向 EMA 漂移 (α) — 吸收慢漂移/簇内抖动
 *    - 仅当簇索引 k' ≠ 活动簇 k_g → 返回"更换滤波器" (论文式 (4))
 *
 *  与单锚点 cos 闸门 (v1.6, cos(anchor,cur)<τ) 的差别:
 *    - 慢漂移: 锚点冻结 → 漂移累计超阈值反复重置; 质心跟随 → 不触发
 *    - 簇内抖动: 锚点被抖动点反复覆盖 → 每帧重置; 质心稳定 → 不触发
 *    - 循环 A→B→A: 两者切换时都会触发 (论文同样如此, 聚类只抑制簇内抖动)
 *
 *  与旧版 (v1.5 ocg.c, 已删除) 的差别: 旧版聚类 softmax 概率向量 + 场景
 *  标签 + stay/rejoin/confirm 滞回; 本版聚类直接权重增益向量, 无场景
 *  概念, 忠实论文的簇索引闸门 (无确认帧延迟).
 */
#ifndef OCG_H
#define OCG_H

#include "gfanc_types.h"

#define OCG_MAX_CLUSTERS 8   /* 簇上限 (LRU 淘汰); 典型噪声模式 1-3 簇足够 */

typedef struct {
    float center[GFANC_C_MAX * GFANC_S_MAX];  /* 质心 (单位方向向量, K 维) */
    unsigned count;          /* 簇内累计样本数 (统计用) */
    unsigned last_used;      /* 最近命中帧号 (LRU 淘汰) */
    int      valid;
} ocg_cluster_t;

typedef struct {
    int   K;                 /* 增益向量维数 = S*C = 30 */
    float tau_cos;           /* 簇半径阈值: cos(g',c) >= τ 归入, 否则新建簇
                                (复用 cfg.switch_threshold, 默认 0.8) */
    float alpha;             /* 质心 EMA 漂移系数 (0~1; 0.1=慢漂移吸收) */
    int   max_clusters;      /* 簇上限 */
    ocg_cluster_t clusters[OCG_MAX_CLUSTERS];
    int   n_clusters;        /* 当前簇数 (诊断) */
    int   active;            /* 当前滤波器的簇索引 (诊断) */
    unsigned frame;          /* 帧计数器 (LRU 时间戳) */
} ocg_t;

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 (在 K 确定后调用, 即 scene_ctrl_init 之后). 失败返回 -1. */
int  ocg_init(ocg_t *ocg, int K, float tau_cos, float alpha, int max_clusters);

/** 用首个增益向量建立簇 0 (INIT 时调用; 之后簇状态由 ocg_step 维护). */
void ocg_reset(ocg_t *ocg, const float *gains);

/** 逐帧决策: 1=应更换滤波器 (簇索引变化), 0=保持 (簇内抖动/漂移). */
int  ocg_step(ocg_t *ocg, const float *gains);

#ifdef __cplusplus
}
#endif

#endif /* OCG_H */
