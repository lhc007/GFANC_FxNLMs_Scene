/** SceneController — CNN 直接权重 Wc 构造 (去场景层).
 *
 * 每秒调用一次, 输入 1 秒音频 (16000 样本), 输出控制滤波器 Wc.
 * 内部调用 cnn_m5_forward 做 CNN 推理.
 *
 * 直接权重模式: CNN 回归 30 维子带增益 (S×C, 2 扬声器 × 15 子带),
 *   gain[i] = tanh(logit[i]) → [-1,1] 带符号, Wc[s,l] = Σ_c gain[s,c]·sub[c,s,l]
 *   再 RMS 标定到 wc_rms_target + 取反 (C 端 FxNLMS 用 anti=+Wc⊗x, err=d+anti 约定).
 *
 * K (CNN 输出维) = cnn_m5_get_K() = S*C = 30, 从 cnn_linear_weight.bin 大小推导.
 */
#ifndef SCENE_CONTROLLER_H
#define SCENE_CONTROLLER_H

#define SC_S      2
#define SC_C      15
#define SC_DW_MAX 30   /* 直接权重输出维上限 (SC_S*SC_C) */

typedef struct {
    const float *sub_filters;   /* [C, S, L] 子滤波器基 (与标注/导出一致) */
    int    K;                   /* CNN 输出维 = S*C = 30 (运行时推导) */
    int    L;                   /* filter_len (1024) */
    float  stub_rms;
    float  wc_rms_target;       /* 自动标定: Wc 构造目标 RMS (基于 Ŝ 物理衰减) */
    float  prev_gains[SC_DW_MAX];  /* 上一秒增益 [S*C] (弱信号/CNN失败时保持) */
    int    prev_gains_valid;       /* 是否有可用历史增益 */
} scene_ctrl_t;

int  scene_ctrl_init(scene_ctrl_t *sc, const float *sub_filters, int filter_len);
void scene_ctrl_free(scene_ctrl_t *sc);
/** 直接权重 Wc 生产者: audio_1s → CNN → tanh 增益 → Wc[S*L].
 *  返回诊断用 argmax |gain| 带索引 (0..S*C-1, 弱信号/失败时沿用历史).
 *  输出 wc_out[S*L] (已 RMS 标定 + 取反), gains_out[S*C] (tanh 增益). */
int  scene_ctrl_process(scene_ctrl_t *sc, const float *audio_1s,
                        float *wc_out, float *gains_out);
/** 用给定 30 维增益构造 Wc: wc[s*L+l] = Σ_c gains[s*C+c]·sub[(c*S+s)*L+l], 然后 RMS 标定 + 取反. */
void scene_ctrl_construct_wc(const scene_ctrl_t *sc, const float *gains, float *wc_out);

#endif
