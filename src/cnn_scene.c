/** m5_scene CNN C 实现 — 1D 残差卷积场景分类.

架构 (来自 gfanc/Network.py):
  conv_block:  Conv1d(1,64,80,s=4,p=38) → BN → ReLU → MaxPool1d(4,s=8)
  res_blocks[0]: 2× ResBlock(64,64,k=3,s=1,p=1) → MaxPool1d(4,s=4)
  res_blocks[1]: 2× ResBlock(64,64,k=3,s=1,p=1) → MaxPool1d(4,s=4)
  AdaptiveAvgPool1d(1) → Dropout(推理时跳过) → Linear(64,K)

所有权重由 export_model.py 生成, 通过 cnn_weights.h 引入.
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cnn_scene.h"
#include "gfanc_types.h"

/* ── 动态 include 权重 ── */
#include "../data/cnn_weights.h"

/* ── 内部常量 ── */
#define CONV0_IN       1
#define CONV0_OUT      64
#define CONV0_KERNEL   80
#define CONV0_STRIDE   4
#define CONV0_PAD      38
#define POOL0_KERNEL   4
#define POOL0_STRIDE   8

#define RES_KERNEL     3
#define RES_STRIDE     1
#define RES_PAD        1
#define RES_CH         64

#define POOL_KERNEL    4
#define POOL_STRIDE    4

#define BN_EPS         1e-5f

/* ── 缓冲区大小 ── */
#define CONV0_OUT_LEN  500     /* (16000+2*38-80)/4+1 = 4000; after MaxPool(4,s=8): (4000-4)/8+1 = 500 */
#define RES1_OUT_LEN   500     /* after ResBlock: 500; after MaxPool(4,s=4): (500-4)/4+1 = 125 */
#define RES1_POOL_LEN  125
#define RES2_OUT_LEN   125     /* after ResBlock: 125; after MaxPool(4,s=4): (125-4)/4+1 = 31 */
#define RES2_POOL_LEN  31

/* ── 辅助宏 ── */
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

/* ── 1D 卷积 (无bias, stride=1) ── */
static void conv1d_s1(const gfanc_float_t *in,  int in_len,   int in_ch,
                       const gfanc_float_t *w,   int out_ch,
                       int ksize, int pad,
                       gfanc_float_t *out, int *out_len)
{
    *out_len = in_len + 2 * pad - ksize + 1;
    for (int oc = 0; oc < out_ch; oc++) {
        for (int t = 0; t < *out_len; t++) {
            gfanc_float_t sum = 0.0f;
            for (int ic = 0; ic < in_ch; ic++) {
                for (int k = 0; k < ksize; k++) {
                    int in_pos = t + k - pad;
                    if (in_pos >= 0 && in_pos < in_len) {
                        sum += in[ic * in_len + in_pos]
                             * w[((oc * in_ch + ic) * ksize) + k];
                    }
                }
            }
            out[oc * (*out_len) + t] = sum;
        }
    }
}

/* ── 1D 卷积 with stride ── */
static void conv1d(const gfanc_float_t *in,  int in_len,   int in_ch,
                    const gfanc_float_t *w,   int out_ch,
                    int ksize, int stride, int pad,
                    gfanc_float_t *out, int *out_len)
{
    *out_len = (in_len + 2 * pad - ksize) / stride + 1;
    for (int oc = 0; oc < out_ch; oc++) {
        for (int t = 0; t < *out_len; t++) {
            gfanc_float_t sum = 0.0f;
            int in_start = t * stride - pad;
            for (int ic = 0; ic < in_ch; ic++) {
                for (int k = 0; k < ksize; k++) {
                    int in_pos = in_start + k;
                    if (in_pos >= 0 && in_pos < in_len) {
                        sum += in[ic * in_len + in_pos]
                             * w[((oc * in_ch + ic) * ksize) + k];
                    }
                }
            }
            out[oc * (*out_len) + t] = sum;
        }
    }
}

/* ── BatchNorm1d (推理模式: y = gamma*(x-mean)/sqrt(var+eps) + beta) ── */
static void batchnorm1d(gfanc_float_t *x, int ch, int len,
                         const gfanc_float_t *gamma, const gfanc_float_t *beta,
                         const gfanc_float_t *running_mean,
                         const gfanc_float_t *running_var)
{
    for (int c = 0; c < ch; c++) {
        gfanc_float_t inv_std = 1.0f / sqrtf(running_var[c] + BN_EPS);
        gfanc_float_t gm = gamma[c];
        gfanc_float_t bm = beta[c];
        gfanc_float_t rm = running_mean[c];
        for (int t = 0; t < len; t++) {
            int idx = c * len + t;
            x[idx] = gm * (x[idx] - rm) * inv_std + bm;
        }
    }
}

