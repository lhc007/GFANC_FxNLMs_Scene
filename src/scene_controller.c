/** SceneController C 实现 — Blend + Wc 构造.

对应 Python: gfanc/SceneController.py
    CNN logits → hard selection (argmax) → 取 centroid → construct_wc()
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfanc_types.h"
#include "cnn_scene.h"

#include "../data/scene_defs.h"    /* SCENE_DEFS: [K, S*C] */
#include "../data/sub_filters.h"    /* SUB_FILTERS: [C, S, FILTER_LEN] */

/* ── 初始化 ── */
void scene_ctrl_init(scene_ctrl_t *sc, const gfanc_float_t *sub_filters)
{
    sc->centroids   = (const gfanc_float_t *)SCENE_DEFS;
    sc->sub_filters = sub_filters;
    sc->scene_id    = -1;
    sc->fade_cnt    = 0;

    /* 计算 stub RMS: 所有子滤波器等权求和 → RMS */
    int C = GFANC_C, S = GFANC_S, L = GFANC_FILTER_LEN;
    gfanc_float_t sum_sq = 0.0f;
    for (int l = 0; l < L; l++) {
        gfanc_float_t val = 0.0f;
        for (int c = 0; c < C; c++) {
            for (int s_ = 0; s_ < S; s_++) {
                val += sc->sub_filters[(c * S + s_) * L + l];
            }
        }
        sum_sq += val * val;
    }
    sc->stub_rms = sqrtf(sum_sq / (gfanc_float_t)L);
}

/* ── 推理: 1s 音频 → scene_id (argmax) + blend_weights ── */
int scene_ctrl_infer(scene_ctrl_t *sc, const gfanc_float_t *audio,
                     gfanc_float_t *blend_out)
{
    /* CNN 前向 */
    gfanc_float_t logits[GFANC_K];
    if (cnn_forward(audio, logits) != 0) return -1;

    /* argmax */
    int top1 = 0;
    gfanc_float_t max_val = logits[0];
    for (int k = 1; k < GFANC_K; k++) {
        if (logits[k] > max_val) {
            max_val = logits[k];
            top1 = k;
        }
    }
    sc->scene_id = top1;

    /* blend = centroid[top1] (hard selection) */
    int SC = GFANC_S * GFANC_C;
    for (int i = 0; i < SC; i++) {
        blend_out[i] = sc->centroids[top1 * SC + i];
    }
    return top1;
}

/* ── construct_wc: blend[s*C + c] × sub_filters[c,s,:] → Wc[s,:] ── */
void scene_ctrl_construct_wc(scene_ctrl_t *sc,
                              const gfanc_float_t *blend,
                              gfanc_float_t *wc_out)
{
    int C = GFANC_C, S = GFANC_S, L = GFANC_FILTER_LEN;
    int SC = S * C;

    /* 归一化: blend / max(blend) */
    gfanc_float_t b_max = blend[0];
    for (int i = 1; i < SC; i++) {
        if (blend[i] > b_max) b_max = blend[i];
    }
    gfanc_float_t inv_max = 1.0f / (b_max + 1e-10f);

    /* Wc[s, l] = Σ_c blend[s,c] * sub[c,s,l] (先 clip blend to [0,1]) */
    for (int s = 0; s < S; s++) {
        for (int l = 0; l < L; l++) {
            gfanc_float_t val = 0.0f;
            for (int c = 0; c < C; c++) {
                gfanc_float_t b = blend[s * C + c] * inv_max;
                if (b < 0.0f) b = 0.0f;
                if (b > 1.0f) b = 1.0f;
                val += b * sc->sub_filters[(c * S + s) * L + l];
            }
            wc_out[s * L + l] = val;
        }
    }

    /* RMS 对齐 stub */
    gfanc_float_t wc_rms_sq = 0.0f;
    for (int i = 0; i < S * L; i++) {
        wc_rms_sq += wc_out[i] * wc_out[i];
    }
    gfanc_float_t wc_rms = sqrtf(wc_rms_sq / (gfanc_float_t)(S * L));
    gfanc_float_t scale = (wc_rms > 1e-10f) ? sc->stub_rms / wc_rms : 1.0f;

    /* 取反: 物理 ANC 需要 Wc*Sec≈-Pri */
    for (int i = 0; i < S * L; i++) {
        wc_out[i] = -wc_out[i] * scale;
    }
}
