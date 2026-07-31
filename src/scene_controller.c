/** SceneController — CNN + Blend + Wc 构造.
 *
 * 对应 Python: gfanc/SceneController.py
 *
 * K 从 scene_defs.bin 自动推导: K = n_centroids / (S * C)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfanc_types.h"
#include "scene_controller.h"
#include "cnn_m5_forward.h"

int scene_ctrl_init(scene_ctrl_t *sc, const float *centroids,
                     const float *sub_filters, int filter_len,
                     int n_centroids)
{
    int S = SC_S, C = SC_C;
    sc->K = n_centroids / (S * C);
    if (sc->K < 1 || sc->K > SC_K_MAX) {
        fprintf(stderr, "[ERROR] Invalid K=%d (n_centroids=%d, S=%d, C=%d)\n",
                sc->K, n_centroids, S, C);
        return -1;
    }
    sc->centroids   = centroids;
    sc->sub_filters = sub_filters;
    sc->L           = filter_len;
    sc->cur_scene   = -1;
    /* R-24: prev_probs 改为定长数组 (SC_K_MAX floats in struct), 无需动态分配 */
    memset(sc->prev_probs, 0, sizeof(sc->prev_probs));

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
    /* R-24: prev_probs 为定长数组, 无需释放. 此处保留为 no-op 供将来扩展. */
    (void)sc;
}

/* 概率加权混合 Wc: wc_out = Σ_k probs[k] * Wc_k / Σ probs[k]
   (忽略 <5% 的低概率场景以节省计算, 不影响总权重归一化)
   R-24: 临时缓冲区改为静态 (消除每秒 malloc/free, S*L≤8192 floats=32KB). */
static void scene_ctrl_blend_wc(const scene_ctrl_t *sc, const float *probs,
                                 float *wc_out)
{
    int K = sc->K, S = SC_S, C = SC_C, L = sc->L;
    int n = S * L;

    /* R-24: 静态临时缓冲区 (单场景 Wc 构造用, K 次调用共享).
       最大 S*L = GFANC_S_MAX * GFANC_L_MAX = 4*2048 = 8192 floats = 32KB.
       单调用者 (主线程), 无重入风险. */
    static float wc_k_buf[GFANC_S_MAX * GFANC_L_MAX];

    if (n > (int)(sizeof(wc_k_buf) / sizeof(wc_k_buf[0]))) {
        /* 理论上不会发生: S*L 超过缓冲上限. 回退到 argmax 单场景. */
        int best = 0;
        for (int k = 1; k < K; k++) if (probs[k] > probs[best]) best = k;
        scene_ctrl_construct_wc(sc, best, wc_out);
        return;
    }
    float *wc_k = wc_k_buf;

    memset(wc_out, 0, n * sizeof(float));
    float total_weight = 0.0f;
    for (int k = 0; k < K; k++) {
        if (probs[k] < 0.05f) continue;   /* 忽略低概率场景, 显著节省计算 */
        scene_ctrl_construct_wc(sc, k, wc_k);
        for (int i = 0; i < n; i++)
            wc_out[i] += probs[k] * wc_k[i];
        total_weight += probs[k];
    }

    if (total_weight > 0.0f) {
        float inv = 1.0f / total_weight;
        for (int i = 0; i < n; i++) wc_out[i] *= inv;
    } else {
        /* 所有 probs < 5% (理论上不会发生, 安全回退) */
        int best = 0;
        for (int k = 1; k < K; k++) if (probs[k] > probs[best]) best = k;
        scene_ctrl_construct_wc(sc, best, wc_out);
    }
}

