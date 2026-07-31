/** SceneController — CNN 场景分类 + Blend + Wc 构造.
 *
 * 每秒调用一次, 输入 1 秒音频 (16000 样本), 输出控制滤波器 Wc.
 * 内部调用 cnn_m5_forward 做 CNN 推理.
 *
 * K (场景数) 运行时从 scene_defs.bin 自动推导: K = n_centroids / (S*C)
 */
#ifndef SCENE_CONTROLLER_H
#define SCENE_CONTROLLER_H

#define SC_S      2
#define SC_C      15
#define SC_K_MAX 16   /* 栈数组上限, 实际 K 不超过此值 */

typedef struct {
    const float *centroids;    /* [K, S*C] */
    const float *sub_filters;  /* [C, S, L] */
    int    K;                  /* 场景数 (运行时推导) */
    int    L;                  /* filter_len (1024) */
    float  stub_rms;
    float  wc_rms_target;        /* 自动标定: Wc 构造目标 RMS (基于 Ŝ 物理衰减) */
    int    cur_scene;
    float  prev_probs[SC_K_MAX];  /* [K] R-24: 定长数组替代动态分配 (最多64B) */
} scene_ctrl_t;

/** @param n_centroids  scene_defs.bin 总 float 数 (K * S * C) */
int  scene_ctrl_init(scene_ctrl_t *sc, const float *centroids,
                     const float *sub_filters, int filter_len,
                     int n_centroids);
void scene_ctrl_free(scene_ctrl_t *sc);
int  scene_ctrl_process(scene_ctrl_t *sc, const float *audio_1s,
                        float *wc_out, float *probs_out);
void scene_ctrl_construct_wc(const scene_ctrl_t *sc, int scene_id, float *wc_out);

#endif
