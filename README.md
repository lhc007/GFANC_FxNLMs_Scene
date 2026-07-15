# GFANC FxNLMS — MIMO 主动降噪 C 实现

从 Python [GFANC_Scene](d:/VSCodeRepository/GFANC_Scene) 移植的离线 WAV 降噪演示程序。
已验证与 Python 参考实现 dB 输出**逐秒完全一致**（56 秒平均 15.00 dB）。

## 快速开始

```bash
# 编译
gcc -O2 -Iinclude main.c src/*.c -lm -o main.exe

# 运行
./main.exe "path/to/noise.wav"

# 输出
#   anti_out.wav   — 反噪声信号 (S=2 声道)
#   error_out.wav  — 误差/残差信号 (E=3 声道)
```

输入支持任意采样率的 16-bit PCM WAV，自动重采样到 16kHz。

## 系统参数

| 参数 | 值 | 说明 |
|------|----|------|
| fs | 16000 Hz | 工作采样率 |
| E | 3 | 误差麦克风数 |
| S | 2 | 扬声器数 |
| C | 15 | 子滤波器数（频带分解） |
| K | 8 | 场景分类数 |
| Len | 1024 tap | 控制滤波器长度 |
| Sec Len | 1024+16 tap | 次级路径（含 DSP 延迟） |
| BP Len | 1024 tap | 带通 FIR 20–1500Hz |
| μ | 0.0001 | FxNLMS 步长 |
| Leak | 1e-5 | 泄露因子 |
| Fade | 16 样本 | 交叉淡化长度 (~1ms) |
| Reset | cos < 0.8 | 场景切换余弦相似度阈值 |

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│  慢速环路: CNN 场景辨识 + Blend 构造 Wc  (每秒执行一次)          │
│                                                                  │
│  raw_audio (全频)                                                │
│      │                                                           │
│      ├─ 峰值归一化                                                │
│      │                                                           │
│      ├─ [① 带通 FIR 20-1500Hz, 1024tap] ──→ noise_bp             │
│      │                                      │                    │
│      │                                      ├─→ [② 带通 FIR]     │
│      │                                      │    → ref_filt       │
│      │                                      │    → 送往前馈环路   │
│      │                                      │                    │
│      │                                      └─→ minmaxscaler     │
│      │                                           → CNN 输入      │
│      │                                               │           │
│      │                              ┌────────────────┘           │
│      │                              ▼                             │
│      │                     ┌─────────────────┐                   │
│      │                     │  CNN m5_scene   │  1D 残差卷积       │
│      │                     │  输出: K=8 维   │  64ch, 4 ResBlock │
│      │                     │  logits         │                   │
│      │                     └────────┬────────┘                   │
│      │                              ▼                             │
│      │                     ┌─────────────────┐                   │
│      │                     │  softmax        │  → probs[K]       │
│      │                     │  argmax         │  → scene_id       │
│      │                     └────────┬────────┘                   │
│      │                              ▼                             │
│      │                     ┌─────────────────┐                   │
│      │                     │  Blend          │  centroid[top1]   │
│      │                     │  → [S×C] 权重   │  / max → clip     │
│      │                     └────────┬────────┘                   │
│      │                              ▼                             │
│      │                     ┌─────────────────┐                   │
│      │                     │  construct Wc   │  Σ blend[c]×      │
│      │                     │  → Wc[S, Len]   │  sub_filter[c]    │
│      │                     │  RMS对齐 stub   │  → 取反           │
│      │                     └────────┬────────┘                   │
│      │                              ▼                             │
│      │                     ┌─────────────────┐                   │
│      │                     │  滞回检测        │  cos_sim(prev,   │
│      │                     │                  │  curr) < 0.8?    │
│      │                     │  是 → CrossFader │  否 → 保持 Wc    │
│      │                     └────────┬────────┘                   │
│      │                              │                            │
│      │                         Wc (更新 or 保持)                  │
│      │                              │                            │
└──────┼──────────────────────────────┼────────────────────────────┘
       │                              │
       │    ref_filt (20-1500Hz)      │  Wc (1024tap × S)
       │    (来自带通FIR ②)           │  (每 1s 更新)
       │                              │
       ▼                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  前馈环路: 实时宽带降噪  (逐样本 62.5μs)                          │
