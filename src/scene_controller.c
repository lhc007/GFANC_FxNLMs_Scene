/** SceneController — CNN 决策层 (去场景层 + SFANC 硬选库).
 *
 * 对应 Python: gfanc/SceneController.py (直接权重回归版, 参考 MIMO_GFANC
 *   Main_GFANC_FxNLMS_Reset.ipynb 的 soft 权重 Wc 构造).
 *
 * 每秒一次: 1s 带通噪声 → minmax → CNN → (calibrate) tanh 增益 → Wc 构造
 *   或 (deploy) argmax → 类索引 → 调用方选库槽.
 *
 * K (CNN 输出维) 从 cnn_linear_weight.bin 大小自动推导 (cnn_m5_forward.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scenezone_types.h"
#include "scene_controller.h"
#include "cnn_m5_forward.h"

int scene_ctrl_init(scene_ctrl_t *sc, const float *sub_filters, int filter_len)
{
    int S = SC_S, C = SC_C;
    sc->K = cnn_m5_get_K();
    if (sc->K != S * C) {
        fprintf(stderr, "[ERROR] direct-weight CNN expects K=%d (=S%d×C%d) outputs, "
                "got K=%d. Retrain with K=SC (see training/network/Train_validate.py).\n",
                S * C, S, C, sc->K);
        return -1;
    }
    sc->sub_filters = sub_filters;
    sc->L           = filter_len;
    sc->prev_gains_valid = 0;
    memset(sc->prev_gains, 0, sizeof(sc->prev_gains));
    sc->gain_smooth_beta   = 0.5f;   /* P0-2 默认, 可被 scene_ctrl_set_gain_smoothing 覆盖 */
    sc->gain_smooth_switch = 0.85f;
    sc->norm_denom_valid = 0;        /* 输入归一化 EMA 稳定标定 (2026-08-10) */
    sc->norm_ema_alpha   = 0.1f;
    /* SFANC 分类选库 (Phase 2) 初始状态: 未接入库, 无选定类, 防抖计数清零 */
    sc->bank_n           = 0;
    sc->sel_class        = 0;
    sc->cand_class       = -1;       /* 无候选 */
    sc->cand_cnt         = 0;
    sc->bank_hold_frames = 2;        /* GFANC_BANK_HOLD 默认 */

    /* stub RMS: 所有子滤波器等权求和 → RMS.
       R-24: 静态临时缓冲 (init 期一次性使用, ≤32KB) */
    int L = filter_len;
    static float stub_buf[GFANC_S_MAX * GFANC_L_MAX];
    float *stub = stub_buf;
    memset(stub, 0, S * L * sizeof(float));
    for (int c = 0; c < C; c++)
        for (int s = 0; s < S; s++)
            for (int l = 0; l < L; l++)
                stub[s * L + l] += sub_filters[(c * S + s) * L + l];
    float ss = 0;
    for (int i = 0; i < S * L; i++) ss += stub[i] * stub[i];
    sc->stub_rms = sqrtf(ss / (S * L));
    sc->wc_rms_target = sc->stub_rms;  /* 默认值, 实时版按 Ŝ 物理特性覆盖 */
    return 0;
}

void scene_ctrl_free(scene_ctrl_t *sc)
{
    (void)sc;   /* 无动态分配, 保留为 no-op */
}

