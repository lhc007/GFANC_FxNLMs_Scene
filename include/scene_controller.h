#ifndef SCENE_CONTROLLER_H
#define SCENE_CONTROLLER_H

#include "gfanc_types.h"

void scene_ctrl_init(scene_ctrl_t *sc, const gfanc_float_t *sub_filters);
int  scene_ctrl_infer(scene_ctrl_t *sc, const gfanc_float_t *audio,
                       gfanc_float_t *blend_out);
void scene_ctrl_construct_wc(scene_ctrl_t *sc,
                              const gfanc_float_t *blend,
                              gfanc_float_t *wc_out);

#endif
