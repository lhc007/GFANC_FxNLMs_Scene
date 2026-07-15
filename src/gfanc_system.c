/** GFANC System — 顶层集成.

使用流程:
    1. gfanc_init(&sys, step_size, reset_threshold)
    2. 逐秒: gfanc_process_second(&sys, ref_1s)  → CNN + Blend + Wc
    3. 逐样本: gfanc_fxnlms_step(&sys, Fx, Dis, anti) → 自适应+输出
    4. gfanc_free(&sys)
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfanc_system.h"
#include "fir_filter.h"
#include "fxnlms_mimo.h"
#include "scene_controller.h"
#include "cross_fader.h"

#include "../data/gfanc_config.h"
#include "../data/sec_path.h"
#include "../data/bandpass_fir.h"
#include "../data/sub_filters.h"

/* ── 初始化 ── */
int gfanc_init(gfanc_system_t *sys, float step_size, float reset_threshold)
{
    memset(sys, 0, sizeof(*sys));
    /* step_size 存在 fxnlms 子结构中 */
    sys->reset_threshold = reset_threshold;
    sys->use_bandpass = 1;

    /* 带通 FIR */
    fir_init(&sys->bp_fir, (const gfanc_float_t *)BANDPASS_FIR, GFANC_BP_LEN);

    /* Scene Controller */
    scene_ctrl_init(&sys->scene_ctrl, (const gfanc_float_t *)SUB_FILTERS);

    /* FxNLMS */
    fxnlms_init(&sys->fxnlms, step_size);

    /* 次级路径卷积 FIR (每个 E×S 通道) */
    int sec_len = GFANC_SEC_LEN;
    for (int e = 0; e < GFANC_E; e++) {
        for (int s = 0; s < GFANC_S; s++) {
            fir_init(&sys->sec_fir[e * GFANC_S + s],
                     (const gfanc_float_t *)SEC_PATH
                         + (e * GFANC_S + s) * sec_len,
                     sec_len);
        }
    }

    /* 参考信号缓冲 */
    sys->ref_signal = (gfanc_float_t *)calloc(GFANC_INPUT_LEN, sizeof(gfanc_float_t));

    return 0;
}

void gfanc_free(gfanc_system_t *sys)
{
    fir_free(&sys->bp_fir);
    for (int i = 0; i < GFANC_E * GFANC_S; i++) {
        fir_free(&sys->sec_fir[i]);
    }
    free(sys->ref_signal);
}

/* ── 每秒 CNN 场景识别 ── */
void gfanc_process_second(gfanc_system_t *sys, const gfanc_float_t *ref_1s)
{
    scene_ctrl_t *sc = &sys->scene_ctrl;

    /* 可选带通滤波 */
    const gfanc_float_t *input;
    gfanc_float_t *bp_buf = NULL;
    if (sys->use_bandpass) {
        bp_buf = (gfanc_float_t *)malloc(GFANC_INPUT_LEN * sizeof(gfanc_float_t));
        fir_process_block(&sys->bp_fir, ref_1s, bp_buf, GFANC_INPUT_LEN);
        input = bp_buf;
    } else {
        input = ref_1s;
    }

    /* CNN 前向 → Blend */
    gfanc_float_t new_blend[GFANC_SC_DIM];
    int new_scene = scene_ctrl_infer(sc, input, new_blend);

    /* 构建新 Wc */
    gfanc_float_t wc_new[GFANC_S * GFANC_FILTER_LEN];
    scene_ctrl_construct_wc(sc, new_blend, wc_new);

    /* 滞回检测: 场景切换启动 CrossFader */
    if (sc->scene_id != -1 && new_scene != sc->scene_id) {
        /* 场景变了: 保存当前Wc为old, 新Wc为target */
        memcpy(sc->wc_old, sc->wc, GFANC_S * GFANC_FILTER_LEN * sizeof(gfanc_float_t));
        memcpy(sc->wc, wc_new, GFANC_S * GFANC_FILTER_LEN * sizeof(gfanc_float_t));
        sc->fade_cnt = GFANC_FADE_LEN;
        sys->scene_switch_pending = 1;
    } else if (sc->scene_id == -1) {
        /* 首次: 直接设 Wc */
        memcpy(sc->wc, wc_new, GFANC_S * GFANC_FILTER_LEN * sizeof(gfanc_float_t));
        fxnlms_set_wc(&sys->fxnlms, sc->wc);
        sys->scene_switch_pending = 0;
    } else {
        /* 同一场景: 保持, FxNLMS 继续 */
        memcpy(sc->wc_old, sc->wc, GFANC_S * GFANC_FILTER_LEN * sizeof(gfanc_float_t));
        sys->scene_switch_pending = 0;
    }

    if (bp_buf) free(bp_buf);
}

/* ── 逐样本 FxNLMS ──
   调用方每样本提供:
     - Fx[e*S+s]: 当前时刻的滤波参考 (ref_bpf 通过 Sec 路径卷积的结果)
     - Dis[e]: 扰动 (噪声通过 Pri 路径)
     - anti_out[s]: 输出反噪声

   内部 CrossFader 处理场景切换平滑过渡 */
gfanc_float_t gfanc_fxnlms_step(gfanc_system_t *sys,
                                  const gfanc_float_t *Fx,
                                  const gfanc_float_t *Dis,
                                  gfanc_float_t *anti_out)
{
    fxnlms_mimo_t *fx = &sys->fxnlms;
    scene_ctrl_t  *sc = &sys->scene_ctrl;

    /* CrossFader: 如果在淡化期, 混合 wc_old → wc_new */
    if (sc->fade_cnt > 0) {
        gfanc_float_t wc_faded[GFANC_S * GFANC_FILTER_LEN];
        crossfader_step(sc->wc_old, sc->wc,
                         sc->fade_cnt, GFANC_FADE_LEN, wc_faded);
        fxnlms_set_wc(fx, wc_faded);
        sc->fade_cnt--;
    }

    /* FxNLMS 前向+更新 */
    fxnlms_tick(fx, Fx, Dis, anti_out);
    return 0.0f;
}
