#ifndef CROSS_FADER_H
#define CROSS_FADER_H

#include "gfanc_types.h"

void crossfader_step(const gfanc_float_t *wc_old,
                      const gfanc_float_t *wc_new,
                      int fade_cnt, int fade_len,
                      gfanc_float_t *wc_out);

#endif
