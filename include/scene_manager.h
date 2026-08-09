/** Scene Manager — 共享场景管理逻辑 (离线 main.c + 实时 main_realtime.c 共用).
 *
 *  提取两者中完全一致的状态机计算, 消除维护漂移风险 (ADVERSARIAL_REVIEW C1).
 *  注意: 此模块仅包含纯函数 (无 I/O, 无线程同步, 无全局状态).
 *        线程同步 (Interlocked + wc_shadow) 和 I/O 仍由各自主程序处理.
 *
 *  gfanc-direct-weight (v1.6): 已删除场景记忆/切换/滞回死代码
 *    (sm_scene_switch_execute / sm_first_sec_init / sm_check_scene_switch / sm_wc_rms),
 *    保留直接权重模式仍在用的纯函数.
 */
#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════
   共享计算函数 (纯函数, main.c + main_realtime.c 共用)
   ══════════════════════════════════════════════════════════ */

/** 计算两个向量之间的余弦相似度 (reset 判定: anchor vs current).
 *
 *  direct-weight 模式: 输入为 30 维子带增益向量 (S×C), 不再输入 softmax 概率.
 *
 *  @param anchor  锚点向量 (上次重置时的增益, K 维)
 *  @param cur     当前增益向量 (K 维)
 *  @param K       向量维数 (=S*C=30)
 *  @return        余弦相似度 ∈ [-1, 1], 1=完全相同, -1=完全相反
 */
static inline float sm_cos_sim(const float *anchor, const float *cur, int K)
{
    float dot = 0.0f, np = 0.0f, nc = 0.0f;
    for (int k = 0; k < K; k++) {
        dot += anchor[k] * cur[k];
        np  += anchor[k] * anchor[k];
        nc  += cur[k]  * cur[k];
    }
    return dot / (sqrtf(np) * sqrtf(nc) + 1e-10f);
}

/** 计算 Wc 系数数组中绝对值最大的元素.
 *
 *  @param wc   滤波器系数数组
 *  @param n    数组长度 (S*L)
 *  @return     max |wc[i]|
 */
static inline float sm_wc_max_abs(const float *wc, int n)
{
    float mx = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = fabsf(wc[i]);
        if (a > mx) mx = a;
    }
    return mx;
}

/** 检查 Wc 发散并更新 freeze 状态机.
 *
 *  此函数封装纯粹的状态机逻辑 (不涉及线程同步).
 *  实时版调用方负责 InterlockedExchange 设置 freeze_lms 和 ramp_cnt.
 *
 *  @param wc_max          当前 max|Wc| (从 wc_snapshot 计算)
 *  @param wc_init_max     收敛时的基准 max|Wc|
 *  @param freeze_ratio    超过 ratio×init_max → 触发 freeze
 *  @param freeze_retry_sec freeze 后重试等待秒数
 *  @param freeze_timer    倒计时器 (输入/输出): >0 冻结中, <0 观察期, 0 正常
 *  @param freeze_permanent 永久冻结标志 (输入/输出)
 *  @return  0=正常, 1=刚触发 freeze, 2=永久冻结, 3=解冻重试
 */
static inline int sm_check_divergence(
    float wc_max, float wc_init_max, float freeze_ratio,
    int freeze_retry_sec,
    int *freeze_timer, int *freeze_permanent)
{
    /* 新触发: 超过阈值且未冻结 */
    if (wc_max > wc_init_max * freeze_ratio) {
        if (!(*freeze_permanent) && *freeze_timer <= 0) {
            *freeze_timer = freeze_retry_sec;
            return 1;  /* 刚触发 freeze */
        }
    }

    /* 冻结中倒计时 */
    if (*freeze_timer > 0 && !(*freeze_permanent)) {
        (*freeze_timer)--;
        if (*freeze_timer <= 0) {
            *freeze_timer = -3;  /* 进入 3s 观察期 */
            return 3;  /* 解冻重试 */
        }
    }

    /* 观察期: 再次超限 → 永久冻结 */
    if (*freeze_timer < 0) {
        (*freeze_timer)++;
        if (wc_max > wc_init_max * freeze_ratio) {
            *freeze_permanent = 1;
            *freeze_timer = 0;
            return 2;  /* 永久冻结 */
        }
        if (*freeze_timer >= 0) {
            /* 观察期安全度过 → 恢复正常 */
            *freeze_timer = 0;
        }
    }

    return 0;  /* 无变化 */
}

