/** CNN M5 分类器 (SFANC 硬选库决策层) — 实例化接口 (ADVERSARIAL_REVIEW C2 修复).
 *
 *  消除全局静态变量 (g_cnn, g_K, g_cnn_buf), 改为显式实例管理.
 *  支持多实例 (未来多窗户场景), 支持 OTA 热切换 (free→init 循环).
 *  base 统一为 "cnn_bank" (分类 CNN, K=N 类, argmax 选库槽).
 */
#ifndef CNN_M5_FORWARD_H
#define CNN_M5_FORWARD_H

/* 线性层输出维上限 — 分类 CNN K 类上限. */
#define CNN_M5_OUT_MAX 30

#ifdef __cplusplus
extern "C" {
#endif

/* ── 权重结构 (从 .c 移出供外部类型声明) ── */
typedef struct {
    float *weight, *bias;
    int out_ch, in_ch, ksize, stride, pad;
} cnn_conv_layer_t;

typedef struct {
    float *gamma, *beta, *mean, *var;
    int ch;
} cnn_bn_layer_t;

typedef struct {
    cnn_conv_layer_t conv1, conv2;
    cnn_bn_layer_t    bn1, bn2;
    float            *proj_weight;
    int               in_ch;
} cnn_resblock_t;

typedef struct {
    cnn_conv_layer_t stem_conv;
    cnn_bn_layer_t    stem_bn;
    cnn_resblock_t    res[4];
    float            *fc_weight, *fc_bias;
} cnn_model_t;

/** CNN 推理实例 (所有权归调用方, 栈或堆分配均可). */
typedef struct {
    cnn_model_t model;
    int         K;          /* 场景数 (运行时从 linear_weight 文件大小推导) */
    float      *act_buf;    /* 推理激活缓冲 (lazy alloc, ~1MB) */
} cnn_instance_t;

/** 带权重集前缀加载: base="cnn_bank" → data/cnn_bank_*.bin (分类 K=N, SFANC 硬选库).
 *  K 由 linear_weight 大小推导. @return 0=成功, -1=失败 */
int cnn_init_base(cnn_instance_t *cnn, const char *base);

/** 前向推理: audio[16000] → logits[K].
 *  @return 0=成功, -1=推理失败 (OOM) */
int cnn_forward(cnn_instance_t *cnn, const float *audio, float *logits);

/** 查询场景数 K. */
static inline int cnn_get_K(const cnn_instance_t *cnn) { return cnn->K; }

/** SFANC 硬选: 返回 logits[K] 最大值索引 (分类决策, argmax 类标签 → 库槽索引).
 *  K<=0 返回 0; logits 含 NaN 时该槽不会成为 argmax (NaN 比较恒 false). */
static inline int cnn_m5_argmax(const float *logits, int K)
{
    int best = 0;
    if (K > 0) {
        float bv = logits[0];
        for (int i = 1; i < K; i++)
            if (logits[i] > bv) { bv = logits[i]; best = i; }
    }
    return best;
}

/** 释放所有权重和激活缓冲. 支持 init→free→init 热循环 (OTA/热切换). */
void cnn_free(cnn_instance_t *cnn);

/* ── 全局单例便捷宏 (单窗口场景; 多窗口场景自行分配 cnn_instance_t) ── */
#define cnn_m5_init_base(b) cnn_init_base(&_gfanc_cnn_singleton, b)
#define cnn_m5_forward(a,l) cnn_forward(&_gfanc_cnn_singleton, a, l)
#define cnn_m5_get_K()     cnn_get_K(&_gfanc_cnn_singleton)
#define cnn_m5_free()      cnn_free(&_gfanc_cnn_singleton)

/** 全局单例 (向后兼容 — 仅用于单窗口场景; 多窗口场景自行分配 cnn_instance_t). */
extern cnn_instance_t _gfanc_cnn_singleton;

#ifdef __cplusplus
}
#endif

#endif /* CNN_M5_FORWARD_H */
