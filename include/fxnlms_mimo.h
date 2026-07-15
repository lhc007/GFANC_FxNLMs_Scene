#ifndef FXNLMS_MIMO_H
#define FXNLMS_MIMO_H

#include "gfanc_types.h"

void fxnlms_init(fxnlms_mimo_t *fx, float step_size);
void fxnlms_set_wc(fxnlms_mimo_t *fx, const gfanc_float_t *wc);
void fxnlms_tick(fxnlms_mimo_t *fx,
                  const gfanc_float_t *Fx, const gfanc_float_t *Dis,
                  gfanc_float_t *anti_out);

#endif
