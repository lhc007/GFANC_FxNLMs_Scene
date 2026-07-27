# GFANC FxNLMS — MIMO 主动降噪系统

一个**主动降噪引擎**的纯 C 语言实现，从 Python 项目 [GFANC_Scene](../GFANC_Scene) 移植。

它会"听"到噪声，然后计算一个**反噪声**（与噪声波形相反的声音），通过扬声器播放出来，让噪声和反噪声在空间中互相抵消——就像噪声从未存在过一样。

## 它能做什么

| 模式 | 说明 |
|------|------|
| **离线降噪** | 输入一段噪声录音（WAV 文件），输出降噪后的结果 |
| **实时降噪** | 连接麦克风和扬声器，实时抵消环境噪声 |

## 你需要什么

### 硬件（实时模式）

- **音频接口**：多通道声卡（4in/2out+，ASIO/WASAPI/WDM-KS 均可）
- **麦克风**：参考麦 ×1 + 误差麦 ×3
- **扬声器**：2 声道
- **电脑**：Windows 10/11

> 当前使用单设备 ASIO 声卡（共时钟驱动所有 ADC/DAC），时钟同步问题已解决。
> 反馈抵消需运行 `calibrate_feedback.exe` 逐扬声器校准。

**噪声源必须同时覆盖参考麦克风和误差麦克风**——ANC 只能抵消两个位置都能"听到"的噪声。参考麦朝向噪声源，误差麦和扬声器朝向听音区。系统总长约 50cm，建议安装在窗户开口处（参考麦朝向窗外，误差麦+扬声器朝向室内）。

### 软件

