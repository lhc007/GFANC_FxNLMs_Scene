/** SceneController — CNN + Blend + Wc 构造.
 *
 * 对应 Python: gfanc/SceneController.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scene_controller.h"

extern int cnn_m5_forward(const float *audio, float *logits);

int scene_ctrl_init(scene_ctrl_t *sc, const float *centroids,
                     const float *sub_filters, int filter_len)
{
    sc->centroids   = centroids;
    sc->sub_filters = sub_filters;
    sc->L           = filter_len;
    sc->cur_scene   = -1;
    memset(sc->prev_probs, 0, sizeof(sc->prev_probs));

    /* stub RMS: 所有子滤波器等权求和 → RMS */
    int S = SC_S, C = SC_C, L = filter_len;
    float *stub = (float *)calloc(S * L, sizeof(float));
    if (!stub) return -1;
    for (int c = 0; c < C; c++)
        for (int s = 0; s < S; s++)
            for (int l = 0; l < L; l++)
                stub[s * L + l] += sub_filters[(c * S + s) * L + l];
    float ss = 0;
    for (int i = 0; i < S * L; i++) ss += stub[i] * stub[i];
    sc->stub_rms = sqrtf(ss / (S * L));
    free(stub);
    return 0;
}

int scene_ctrl_process(scene_ctrl_t *sc, const float *audio,
                        float *wc_out, float *probs_out)
{
    int K = SC_K, S = SC_S, C = SC_C, L = sc->L;

    /* minmaxscaler (阈值 1e-6 防止静默信号过度放大 → CNN 输入爆炸) */
    float mx = audio[0], mn = audio[0];
    for (int i = 1; i < 16000; i++) {
        if (audio[i] > mx) mx = audio[i];
        if (audio[i] < mn) mn = audio[i];
    }
    float denom = mx - mn;

    /* 信号太弱 (<1%满幅峰峰值) → 不值得分类, 保持当前场景不切换
       避免深夜底噪被逐帧归一化放大后引发误分类污染 scene_wc */
    if (denom <= 0.01f) {
        if (sc->cur_scene >= 0) {
            scene_ctrl_construct_wc(sc, sc->cur_scene, wc_out);
            memcpy(probs_out, sc->prev_probs, K * sizeof(float));
            return sc->cur_scene;
        }
        memset(probs_out, 0, K * sizeof(float));
        probs_out[0] = 1.0f;
        scene_ctrl_construct_wc(sc, 0, wc_out);
        return 0;
    }

    float *cnn_in = (float *)malloc(16000 * sizeof(float));
    for (int i = 0; i < 16000; i++) cnn_in[i] = audio[i] / denom;

    /* CNN 前向 */
    float logits[SC_K];
    int cnn_ret = cnn_m5_forward(cnn_in, logits);
    free(cnn_in);

    if (cnn_ret != 0) {
        /* CNN 推理失败 (malloc 失败等), 保持上一帧场景 */
        if (sc->cur_scene >= 0) {
            scene_ctrl_construct_wc(sc, sc->cur_scene, wc_out);
            for (int k = 0; k < K; k++) probs_out[k] = sc->prev_probs[k];
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

    /* argmax */
    int scene_id = 0;
    for (int k = 1; k < K; k++) if (probs_out[k] > probs_out[scene_id]) scene_id = k;

    /* 构造 Wc */
    scene_ctrl_construct_wc(sc, scene_id, wc_out);

    sc->cur_scene = scene_id;
    return scene_id;
}

void scene_ctrl_construct_wc(const scene_ctrl_t *sc, int scene_id, float *wc_out)
{
    int S = SC_S, C = SC_C, L = sc->L, SC = S * C;

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

    /* 取反 (S-4修复: 不再强制对齐stub_rms, LMS功率归一化自动适应增益) */
    float rms_sq = 0;
    for (int i = 0; i < S * L; i++) rms_sq += wc_out[i] * wc_out[i];
    float wc_rms = sqrtf(rms_sq / (S * L));
    if (wc_rms < 1e-6f) {
        static int warn_cnt = 0;
        if (warn_cnt < 3) {
            fprintf(stderr, "[WARN] Wc RMS=%.6f near zero for scene=%d, ANC may be silent\n",
                    wc_rms, scene_id);
            warn_cnt++;
        }
    }
    for (int i = 0; i < S * L; i++) wc_out[i] = -wc_out[i];
}