/** 检查收敛并保存 Wc (去场景层后保存至单一 known-good Wc 供救援/回滚).
 *
 *  @param nr_level        当前 NR (dB)
 *  @param nr_threshold    收敛阈值 (dB)
 *  @param safety_mute     安全静音标志 (非零 → 跳过)
 *  @param diverged        发散标志 (非零 → 跳过)
 *  @param converged_frames 连续达标帧数 (输入/输出)
 *  @param scene_wc        收敛 Wc 保存目标 (旧场景记忆场景数组; 去场景层后传 last_good_wc)
 *  @param cur_scene_id    场景偏移 (旧 K 场景槽位 × wc_n; 去场景层后恒 0)
 *  @param wc_snapshot     收敛期 Wc 快照源 (调用方拷贝自 wc_shadow)
 *  @param wc_n            系数数 (S*L)
 *  @param wc_init_max     输出: 更新为收敛时 max|Wc| (自适应 freeze 基准)
 *  @return  1=本次帧触发了保存, 0=未触发
 */
static inline int sm_check_convergence(
    float nr_level, float nr_threshold,
    int safety_mute, int diverged,
    int *converged_frames,
    float *scene_wc, int cur_scene_id,
    const float *wc_snapshot, int wc_n,
    float *wc_init_max)
{
    if (nr_level > nr_threshold && !safety_mute && !diverged) {
        (*converged_frames)++;
        if (*converged_frames >= 3) {
            memcpy(scene_wc + cur_scene_id * wc_n, wc_snapshot, wc_n * sizeof(float));
            *converged_frames = 0;

            /* 自适应 freeze 阈值: 收敛期 max|Wc| → 基准 */
            float mx = sm_wc_max_abs(wc_snapshot, wc_n);
            if (mx > 0.001f) *wc_init_max = mx;
            return 1;
        }
    } else {
        *converged_frames = 0;
    }
    return 0;
}

/** 格式化 30 维直接权重增益中 |gain| 占比最高的 3 个子滤波器.
 *
 *  替代旧的 Band 列 (argmax 单值): 单看最大值信息量低 (CNN 输出层 bias[2] 恒占优,
 *  argmax 常钉死在一个低频带上), 但真实信息在整套增益向量的分布. top-3 + 占比
 *  能同时看到"哪个带最重"和"各带权重如何分配".
 *
 *  @param gains   30 维子带增益 (S×C, 直接权重输出, tanh 后 [-1,1])
 *  @param K       维数 (=S*C=30)
 *  @param buf     输出缓冲 (建议 ≥64B)
 *  @param buflen  buf 大小
 *  输出形如 "2(32%) 14(18%) 17(11%)"; 全部 ~0 时输出 "-".
 *  占比 = |gain[i]| / Σ|gain[j]| (各子滤波器对控制信号的权重份额).
 */
static inline void sm_fmt_top_gains(const float *gains, int K, char *buf, int buflen)
{
    int   top[3] = {-1, -1, -1};
    float tv[3]  = {0.0f, 0.0f, 0.0f};
    float sum = 0.0f;
    for (int i = 0; i < K; i++) {
        float a = fabsf(gains[i]);
        sum += a;
        if      (a > tv[0]) { tv[2]=tv[1]; top[2]=top[1]; tv[1]=tv[0]; top[1]=top[0]; tv[0]=a; top[0]=i; }
        else if (a > tv[1]) { tv[2]=tv[1]; top[2]=top[1]; tv[1]=a; top[1]=i; }
        else if (a > tv[2]) { tv[2]=a; top[2]=i; }
    }
    if (sum <= 1e-9f || top[0] < 0) { snprintf(buf, buflen, "-"); return; }
    int off = 0;
    for (int t = 0; t < 3 && top[t] >= 0; t++) {
        int pct = (int)lroundf(100.0f * tv[t] / sum);
        off += snprintf(buf + off, buflen - off, "%s%d(%d%%)", t ? " " : "", top[t], pct);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* SCENE_MANAGER_H */