- **编译器**：GCC（通过 [MSYS2](https://www.msys2.org/) 安装）
- **权重文件**：`data/` 目录下的 `.bin` 文件（已包含在项目中）

## 快速开始

### 1. 下载项目

```bash
git clone https://github.com/lhc007/GFANC_FxNLMs_Scene.git
cd GFANC_FxNLMs_Scene
```

项目目录中的 `data/` 文件夹包含了训练好的模型权重，不需要额外下载。

### 2. 导出权重（仅需做过训练后执行）

如果你在 Python 项目 [GFANC_Scene](../GFANC_Scene) 中重新训练了 CNN 模型或子滤波器，需要重新导出为 C 可用的 `.bin` 文件：

```bash
pip install numpy scipy torch   # 一次性依赖
python export/export_bin.py     # 读取 GFANC_Scene 的模型和路径，写入 data/
```

默认自动查找同级目录的 `GFANC_Scene`，也可以手动指定：

```bash
set GFANC_PYTHON_PROJ=D:\你的路径\GFANC_Scene
python export/export_bin.py
```

导出内容：CNN 权重（58 个 `.bin`）、子滤波器、场景 centroids、主/次路径、带通 FIR、配置 JSON。

### 3. 编译

打开终端（PowerShell 或 Git Bash），在项目目录下执行：

**离线版**（处理 WAV 文件）：
```bash
gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c -lm -o main.exe
```

**实时版**（麦克风 → 扬声器）：
```bash
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 main_realtime.c src/scene_controller.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c src/howling_detect.c src/pa_loader.c -lm -lole32 -o gfanc_realtime.exe
```

需要 `libportaudio64bit-asio.dll` 在同目录（项目自带）。

也可以用 `make`：
```bash
make          # 编译离线版
make realtime # 编译实时版
make all      # 两个都编译
make clean    # 清理
```

### 4. 运行

**离线版** — 处理一段噪声录音：
```bash
./main.exe "Noise Examples/mixed_7types_56s.wav"
```

运行后会生成两个文件：
- `anti_out.wav` — 反噪声信号（2 声道，这是播放到扬声器的声音）
- `error_out.wav` — 残差信号（3 声道，降噪后剩余的声音）

**实时版** — 实时抵消环境噪声：
```bash
./gfanc_realtime.exe
```

运行后会列出音频设备，输入麦克风和扬声器的设备编号（如 `23`），然后开始实时降噪。按 `Ctrl+C` 停止。

## 运行示例

```
PS D:\VSCodeRepository\GFANC_FxNLMs_Scene> ./main.exe "Noise Examples/road_noise-15.wav"
Loading weights...
  OK: sec=6144 pri=6144 sub=30720 bp=1024 L=1024
  CNN loaded.
  System ready.

Input: 16000 Hz, 1 ch, 240000 samples (15.0s)

 Sec |              Top-3 Scenes |   dB(Band) |   dB(Full) | Action
-------------------------------------------------------------------------------------
[Diag] Wc_new RMS: 0.0346
[Diag] Dis (带内) RMS: 1.6262
   1 |      0:0.33,6:0.26,3:0.10 |    14.44 dB |    16.20 dB | INIT
   2 |      0:0.36,6:0.26,3:0.10 |    15.18 dB |    16.69 dB | -
   3 |      0:0.34,6:0.25,3:0.10 |    15.10 dB |    16.81 dB | -
  ...
  15 |      0:0.32,3:0.28,7:0.19 |    15.89 dB |    17.89 dB | -
-------------------------------------------------------------------------------------
  Avg |                           |    15.00 dB |    16.52 dB |

Processing: 6.1s for 15.0s audio (2.5x)
Output: anti_out.wav (2 ch), error_out.wav (3 ch)
Done.
```

## 效果解读

运行后会看到一张表格，每秒一行：

| 列 | 含义 | 举例 |
|----|------|------|
| `Sec` | 第几秒 | `1` |
| `Top-3 Scenes` | AI 识别出的噪声类型（及其置信度） | `0:0.33` = 场景 0 置信度 33% |
| `dB(Band)` | **实际降噪量**（数字越大越好） | `14.44 dB` = 噪声被压低了 14.44 分贝 |
| `dB(Full)` | 全频段参考值（包含不可控的高频） | `16.20 dB` |
| `Action` | 状态 | `INIT`(启动) / `-`(正常) / `RESET`(噪声类型变了，切换策略) |

- **dB(Band) 是最重要的指标**：它告诉你 20-1500Hz 范围内的噪声被压了多少
- 10 dB 意味着噪声能量降到原来的 1/10，20 dB 意味着降到 1/100
- 这个系统在测试文件上平均达到 **15 dB**（噪声能量降到约 1/30）

## 项目结构

```
GFANC_FxNLMs_Scene/
│
├── README.md              你正在看的文件
├── Makefile               编译脚本
│
├── main.c                 【离线降噪】主程序 — WAV 文件输入/输出
├── main_realtime.c        【实时降噪】主程序 — 麦克风输入/扬声器输出
│
├── include/               头文件（API 定义）
│   ├── gfanc_types.h      基础类型（FIR 滤波器）
│   ├── fir_filter.h       FIR 滤波器
│   ├── scene_controller.h CNN 场景识别 + 滤波器构造
│   ├── fxnlms_mimo.h      自适应降噪算法（离线+实时双路径）
│   ├── howling_detect.h   啸叫检测 + IIR 陷波
│   ├── binary_loader.h    模型加载器
│   └── pa_loader.h        PortAudio DLL 共享加载层
│
├── src/                   源代码（实现）
│   ├── fir_filter.c       FIR 滤波器（双段循环, 零取模）
│   ├── scene_controller.c AI 场景识别 + 控制滤波器构造
│   ├── fxnlms_mimo.c      自适应降噪核心（离线仿真+实时双路径）
│   ├── cnn_m5_forward.c   神经网络推理（静态缓冲）
│   ├── howling_detect.c   啸叫 DFT 检测 + IIR 陷波（逐扬声器独立状态）
│   ├── binary_loader.c    从文件加载模型参数
│   ├── pa_loader.c        PortAudio 运行时 DLL 加载
│   └── calibrate_feedback.c  反馈路径校准（逐扬声器, 16k ZOH×3）
│
├── data/                  模型参数文件（运行时加载）
│   ├── *.bin              二进制权重（滤波器系数、神经网络权重）
│   └── ...
│
├── docs/                  文档
│   ├── COMPREHENSIVE_REVIEW.md  综合审查报告（架构·算法·问题追踪）
│   ├── UPGRADE_ROADMAP.md 升级路线图
│   └── micphone.md        麦克风数据手册
│
└── export/                工具脚本（Python → C 格式转换）
    └── export_bin.py      导出为 .bin 文件
```

## 系统架构

系统由两个"环路"组成，协同工作：

### 慢速环路（每秒执行一次）— "大脑"

CNN 神经网络每秒分析一次 1 秒窗口的噪声，识别场景类型（4 种），混合 15 个预训练子滤波器构造控制滤波器 Wc。双缓冲机制确保零样本丢失。

```
噪声 → 带通(20-1500Hz) → CNN(M5, 4类) → Blend(15子滤波器) → Wc(1024tap)
                                    ↑ 1Hz, 双缓冲+原子交接
```

场景识别为 1Hz 是正确设计——噪声类型变化是秒级到分钟级的，1 秒窗口保证足够的频率分辨率。场景切换时 CrossFader 在 100ms 内平滑过渡。

### 前馈环路（每秒 16000 次）— "肌肉"

F-A 修复后，实时和离线使用独立路径，互不影响：

**实时路径**（`fxnlms_tick_rt`）：
```
ref → bp_fir → x_ref ─┬─→ [Wc ⊗ x_ref] → anti (物理扬声器输出)
                      │
                      └─→ Ŝ ⊗ ref → Fx → 梯度更新
                                         ↑
                           err_meas = bp(err_mic) (实测误差直驱)
```

**离线路径**（`fxnlms_tick`，保留）：
```
ref → bp_fir → Ŝ ⊗ ref → Fx → anti = Wc ⊗ Fx (Ŝ域, 仅写WAV)
              Pri(ref) → Dis → err = Dis + anti → 梯度
```

关键区别：实时 anti 输出是 `Wc ⊗ ref`（直接卷积带通参考），梯度用实测误差麦信号直接驱动；离线 anti 是 `Wc ⊗ (Ŝ ⊗ ref)`（经模型滤波），用于 WAV 仿真评估。

### 三层架构

```
┌─ 慢速环路 (1Hz, 主线程) ─────────────────────────────────────┐
│                                                               │
│  ref → bp_fir(1024tap) → cnn_buf[2][16000] 双缓冲            │
│    │                         │                                │
│    │                    CNN M5 (4类)                          │
│    │                      ↓ softmax                          │
│    │                    Blend: centroid[4][30] softmax加权     │
│    │                      ↓                                  │
│    │                    Wc = Σ blend[c]×sub_filter[c]         │
│    │                    RMS对齐 stub_rms, 取反                │
│    │                      ↓                                  │
│    │                    滞回检测 (cos<0.8)                    │
│    │                      ↓ 切换                             │
│    │                    CrossFader 100ms → Wc[2048]           │
│    │                                                         │
├─ 前馈环路 (16kHz, 音频回调) ─────────────────────────────────┤
│                                                               │
│  ref_filt → x_hist[1024] → anti = Wc ⊗ x_ref (直接卷积)      │
│    │            ↓                          ↓                  │
│    │         [Wc⊗ref]                  物理扬声器输出          │
│    │                                                         │
│    └→ sec_firs[6] (Ŝ,1040tap) → Xd[E×S×L]                   │
│                                     ↓                        │
│                              power = ΣXd²/(E·L)              │
│                                     ↓                        │
│  err_meas = bp(err_mic) ──→ ΔWc = -μ·err_meas·Xd/power       │
│                              (per-sample LMS)                 │
│                              + leak (1e-6)                    │
│                              + freeze_lms (max|Wc|>5×stub)    │
│                                                               │
├─ 反馈环路 (规划中, B-1) ─────────────────────────────────────┤
│                                                               │
│  err_mic → IIR 8阶(20-200Hz) → anti_fb[s]                    │
│  (固定系数, 出厂烧录)                                         │
│                                                               │
├─ 辅助 ───────────────────────────────────────────────────────┤
│                                                               │
│  反馈抵消: fb_fir[2] FIR(256tap) 逐扬声器校准                  │
│  啸叫检测: DFT 256pt + IIR notch ×2, 15dB阈值                 │
│  anti_total = anti_ff + anti_fb → 限幅±1.0 → ZOH×3 → DAC     │
│                                                               │
└──────────────────────────────────────────────────────────────┘
## 反馈路径校准

反馈抵消功能需逐扬声器校准声学路径。**只需在麦克风或扬声器位置变化时重新校准**。

### 校准步骤

```bash
# 编译校准程序（只需一次）
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 src/calibrate_feedback.c src/fir_filter.c src/binary_loader.c src/pa_loader.c -lm -lole32 -o calibrate_feedback.exe

# 运行校准（自动逐扬声器两轮）
./calibrate_feedback.exe
```

保持安静 4 秒×2 轮，自动生成 `data/feedback_path_0.bin` 和 `data/feedback_path_1.bin`。

校准完成后运行 `./gfanc_realtime.exe`，日志显示：
```
Feedback spk0: 256 taps, RMS=0.0005
Feedback spk1: 256 taps, RMS=0.0006
```

文件缺失时反馈抵消自动禁用，不影响降噪。

## 啸叫检测

系统内置 DFT 频谱检测 + IIR 陷波滤波器。当检测到持续窄带峰值（啸叫特征）时，
自动在输出端施加陷波器，打断反馈环路。运行时状态行会显示啸叫检测信息：

```
HW:  f=850Hz peak=18.2dB notches=1 [NOTCH]   ← 检测到 850Hz 啸叫, 已陷波
```

## 系统参数

| 参数 | 值 | 说明 |
|------|----|------|
| 内部采样率 | 16000 Hz | ANC 处理速率 |
| 硬件采样率 | 48000 Hz | ASIO 声卡采样率（3:1 抽取/内插） |
| 误差麦克风 (E) | 3 | 放在降噪目标位置 |
| 扬声器 (S) | 2 | 播放反噪声 |
| 子滤波器 (C) | 15 | 预训练滤波器, 按场景混合 |
| 场景类型 (K) | 4 | CNN 可识别的噪声环境数 (运行时从数据推导) |
| 滤波器长度 (L) | 1024 tap | 控制滤波器 Wc, 频域分辨率 ~15.6Hz |
| 带通频率 | 20-1500 Hz | ANC 有效频率范围 |
| 输入预增益 | 10x (+20dB) | MIC_PRE_GAIN, 可调 |
| 步长 (μ) | 0.0001 | LMS 自适应步长 |
| 泄漏因子 | 1e-6 (~1.5%/秒) | Wc 正则化, 与 step_size 解耦 |
| 输出限幅 | ±1.0 | DAC 满幅保护 + NaN/Inf 防护 |
| 场景切换阈值 | 余弦相似度 < 0.8 | 触发场景切换 |
| 切换过渡 | 1600 样本 (100ms) | CrossFader, =20Hz×2 周期 |
| 冷启动 ramp | 400ms | 输出从 0 平滑渐入 |
| Wc 发散阈值 | max\|Wc\| > 5×stub_rms | 自动冻结 LMS 梯度 |
| 反馈 FIR | 256 tap ×2 扬声器 | 逐扬声器独立校准 |
| 啸叫陷波 | DFT 256pt, IIR ×2 | 15dB 峰均值阈值, 逐扬声器独立状态 |
| CNN 推理 | ~8ms/次 @1Hz | 静态缓冲, 无动态分配 |
| 回调预算 | ~30-45% (SIMD ~5-10%) | 已优化: 双段循环零取模, 含安全边际 |

## 代码结构

C 实现已超越原始 Python 参考（新增实时 ASIO 音频栈、啸叫检测、反馈抵消、发散保护、双缓冲等模块），是独立的工程实现。

| 文件 | 功能 |
|------|------|
| `main.c` | 离线降噪: WAV 输入/输出, `fxnlms_tick` 仿真路径 |
| `main_realtime.c` | 实时降噪: ASIO 音频, `fxnlms_tick_rt` 实时路径, 场景状态机 |
| `src/scene_controller.c` | CNN M5 推理 → softmax → max 归一化 Blend → Wc 构造 (RMS 对齐) |
| `src/fxnlms_mimo.c` | FxNLMS 自适应 (离线+实时双路径, Wc 发散冻结) |
| `src/cnn_m5_forward.c` | M5 CNN 前向推理 (静态缓冲, 1Hz) |
| `src/fir_filter.c` | FIR 滤波器 (双段线性循环, 零取模) |
| `src/howling_detect.c` | DFT 频谱峰值检测 + IIR 双二阶陷波 (逐扬声器独立状态) |
| `src/pa_loader.c` | PortAudio ASIO DLL 运行时加载 |
| `src/calibrate_feedback.c` | 反馈路径 NLMS 校准 (逐扬声器, 16k ZOH×3 激励) |
| `src/binary_loader.c` | .bin 二进制权重文件加载 |

## 离线验证

离线模式 (`main.exe`) 在 56 秒混合噪声测试文件上：

| 指标 | 结果 |
|------|------|
| 平均降噪量 (dB) | **15.00** |
| 场景识别 | 4 类, 每秒 1 次 |
| 处理速度 | 2.5× 实时 (15s 音频 6.1s 完成) |

实时模式在 50cm 窗户开口 + ASIO 声卡上实测 NR 4-9dB（稳态噪声），取决于噪声源与参考/误差麦的声学耦合。

## 常见问题

**Q: 为什么 error_out.wav 听起来比原噪声还大？**
A: error_out.wav 是 3 声道文件（对应 3 个麦克风位置），播放器同时播 3 个声道叠加后音量更大。另外，ANC 只在 20-1500Hz 有效，高频部分反而增加了少量能量。降噪效果要看 dB(Band) 数字，不要用耳朵直接听 error_out.wav。

**Q: 实时模式怎么验证效果？**
A: 终端每秒输出 NR(dB)、err/anti RMS、啸叫状态。NR > 3dB 表示有效降噪。

**Q: 可以处理其他采样率的文件吗？**
A: 离线模式自动将输入重采样到 16000 Hz。支持 16-bit PCM WAV。

**Q: 实时版使用什么音频 API？**
A: PortAudio 运行时加载 (`libportaudio64bit-asio.dll`)，支持 ASIO / WASAPI / WDM-KS 后端，通过 `src/pa_loader.c` 动态加载 DLL。
