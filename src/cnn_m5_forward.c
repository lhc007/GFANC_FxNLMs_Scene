/** m5_scene CNN 前向 — 从 .bin 文件加载权重, 纯 C float 实现.

架构: stem(Conv+BN+ReLU+MaxPool) → 2×2 ResBlock(64,64) → Pool → Linear(64,K)

C2 修复 (ADVERSARIAL_REVIEW): 全局静态变量 → 实例化接口.
用法:
    cnn_instance_t cnn;
    cnn_init(&cnn);                    // 从 data/ 下 .bin 文件加载所有权重
    cnn_forward(&cnn, audio, logits);  // 前向推理
    cnn_free(&cnn);                   // 释放
向后兼容: cnn_m5_init/forward/get_K/free 宏 → 全局单例.
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "binary_loader.h"
#include "cnn_m5_forward.h"

/* ── 内部类型别名 (与头文件类型对应) ── */
typedef cnn_conv_layer_t conv_layer_t;
typedef cnn_bn_layer_t    bn_layer_t;
typedef cnn_resblock_t    resblock_t;
typedef cnn_model_t       model_t;

#define CH       64
#define INPUT_LEN 16000
#define STEM_K   80
#define STEM_S   4
#define STEM_P   38
#define POOL0_K  4
#define POOL0_S  8
#define RES_K    3
#define RES_S    1
#define RES_P    1
#define POOL_K   4
#define POOL_S   4

/* 缓冲区大小 */
#define STEM_OUT_LEN  ((INPUT_LEN + 2*STEM_P - STEM_K)/STEM_S + 1)  /* 4000 */
#define STEM_POOL_LEN ((STEM_OUT_LEN - POOL0_K)/POOL0_S + 1)        /* 500 */
#define RES1_OUT_LEN  STEM_POOL_LEN                                 /* 500 */
#define RES1_POOL_LEN ((RES1_OUT_LEN - POOL_K)/POOL_S + 1)          /* 125 */
#define RES2_OUT_LEN  RES1_POOL_LEN                                 /* 125 */
#define RES2_POOL_LEN ((RES2_OUT_LEN - POOL_K)/POOL_S + 1)          /* 31 */

/* C2: 全局单例 — 向后兼容宏 (cnn_m5_*) 使用的实例 */
cnn_instance_t _gfanc_cnn_singleton;

/* ── 加载 (base = 权重集前缀: "cnn"=回归(K=30, calibrate), "cnn_bank"=分类(K=N, deploy)) ── */
static int load_conv(const char *base, const char *tag, conv_layer_t *c, int oc, int ic, int k, int s, int p) {
    c->out_ch = oc; c->in_ch = ic; c->ksize = k; c->stride = s; c->pad = p;
    char path[256];
    snprintf(path, sizeof(path), "data/%s_%s_weight.bin", base, tag);
    int n = bin_load_float(path, &c->weight);
    if (n < 0) { fprintf(stderr, "  FAIL load %s\n", path); return -1; }
    snprintf(path, sizeof(path), "data/%s_%s_bias.bin", base, tag);
    n = bin_load_float(path, &c->bias);
    if (n < 0) { fprintf(stderr, "  FAIL load %s\n", path); return -1; }
    return 0;
}

static int load_bn(const char *base, const char *tag, bn_layer_t *b, int ch) {
    b->ch = ch;
    char path[256];
    snprintf(path, sizeof(path), "data/%s_%s_gamma.bin", base, tag);
    if (bin_load_float(path, &b->gamma) < 0) return -1;
    snprintf(path, sizeof(path), "data/%s_%s_beta.bin", base, tag);
    if (bin_load_float(path, &b->beta) < 0) return -1;
    snprintf(path, sizeof(path), "data/%s_%s_mean.bin", base, tag);
    if (bin_load_float(path, &b->mean) < 0) return -1;
    snprintf(path, sizeof(path), "data/%s_%s_var.bin", base, tag);
    if (bin_load_float(path, &b->var) < 0) return -1;
    return 0;
}

