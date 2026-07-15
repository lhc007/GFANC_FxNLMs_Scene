/** CrossFader C 实现 — Wc 系数级平滑过渡.

对应 Python: gfanc/cross_fader.py
    在 fade_len 个采样周期内, 从 wc_old 线性过渡到 wc_new.
    fade_cnt 从 fade_len 倒数到 0.
*/
#include "gfanc_types.h"

/* ── 每帧步进, 返回当前混合 Wc ── */
void crossfader_step(const gfanc_float_t *wc_old,
                      const gfanc_float_t *wc_new,
                      int fade_cnt, int fade_len,
                      gfanc_float_t *wc_out)
{
    int total = GFANC_S * GFANC_FILTER_LEN;
    gfanc_float_t alpha = (fade_len > 0)
                         ? (gfanc_float_t)fade_cnt / (gfanc_float_t)fade_len
                         : 1.0f;

    for (int i = 0; i < total; i++) {
        wc_out[i] = alpha * wc_old[i] + (1.0f - alpha) * wc_new[i];
    }
}