│                                                                  │
│  ref_filt[n]                                                     │
│      │                                                           │
│      ├──→ ┌──────────────────────────┐                          │
│      │    │ ③ 次级路径 FIR            │  1024+16 tap            │
│      │    │   Sec ⊗ ref_filt          │  E×S=6 通道             │
│      │    │   (含 16 样本 DSP 延迟)    │                         │
│      │    │   → Fx[e][s]              │                         │
│      │    └──────────┬───────────────┘                          │
│      │               │                                          │
│      │               ├──→ Xd 延迟线 [E,S,Len]                   │
│      │               │    roll → Xd[:,:,0]=Fx                   │
│      │               │         │                                │
│      │               │         └──→ ┌──────────────────┐       │
│      │               │              │ y_ff[e] =         │       │
│      │               │              │ Σ Wc[s,k]×Xd[e,s,k]│      │
│      │               │              │ → anti_est[e]     │       │
│      │               │              └────────┬─────────┘       │
│      │               │                       │                  │
│      │               │        err[e] = Dis[e] + anti_est[e]    │
│      │               │                       │                  │
│      │               │         ┌─────────────┘                  │
│      │               │         ▼                                │
│      │               │  ┌──────────────────────┐               │
│      │               │  │  FxNLMS 梯度更新      │               │
│      │               │  │  grad[s,k] =         │               │
│      │               │  │    Σ_e err[e]×Xd[e,s,k]              │
│      │               │  │  power[s] = mean(Xd²) │               │
│      │               │  │  Wc[s,k] -= μ×grad/pwr│               │
│      │               │  │  Wc[s,k] *= 1-μ×leak  │               │
│      │               │  └──────────┬───────────┘               │
│      │               │             │                            │
│      │               │        Wc 微调 (跨秒持续自适应)           │
│      │               │                                         │
│      ├──→ ┌──────────────────────────┐                          │
│      │    │ Pri 路径 FIR              │  1024 tap               │
│      │    │ Pri ⊗ ref_filt            │  E=3 通道               │
│      │    │ → Dis[e] (扰动信号)       │                         │
│      │    └──────────────────────────┘                          │
│      │                                                          │
│      └──→ ┌──────────────────────────┐                          │
│           │ ② 反噪声 FIR              │  1024 tap               │
│           │ Wc ⊗ Xd                   │  S=2 通道               │
│           │ → anti_spk[s] (反噪声)    │                         │
│           └──────────────────────────┘                          │
│                       │                                          │
│                       ▼                                          │
│                anti_out.wav (S 声道)                              │
│                error_out.wav (E 声道)                             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

       注: 反馈环路 (IIR 20-200Hz) 在离线仿真中未启用 (FB_GAIN=0)
       限幅 ±0.5、内插 16→48k、DAC 为硬件部署步骤，离线仿真不需要
```

## 数据文件

运行时从 `data/` 目录加载以下 `.bin` 文件（由 `export/export_bin.py` 从 Python 项目导出）：

| 文件 | 内容 | 大小 |
|------|------|------|
| `secondary_path.bin` | 次级路径 IR [E,S,Len] | 6144 float |
| `primary_path.bin` | 初级路径 IR [E,R,Len] | 6144 float |
| `sub_filters.bin` | 子滤波器 Wc_v [C,S,Len] | 30720 float |
| `scene_defs.bin` | 场景质心 [K,S×C] | 240 float |
| `bandpass_fir.bin` | 带通 FIR 系数 [1024] | 1024 float |
| `cnn_*.bin` | CNN 权重 (stem/res0-3/linear) | ~50 文件 |

## 输出解读

```
 Sec |              Top-3 Scenes |   dB(Band) |   dB(Full) | Action
   1 |      0:0.33,6:0.26,3:0.10 |    14.47 dB |    16.23 dB | INIT
```

- **Top-3 Scenes**: `场景ID:softmax概率` 前三名
- **dB(Band)**: 带内降噪量 = 10·log₁₀(P_dis/P_err)，只在 20-1500Hz 评估（**主要指标**）
- **dB(Full)**: 全频参考，含不可控的 1500Hz+ 带外能量（仅对比用）
- **Action**: `INIT`=首秒初始化，`RESET`=场景切换触发 CrossFader，`-`=无切换

## 与 Python 的对应关系

| C 文件 | Python 对应 | 功能 |
|--------|------------|------|
| `main.c` | `Main_GFANC_Realtime.ipynb` | 主流程：WAV I/O + 逐秒 CNN + 逐样本 FxNLMS |
| `src/cnn_m5_forward.c` | `gfanc/Network.py` | m5_scene CNN (64ch, 4 ResBlock) |
| `src/scene_controller.c` | `gfanc/SceneController.py` | CNN → Blend → construct_wc |
| `src/fxnlms_mimo.c` | `gfanc/Combine_GFANC_with_FxNLMS_MIMO.py` | MIMO FxNLMS 算法 |
| `src/fir_filter.c` | `scipy.signal.lfilter` | FIR 滤波器 (double 精度) |
| `src/cross_fader.c` | `gfanc/cross_fader.py` | Wc 系数交叉淡化 |
| `export/export_bin.py` | — | PyTorch/NumPy → C .bin 文件导出 |

## 编译选项

```bash
# GCC (推荐)
gcc -O2 -Iinclude main.c src/*.c -lm -o main.exe

# MSVC
cl /O2 /Iinclude main.c src\*.c /Fe:main.exe

# CMake
mkdir build && cd build && cmake .. && make
```