int cnn_init_base(cnn_instance_t *cnn, const char *base)
{
    memset(cnn, 0, sizeof(*cnn));
    model_t *m = &cnn->model;

    /* Stem */
    if (load_conv(base, "stem_conv", &m->stem_conv, CH, 1, STEM_K, STEM_S, STEM_P)) return -1;
    if (load_bn(base, "stem_bn", &m->stem_bn, CH)) return -1;

    /* 4 ResBlocks: res0, res1, res2, res3 */
    for (int i = 0; i < 4; i++) {
        char tag[32];
        resblock_t *r = &m->res[i];
        r->in_ch = CH;
        /* R-35: 尝试加载 projection 权重 (in_ch≠out_ch 时需要, 当前64→64无投影) */
        snprintf(tag, sizeof(tag), "data/%s_res%d_proj_weight.bin", base, i);
        r->proj_weight = NULL;
        bin_load_float(tag, &r->proj_weight);  /* 文件不存在返回-1 → NULL, 正确降级 */

        snprintf(tag, sizeof(tag), "res%d_conv1", i);
        if (load_conv(base, tag, &r->conv1, CH, CH, RES_K, RES_S, RES_P)) return -1;
        snprintf(tag, sizeof(tag), "res%d_bn1", i);
        if (load_bn(base, tag, &r->bn1, CH)) return -1;
        snprintf(tag, sizeof(tag), "res%d_conv2", i);
        if (load_conv(base, tag, &r->conv2, CH, CH, RES_K, RES_S, RES_P)) return -1;
        snprintf(tag, sizeof(tag), "res%d_bn2", i);
        if (load_bn(base, tag, &r->bn2, CH)) return -1;
    }

    /* FC — K 从 linear_weight 文件大小推导: n = K*CH → K = n/CH */
    char path[256];
    snprintf(path, sizeof(path), "data/%s_linear_weight.bin", base);
    int n_w = bin_load_float(path, &m->fc_weight);
    snprintf(path, sizeof(path), "data/%s_linear_bias.bin", base);
    int n_b = bin_load_float(path, &m->fc_bias);
    if (n_w < 0 || n_b < 0) return -1;
    cnn->K = n_w / CH;
    if (cnn->K < 1 || cnn->K > CNN_M5_OUT_MAX) {
        fprintf(stderr, "  Invalid K=%d from %s_linear_weight (%d floats) — expected 1..%d\n",
                cnn->K, base, n_w, CNN_M5_OUT_MAX);
        return -1;
    }
    (void)n_b;

    return 0;
}

int cnn_init(cnn_instance_t *cnn)
{
    return cnn_init_base(cnn, "cnn");
}

static void free_conv(conv_layer_t *c)
{
    free(c->weight); c->weight = NULL;
    free(c->bias);   c->bias   = NULL;
}

static void free_bn(bn_layer_t *b)
{
    free(b->gamma); b->gamma = NULL;
    free(b->beta);  b->beta  = NULL;
    free(b->mean);  b->mean  = NULL;
    free(b->var);   b->var   = NULL;
}

static void free_resblock(resblock_t *r)
{
    free_conv(&r->conv1);
    free_conv(&r->conv2);
    free_bn(&r->bn1);
    free_bn(&r->bn2);
    free(r->proj_weight); r->proj_weight = NULL;
}

void cnn_free(cnn_instance_t *cnn)
{
    model_t *m = &cnn->model;

    /* Stem */
    free_conv(&m->stem_conv);
    free_bn(&m->stem_bn);

    /* 4 ResBlocks */
    for (int i = 0; i < 4; i++)
        free_resblock(&m->res[i]);

    /* FC */
    free(m->fc_weight); m->fc_weight = NULL;
    free(m->fc_bias);   m->fc_bias   = NULL;

    cnn->K = 0;

    /* R-33: 释放推理激活缓冲, 下次 forward 自动重新分配 */
    free(cnn->act_buf);
    cnn->act_buf = NULL;
}

/* ── 层实现 ── */

static void conv1d(const float *in, int in_len, int in_ch,
                    const float *w, const float *bias,
                    int out_ch, int k, int s, int p,
                    float *out, int *out_len)
{
    *out_len = (in_len + 2*p - k) / s + 1;
    for (int oc = 0; oc < out_ch; oc++) {
        for (int t = 0; t < *out_len; t++) {
            float sum = bias ? bias[oc] : 0.0f;
            int start = t * s - p;
            for (int ic = 0; ic < in_ch; ic++) {
                for (int ki = 0; ki < k; ki++) {
                    int pos = start + ki;
                    if (pos >= 0 && pos < in_len)
                        sum += in[ic * in_len + pos]
                             * w[((oc * in_ch + ic) * k) + ki];
                }
            }
            out[oc * (*out_len) + t] = sum;
        }
    }
}

static void batchnorm(float *x, int ch, int len,
                       const float *gamma, const float *beta,
                       const float *mean, const float *var)
{
    for (int c = 0; c < ch; c++) {
        float istd = 1.0f / sqrtf(var[c] + 1e-5f);
        float gm = gamma ? gamma[c] : 1.0f;
        float bm = beta  ? beta[c]  : 0.0f;
        float rm = mean  ? mean[c]  : 0.0f;
        for (int t = 0; t < len; t++) {
            int idx = c * len + t;
            x[idx] = gm * (x[idx] - rm) * istd + bm;
        }
    }
}

static void relu(float *x, int n) {
    for (int i = 0; i < n; i++) if (x[i] < 0.0f) x[i] = 0.0f;
}

static void maxpool1d(const float *in, int ch, int in_len,
                       int k, int s, float *out, int *out_len)
{
    *out_len = (in_len - k) / s + 1;
    for (int c = 0; c < ch; c++) {
        for (int t = 0; t < *out_len; t++) {
            float mx = -1e30f;
            for (int ki = 0; ki < k; ki++)
                if (in[c * in_len + t * s + ki] > mx)
                    mx = in[c * in_len + t * s + ki];
            out[c * (*out_len) + t] = mx;
        }
    }
}