/** P0-2 增益时间平滑参数 (beta∈[0,1], 1=关闭; switch_cos=场景切换旁路阈值). */
void scene_ctrl_set_gain_smoothing(scene_ctrl_t *sc, float beta, float switch_cos)
{
    sc->gain_smooth_beta   = (beta >= 0.0f && beta <= 1.0f) ? beta : 0.5f;
    sc->gain_smooth_switch = switch_cos;
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

/** 直接权重 Wc 构造: wc[s*L+l] = Σ_c gains[s*C+c] · sub[(c*S+s)*L+l].
 *  gains 为 tanh 输出 (带符号 [-1,1]), 不额外归一化/钳位.
 *  末尾 S-4 取反 + 缩放到目标 RMS (自动标定, 基于 Ŝ 物理衰减). */
void scene_ctrl_construct_wc(const scene_ctrl_t *sc, const float *gains, float *wc_out)
{
    int S = SC_S, C = SC_C, L = sc->L;

    for (int s = 0; s < S; s++)
        for (int l = 0; l < L; l++) {
            float v = 0;
            for (int c = 0; c < C; c++)
                v += gains[s * C + c] * sc->sub_filters[(c * S + s) * L + l];
            wc_out[s * L + l] = v;
        }

    /* S-4 取反. 然后缩放到目标 RMS (自动标定, 基于 Ŝ 物理衰减) */
    float rms_sq = 0;
    for (int i = 0; i < S * L; i++) rms_sq += wc_out[i] * wc_out[i];
    float wc_rms = sqrtf(rms_sq / (S * L));
    float target_rms = sc->wc_rms_target;
    if (wc_rms > 1e-6f && target_rms > 1e-6f) {
        float scale = target_rms / wc_rms;
        for (int i = 0; i < S * L; i++) wc_out[i] = -wc_out[i] * scale;
    } else {
        if (wc_rms < 1e-6f) {
            static int warn_cnt = 0;
            if (warn_cnt < 3) {
                fprintf(stderr, "[WARN] Wc RMS=%.6f near zero (all gains ~0)\n", wc_rms);
                warn_cnt++;
            }
        }
        for (int i = 0; i < S * L; i++) wc_out[i] = -wc_out[i];
    }
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

int scene_ctrl_process(scene_ctrl_t *sc, const float *audio,
                       float *wc_out, float *gains_out)
{
    int S = SC_S, C = SC_C, L = sc->L, SC = S * C;
    int K = sc->K;

    /* CNN 前向 → 30 维原始 logits */
    float logits[SC_DW_MAX];
    int rc = scene_ctrl_norm_forward(sc, audio, logits);
    if (rc != 0) {
        /* 弱信号 (1) 或 CNN 失败 (-1): 保持上一秒增益 (若有), 否则零增益
           (FxNLMS 从零自适应收敛) */
        if (sc->prev_gains_valid) {
            memcpy(gains_out, sc->prev_gains, SC * sizeof(float));
            scene_ctrl_construct_wc(sc, gains_out, wc_out);
            return 0;
        }
        memset(gains_out, 0, SC * sizeof(float));
        memset(wc_out, 0, S * L * sizeof(float));
        return 0;
    }

    /* tanh → 带符号子带增益 [-1,1] (与训练 tanh+MSE 一致) */
    int argmax = 0;
    float gmax = fabsf(logits[0]);
    for (int i = 0; i < SC; i++) {
        float g = tanhf(logits[i]);
        gains_out[i] = g;
        if (fabsf(g) > gmax) { gmax = fabsf(g); argmax = i; }
    }

    /* P0-2 自适应增益时间平滑: 纯音下 bands 跨秒翻转 (抖动) 会让 OCG/cos 闸门受害.
       大变化 (帧间 cos < switch, 场景真切换) → β=1 立即跟随, 无切换延迟;
       小抖动 (cos ≥ switch) → β 慢速 EMA 吸收, 增益方向稳定.
       注: 平滑后的 gains_out 同时用于 Wc 构造、cos 闸门、OCG 簇判定. */
    if (sc->prev_gains_valid && sc->gain_smooth_beta < 1.0f) {
        float dot = 0, na = 0, nb = 0;
        for (int i = 0; i < SC; i++) {
            dot += sc->prev_gains[i] * gains_out[i];
            na  += sc->prev_gains[i] * sc->prev_gains[i];
            nb  += gains_out[i] * gains_out[i];
        }
        float cos_prev = (na > 1e-9f && nb > 1e-9f)
                       ? dot / (sqrtf(na) * sqrtf(nb)) : 1.0f;
        float beta = (cos_prev < sc->gain_smooth_switch) ? 1.0f : sc->gain_smooth_beta;
        if (beta < 1.0f) {
            for (int i = 0; i < SC; i++)
                gains_out[i] = (1.0f - beta) * sc->prev_gains[i] + beta * gains_out[i];
        }
    }

    /* 直接权重构造 Wc + RMS 标定 + 取反 (用平滑后增益, 抑制 Wc 逐秒跳变) */
    scene_ctrl_construct_wc(sc, gains_out, wc_out);

    /* 更新历史增益 (存平滑后值, 作为下帧抖动比较基准) */
    memcpy(sc->prev_gains, gains_out, SC * sizeof(float));
    sc->prev_gains_valid = 1;
    (void)K;

    return argmax;
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
