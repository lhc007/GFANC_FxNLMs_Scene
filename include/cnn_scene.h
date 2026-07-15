#ifndef CNN_SCENE_H
#define CNN_SCENE_H

#include "gfanc_types.h"

/* ── m5_scene CNN 前向推理 ──
   输入: audio [16000] 单通道 16kHz, 已归一化到 [-1,1]
   输出: logits [K=8], 不包含 softmax (调用方自行处理)
   data_dir: 权重文件所在目录 (包含 cnn_00.h ~ cnn_NN.h)
   返回: 0 成功, -1 失败
*/
int cnn_forward(const gfanc_float_t *audio, gfanc_float_t *logits_out);

#endif /* CNN_SCENE_H */
