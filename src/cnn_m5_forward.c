/** m5_scene CNN 前向 — 从 .bin 文件加载权重, 纯 C float 实现.

架构: stem(Conv+BN+ReLU+MaxPool) → 2×2 ResBlock(64,64) → Pool → Linear(64,K)
用法:
    cnn_m5_init() — 从 data/*.bin 加载所有权重 (一次)
    cnn_m5_forward(audio_16000, logits_out) — 前向推理
    cnn_m5_free() — 释放
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "binary_loader.h"

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

/* ── 权重结构 ── */
typedef struct {
    float *weight, *bias;
    int out_ch, in_ch, ksize, stride, pad;
} conv_layer_t;

typedef struct {
    float *gamma, *beta, *mean, *var;
    int ch;
} bn_layer_t;

typedef struct {
    conv_layer_t conv1, conv2;
    bn_layer_t    bn1, bn2;
    float        *proj_weight;
    int           in_ch;
} resblock_t;

typedef struct {
    conv_layer_t stem_conv;
    bn_layer_t    stem_bn;
    resblock_t    res[4];
    float        *fc_weight, *fc_bias;
} cnn_m5_t;

static cnn_m5_t g_cnn;
static int g_K = 0;  /* 运行时从 linear_weight 文件大小推导 */

int cnn_m5_get_K(void) { return g_K; }

/* ── 加载 ── */
static int load_conv(const char *tag, conv_layer_t *c, int oc, int ic, int k, int s, int p) {
    c->out_ch = oc; c->in_ch = ic; c->ksize = k; c->stride = s; c->pad = p;
    char path[256];
    snprintf(path, sizeof(path), "data/cnn_%s_weight.bin", tag);
    int n = bin_load_float(path, &c->weight);
    if (n < 0) { fprintf(stderr, "  FAIL load %s\n", path); return -1; }
    snprintf(path, sizeof(path), "data/cnn_%s_bias.bin", tag);
    n = bin_load_float(path, &c->bias);
    if (n < 0) { fprintf(stderr, "  FAIL load %s\n", path); return -1; }
    return 0;
}

static int load_bn(const char *tag, bn_layer_t *b, int ch) {
    b->ch = ch;
    char path[256];
    snprintf(path, sizeof(path), "data/cnn_%s_gamma.bin", tag);
    if (bin_load_float(path, &b->gamma) < 0) return -1;
    snprintf(path, sizeof(path), "data/cnn_%s_beta.bin", tag);
    if (bin_load_float(path, &b->beta) < 0) return -1;
    snprintf(path, sizeof(path), "data/cnn_%s_mean.bin", tag);
    if (bin_load_float(path, &b->mean) < 0) return -1;
    snprintf(path, sizeof(path), "data/cnn_%s_var.bin", tag);
    if (bin_load_float(path, &b->var) < 0) return -1;
    return 0;
}

int cnn_m5_init(void)
{
    memset(&g_cnn, 0, sizeof(g_cnn));

    /* Stem */
    if (load_conv("stem_conv", &g_cnn.stem_conv, CH, 1, STEM_K, STEM_S, STEM_P)) return -1;
    if (load_bn("stem_bn", &g_cnn.stem_bn, CH)) return -1;

    /* 4 ResBlocks: res0, res1, res2, res3 */
    for (int i = 0; i < 4; i++) {
        char tag[32];
        resblock_t *r = &g_cnn.res[i];
        r->in_ch = CH;
        r->proj_weight = NULL;  /* 64→64, no projection needed */

        snprintf(tag, sizeof(tag), "res%d_conv1", i);
        if (load_conv(tag, &r->conv1, CH, CH, RES_K, RES_S, RES_P)) return -1;
        snprintf(tag, sizeof(tag), "res%d_bn1", i);
        if (load_bn(tag, &r->bn1, CH)) return -1;
        snprintf(tag, sizeof(tag), "res%d_conv2", i);
        if (load_conv(tag, &r->conv2, CH, CH, RES_K, RES_S, RES_P)) return -1;
        snprintf(tag, sizeof(tag), "res%d_bn2", i);
        if (load_bn(tag, &r->bn2, CH)) return -1;
    }

    /* FC — K 从 linear_weight 文件大小推导: n = K*CH → K = n/CH */
    int n_w = bin_load_float("data/cnn_linear_weight.bin", &g_cnn.fc_weight);
    int n_b = bin_load_float("data/cnn_linear_bias.bin", &g_cnn.fc_bias);
    if (n_w < 0 || n_b < 0) return -1;
    g_K = n_w / CH;
    if (g_K < 1 || g_K > 16) {
        fprintf(stderr, "  Invalid K=%d from linear_weight (%d floats)\n", g_K, n_w);
        return -1;
    }
    (void)n_b;

    return 0;
}

void cnn_m5_free(void)
{
    /* Just free the globals (simplified — in production, track all allocations) */
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

/* ── 主前向 ── */
int cnn_m5_forward(const float *audio, float *logits)
{
    /* 静态缓冲: 4块×1MB, 一次性分配避免每1Hz calloc/free碎片化 */
    static float *b_buf = NULL;
    static int   b_len = 0;
    int max_buf = CH * STEM_OUT_LEN; /* 64*4000 = 256000 */
    if (!b_buf || b_len < max_buf * 4) {
        free(b_buf);
        b_buf = (float *)calloc(max_buf * 4, sizeof(float));
        if (!b_buf) return -1;
        b_len = max_buf * 4;
    }
    float *b[4];
    for (int i = 0; i < 4; i++) b[i] = b_buf + i * max_buf;

    /* Stem: Conv → BN → ReLU → MaxPool (in:b[0] tmp, out:b[1]) */
    int slen, plen;
    conv1d(audio, INPUT_LEN, 1, g_cnn.stem_conv.weight, g_cnn.stem_conv.bias,
           CH, STEM_K, STEM_S, STEM_P, b[0], &slen);
    batchnorm(b[0], CH, slen, g_cnn.stem_bn.gamma, g_cnn.stem_bn.beta,
              g_cnn.stem_bn.mean, g_cnn.stem_bn.var);
    relu(b[0], CH * slen);
    maxpool1d(b[0], CH, slen, POOL0_K, POOL0_S, b[1], &plen);
    /* b[1]=current feature map, b[0]=free, b[2]=scratch1, b[3]=scratch2 */

    /* ResBlock groups */
    for (int g = 0; g < 2; g++) {
        for (int rb = g*2; rb < g*2+2; rb++) {
            /* in=b[1], scratch1=b[2], scratch2=b[3], out=b[0] */
            int rlen;
            resblock_forward(b[1], CH, CH, plen, &g_cnn.res[rb],
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
    for (int o = 0; o < g_K; o++) {
        float sum = g_cnn.fc_bias ? g_cnn.fc_bias[o] : 0.0f;
        for (int i = 0; i < CH; i++)
            sum += gap[i] * g_cnn.fc_weight[o * CH + i];
        logits[o] = sum;
    }

    return 0;
}
