/** howling_detect — DFT 频谱峰值检测 + IIR 陷波滤波器
 *
 * 原理: 啸叫是正反馈环路在特定频率自激产生的窄带单音.
 *       对误差信号做短时频谱分析, 检测持续存在的窄带峰值,
 *       确认后用 IIR 陷波器从输出中移除该频率分量.
 *
 * 用法:
 *   howling_detect_t hw;
 *   howling_init(&hw, 1);  // 1=启用
 *
 *   在音频回调中每样本调用:
 *     howling_tick(&hw, err_avg, anti_out);
 *   内部自动累积 256 样本 → DFT → 峰值检测 → 陷波 anti_out
 */
#ifndef HOWLING_DETECT_H
#define HOWLING_DETECT_H

#define HW_FFT_N        256      /* DFT 窗口 (16ms @ 16kHz) */
#define HW_MIN_BIN      2        /* 最低检测 bin (125Hz, 跳过 DC+工频) */
#define HW_MAX_BIN      24       /* 最高检测 bin (~1500 Hz, 匹配带通上限) */
#define HW_THRESH_DB    15.0f    /* 峰值需高于均值此 dB 数才算候选 (避开宽带噪声伪峰) */
#define HW_PERSIST       4       /* 连续帧数确认啸叫 (4帧≈64ms) */
#define HW_RELEASE       8       /* 啸叫消失后延迟释放帧数 */
#define HW_NOTCH_R       0.96f   /* 陷波器带宽 (越接近1越窄, 0.9-0.99) */
#define HW_MAX_NOTCHES   2       /* 最多同时陷波数 */
#define HW_MIN_HOLD      32      /* 陷波最小保持帧数 (32帧≈512ms, 防释放死循环) */
#define HW_S             2       /* 扬声器数 (每个扬声器独立 IIR 状态) */

typedef struct {
    int    enabled;

    /* DFT 累积缓冲 */
    float  buf[HW_FFT_N];
    int    buf_pos;

    /* 检测状态 */
    float  candidate_freq;      /* 候选啸叫频率 (Hz) */
    int    candidate_count;     /* 连续出现帧数 */
    float  active_freqs[HW_MAX_NOTCHES];  /* 已确认啸叫频率 */
    int    active_count;        /* 当前激活的陷波数 */
    int    notch_age[HW_MAX_NOTCHES];  /* 每个陷波器的帧龄 (≥HW_MIN_HOLD才能释放) */
    int    release_timer;       /* 啸叫消失后延迟释放计时 */

    /* IIR 陷波器状态 (每扬声器 × 每频率独立, 避免跨通道串扰) */
    float  b1[HW_MAX_NOTCHES];  /* b0=1, b2=1 固定, 只需存 b1 */
    float  a1[HW_MAX_NOTCHES], a2[HW_MAX_NOTCHES];
    float  x1[HW_S][HW_MAX_NOTCHES], x2[HW_S][HW_MAX_NOTCHES];
    float  y1[HW_S][HW_MAX_NOTCHES], y2[HW_S][HW_MAX_NOTCHES];

    /* 监控 */
    volatile float dominant_freq;  /* 当前最强候选频率 (Hz, 用于显示) */
    volatile float dominant_db;    /* 当前最强候选峰均值比 (dB) */
} howling_detect_t;

void howling_init(howling_detect_t *hw, int enabled);

/** 每样本调用: 累积误差, 满一帧后 DFT 检测, 陷波 anti_spk */
void howling_tick(howling_detect_t *hw, float err_sample,
                  float *anti_spk, int S);

#endif
