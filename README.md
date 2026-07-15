# GFANC FxNLMS — MIMO 主动降噪系统 (C 实现)

基于 Python [GFANC_Scene](../GFANC_Scene) 移植的 MIMO GFANC + FxNLMS 主动降噪引擎。

- **离线模式**: WAV 文件输入 → 降噪处理 → WAV 输出，与 Python 参考实现 **dB 逐秒完全一致**（56 秒平均 15.00 dB）
- **实时模式**: WASAPI 麦克风捕获 → ANC 处理 → 扬声器输出，48kHz 硬件采样率

## 项目结构

```
GFANC_FxNLMs_Scene/
├── main.c                 离线降噪主程序 (WAV → ANC → WAV)
├── main_realtime.c        实时降噪主程序 (WASAPI 麦克风 → ANC → 扬声器)
├── Makefile               编译脚本
├── CMakeLists.txt         CMake 构建
│
├── include/               头文件
│   ├── gfanc_types.h      核心类型定义 (FIR, SceneCtrl, FxNLMS)
│   ├── fir_filter.h       FIR 滤波器 API
│   ├── wasapi_io.h        WASAPI 音频 I/O API
│   ├── binary_loader.h    .bin 文件加载器
│   └── ...
│
├── src/                   实现
│   ├── fir_filter.c       FIR 滤波器 (环形缓冲, double 精度)
│   ├── cnn_m5_forward.c   CNN m5_scene 前向推理
│   ├── wasapi_io.c        WASAPI 捕获/渲染封装
│   ├── binary_loader.c    二进制权重加载
│   └── ...
│
├── data/                  模型权重 (.bin 文件, 运行时加载)
│   ├── secondary_path.bin  次级路径 IR [E,S,Len]
│   ├── primary_path.bin    初级路径 IR [E,R,Len]
│   ├── sub_filters.bin     子滤波器 [C,S,Len]
│   ├── scene_defs.bin      场景质心 [K,S×C]
│   ├── bandpass_fir.bin    带通 FIR 系数
│   └── cnn_*.bin           CNN 权重 (~50 文件)
│
└── export/                权重导出脚本 (Python → C .bin)
    ├── export_bin.py
    └── export_model.py
```

## 快速开始

### 离线降噪 (WAV 文件)

```bash
# 编译
gcc -O2 -Iinclude main.c src/binary_loader.c src/cnn_m5_forward.c src/fir_filter.c -lm -o main.exe

# 运行
./main.exe "path/to/noise.wav"

# 输出: anti_out.wav (S=2ch 反噪声), error_out.wav (E=3ch 残差)
```

### 实时降噪 (WASAPI 麦克风 → 扬声器)

```bash
# 编译 (需 -lole32 链接 COM)
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 \
    main_realtime.c src/wasapi_io.c src/binary_loader.c \
    src/cnn_m5_forward.c src/fir_filter.c \
    -lm -lole32 -o gfanc_realtime.exe

# 运行
./gfanc_realtime.exe

# Ctrl+C 停止
```

### Makefile

```bash
make          # 编译离线版 main.exe
make realtime # 编译实时版 gfanc_realtime.exe
make all      # 两个都编译
make clean    # 清理
```

## 系统参数

| 参数 | 值 | 说明 |
|------|----|------|
| fs (内部) | 16000 Hz | ANC 算法采样率 |
| fs (硬件) | 48000 Hz | WASAPI 捕获/播放采样率 |
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
| WASAPI Latency | 10 ms | 麦克风→处理→扬声器延迟 |

## 硬件拓扑 (实时模式)

```
输入:  YDM6MIC 麦克风阵列, 48kHz, 6ch
       ch0 = 参考麦 (噪声源头侧)
       ch1 = 误差麦 Mic1 (人耳位置)
       ch2 = 误差麦 Mic2
       ch3 = 误差麦 Mic3
       ch4-5 = 未使用

输出:  USB Audio Device 扬声器, 48kHz, 2ch
       ch0 = 扬声器 SPK0
       ch1 = 扬声器 SPK1

处理:  48kHz → 重采样到 16kHz → ANC → 反噪声 → 重采样到 48kHz
```

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│  慢速环路: CNN 场景辨识 + Blend 构造 Wc  (每秒执行一次)          │
│                                                                  │
│  audio (全频 / 实时为参考麦信号)                                  │
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
│  前馈环路: 实时宽带降噪  (逐样本 62.5μs @ 16kHz)                  │
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
│      │               │              │ anti_est[e] =     │       │
│      │               │              │ Σ Wc[s,k]×Xd[e,s,k]│      │
│      │               │              └────────┬─────────┘       │
│      │               │                       │                  │
│      │    ┌──────────┘                       │                  │
│      │    │  err[e] = mic_err_filt[e]        │                  │
│      │    │           + anti_est[e]          │                  │
│      │    │  (离线: Dis[e] = Pri⊗ref_filt)   │                  │
│      │    │                                  │                  │
│      │    │   ┌──────────────────────────────┘                  │
│      │    │   ▼                                                │
│      │    │  ┌──────────────────────┐                          │
│      │    │  │  FxNLMS 梯度更新      │                          │
│      │    │  │  grad[s,k] =         │                          │
│      │    │  │    Σ_e err[e]×Xd[e,s,k]                         │
│      │    │  │  power[s] = mean(Xd²) │                          │
│      │    │  │  Wc[s,k] -= μ×grad/pwr│                          │
│      │    │  │  Wc[s,k] *= 1-μ×leak  │                          │
│      │    │  └──────────┬───────────┘                          │
│      │    │             │                                      │
│      │    │        Wc 微调 (跨秒持续自适应)                      │
│      │    │                                                    │
│      │    └──→ ┌──────────────────────────┐                     │
│      │         │ ② 反噪声 FIR              │  1024 tap          │
│      │         │ Wc ⊗ Xd                   │  S=2 通道          │
│      │         │ → anti_spk[s] (反噪声)    │                    │
│      │         └──────────────────────────┘                     │
│      │                       │                                  │
│      ▼                       ▼                                  │
│  ┌──────────────────────────────────────┐                       │
│  │  实时模式: WASAPI 渲染 (S=2ch, 48kHz) │                       │
│  │  离线模式: anti_out.wav / error_out   │                       │
│  └──────────────────────────────────────┘                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

   注: 反馈环路 (IIR 20-200Hz) 未启用 (FB_GAIN=0)
   限幅 ±0.5、DAC 为硬件部署步骤，离线仿真不需要
