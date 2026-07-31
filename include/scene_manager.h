/** Scene Manager — 共享场景管理逻辑 (离线 main.c + 实时 main_realtime.c 共用).
 *
 *  提取两者中完全一致的状态机计算, 消除维护漂移风险 (ADVERSARIAL_REVIEW C1).
 *  注意: 此模块仅包含纯函数 (无 I/O, 无线程同步, 无全局状态).
 *        线程同步 (Interlocked + wc_shadow) 和 I/O 仍由各自主程序处理.
 */
#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════
   共享计算函数 (纯函数, main.c + main_realtime.c 共用)
   ══════════════════════════════════════════════════════════ */

/** 计算两个概率分布之间的余弦相似度 (S-1: anchor vs current).
 *  用于场景切换滞回检测 — cos < threshold 且场景不同 → 候选切换.
 *
 *  @param anchor  进入当前场景时的概率锚点 (K 维)
 *  @param probs   当前帧 softmax 概率 (K 维)
 *  @param K       场景数
 *  @return        余弦相似度 ∈ [-1, 1], 1=完全相同, -1=完全相反
 */
static inline float sm_cos_sim(const float *anchor, const float *probs, int K)
{
    float dot = 0.0f, np = 0.0f, nc = 0.0f;
    for (int k = 0; k < K; k++) {
        dot += anchor[k] * probs[k];
        np  += anchor[k] * anchor[k];
        nc  += probs[k]  * probs[k];
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

/** 计算 Wc 的 RMS 值.
 *
 *  @param wc   滤波器系数数组
 *  @param n    数组长度 (S*L)
 *  @return     sqrt(Σ wc² / n)
 */
static inline float sm_wc_rms(const float *wc, int n)
{
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += wc[i] * wc[i];
    return sqrtf(ss / (float)n);
}

/** 执行场景切换: 保存旧 Wc → 恢复新 Wc → 可选 CNN 预设回退 → 初始化 CrossFader.
 *
 *  此函数封装主线程中的场景切换逻辑 (实时版和离线版完全一致).
 *  实时版调用方负责通过 wc_shadow+wc_seq 提交 Wc (本函数不处理).
 *
 *  @param scene_wc        场景记忆数组 [K][S*L]
 *  @param scene_wc_valid  场景记忆有效标志 [K]
 *  @param cur_scene_id    当前场景 ID (输入/输出)
 *  @param new_scene       目标场景 ID
 *  @param wc_snapshot     当前 Wc 快照 (保存到旧场景记忆)
 *  @param wc_cur          新场景 Wc (输出: 被恢复的记忆或 CNN 预设覆盖)
 *  @param wc_old          CrossFader 旧端 Wc (输出: 被当前快照覆盖)
 *  @param wc_n            每个扬声器的系数数 (=S*L)
 *  @param wc_cold_start   首次场景衰减 (0.3=30%起步, 1.0=关闭)
 *  @return 1=使用了已收敛记忆, 0=使用了 CNN 预设 (首次)
 */
static inline int sm_scene_switch_execute(
    float *scene_wc, int *scene_wc_valid,
    int *cur_scene_id, int new_scene,
    const float *wc_snapshot,
    float *wc_cur, float *wc_old,
    int wc_n, float wc_cold_start)
{
    int restored = 0;

    /* 保存旧场景的当前 Wc (快照是回调最新值, 比 wc_cur 更实时) */
    memcpy(scene_wc + (*cur_scene_id) * wc_n, wc_snapshot, wc_n * sizeof(float));
    scene_wc_valid[*cur_scene_id] = 1;

    /* 新场景: 有收敛记忆就用记忆, 否则用 CNN 预设 (已在 wc_cur 中) */
    if (scene_wc_valid[new_scene]) {
        memcpy(wc_cur, scene_wc + new_scene * wc_n, wc_n * sizeof(float));
        restored = 1;
    } else {
        /* 冷启动衰减: 首次未收敛场景, CNN 预设可能偏离真实噪声,
           LMS 从低向上收敛 (安全) 避免从过高值 overshoot 发散. */
        if (wc_cold_start < 1.0f && wc_cold_start > 0.0f) {
            for (int i = 0; i < wc_n; i++) wc_cur[i] *= wc_cold_start;
        }
        /* CNN 预设同步存入场景记忆 — 防止下次恢复时拿到未初始化的空滤波器 */
        memcpy(scene_wc + new_scene * wc_n, wc_cur, wc_n * sizeof(float));
        scene_wc_valid[new_scene] = 1;
    }

    /* CrossFader: wc_old = 切换瞬间的当前 Wc (过渡起点) */
    memcpy(wc_old, wc_snapshot, wc_n * sizeof(float));

    *cur_scene_id = new_scene;
    return restored;
}

/** 首次初始化: 存入 CNN 预设 Wc 作为场景记忆, 计算 wc_init_max 作为 freeze 基准.
 *
 *  @param scene_wc        场景记忆数组
 *  @param scene_wc_valid  场景记忆有效标志
 *  @param cur_scene_id    当前场景 ID (输出)
 *  @param new_scene       CNN 分类的场景 ID
 *  @param wc_cur          CNN 构造的初始 Wc (in/out: 会被 wc_cold_start 衰减)
 *  @param wc_n            系数数 (=S*L)
 *  @param wc_init_max     输出: max|Wc| 作为 freeze 基准 (最小 0.01)
 *  @param wc_cold_start   首次衰减系数 (0.3=30%起步, 1.0=关闭)
 */
static inline void sm_first_sec_init(
    float *scene_wc, int *scene_wc_valid,
    int *cur_scene_id, int new_scene,
    float *wc_cur, int wc_n,
    float *wc_init_max, float wc_cold_start)
{
    /* 冷启动衰减: CNN 预设可能大幅偏离当前噪声 → LMS 从低向上收敛, 防 overshoot */
    if (wc_cold_start < 1.0f && wc_cold_start > 0.0f) {
        for (int i = 0; i < wc_n; i++) wc_cur[i] *= wc_cold_start;
    }

    memcpy(scene_wc + new_scene * wc_n, wc_cur, wc_n * sizeof(float));
    scene_wc_valid[new_scene] = 1;
    *cur_scene_id = new_scene;

    float mx = sm_wc_max_abs(wc_cur, wc_n);
    *wc_init_max = (mx > 0.001f) ? mx : 0.01f;
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

/** 检查收敛并保存 Wc 到场景记忆.
 *
 *  @param nr_level        当前 NR (dB)
 *  @param nr_threshold    收敛阈值 (dB)
 *  @param safety_mute     安全静音标志 (非零 → 跳过)
 *  @param diverged        发散标志 (非零 → 跳过)
 *  @param converged_frames 连续达标帧数 (输入/输出)
 *  @param scene_wc        场景记忆
 *  @param cur_scene_id    当前场景
 *  @param wc_snapshot     当前 Wc (收敛时保存至此)
 *  @param wc_n            系数数
 *  @param wc_init_max     输出: 更新为收敛时 max|Wc| (自适应 freeze 基准)
 *  @return  1=本次帧触发了 scene_wc 保存, 0=未触发
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

/** 场景切换滞回检测: 候选场景需连续 confirm_frames 帧一致才确认.
 *
 *  返回值语义: 0=无切换, 1=确认切换 (调用方随后执行 sm_scene_switch_execute)
 *
 *  @param cos_sim         当前帧 cos(anchor, probs)
 *  @param switch_threshold 切换阈值 (cos < threshold → 候选)
 *  @param new_scene       当前帧 CNN 分类 scene_id
 *  @param cur_scene_id    当前场景
 *  @param scene_cand      候选场景 (输入/输出)
 *  @param scene_cand_cnt  候选连续帧数 (输入/输出)
 *  @param confirm_frames  确认所需帧数 (通常=3)
 *  @return  1=确认切换, 0=未确认
 */
static inline int sm_check_scene_switch(
    float cos_sim, float switch_threshold,
    int new_scene, int cur_scene_id,
    int *scene_cand, int *scene_cand_cnt,
    int confirm_frames)
{
    if (cos_sim < switch_threshold && new_scene != cur_scene_id) {
        if (new_scene == *scene_cand)
            (*scene_cand_cnt)++;
        else {
            *scene_cand = new_scene;
            *scene_cand_cnt = 1;
        }
        if (*scene_cand_cnt >= confirm_frames) {
            *scene_cand = -1;
            *scene_cand_cnt = 0;
            return 1;
        }
    } else {
        *scene_cand = -1;
        *scene_cand_cnt = 0;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SCENE_MANAGER_H */