/* ── ReLU ── */
static void relu(gfanc_float_t *x, int n)
{
    for (int i = 0; i < n; i++) {
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
}

/* ── MaxPool1d ── */
static void maxpool1d(const gfanc_float_t *in, int ch, int in_len,
                       int ksize, int stride,
                       gfanc_float_t *out, int *out_len)
{
    *out_len = (in_len - ksize) / stride + 1;
    for (int c = 0; c < ch; c++) {
        for (int t = 0; t < *out_len; t++) {
            gfanc_float_t mx = -1e30f;
            int start = t * stride;
            for (int k = 0; k < ksize; k++) {
                gfanc_float_t v = in[c * in_len + start + k];
                if (v > mx) mx = v;
            }
            out[c * (*out_len) + t] = mx;
        }
    }
}

/* ── AdaptiveAvgPool1d(output_size=1) = 全局平均池化 ── */
static void adaptive_avg_pool1d_1(const gfanc_float_t *in, int ch, int in_len,
                                   gfanc_float_t *out)
{
    for (int c = 0; c < ch; c++) {
        gfanc_float_t sum = 0.0f;
        for (int t = 0; t < in_len; t++) {
            sum += in[c * in_len + t];
        }
        out[c] = sum / (gfanc_float_t)in_len;
    }
}

/* ── Linear (全连接) ── */
static void linear(const gfanc_float_t *in, int in_dim,
                    const gfanc_float_t *weight, const gfanc_float_t *bias,
                    int out_dim, gfanc_float_t *out)
{
    for (int o = 0; o < out_dim; o++) {
        gfanc_float_t sum = bias ? bias[o] : 0.0f;
        for (int i = 0; i < in_dim; i++) {
            sum += in[i] * weight[o * in_dim + i];
        }
        out[o] = sum;
    }
}

/* ── ResBlock (带残差连接 + 投影) ──
   每个 ResBlock = Conv1d→BN→ReLU→Conv1d→BN → +shortcut → ReLU
   prev_ch: 输入通道, channel: 输出通道
   权重顺序: res.0.weight → conv1, res.0.bias→conv1_bias,
              res.1.{weight,bias,running_mean,running_var} → BN1,
              res.3.{weight,bias} → conv2,
              res.4.{weight,bias,running_mean,running_var} → BN2
              proj.weight → 1×1 Conv (用于通道不匹配) */

static void resblock(const gfanc_float_t *in, int in_ch, int out_ch, int in_len,
                      const gfanc_float_t **weights,  /* 10个权重数组指针 */
                      gfanc_float_t *out, int *out_len,
                      gfanc_float_t *workspace1, gfanc_float_t *workspace2)
{
    int w_idx = 0;
    /* Conv1d #1: in_ch→out_ch, k=3, s=1, p=1 */
    int conv1_out_len;
    conv1d_s1(in, in_len, in_ch, weights[w_idx++], out_ch, RES_KERNEL, RES_PAD,
              workspace1, &conv1_out_len);
    /* BN #1 */
    batchnorm1d(workspace1, out_ch, conv1_out_len,
                weights[w_idx], weights[w_idx+1],
                weights[w_idx+2], weights[w_idx+3]);
    w_idx += 4;
    relu(workspace1, out_ch * conv1_out_len);

    /* Conv1d #2: out_ch→out_ch, k=3, s=1, p=1 */
    int conv2_out_len;
    conv1d_s1(workspace1, conv1_out_len, out_ch, weights[w_idx++], out_ch,
              RES_KERNEL, RES_PAD, workspace2, &conv2_out_len);
    /* BN #2 */
    batchnorm1d(workspace2, out_ch, conv2_out_len,
                weights[w_idx], weights[w_idx+1],
                weights[w_idx+2], weights[w_idx+3]);
    w_idx += 4;

    /* Shortcut */
    if (in_ch != out_ch) {
        /* 1×1 Conv 投影 */
        conv1d_s1(in, in_len, in_ch, weights[w_idx], out_ch, 1, 0,
                  out, out_len);
    } else {
        /* Identity */
        *out_len = in_len;
        memcpy(out, in, out_ch * in_len * sizeof(gfanc_float_t));
    }

    /* Add + ReLU */
    for (int i = 0; i < out_ch * (*out_len); i++) {
        out[i] += workspace2[i];
        if (out[i] < 0.0f) out[i] = 0.0f;
    }
}

/* ── m5_scene Forward ── */
int cnn_forward(const gfanc_float_t *audio, gfanc_float_t *logits_out)
{
    /* 工作缓冲区 (堆分配, 一次调用后释放) */
    gfanc_float_t *buf1, *buf2, *buf3;

    buf1 = (gfanc_float_t *)malloc(64 * CONV0_OUT_LEN * sizeof(gfanc_float_t));
    buf2 = (gfanc_float_t *)malloc(64 * CONV0_OUT_LEN * sizeof(gfanc_float_t));
    buf3 = (gfanc_float_t *)malloc(64 * RES1_POOL_LEN * sizeof(gfanc_float_t));
    if (!buf1 || !buf2 || !buf3) {
        free(buf1); free(buf2); free(buf3);
        return -1;
    }

    /* ── Layer 0: conv_block ── */
    /* Conv1d(1, 64, 80, s=4, p=38) + bias */
    int len0;
    conv1d(audio, GFANC_INPUT_LEN, CONV0_IN,
           cnn_weights[0], CONV0_OUT, CONV0_KERNEL, CONV0_STRIDE, CONV0_PAD,
           buf1, &len0);
    /* BN + ReLU */
    batchnorm1d(buf1, CONV0_OUT, len0,
                cnn_weights[2], cnn_weights[3],  /* gamma, beta */
                cnn_weights[4], cnn_weights[5]); /* mean, var */
    relu(buf1, CONV0_OUT * len0);
    /* MaxPool1d(4, stride=8) */
    int pool0_len;
    maxpool1d(buf1, CONV0_OUT, len0, POOL0_KERNEL, POOL0_STRIDE,
              buf2, &pool0_len);

    /* conv block 消耗了 6 个权重 (conv weight/bias, bn gamma/beta/mean/var) */
    int widx = 6;

    /* ── ResBlock Group 0 (2 ResBlocks, 64→64) ── */
    for (int b = 0; b < 2; b++) {
        const gfanc_float_t *rb_weights[9];
        for (int j = 0; j < 8; j++) rb_weights[j] = cnn_weights[widx++];
        /* proj only if channel mismatch (64→64: no proj needed) */
        rb_weights[8] = cnn_weights[widx++]; /* always consumes proj slot, unused */
        int rb_out_len;
        resblock(buf2, 64, 64, pool0_len, rb_weights,
                 buf1, &rb_out_len, buf3, buf1 + 64 * rb_out_len);
        /* swap buf1, buf2 */
        gfanc_float_t *tmp = buf1; buf1 = buf2; buf2 = tmp;
        pool0_len = rb_out_len;
    }

    /* MaxPool1d(4, stride=4) */
    int pool1_len;
    maxpool1d(buf2, 64, pool0_len, POOL_KERNEL, POOL_STRIDE,
              buf1, &pool1_len);

    /* ── ResBlock Group 1 (2 ResBlocks, 64→64) ── */
    for (int b = 0; b < 2; b++) {
        const gfanc_float_t *rb_weights[9];
        for (int j = 0; j < 8; j++) rb_weights[j] = cnn_weights[widx++];
        rb_weights[8] = cnn_weights[widx++];
        int rb_out_len;
        resblock(buf1, 64, 64, pool1_len, rb_weights,
                 buf2, &rb_out_len, buf3, buf2 + 64 * rb_out_len);
        gfanc_float_t *tmp = buf1; buf1 = buf2; buf2 = tmp;
        pool1_len = rb_out_len;
    }

    /* MaxPool1d(4, stride=4) */
    int pool2_len;
    maxpool1d(buf2, 64, pool1_len, POOL_KERNEL, POOL_STRIDE,
              buf1, &pool2_len);

    /* ── Global Average Pool → Linear ── */
    gfanc_float_t global_pool[64];
    adaptive_avg_pool1d_1(buf1, 64, pool2_len, global_pool);

    /* Linear(64, K) — final 2 weights */
    const gfanc_float_t *fc_weight = cnn_weights[widx++];
    const gfanc_float_t *fc_bias   = cnn_weights[widx++];
    linear(global_pool, 64, fc_weight, fc_bias, GFANC_K, logits_out);

    free(buf1); free(buf2); free(buf3);
    return 0;
}
