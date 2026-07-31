/** CNN M5 场景分类器 — 实例化接口 (ADVERSARIAL_REVIEW C2 修复).
 *
 *  消除全局静态变量 (g_cnn, g_K, g_cnn_buf), 改为显式实例管理.
 *  支持多实例 (未来多窗户场景), 支持 OTA 热切换 (free→init 循环).
 */
#ifndef CNN_M5_FORWARD_H
#define CNN_M5_FORWARD_H

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

/** CNN 推理实例 (所有权归调用方, 栈或堆分配均可).
 *
 *  用法:
 *    cnn_instance_t cnn;
 *    cnn_init(&cnn);                    // 从 data/ 下 .bin 文件加载权重
 *    cnn_forward(&cnn, audio, logits);  // 前向推理
 *    cnn_get_K(&cnn);                  // 查询场景数
 *    cnn_free(&cnn);                   // 释放权重 + 激活缓冲
 */
typedef struct {
    cnn_model_t model;
    int         K;          /* 场景数 (运行时从 linear_weight 文件大小推导) */
    float      *act_buf;    /* 推理激活缓冲 (lazy alloc, ~1MB) */
} cnn_instance_t;

/** 从 data/ 下 .bin 文件加载所有权重, 初始化 CNN 实例.
 *  @return 0=成功, -1=文件缺失/格式错误 */
int cnn_init(cnn_instance_t *cnn);

/** 前向推理: audio[16000] → logits[K].
 *  @return 0=成功, -1=推理失败 (OOM) */
int cnn_forward(cnn_instance_t *cnn, const float *audio, float *logits);

/** 查询场景数 K. */
static inline int cnn_get_K(const cnn_instance_t *cnn) { return cnn->K; }

/** 释放所有权重和激活缓冲. 支持 init→free→init 热循环 (OTA/热切换). */
void cnn_free(cnn_instance_t *cnn);

/* ── 向后兼容: 保留旧函数名用于减少改动量 ── */
#define cnn_m5_init()      cnn_init(&_gfanc_cnn_singleton)
#define cnn_m5_forward(a,l) cnn_forward(&_gfanc_cnn_singleton, a, l)
#define cnn_m5_get_K()     cnn_get_K(&_gfanc_cnn_singleton)
#define cnn_m5_free()      cnn_free(&_gfanc_cnn_singleton)

/** 全局单例 (向后兼容 — 仅用于单窗口场景; 多窗口场景自行分配 cnn_instance_t). */
extern cnn_instance_t _gfanc_cnn_singleton;

#ifdef __cplusplus
}
#endif

#endif /* CNN_M5_FORWARD_H */
