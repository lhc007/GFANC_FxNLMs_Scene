/** SceneController — CNN 场景分类 + Blend + Wc 构造.
 *
 * 每秒调用一次, 输入 1 秒音频 (16000 样本), 输出控制滤波器 Wc.
 * 内部调用 cnn_m5_forward 做 CNN 推理.
 */
#ifndef SCENE_CONTROLLER_H
#define SCENE_CONTROLLER_H

#define SC_K  8
#define SC_S  2
#define SC_C  15

typedef struct {
    const float *centroids;    /* [K, S*C] */
    const float *sub_filters;  /* [C, S, L] */
    int    L;                  /* filter_len (1024) */
    float  stub_rms;
    int    cur_scene;
    float  prev_probs[SC_K];
} scene_ctrl_t;

void scene_ctrl_init(scene_ctrl_t *sc, const float *centroids,
                     const float *sub_filters, int filter_len);
int  scene_ctrl_process(scene_ctrl_t *sc, const float *audio_1s,
                        float *wc_out, float *probs_out);
void scene_ctrl_construct_wc(const scene_ctrl_t *sc, int scene_id, float *wc_out);

#endif
