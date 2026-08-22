/** SceneController — CNN 决策层 (SFANC 硬选库分类).
 *
 * 每秒一次: 1s 带通噪声 → minmax → CNN → argmax → 防抖 → 类索引 → 调用方选库槽.
 *
 * K (CNN 输出维) 从 cnn_bank_linear_weight.bin 大小自动推导 (cnn_m5_forward.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scenezone_types.h"
#include "scene_controller.h"
#include "cnn_m5_forward.h"

int scene_ctrl_init(scene_ctrl_t *sc)
{
    sc->K = cnn_m5_get_K();
    printf("  scene_ctrl: 分类模式 (K=%d, SFANC 硬选库 — deploy)\n", sc->K);
    sc->norm_denom_valid = 0;        /* 输入归一化 EMA 稳定标定 (2026-08-10) */
    sc->norm_ema_alpha   = 0.1f;
    /* SFANC 分类选库 初始状态: 未接入库, 无选定类, 防抖计数清零 */
    sc->bank_n           = 0;
    sc->sel_class        = 0;
    sc->cand_class       = -1;       /* 无候选 */
    sc->cand_cnt         = 0;
    sc->bank_hold_frames = 2;        /* GFANC_BANK_HOLD 默认 */
    return 0;
}

void scene_ctrl_free(scene_ctrl_t *sc)
{
    (void)sc;   /* 无动态分配, 保留为 no-op */
}

void scene_ctrl_set_bank(scene_ctrl_t *sc, int n_slots)
{
    sc->bank_n = (n_slots > 0) ? n_slots : 0;
    /* 库接入时重置决策状态: 从类 0 起步, 防抖从新起点计数 */
    if (sc->bank_n > 0) {
        sc->sel_class  = 0;
        sc->cand_class = -1;
        sc->cand_cnt   = 0;
    }
}

void scene_ctrl_set_bank_hold(scene_ctrl_t *sc, int hold_frames)
{
    sc->bank_hold_frames = (hold_frames > 0) ? hold_frames : 2;
}

/* 共享前置: minmax → EMA denom 稳定 → 归一化 → CNN 前向 → logits.
 * 返回 0=logits 有效, 1=弱信号 (logits 未填, 调用方保持当前状态),
 *      -1=CNN 失败 (logits 未填, 调用方保持当前状态).
 * R-24: 静态 cnn_in 缓冲, 单调用者 (主线程), 无重入风险. */
static int scene_ctrl_norm_forward(scene_ctrl_t *sc, const float *audio, float *logits)
{
    /* minmaxscaler (阈值 1e-6 防止静默信号过度放大 → CNN 输入爆炸) */
    float mx = audio[0], mn = audio[0];
    for (int i = 1; i < 16000; i++) {
        if (audio[i] > mx) mx = audio[i];
        if (audio[i] < mn) mn = audio[i];
    }
    float denom = mx - mn;

    /* 信号太弱 (<1%满幅峰峰值) → 不值得决策, 保持当前状态 */
    if (denom <= 0.01f) return 1;

    /* EMA 稳定标定 (2026-08-10): 每秒独立 denom 逐秒漂移 → CNN 输入逐秒抖
       (实机抖动根因). 平滑基准慢速跟随, 归一化用平滑值; 弱信号保底 (上面
       denom<=0.01 return) 仍用 raw denom, 静音帧不污染基准. */
    if (sc->norm_denom_valid)
        sc->norm_denom_smooth = (1.0f - sc->norm_ema_alpha) * sc->norm_denom_smooth
                              + sc->norm_ema_alpha * denom;
    else {
        sc->norm_denom_smooth = denom;
        sc->norm_denom_valid = 1;
    }
    float use_denom = fmaxf(sc->norm_denom_smooth, 0.01f);   /* 防除零/过小 */

    static float cnn_in_buf[16000];
    float *cnn_in = cnn_in_buf;
    for (int i = 0; i < 16000; i++) cnn_in[i] = audio[i] / use_denom;

    if (cnn_m5_forward(cnn_in, logits) != 0) return -1;
    return 0;
}

/* SFANC 分类决策 (deploy): audio_1s → minmax → CNN → argmax → 防抖 → 更新 sel_class.
 * 返回当前选定类索引 (0..K-1). logits_out[K] 可空 (诊断用).
 * 弱信号/CNN 失败 → 保持 sel_class.
 * 防抖 (GFANC_BANK_HOLD): 候选类需连续 bank_hold_frames 帧命中才切换 —
 * 抑制单帧 logits 抖动 (实机 CNN 稳定后 cos>0.95, 但偶发误分类帧会造成
 * 开环误选 → 反相更差, 计划风险 #1). 类真变 (多帧稳定) 才换库槽. */
int scene_ctrl_classify(scene_ctrl_t *sc, const float *audio_1s, float *logits_out)
{
    int K = sc->K;
    if (K < 1) return sc->sel_class;

    /* 共享前向 → logits (弱信号/CNN 失败保持当前类) */
    float logits[SC_DW_MAX];
    if (logits_out) memset(logits_out, 0, K * sizeof(float));
    if (scene_ctrl_norm_forward(sc, audio_1s, logits) != 0) {
        if (logits_out) memcpy(logits_out, logits, K * sizeof(float)); /* 全零 */
        return sc->sel_class;
    }

    int c = cnn_m5_argmax(logits, K);
    if (logits_out) memcpy(logits_out, logits, K * sizeof(float));

    if (sc->bank_n < 1) {           /* 未接入库: 决策层不工作, 仅返回 argmax */
        sc->sel_class = c;
        return c;
    }
    if (c >= sc->bank_n) c = sc->bank_n - 1;   /* 防御: argmax 越库槽上限 */

    /* 防抖: 候选类连续命中 */
    if (c == sc->cand_class) {
        sc->cand_cnt++;
    } else {
        sc->cand_class = c;
        sc->cand_cnt   = 1;
    }
    if (sc->cand_cnt >= sc->bank_hold_frames) {
        sc->sel_class = c;          /* 防抖通过 → 提交类切换 */
        sc->cand_cnt  = 0;
    }
    return sc->sel_class;
}
