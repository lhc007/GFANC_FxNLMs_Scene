/** calibrate_feedback — 离线测量扬声器→参考麦反馈路径
 *
 * 编译: gcc -O2 -Iinclude src/calibrate_feedback.c src/fir_filter.c src/binary_loader.c src/pa_loader.c -lm -o calibrate_feedback.exe
 * 运行: ./calibrate_feedback.exe
 * 输出: data/feedback_path.bin (256 tap float32 FIR)
 *
 * 原理: 扬声器播放白噪声, 参考麦录制, NLMS 辨识 FIR 冲激响应.
 *       Fb_path = speaker_output → acoustic → ref_mic
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "os_port.h"        /* R-28: gf_sleep_ms 可移植睡眠 */

#include "fir_filter.h"
#include "binary_loader.h"

/* 2阶 Butterworth 低通 biquad — 与 main_realtime.c R-14 抗混叠一致.
   旧代码直接 3:1 抽取, >8kHz 分量折叠入通带污染 FIR 辨识. */
typedef struct { float b0,b1,b2,a1,a2,z1,z2; } biquad_t;

static void biquad_init_lpf(biquad_t *f, float fc, float fs)
{
    float w0 = 2.0f * 3.14159265f * fc / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / (2.0f * 0.70710678f);  /* Q=1/√2 Butterworth */
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f - c) / (2.0f * a0);
    f->b1 = (1.0f - c) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0f * c / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_tick(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ══════════════════════════════════════════════════════════ */
#define FS_HW       48000
#define FS_CAL      16000
#define FB_TAPS     512         /* 512样本 = 32ms @16k: 覆盖 USB 设备往返(~238样本/14.9ms)
                                    + 声学响应尾. 旧 256 只给响应留 18 样本 → 截断,
                                    NLMS 不收敛 (max|err| 平 0.42), FIR 全坏. */
#define CAL_SEC     4           /* 校准时长 (秒) */
#define NOISE_AMP   0.9f        /* 白噪声幅度 (env: GFANC_CAL_NOISE 可覆盖) */
#define NLMS_MU     0.2f        /* NLMS 步长 */
#define FB_FILE     "data/feedback_path.bin"

/* R-50(修订, 2026-08-13): 峰位物理 sanity 由"11ms 硬上限"改为"聚类可复现 + 窗口边沿"双检.
   旧假设"spk→ref 不含设备延迟, 必 <11ms"对 USB-ASIO 不成立: 反相经 DAC→喇叭→参考麦→ADC,
   设备往返 (~12ms) 就在反馈路径里 — 4 次实测定峰位恒 ~238样本 (14.9ms) 且可复现, 是真实
   延迟, 不是噪声伪峰. 真正的坏 FIR 是响应被窗口截断 (峰贴尾) → NLMS 无法收敛. 判定:
   1) 聚类投票验证延迟在多个子窗反复复现 (真响应) — 与 calibrate_secondary v4 同法;
   2) 峰距窗口尾 <10% → 截断 → 拒收. */
#define PROBE_MAXLAG  8000      /* 探测范围 0.5s (实测环路 ~14ms) */
#define SKIP_HEAD     16000     /* 跳过前 1s 流启动瞬态 */
#define SUBWIN        4000      /* 探测子窗 0.25s */
#define CLUSTER_TOL   150       /* 聚类半径 (样本) */

/* ══════════════════════════════════════════════════════════ */
typedef struct {
    float *noise_16k;   /* 预生成白噪声 (16kHz, 用于ZOH×3播放+NLMS辨识) */
    float *ref_hw;      /* 参考麦录制 (48kHz, 仅在回调中写入) */
    int    idx;         /* 48k 样本计数 */
    int    total;       /* 48k 总样本数 */
    int    spk_idx;     /* 校准扬声器: 0=spk0, 1=spk1 */
} cal_data_t;

#include "pa_loader.h"

/* ══════════════════════════════════════════════════════════
   音频回调: 播放白噪声, 录制参考麦
   ══════════════════════════════════════════════════════════ */
static int cal_cb(const void *input, void *output, unsigned long fcount,
                   const PaCbTimeInfo *ti, unsigned long flags, void *user)
{
    cal_data_t *cal = (cal_data_t *)user;
    const float *in = (const float *)input;
    float *out = (float *)output;
    (void)ti; (void)flags;

    for (unsigned long i = 0; i < fcount; i++) {
        if (cal->idx >= cal->total) {
            out[i*2] = out[i*2+1] = 0;
            continue;
        }
        /* 16k白噪声 ZOH×3 → 48k播放 (仅校准扬声器输出, 另一通道静音) */
        float n = cal->noise_16k[cal->idx / 3];
        out[i*2]   = (cal->spk_idx == 0) ? n : 0.0f;
        out[i*2+1] = (cal->spk_idx == 1) ? n : 0.0f;
        /* 录制参考麦 (ch0) */
        cal->ref_hw[cal->idx] = in[i*4 + 0];
        cal->idx++;
    }
    return 0; /* paContinue */
}

/* ══════════════════════════════════════════════════════════
   R-50(修订): 聚类投票探测 — 与 calibrate_secondary v4 同法.
   nsub 个子窗各自找全程峰, 最大 ±TOL 聚类 ≥ 1/3 → 延迟可复现 (真实响应);
   噪声伪峰散布全程, 聚不成团. 返回聚类中最早子窗的 lag, -1=失败.
   ══════════════════════════════════════════════════════════ */
static double xcorr_at(const float *x, const float *y, int n, int lag)
{
    double c = 0;
    for (int i = 0; i < n; i++) c += (double)x[i] * y[i + lag];
    return c;
}

static int argmax_lag(const float *x, const float *y, int n,
                      int lag_lo, int lag_hi)
{
    /* R-50(修订): 用带符号相关峰 (正相关) 而非 fabs — 反馈路径是正耦合,
       noise→speaker→声学→mic 同相, 真实延迟处 xcorr>0. fabs 会把负相关
       伪峰 (反相噪声/反相串扰) 也当候选, 锁错 lag. */
    double best = -1e300; int bl = lag_lo;
    for (int lag = lag_lo; lag <= lag_hi; lag++) {
        double c = xcorr_at(x, y, n, lag);
        if (c > best) { best = c; bl = lag; }
    }
    return bl;
}

static float rms_of(const float *x, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
    return (float)sqrt(s / n);
}

static int probe_vote(const float *noise, const float *mic, int n16,
                      double *slip_out)
{
    int lags[64], offs[64], nsub = 0;
    for (int j = 0; j < 64; j++) {
        long o = SKIP_HEAD + (long)j * SUBWIN;
        if (o + SUBWIN + PROBE_MAXLAG >= n16) break;
        lags[nsub] = argmax_lag(noise + o, mic + o, SUBWIN, 0, PROBE_MAXLAG - 1);
        offs[nsub] = (int)o;
        nsub++;
    }

    /* 最大聚类 */
    int best_cnt = 0, best_i = -1;
    for (int i = 0; i < nsub; i++) {
        int cnt = 0;
        for (int j = 0; j < nsub; j++)
            if (abs(lags[j] - lags[i]) <= CLUSTER_TOL) cnt++;
        if (cnt > best_cnt) { best_cnt = cnt; best_i = i; }
    }

    printf("    子窗峰: ");
    for (int j = 0; j < nsub; j++) {
        int in_c = abs(lags[j] - lags[best_i]) <= CLUSTER_TOL;
        printf("%s%d%s ", in_c ? "[" : "", lags[j], in_c ? "]" : "");
    }
    printf("\n    聚类: %d/%d 子窗聚在 %d±%d\n",
           best_cnt, nsub, lags[best_i], CLUSTER_TOL);

    if (best_cnt * 3 < nsub) return -1;   /* 未达 1/3 法定数 */

    /* 聚类内: 最早子窗的 lag = 流启动延迟; 首尾差 → 滑移率 */
    int first_lag = -1, last_lag = 0;
    long first_off = 0, last_off = 0;
    for (int j = 0; j < nsub; j++) {
        if (abs(lags[j] - lags[best_i]) <= CLUSTER_TOL) {
            if (first_lag < 0) { first_lag = lags[j]; first_off = offs[j]; }
            last_lag = lags[j]; last_off = offs[j];
        }
    }
    *slip_out = (last_off > first_off)
        ? (double)(last_lag - first_lag) / (double)(last_off - first_off) * 1e6 : 0;
    return first_lag;
}

/* ══════════════════════════════════════════════════════════
   NLMS 辨识: 已知激励(out) 和响应(ref), 求 FIR 系数
   ══════════════════════════════════════════════════════════ */
static int nlms_identify(const float *noise_16k, const float *ref_16k,
                          int n_samples, float *fb_coeffs, int n_taps)
{
    float *x = (float *)calloc(n_taps, sizeof(float));  /* delay line */
    int ptr = 0;

    memset(fb_coeffs, 0, n_taps * sizeof(float));

    printf("  NLMS identifying %d taps from %d samples...\n", n_taps, n_samples);

    float max_err = 0, max_coeff = 0;
    int check_interval = n_samples / 10;
    for (int n = 0; n < n_samples; n++) {
        if (n % check_interval == 0 && n > 0) {
            float crms = 0;
            for (int k = 0; k < n_taps; k++) crms += fb_coeffs[k] * fb_coeffs[k];
            printf("  NLMS %d%%: max|err|=%.4f coeff_rms=%.6f\n",
                   n * 100 / n_samples, max_err, sqrtf(crms / n_taps));
        }
        /* shift in new input sample */
        x[ptr] = noise_16k[n];

        /* compute filter output estimate */
        float y = 0;
        for (int k = 0; k < n_taps; k++)
            y += fb_coeffs[k] * x[(ptr - k + n_taps) % n_taps];

        /* error = actual - estimate */
        float e = ref_16k[n] - y;

        /* signal power sum (with regularization), NOT averaged.
           R-48 note: 此处 power 是 Σx² 不除以 n_taps, NLMS_MU=0.2 对应标准 β∈(0,2).
           floor=1e-6 合理 (power 预期 ~7.7 for noise_amp=0.3, n_taps=256),
           与 fxnlms_mimo.c 的归一化 power 不同, 无需修改. */
        float power = 1e-6f;
        for (int k = 0; k < n_taps; k++) {
            float v = x[(ptr - k + n_taps) % n_taps];
            power += v * v;
        }
        /* power = sum(x²), 不除以 n_taps. NLMS_MU 直接对应标准 β (0 < β < 2) */

        /* NLMS update */
        float mu = NLMS_MU / power;
        for (int k = 0; k < n_taps; k++)
            fb_coeffs[k] += mu * e * x[(ptr - k + n_taps) % n_taps];

        if (fabsf(e) > max_err) max_err = fabsf(e);
        for (int k = 0; k < n_taps; k++)
            if (fabsf(fb_coeffs[k]) > max_coeff) max_coeff = fabsf(fb_coeffs[k]);

        ptr = (ptr + 1) % n_taps;
    }

    printf("  NLMS: max|err|=%.4f  max|coeff|=%.4f\n", max_err, max_coeff);
    free(x);

    /* 计算 FIR 的 RMS 和峰值信息 */
    float rms = 0, peak = 0;
    int peak_idx = 0;
    for (int k = 0; k < n_taps; k++) {
        rms += fb_coeffs[k] * fb_coeffs[k];
        if (fabsf(fb_coeffs[k]) > peak) {
            peak = fabsf(fb_coeffs[k]);
            peak_idx = k;
        }
    }
    rms = sqrtf(rms / n_taps);
    float peak_ms = (float)peak_idx / FS_CAL * 1000.0f;
    float pnr = (rms > 0.0f) ? peak / rms : 0.0f;
    printf("  FIR: peak=%.4f @ tap %d (%.2fms), RMS=%.4f, PNR=%.1f\n",
           peak, peak_idx, peak_ms, rms, pnr);
    /* R-50(修订): 窗口边沿截断检查 — 峰贴窗口尾 = 真实响应超出 FIR 长度, NLMS 截断拟合,
       FIR 不可信 (旧 256 样本: 峰@238 仅留 18 样本给响应尾 → max|err| 平 0.42).
       注意: 峰位 ~238样本/14.9ms 属正常 — USB 设备往返 (~12ms) 就在反馈路径里.
       距尾 <10% → 拒收 (不保存), 避免坏 FIR 进运行时制造虚假 fb_est. */
    if (peak_idx > n_taps - (n_taps / 10)) {
        printf("  ❌ 峰位 @tap %d (%.2fms) 距窗口尾 <10%% — 响应被截断, NLMS 未收敛.\n"
               "     增大 FB_TAPS 或检查流对齐后重标定 (R-50).\n",
               peak_idx, peak_ms);
        return -1;
    }
    /* R-57: 弱路径质量门禁 — RMS<0.0005 时 FIR 基本是噪声, 反馈抵消形同虚设.
       此时不保存文件, 避免运行时加载无效 FIR 产生虚假 fb_est.
       实测 RMS=0.0001 的 FIR 装载后 fb_est≈0, 扬声器满幅反馈直进参考麦. */
    if (rms < 0.00005f) {
        printf("  ❌ 反馈路径过弱 (FIR RMS=%.6f < 0.00005) — 标定失败, 不保存文件!\n", rms);
        printf("     请确认扬声器有输出、参考麦能拾音后重新标定.\n");
        printf("     提示: 设置 GFANC_CAL_NOISE 环境变量可调整噪声幅度 (默认=%.2f).\n",
               (double)NOISE_AMP);
        return -1;
    }
    if (rms < 0.0005f)
        printf("  ⚠ 反馈路径偏弱 (FIR RMS=%.6f) — 物理隔离好, 高增益下反馈抵消仍有价值.\n", rms);
    return 0;  /* 标定成功 */
}

/* ══════════════════════════════════════════════════════════ */
int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    printf("\n=== Feedback Path Calibration ===\n\n");

    if (pa_init() != 0) return 1;
    p_Pa_Initialize();

    /* 列出设备 (跳过 MME/DirectSound, 只显示 ASIO/WASAPI/WDM-KS) */
    int nd = p_Pa_GetDeviceCount();
    int napi = 0;
    printf("\n=== Audio Devices ===\n");
    for (int api_idx = 0; ; api_idx++) {
        const PaHostApiInfo2 *api = (const PaHostApiInfo2 *)p_Pa_GetHostApiInfo(api_idx);
        if (!api) break;
        if (strstr(api->name, "MME") || strstr(api->name, "DirectSound")) continue;
        napi++;
        int has_dev = 0;
        for (int i = 0; i < nd; i++) {
            const PaDeviceInfo2 *info = (const PaDeviceInfo2 *)p_Pa_GetDeviceInfo(i);
            if (info && info->hostApi == api_idx
                && info->maxInputChannels > 0 && info->maxOutputChannels > 0) {
                if (!has_dev) { printf("\n[%s]\n", api->name); has_dev = 1; }
                printf("  %2d: %s (in=%d out=%d fs=%.0f)\n",
                    i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
            }
        }
    }
    if (napi == 0) { fprintf(stderr, "PA: no host APIs found\n"); return 1; }

    int in_dev, out_dev;
    printf("\nInput device ID (ASIO MIC): "); fflush(stdout); scanf("%d", &in_dev);
    printf("Output device ID (ASIO Speaker): "); fflush(stdout); scanf("%d", &out_dev);

    /* 预生成 16kHz 白噪声 — ZOH×3 播放, 与运行时输出路径一致 (F-F修复) */
    float noise_amp = NOISE_AMP;
    {   const char *s = getenv("GFANC_CAL_NOISE");
        if (s) noise_amp = (float)atof(s); }
    printf("Noise amplitude: %.2f (set GFANC_CAL_NOISE to override)\n", noise_amp);
    int total_hw = FS_HW * CAL_SEC;
    int n_16k     = total_hw / 3;
    float *noise_16k = (float *)malloc(n_16k * sizeof(float));
    float *ref_hw    = (float *)malloc(total_hw * sizeof(float));
    float *ref_16k   = (float *)malloc(n_16k * sizeof(float));
    srand(42);  /* 固定种子, 可复现 */
    for (int i = 0; i < n_16k; i++)
        noise_16k[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * noise_amp;

    /* 缓冲可调: GFANC_BUFFER (样本), 默认 128 — 与运行时同步, suggestedLatency 由缓冲推导
       (避免旧 0.01s 把 ASIO 驱动顶到 512 样本大缓冲). */
    int buf_frames = 128;
    {   const char *s = getenv("GFANC_BUFFER");
        if (s && atoi(s) >= 32 && atoi(s) <= 1024) buf_frames = atoi(s); }
    double buf_lat = (double)buf_frames / FS_HW;
    PaStreamParams in_p  = { in_dev,  4, paFloat32, buf_lat, NULL };
    PaStreamParams out_p = { out_dev, 2, paFloat32, buf_lat, NULL };

    /* ── 逐扬声器校准 (F-G修复) ── */
    for (int spk = 0; spk < 2; spk++) {
        printf("\n--- Calibrating Speaker %d ---\n", spk);
        printf("  Playing white noise on speaker %d only for %d seconds...\n", spk, CAL_SEC);
        printf("  Keep the room quiet - no talking or moving!\n");

        memset(ref_hw, 0, total_hw * sizeof(float));
        cal_data_t cal = { noise_16k, ref_hw, 0, total_hw, spk };

        PaStream *stream = NULL;
        int err = p_Pa_OpenStream(&stream, &in_p, &out_p, FS_HW, buf_frames, paNoFlag, cal_cb, &cal);
        if (err) { fprintf(stderr, "PA open error: %s\n", p_Pa_GetErrorText(err)); return 1; }
        p_Pa_StartStream(stream);
        while (cal.idx < total_hw) gf_sleep_ms(100);  /* R-28 */
        p_Pa_StopStream(stream);
        p_Pa_CloseStream(stream);

        /* ref_hw 抗混叠低通 → 3:1 抽取 → ref_16k (与运行时 R-14 链路一致) */
        {
            biquad_t aa;
            biquad_init_lpf(&aa, 6500.0f, (float)FS_HW);
            for (int i = 0; i < total_hw; i++)
                ref_hw[i] = biquad_tick(&aa, ref_hw[i]);
            for (int i = 0; i < n_16k; i++)
                ref_16k[i] = ref_hw[i * 3];
        }

        /* 诊断 */
        {   float nrms = 0, rrms = 0;
            for (int i = 0; i < n_16k; i++) {
                nrms += noise_16k[i] * noise_16k[i];
                rrms += ref_16k[i] * ref_16k[i];
            }
            printf("  Signal RMS: noise=%.4f  ref=%.4f\n",
                   sqrtf(nrms / n_16k), sqrtf(rrms / n_16k));
        }

        /* R-50(修订): 聚类投票验证反馈延迟可复现 — 真响应在多子窗反复出现, 噪声伪峰散布.
           峰位 ~238样本/14.9ms 属正常 (USB 设备往返就在反馈路径里), 但必须可复现才可信. */
        double slip = 0;
        int anchor = probe_vote(noise_16k, ref_16k, n_16k, &slip);
        if (anchor < 0) {
            printf("  ❌ 聚类未达法定数 — 参考麦未捕获可复现的扬声器响应.\n"
                   "     检查扬声器音量 / 参考麦拾音 / 距离后重标定 (R-50).\n");
            continue;  /* 不保存 */
        }
        printf("  反馈延迟: 峰≈%d样本 (%.2fms), 滑移 %.0fppm — 可复现, 通过聚类验证\n",
               anchor, (float)anchor * 1000.0f / FS_CAL, slip);

        /* NLMS 辨识 */
        float fb_coeffs[FB_TAPS];
        int nlms_ret = nlms_identify(noise_16k, ref_16k, n_16k, fb_coeffs, FB_TAPS);
        if (nlms_ret != 0) {
            printf("  ⚠ Speaker %d calibration failed, skipping file save.\n", spk);
            continue;  /* R-57/R-50: 标定质量不达标, 不保存无效 FIR */
        }

        /* 保存 (feedback_path_0.bin / feedback_path_1.bin) */
        char fname[64];
        snprintf(fname, sizeof(fname), "data/feedback_path_%d.bin", spk);
        FILE *f = fopen(fname, "wb");
        if (!f) { fprintf(stderr, "ERROR: Cannot write %s\n", fname); return 1; }
        fwrite(fb_coeffs, sizeof(float), FB_TAPS, f);
        fclose(f);
        printf("  Saved: %s (%d taps)\n", fname, FB_TAPS);
    }

    p_Pa_Terminate();

    /* 清理 */
    free(noise_16k); free(ref_hw); free(ref_16k);

    printf("\nDone. Now run gfanc_realtime.exe with feedback cancellation.\n\n");
    return 0;
}
