/** SceneController — CNN 直接权重 Wc 构造 (去场景层).
 *
 * 对应 Python: gfanc/SceneController.py (直接权重回归版, 参考 MIMO_GFANC
 *   Main_GFANC_FxNLMS_Reset.ipynb 的 soft 权重 Wc 构造).
 *
 * 每秒一次: 1s 带通噪声 → minmax → CNN (K=S*C=30 维) → tanh 增益 →
 *   Wc[s,l] = Σ_c gain[s,c]·sub[c,s,l] → RMS 标定到 wc_rms_target + 取反.
 *
 * K (CNN 输出维) 从 cnn_linear_weight.bin 大小自动推导 (cnn_m5_forward.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfanc_types.h"
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

int scene_ctrl_process(scene_ctrl_t *sc, const float *audio,
                       float *wc_out, float *gains_out)
{
    int S = SC_S, C = SC_C, L = sc->L, SC = S * C;
    int K = sc->K;

    /* minmaxscaler (阈值 1e-6 防止静默信号过度放大 → CNN 输入爆炸) */
    float mx = audio[0], mn = audio[0];
    for (int i = 1; i < 16000; i++) {
        if (audio[i] > mx) mx = audio[i];
        if (audio[i] < mn) mn = audio[i];
    }
    float denom = mx - mn;

    /* 信号太弱 (<1%满幅峰峰值) → 不值得回归, 保持上一秒增益 */
    if (denom <= 0.01f) {
        if (sc->prev_gains_valid) {
            memcpy(gains_out, sc->prev_gains, SC * sizeof(float));
            scene_ctrl_construct_wc(sc, gains_out, wc_out);
            return 0;
        }
        /* 无历史 → 零增益 (FxNLMS 从零自适应收敛) */
        memset(gains_out, 0, SC * sizeof(float));
        memset(wc_out, 0, S * L * sizeof(float));
        return 0;
    }

    /* R-24: CNN 输入缓冲改为静态 (消除每秒 64KB malloc/free).
       单调用者 (主线程), 无重入风险. */
    static float cnn_in_buf[16000];
    float *cnn_in = cnn_in_buf;
    for (int i = 0; i < 16000; i++) cnn_in[i] = audio[i] / denom;

    /* CNN 前向 → 30 维原始 logits */
    float logits[SC_DW_MAX];
    int cnn_ret = cnn_m5_forward(cnn_in, logits);

    if (cnn_ret != 0) {
        /* CNN 推理失败 (malloc 失败等), 保持上一帧增益 */
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

    /* 直接权重构造 Wc + RMS 标定 + 取反 */
    scene_ctrl_construct_wc(sc, gains_out, wc_out);

    /* 更新历史增益 */
    memcpy(sc->prev_gains, gains_out, SC * sizeof(float));
    sc->prev_gains_valid = 1;
    (void)K;

    return argmax;
}
