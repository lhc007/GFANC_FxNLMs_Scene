/** verify_fa — F-A 结构性错误的数值验证 (CODE_REVIEW.md §5.2 F-A)
 *
 * 在同一物理仿真 (mic = d + S⊗speaker, 完美模型 Ŝ=S, 隔离纯结构问题) 下对比:
 *
 *   OLD: 部署代码原接线 — fxnlms_tick(实测 mic 当作 disturbance),
 *        扬声器播 anti_out = Wc ⊗ Σ_e Ŝ⊗x  (误差双重计入 + Ŝ 二次滤波)
 *   NEW: fxnlms_tick_rt — 扬声器播 Wc ⊗ x, 实测 mic 直接作误差
 *
 * 两种配置:
 *   SISO (E=1,S=1): 物理上可完全抵消 → 直接暴露算法结构对错
 *   MIMO (E=3,S=2): 部署配置, 随机独立路径下 2 音源消 3 目标物理受限
 *
 * 编译: gcc -O2 -Iinclude tools/verify_fa.c src/fxnlms_mimo.c -lm -o build/verify_fa.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fxnlms_mimo.h"

#define MAX_E 3
#define MAX_S 2
#define L     64
#define PLEN  32      /* 初级/次级路径 FIR 长度 */
#define HIST  64
#define N     400000  /* 仿真样本数 (25s @16k) */
#define WIN   100000  /* NR 统计窗 */

static float sec[MAX_E*MAX_S][PLEN], pri[MAX_E][PLEN];

static void gen_paths(void)
{
    memset(sec, 0, sizeof(sec)); memset(pri, 0, sizeof(pri));
    srand(42);
    /* 次级路径 S(e,s): 延迟 4 样本, 指数衰减随机 FIR */
    for (int i = 0; i < MAX_E*MAX_S; i++)
        for (int k = 4; k < PLEN; k++)
            sec[i][k] = ((float)rand()/RAND_MAX*2-1) * 0.8f * expf(-(k-4)/6.0f);
    /* 初级路径 P(e): 延迟 12 样本 (因果裕度 > 次级路径延迟).
       幅度 ×0.3 使最优扬声器信号远离 ±1 钳位 (隔离削波非线性, 只看结构对错) */
    for (int e = 0; e < MAX_E; e++)
        for (int k = 12; k < PLEN; k++)
            pri[e][k] = ((float)rand()/RAND_MAX*2-1) * 0.3f * expf(-(k-12)/8.0f);
}

/* 返回末窗 NR(dB). mode: 0=旧接线, 1=新接线 */
static double run_case(int Ecnt, int Scnt, int mode)
{
    fxnlms_mimo_t fx;
    fxnlms_init(&fx, Ecnt, Scnt, L, 0.0005f, 1e-5f);

    float xbuf[HIST];        memset(xbuf, 0, sizeof(xbuf));
    float spk[MAX_S][HIST];  memset(spk, 0, sizeof(spk));
    double accd = 0, accm = 0, nr_last = 0;
    srand(7);  /* 各用例喂完全相同的噪声 */

    for (int n = 0; n < N; n++) {
        memmove(xbuf+1, xbuf, (HIST-1)*sizeof(float));
        xbuf[0] = (float)rand()/RAND_MAX*2.0f - 1.0f;

        /* 扰动 d[e] = P(e)⊗x; 实测 mic = d + S⊗spk (仅过去输出, sec[0..3]=0) */
        float d[MAX_E], mic[MAX_E];
        for (int e = 0; e < Ecnt; e++) {
            d[e] = 0;
            for (int k = 0; k < PLEN; k++) d[e] += pri[e][k] * xbuf[k];
            mic[e] = d[e];
            for (int s = 0; s < Scnt; s++)
                for (int k = 1; k < PLEN; k++)
                    mic[e] += sec[e*MAX_S+s][k] * spk[s][k-1];
        }

        /* Fx = Ŝ ⊗ x (完美模型) — 注意按 fx 的 [E*S] 紧凑排布 */
        float Fx[MAX_E*MAX_S];
        for (int e = 0; e < Ecnt; e++)
            for (int s = 0; s < Scnt; s++) {
                float v = 0;
                for (int k = 0; k < PLEN; k++) v += sec[e*MAX_S+s][k] * xbuf[k];
                Fx[e*Scnt+s] = v;
            }

        float anti[MAX_S], err_sig[MAX_E], anti_est[MAX_E];
        if (mode == 0)
            fxnlms_tick(&fx, Fx, mic, anti, err_sig);              /* 旧接线 */
        else
            fxnlms_tick_rt(&fx, xbuf[0], Fx, mic, anti, anti_est); /* 新接线 */

        /* 输出钳位 ±1 (同部署) + 推入扬声器历史 */
        for (int s = 0; s < Scnt; s++) {
            if (anti[s] >  1.0f) anti[s] =  1.0f;
            if (anti[s] < -1.0f) anti[s] = -1.0f;
            memmove(&spk[s][1], &spk[s][0], (HIST-1)*sizeof(float));
            spk[s][0] = anti[s];
        }

        for (int e = 0; e < Ecnt; e++) { accd += (double)d[e]*d[e]; accm += (double)mic[e]*mic[e]; }
        if ((n+1) % WIN == 0) {
            nr_last = 10.0*log10(accd/(accm+1e-12));
            printf("    n=%6d  NR(真实) = %6.2f dB\n", n+1, nr_last);
            accd = accm = 0;
        }
    }
    fxnlms_free(&fx);
    return nr_last;
}

int main(void)
{
    gen_paths();
    const char *mode_name[2] = {
        "OLD (部署原接线: mic 当 disturbance, 输出经 Ŝ 二次滤波)",
        "NEW (fxnlms_tick_rt: 输出 Wc⊗x, 实测误差驱动)" };
    double nr[2][2];

    for (int cfg = 0; cfg < 2; cfg++) {
        int Ecnt = cfg ? 3 : 1, Scnt = cfg ? 2 : 1;
        printf("═══ %s (E=%d, S=%d) ═══\n", cfg ? "MIMO 部署配置" : "SISO 可完全抵消", Ecnt, Scnt);
        for (int mode = 0; mode < 2; mode++) {
            printf("  %s\n", mode_name[mode]);
            nr[cfg][mode] = run_case(Ecnt, Scnt, mode);
        }
        printf("\n");
    }

    printf("═══ 汇总 (末窗稳态 NR) ═══\n");
    printf("  SISO: OLD %6.2f dB  →  NEW %6.2f dB\n", nr[0][0], nr[0][1]);
    printf("  MIMO: OLD %6.2f dB  →  NEW %6.2f dB\n", nr[1][0], nr[1][1]);
    return 0;
}