static void resblock_forward(const float *in, int in_ch, int out_ch, int in_len,
                              const resblock_t *rb,
                              float *out, int *out_len,
                              float *tmp1, float *tmp2)
{
    int len1;
    conv1d(in, in_len, in_ch, rb->conv1.weight, rb->conv1.bias,
           out_ch, RES_K, RES_S, RES_P, tmp1, &len1);
    batchnorm(tmp1, out_ch, len1, rb->bn1.gamma, rb->bn1.beta,
              rb->bn1.mean, rb->bn1.var);
    relu(tmp1, out_ch * len1);

    int len2;
    conv1d(tmp1, len1, out_ch, rb->conv2.weight, rb->conv2.bias,
           out_ch, RES_K, RES_S, RES_P, tmp2, &len2);
    batchnorm(tmp2, out_ch, len2, rb->bn2.gamma, rb->bn2.beta,
              rb->bn2.mean, rb->bn2.var);

    /* Shortcut */
    if (in_ch != out_ch && rb->proj_weight) {
        conv1d(in, in_len, in_ch, rb->proj_weight, NULL,
               out_ch, 1, 1, 0, out, out_len);
    } else {
        *out_len = in_len;
        memcpy(out, in, out_ch * in_len * sizeof(float));
    }

    /* Add + ReLU */
    for (int i = 0; i < out_ch * (*out_len); i++) {
        out[i] += tmp2[i];
        if (out[i] < 0.0f) out[i] = 0.0f;
    }
}

/* ── 主前向 (实例化) ── */
int cnn_forward(cnn_instance_t *cnn, const float *audio, float *logits)
{
    model_t *m = &cnn->model;
    int K = cnn->K;

    /* R-25: 缓冲区分级分配.
       只有 stem conv 输出需要 256K (CH*STEM_OUT_LEN=64*4000).
       Stem pool 之后所有特征图 ≤ CH*STEM_POOL_LEN=64*500=32K.
       旧方案: 4×256K=4MB → 新方案: 1×256K+3×32K≈1.34MB (−66%).
       缓冲迁移: stem_buf 在 b[0]↔b[1] 交换中迁移, 容量始终 ≥ 当前需要. */
    const int stem_sz = CH * STEM_OUT_LEN;   /* 256,000 floats = 1MB */
    const int feat_sz = CH * STEM_POOL_LEN;  /*  32,000 floats = 128KB */
    const int total_sz = stem_sz + 3 * feat_sz;  /* ~1.34MB */

    /* C2: 激活缓冲移到实例内, 支持多实例 */
    static int b_len = 0;
    if (!cnn->act_buf || b_len < total_sz) {
        free(cnn->act_buf);
        cnn->act_buf = (float *)calloc(total_sz, sizeof(float));
        if (!cnn->act_buf) return -1;
        b_len = total_sz;
    }
    float *b[4];
    b[0] = cnn->act_buf;                       /* stem 区: 256K */
    b[1] = cnn->act_buf + stem_sz;             /* feat 区 0: 32K */
    b[2] = cnn->act_buf + stem_sz + feat_sz;   /* feat 区 1: 32K */
    b[3] = cnn->act_buf + stem_sz + 2*feat_sz; /* feat 区 2: 32K */

    /* Stem: Conv → BN → ReLU → MaxPool (in:b[0] tmp, out:b[1]) */
    int slen, plen;
    conv1d(audio, INPUT_LEN, 1, m->stem_conv.weight, m->stem_conv.bias,
           CH, STEM_K, STEM_S, STEM_P, b[0], &slen);
    batchnorm(b[0], CH, slen, m->stem_bn.gamma, m->stem_bn.beta,
              m->stem_bn.mean, m->stem_bn.var);
    relu(b[0], CH * slen);
    maxpool1d(b[0], CH, slen, POOL0_K, POOL0_S, b[1], &plen);
    /* b[1]=current feature map, b[0]=free, b[2]=scratch1, b[3]=scratch2 */

    /* ResBlock groups */
    for (int g = 0; g < 2; g++) {
        for (int rb = g*2; rb < g*2+2; rb++) {
            /* in=b[1], scratch1=b[2], scratch2=b[3], out=b[0] */
            int rlen;
            resblock_forward(b[1], CH, CH, plen, &m->res[rb],
                             b[0], &rlen, b[2], b[3]);
            /* swap b[0]<->b[1], new feature map in b[1] */
            { float *t = b[0]; b[0] = b[1]; b[1] = t; plen = rlen; }
        }
        /* Pool: in=b[1], out=b[0] */
        maxpool1d(b[1], CH, plen, POOL_K, POOL_S, b[0], &plen);
        { float *t = b[0]; b[0] = b[1]; b[1] = t; } /* result in b[1] */
    }

    /* Global Average Pool on b[1] */
    float gap[CH];
    for (int c = 0; c < CH; c++) {
        float sum = 0.0f;
        for (int t = 0; t < plen; t++) sum += b[1][c * plen + t];
        gap[c] = sum / (float)plen;
    }

    /* Linear */
    for (int o = 0; o < K; o++) {
        float sum = m->fc_bias ? m->fc_bias[o] : 0.0f;
        for (int i = 0; i < CH; i++)
            sum += gap[i] * m->fc_weight[o * CH + i];
        logits[o] = sum;
    }

    return 0;
}
