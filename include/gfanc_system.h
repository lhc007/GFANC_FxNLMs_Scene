#ifndef GFANC_SYSTEM_H
#define GFANC_SYSTEM_H

#include "gfanc_types.h"

/* ── 系统初始化/销毁 ── */
int  gfanc_init(gfanc_system_t *sys, float step_size, float reset_threshold);
void gfanc_free(gfanc_system_t *sys);

/* ── 逐秒处理 ──
   ref_1s: 1秒参考信号 [16000] 16kHz float, 值域 [-1,1]
   error_mic_buffer: 误差麦实时缓冲 (由 ADC 每样本写入)
   anti_out: 输出反噪声 [S], 应送 DAC

   每秒调用一次 cnn_process_second 做场景识别+Wc构造
   逐样本调用 fxnlms_step 做自适应更新
*/
void gfanc_process_second(gfanc_system_t *sys,
                           const gfanc_float_t *ref_1s);

gfanc_float_t gfanc_fxnlms_step(gfanc_system_t *sys,
                                  const gfanc_float_t *Fx,   /* [E*S] */
                                  const gfanc_float_t *Dis,  /* [E] */
                                  gfanc_float_t *anti_out); /* [S] */

#endif /* GFANC_SYSTEM_H */