int scene_ctrl_process(scene_ctrl_t *sc, const float *audio,
                        float *wc_out, float *probs_out)
{
    int K = sc->K, S = SC_S, C = SC_C, L = sc->L;

    /* minmaxscaler (阈值 1e-6 防止静默信号过度放大 → CNN 输入爆炸) */
    float mx = audio[0], mn = audio[0];
    for (int i = 1; i < 16000; i++) {
        if (audio[i] > mx) mx = audio[i];
        if (audio[i] < mn) mn = audio[i];
    }
    float denom = mx - mn;

    /* 信号太弱 (<1%满幅峰峰值) → 不值得分类, 保持当前概率分布
       避免深夜底噪被逐帧归一化放大后引发误分类污染 scene_wc */
    if (denom <= 0.01f) {
        if (sc->cur_scene >= 0) {
            memcpy(probs_out, sc->prev_probs, K * sizeof(float));
            scene_ctrl_blend_wc(sc, probs_out, wc_out);
            return sc->cur_scene;
        }
        memset(probs_out, 0, K * sizeof(float));
        probs_out[0] = 1.0f;
        scene_ctrl_construct_wc(sc, 0, wc_out);
        return 0;
    }

    /* R-24: CNN 输入缓冲改为静态 (消除每秒 64KB malloc/free).
       单调用者 (主线程), 无重入风险. */
    static float cnn_in_buf[16000];
    float *cnn_in = cnn_in_buf;
    for (int i = 0; i < 16000; i++) cnn_in[i] = audio[i] / denom;

    /* CNN 前向 */
    float logits[SC_K_MAX];
    int cnn_ret = cnn_m5_forward(cnn_in, logits);

    if (cnn_ret != 0) {
        /* CNN 推理失败 (malloc 失败等), 保持上一帧概率分布 */
        if (sc->cur_scene >= 0) {
            for (int k = 0; k < K; k++) probs_out[k] = sc->prev_probs[k];
            scene_ctrl_blend_wc(sc, probs_out, wc_out);
            return sc->cur_scene;
        }
        /* 无历史场景 → 回退到 scene 0 */
        memset(probs_out, 0, K * sizeof(float));
        probs_out[0] = 1.0f;
        scene_ctrl_construct_wc(sc, 0, wc_out);
        return 0;
    }

    /* softmax */
    float logit_mx = logits[0], sum_exp = 0;
    for (int k = 0; k < K; k++) if (logits[k] > logit_mx) logit_mx = logits[k];
    for (int k = 0; k < K; k++) { probs_out[k] = expf(logits[k] - logit_mx); sum_exp += probs_out[k]; }
    for (int k = 0; k < K; k++) probs_out[k] /= sum_exp;

    /* argmax (仅用于日志/滞回检测, 不参与 Wc 构造) */
    int scene_id = 0;
    for (int k = 1; k < K; k++) if (probs_out[k] > probs_out[scene_id]) scene_id = k;

    /* 概率加权混合 Wc (软分配 — 替代旧的单场景 argmax) */
    scene_ctrl_blend_wc(sc, probs_out, wc_out);

    sc->cur_scene = scene_id;
    memcpy(sc->prev_probs, probs_out, K * sizeof(float));
    return scene_id;
}

void scene_ctrl_construct_wc(const scene_ctrl_t *sc, int scene_id, float *wc_out)
{
    int S = SC_S, C = SC_C, L = sc->L, SC = S * C;

    /* R-4: 防御性钳位 — scene_id 越界时回退到 scene 0 (不应发生, 但兜底) */
    if (scene_id < 0 || scene_id >= sc->K) {
        fprintf(stderr, "[WARN] construct_wc: scene_id=%d out of [0,%d), clamp to 0\n",
                scene_id, sc->K);
        scene_id = 0;
    }

    /* blend = centroid[scene_id] */
    const float *blend = sc->centroids + scene_id * SC;
    float bmax = blend[0];
    for (int i = 1; i < SC; i++) if (blend[i] > bmax) bmax = blend[i];
    float inv_max = 1.0f / (bmax + 1e-10f);

    /* Wc[s,l] = Σ_c blend[s,c] * sub[c,s,l] */
    for (int s = 0; s < S; s++)
        for (int l = 0; l < L; l++) {
            float v = 0;
            for (int c = 0; c < C; c++) {
                float b = blend[s * C + c] * inv_max;
                if (b < 0) b = 0; if (b > 1) b = 1;
                v += b * sc->sub_filters[(c * S + s) * L + l];
            }
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
                fprintf(stderr, "[WARN] Wc RMS=%.6f near zero for scene=%d\n",
                        wc_rms, scene_id);
                warn_cnt++;
            }
        }
        for (int i = 0; i < S * L; i++) wc_out[i] = -wc_out[i];
    }
}
