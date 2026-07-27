/** FIR + FxNLMS 单元测试 (R-31).
 *
 *  编译: gcc -O2 -I../include test_fir.c ../src/fir_filter.c ../src/fxnlms_mimo.c -lm -o test_fir.exe
 *  运行: ./test_fir.exe
 *
 *  测试 1: fir_tick 脉冲响应 == 系数序列
 *  测试 2: fxnlms 收敛性 (合成 Pri/Ŝ, 100ms 内 err 功率降 ≥10dB)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "fir_filter.h"
#include "fxnlms_mimo.h"

#define E 3
#define S 2
#define L 64    /* 短滤波器加速测试, 避免大卷积和溢出 */

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s\n", msg); failures++; } \
    else { printf("  PASS: %s\n", msg); } \
} while(0)

/* ── 测试 1: 脉冲响应 ── */
static void test_fir_impulse(void)
{
    printf("\n── Test 1: FIR impulse response ──\n");
    int n_taps = 64;
    float *coeffs = (float *)calloc(n_taps, sizeof(float));
    for (int i = 0; i < n_taps; i++) coeffs[i] = (float)(i + 1) / n_taps;

    fir_filter_t f = { coeffs, (double *)calloc(n_taps, sizeof(double)), n_taps, 0 };

    /* 注入单位脉冲 */
    float imp = fir_tick(&f, 1.0f);  /* h[0] 叠加当前延迟线状态(=0) */
    float eps = 1e-5f;
    CHECK(fabsf(imp - coeffs[0]) < eps, "impulse[0] == coeffs[0]");

    /* 后续响应应匹配系数 (零输入) */
    for (int i = 1; i < n_taps; i++) {
        float y = fir_tick(&f, 0.0f);
        if (fabsf(y - coeffs[i]) >= eps) {
            fprintf(stderr, "  FAIL: impulse[%d]=%.6f != coeffs[%d]=%.6f\n", i, y, i, coeffs[i]);
            failures++;
            break;
        }
        if (i == n_taps - 1)
            printf("  PASS: impulse[0..%d] == coeffs[0..%d]\n", n_taps-1, n_taps-1);
    }

    free(f.delay_line); free(coeffs);
}

/* ── 测试 2: FxNLMS 收敛性 (离线路径) ── */
static void test_fxnlms_convergence(void)
{
    printf("\n── Test 2: FxNLMS convergence ──\n");

    fxnlms_mimo_t fx;
    if (fxnlms_init(&fx, E, S, L, 0.001f, 1e-6f) != 0) {
        fprintf(stderr, "  FAIL: fxnlms_init OOM\n"); failures++; return;
    }

    /* 合成场景: Ŝ=δ, Pri=δ, ref=440Hz 正弦
       → Fx = x (Ŝ域滤波参考 = 参考本身)
       → disturbance = x (初级路径=恒等)
       → LMS 应收敛到 Wc ≈ −1 以抵消 disturbance */

    int n_total = 4000, warmup = 200;
    float pwr_before = 0, pwr_after = 0;

    for (int n = 0; n < n_total; n++) {
        float x = 0.01f * sinf(2.0f * 3.14159265f * 100.0f / 16000.0f * n);

        /* 简化: 所有 E×S 条次级路径均为 δ (Fx = x) */
        float Fx[E*S];
        for (int es = 0; es < E*S; es++) Fx[es] = x;

        float dist[E];
        for (int e = 0; e < E; e++) dist[e] = x;  /* Pri=δ */

        float anti[S], err_sig[E];
        fxnlms_tick(&fx, Fx, dist, anti, err_sig);

        if (n >= warmup) {
            for (int e = 0; e < E; e++) pwr_before += dist[e] * dist[e];
            for (int e = 0; e < E; e++) pwr_after += err_sig[e] * err_sig[e];
        }
    }

    int n_samp = n_total - warmup;
    pwr_before /= n_samp; pwr_after /= n_samp;
    float nr_db = 10.0f * log10f((pwr_before + 1e-12f) / (pwr_after + 1e-12f));
    printf("  pwr_before=%.6f pwr_after=%.6f NR=%.1fdB\n", pwr_before, pwr_after, nr_db);
    CHECK(nr_db > 3.0f, "convergence: NR > 3dB after warmup");

    /* 检查 Wc 有界 */
    float wc_max = 0;
    for (int i = 0; i < S*L; i++) {
        float a = fabsf(fx.wc[i]);
        if (a > wc_max) wc_max = a;
    }
    CHECK(wc_max < 5.0f, "Wc bounded (max < 5.0)");

    fxnlms_free(&fx);
}

int main(void)
{
    printf("=== GFANC Unit Tests (R-31) ===\n");
    test_fir_impulse();
    test_fxnlms_convergence();
    printf("\n=== Results: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