```

### 离线 vs 实时 差异

| | 离线 (main.c) | 实时 (main_realtime.c) |
|---|---|---|
| 输入 | WAV 文件 | WASAPI 麦克风 (YDM6MIC, 48kHz) |
| 输出 | WAV 文件 | WASAPI 扬声器 (USB Audio, 48kHz) |
| Dis (扰动) | Pri ⊗ ref_filt (仿真) | 实际误差麦信号 (带通后) |
| 重采样 | 输入一次性 44.1k→16k | 流式 48k→16k→48k |
| CNN | 全频 WAV 数据 | 参考麦信号 (带通滤波) |
| 退出 | 处理完自动退出 | Ctrl+C 退出 |

## 输出解读 (离线模式)

```
 Sec |              Top-3 Scenes |   dB(Band) |   dB(Full) | Action
   1 |      0:0.33,6:0.26,3:0.10 |    14.47 dB |    16.23 dB | INIT
```

- **Top-3 Scenes**: `场景ID:softmax概率` 前三名
- **dB(Band)**: 带内降噪量 = 10·log₁₀(P_dis/P_err)，20-1500Hz 评估（**主要指标**）
- **dB(Full)**: 全频参考，含 1500Hz+ 带外能量（仅对比用）
- **Action**: `INIT`=首秒初始化，`RESET`=场景切换触发 CrossFader，`-`=无切换

## 数据文件

运行时从 `data/` 加载 `.bin` 文件（由 `export/export_bin.py` 从 Python 模型导出）：

| 文件 | 内容 | 大小 (float32) |
|------|------|---------------|
| `secondary_path.bin` | 次级路径 IR [E,S,Len] | 6144 |
| `primary_path.bin` | 初级路径 IR [E,R,Len] | 6144 |
| `sub_filters.bin` | 子滤波器 Wc_v [C,S,Len] | 30720 |
| `scene_defs.bin` | 场景质心 [K,S×C] | 240 |
| `bandpass_fir.bin` | 带通 FIR 系数 [1024] | 1024 |
| `cnn_*.bin` | CNN 权重 (stem/res0-3/linear) | ~50 文件 |

## C 与 Python 的对应关系

| C 文件 | Python 对应 | 功能 |
|--------|------------|------|
| `main.c` | `notebooks/Main_GFANC_Realtime.ipynb` | 离线: WAV I/O + 逐秒 CNN + 逐样本 FxNLMS |
| `main_realtime.c` | — | 实时: WASAPI I/O + 重采样 + ANC |
| `src/cnn_m5_forward.c` | `gfanc/Network.py` | m5_scene CNN (64ch, 4 ResBlock) |
| `src/scene_controller.c` | `gfanc/SceneController.py` | CNN → Blend → construct_wc |
| `src/fxnlms_mimo.c` | `gfanc/Combine_GFANC_with_FxNLMS_MIMO.py` | MIMO FxNLMS 算法 |
| `src/fir_filter.c` | `scipy.signal.lfilter` | FIR 滤波器 (double 精度延迟线) |
| `src/cross_fader.c` | `gfanc/cross_fader.py` | Wc 系数交叉淡化 |
| `src/wasapi_io.c` | `sounddevice.Stream` | WASAPI 捕获/渲染 |
| `export/export_bin.py` | — | PyTorch/NumPy → C .bin 导出 |

## 已验证精度

离线模式与 Python 参考实现对比（`mixed_7types_56s.wav`, 56 秒）：

| 指标 | C | Python | 差异 |
|------|---|--------|------|
| dB(Band) 平均 | 15.00 | 15.00 | 0.00 |
| 场景分类 probs | 逐秒一致 | — | 0 |
| Dis RMS (带内) | 1.6262 | 1.6262 | 0 |
| Dis RMS (全频) | 1.9914 | 1.9914 | 0 |
| Fx RMS | 0.5528 | 0.5528 | 0 |

## 编译环境

- **离线版**: GCC/MinGW 或 MSVC, 仅需 `-lm`
- **实时版**: GCC/MinGW, 需 `-lm -lole32`, Windows Vista+
- Python 导出: `pip install torch numpy scipy`
