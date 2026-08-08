# GFANC FxNLMS — MIMO 主动降噪系统

> **版本**: v1.6 (2026-08-08) | **分支**: gfanc-direct-weight

> **v1.6 变更**: C 运行时改为**直接权重 Wc 生产者**（CNN 回归 30 维子带增益 → `tanh` → `Wc=Σ gain·sub`，彻底去掉 centroid/softmax 场景路径与 `scene_defs.bin` 依赖）；**死代码清理**（删除 OCG 聚类闸门 `ocg.c/ocg.h`、`scene_manager.h` 死函数、`test/` 脚手架、`GFANC_OCG_*` 参数）。v1.5 起已去掉场景层（无场景记忆/滞回/OCG）→ **Reset / Continuous 双模式**（CNN 只产 Wc，`gfanc_mode` 切换）；离线默认按**嵌入式处理延迟 3ms** 建模因果性（`GFANC_EMBED_DELAY_MS`）。

一个**主动降噪引擎**的纯 C 语言实现，从 Python 项目 [GFANC_Scene](GFANC_Scene) 移植。

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

**噪声源必须同时覆盖参考麦克风和误差麦克风**——ANC 只能抵消两个位置都能"听到"的噪声。参考麦朝向噪声源，误差麦和扬声器朝向听音区。系统几何：**参考麦↔误差麦 64cm、扬声器↔误差麦 15cm**，建议安装在窗户开口处（参考麦朝向窗外，误差麦+扬声器朝向室内）。

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

导出内容：CNN 权重（58 个 `.bin`）、子滤波器、主/次路径、带通 FIR、配置 JSON。**直接权重模式（v1.6）跳过场景 centroids**（`scene_defs.bin` 不再生成/加载）。

### 3. 编译

打开终端（PowerShell 或 Git Bash），在项目目录下执行：

**离线版**（处理 WAV 文件）：
```bash
gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c -lm -o main.exe
```

**实时版**（麦克风 → 扬声器）：
```bash
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 main_realtime.c src/scene_controller.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c src/howling_detect.c src/sec_online.c src/pa_loader.c -lm -lole32 -o gfanc_realtime.exe
```

需要 `libportaudio64bit-asio.dll` 在同目录（项目自带）。


### 4. 校准反馈路径（实时模式首次运行前必须执行）

扬声器的反噪声会通过空气耦合回参考麦克风，形成正反馈（啸叫）。校准程序会测量这条声学路径并生成抵消滤波器：

```bash
# 编译校准程序（只需一次）
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 src/calibrate_feedback.c src/fir_filter.c src/binary_loader.c src/pa_loader.c -lm -lole32 -o calibrate_feedback.exe

# 运行校准（自动逐扬声器两轮，每轮 4 秒）
./calibrate_feedback.exe
```

选择你的 ASIO 设备（如 `23`），保持房间安静，程序会自动在扬声器 0 和 1 上播放白噪声，用 NLMS 辨识反馈路径。完成后生成 `data/feedback_path_0.bin` 和 `data/feedback_path_1.bin`。

> **只在麦克风或扬声器位置变化时需要重新校准**。文件缺失时反馈抵消自动禁用，不影响降噪效果但可能引发啸叫。

> 💡 **首次运行前还需测环路延迟**（FxLMS 对齐必需）：
> ```bash
> gcc -O2 -Iinclude src/calibrate_secondary.c -lm -o calibrate_secondary.exe
> ./calibrate_secondary.exe   # 生成 data/sec_bulk_delay.bin (环路延迟) + secondary_path_measured.bin
> ```
> 运行时自动加载最新的实测 Ŝ 并补偿环路延迟（启动日志 `Loop delay auto-loaded` / `Ŝ model delay`）。**每次换安装位置/几何后都要重测**（详见 [次级路径测量](#次级路径测量python-farina-扫频法)）。

> 📏 **校准质量规则**：校准时声卡输入 **SIG 常亮、CLIP 不亮**。CLIP 亮 = 输入削波，会污染路径辨识。
> 探针响度由 `GFANC_CAL_NOISE` 控制（默认 0.9，反馈/次级两个校准程序都支持）：削波就调小（如 0.4），SNR 不足就调大。
> ERLE < 8dB 的弱耦合路径会被自动置零（运行时忽略该扬声器→误差麦耦合），属正常保护。

### 5. 运行

**离线版** — 处理一段噪声录音：
```bash
./main.exe "Noise Examples/mixed_7types_56s.wav"

./main.exe "Noise Examples/road_noise_0-34.wav"

./main.exe "Noise Examples/road_noise-15.wav"
```

运行后会生成两个文件：
- `anti_out.wav` — 反噪声信号（2 声道，这是播放到扬声器的声音）
- `error_out.wav` — 残差信号（3 声道，降噪后剩余的声音）

**实时版** — 实时抵消环境噪声（默认参数即可，自动增益）：
```bash
./gfanc_realtime.exe
```

关键操作要点：

- **模拟增益旋钮是灵敏度关键**：ECM8000 弱信号，输入增益旋钮要调到能清晰收音（启动日志 `refFilt≈0.03`、无「输入电平过低」警告）。**校准与运行必须用同一旋钮位置**（路径系数嵌入模拟增益）。
- **误差麦克风 = 安静区目标，不是测试拾音器**：在误差麦旁说话/拍手（参考麦预测不到的突发）会触发静音保护，属正常。噪声源应在参考麦上游（保持「参考麦 → 误差麦」的前馈几何）。
- **啸叫陷波（~125Hz）**：扬声器→误差麦反馈会在 ~125Hz 形成临界啸叫，由啸叫检测陷波压制。**不要调高 `GFANC_HW_THRESH`（默认 12）** —— 调高会放开反馈，导致周期性「收敛→爆炸→静音」循环。
- **几何限制**：ANC 带通已砍 256→64tap（群延迟 8→2ms），控制路径 ≈ 10.8ms（带通 2ms + ASIO 8.4ms + 声学 0.4ms）vs 参考→误差预览 ~1.9ms → **净预览 ≈ −9ms**（原 256tap −18.5ms，详见 [窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）→ 只能实时消 <~55Hz 宽带 + 周期/窄带成分（实测 NR 约 4-6dB）。要更高需拉大参考麦与误差麦距离，或降声卡缓冲。
- 可选调参：`GFANC_MIC_GAIN`（输入预增益）、`GFANC_STEP`、`GFANC_WC_TARGET` 等见[系统参数表](#系统参数)。

运行后会列出音频设备，输入麦克风和扬声器的设备编号（如 `23`），然后开始实时降噪。按 `Ctrl+C` 停止。

## 运行示例

```
PS D:\VSCodeRepository\GFANC_FxNLMs_Scene> ./main.exe "Noise Examples/road_noise-15.wav"
Loading weights...
  BP ANC: bandpass_anc.bin (64tap, gd=2.0ms)
  OK: sec=6144 pri=6144 sub=30720 bp=1024 L=1024
  CNN loaded.
  System ready (CNN loaded).

Input: 44100 Hz, 1 ch, 661500 samples (15.0s)
Resampled: 44100 Hz -> 16000 Hz (240000 samples)

 Sec | Band | NR_est | NR_true |    err | refFilt |   anti | Note
-------------------------------------------------------------------------------------
   1 |   10 |    0.3 |    0.3 | 0.255 | 0.0098 | 0.0086 | INIT [FxRMS=0.0098]
   2 |   10 |    0.5 |    0.5 | 0.272 | 0.0099 | 0.0084 | -
   3 |   10 |    0.8 |    0.8 | 0.296 | 0.0142 | 0.0139 | -
  ...
  15 |   10 |    3.0 |    2.9 | 0.315 | 0.0144 | 0.0417 | -
-------------------------------------------------------------------------------------
  Avg |                           |        |    1.8 |

Processing: 7.6s for 15.0s audio (2.0x)
Output: anti_out.wav (2 ch), error_out.wav (3 ch)
Done.
```

> 上为**默认参数**（step=1e-7）的 15s 文件结果，NR 受收敛速度限制。用 `GFANC_STEP=1e-5` 可加快收敛到 ~13-15dB（见 [离线验证](#离线验证) 参数速查）。

## 效果解读

运行后会看到一张表格，每秒一行：

| 列 | 含义 | 举例 |
|----|------|------|
| `Sec` | 第几秒 | `1` |
| `Band` | CNN 直接权重增益的 argmax \|gain\| 带索引（0..29 = 扬声器×子带，仅诊断，不参与 Wc 切换） | `10` |
| `NR_est` | **估计降噪量**（与实时版同公式，数字越大越好） | `4.6 dB` = 估计压低了 4.6 分贝 |
| `NR_true` | **已知真值降噪量**（仅离线可用，Pri 模型精确计算扰动，最可信） | `4.4 dB` |
| `err` / `refFilt` / `anti` | 残差 / 带通参考 / 反噪声 RMS | |
| `Note` | 状态 | `INIT`(启动) / `-`(正常) / `RESET`(reset 模式 cos<阈值 → 重置 Wc) |

- **NR_true 是最可信的指标**（离线用 Pri 模型精确算出扰动，NR_est 与它逐秒偏差 <0.5dB）；10 dB 意味着噪声能量降到 1/10，20 dB 意味着降到 1/100
- 注意：**离线 NR_true 当前按嵌入式处理延迟 3ms 建模因果性**（默认 `GFANC_EMBED_DELAY_MS=3`，启动日志打印净预览时间），不再是"无因果缺口"的理想上限；实时还受 PC 控制路径延迟限制（净预览 ≈ −9ms，见下文"离线验证"）

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
│   ├── scene_controller.h CNN 直接权重 Wc 生产者
│   ├── fxnlms_mimo.h      自适应降噪算法（离线+实时双路径）
│   ├── howling_detect.h   啸叫检测 + IIR 陷波
│   ├── binary_loader.h    模型加载器
│   └── pa_loader.h        PortAudio DLL 共享加载层
│
├── src/                   源代码（实现）
│   ├── fir_filter.c       FIR 滤波器（双段循环, 零取模）
│   ├── scene_controller.c CNN 直接权重 Wc 构造 (tanh 增益)
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
│   ├── GFANC_综合审查报告_合并版.md  综合审查（唯一审查文档，七段式）
│   ├── micphone.md        麦克风数据手册
│   └── 2026-07-28_硬件调试记录.md  UMC404HD 面板操作指引 + 调试记录
│
└── export/                工具脚本（Python → C 格式转换）
    └── export_bin.py      导出为 .bin 文件
```

## 系统架构

系统由两个"环路"组成，协同工作：

### 慢速环路（每秒执行一次）— "大脑"

CNN 神经网络每秒分析一次 1 秒窗口的噪声，回归 **30 维子带增益**（2 扬声器 × 15 子带，`tanh` → [-1,1] 带符号），并直接构造控制滤波器 Wc = Σ 增益 × 子滤波器（v1.6 直接权重，替换旧 K=3 场景 CNN + centroid blend）。双缓冲机制确保零样本丢失。

```
噪声 → 带通(20-1500Hz) → CNN(M5, 30维回归) → tanh 增益 → Wc=Σ gain×sub → Wc(1024tap)
                                                    ↑ 1Hz, 双缓冲+原子交接
```

**去场景层双模式**（`gfanc_mode`，v1.5）——CNN 不再做"场景切换"（无场景记忆/滞回/OCG），只产 Wc：

| 模式 | CNN 行为 | Wc 行为 |
|---|---|---|
| **reset**（默认） | 每秒跑，输出新 30 维增益 + 候选 wc_cur | **OCG 多质心聚类闸门**（v1.7，ICASSP 2026）：对增益向量做在线聚类，仅**簇索引变化**才 → CrossFader 100ms 平滑过渡到新 Wc（`GFANC_OCG=0` 回退 `cos(anchor,cur)<0.8`） |
| **continuous** | 每秒跑（仅诊断日志） | 仅首秒 INIT 设一次，FxNLMS 永不重置 |

噪声类型变化是秒级到分钟级的，1 秒窗口保证足够的频率分辨率。reset 模式触发时 CrossFader 在 100ms 内平滑过渡，防止可闻瞬态。

**Reset 判定粒度：整向量 vs 逐扬声器（设计决策记录）**

本方案 reset 判定采用**整向量 30 维 cos**（`cos(anchor_gains, cur_gains) < 阈值`），与 MIMO_GFANC 的**逐扬声器独立判定**（K=3 softmax，每扬声器各自阈值 0.7）不同。区别与权衡：

| 维度 | 整向量 30 维 cos（本方案） | 逐扬声器独立判定（MIMO_GFANC） |
|---|---|---|
| 判定单元 | 2×15 增益向量整体算一个 cos | 每扬声器按各自子带增益单独判定 |
| 对噪声切换敏感度 | 全场切换（大部分维度同变）→ 敏感 | 单扬声器相关变化 → 敏感 |
| 抗抖动 | 高——30 维方向稳定，随机抖动对 cos 影响小 | 低——2~3 维判定易被噪声抖动误触 |
| 同步性 | 两扬声器联合切换，CrossFader 过渡时空间响应一致 | 两扬声器可能先后切换，瞬时空间失衡 |
| 实现 | 与 CNN 整向量输出天然对齐，无需拆分子判定 | 需按扬声器分组子向量分别判定 |

选整向量的理由：
1. **稀释效应（dilution）**：单个子带/扬声器的局部变化被其余维度"稀释"进整向量 cos，变化被平滑——抑制**误重置**（微小扰动不触发）；代价是**漏检**局部显著变化。但真实噪声切换（如马路→直升机）是宽带全场变化，各维度同时漂移、cos 掉幅明显，稀释效应影响有限。MIMO_GFANC 的 2 维逐扬声器判定相反：对局部变化敏感，但噪声敏感度也高（其 softmax 又近恒定 → 实际几乎不触发）。
2. **物理耦合**：两扬声器共享同一参考噪声与误差声场（3 误差麦各收两路叠加），是**联合控制系统**。整向量判定保证两路 Wc 同步过渡，避免一改一不改造成瞬时声场失衡。
3. **实现与语义最简**：CNN 一次前向即整向量 `gain[s*C+c]`，整向量判定无需拆分再聚合。

**已知代价**：无法单独重置某一扬声器——若某一路空间位置噪声独立剧变，会因稀释效应延迟重置（可接受：两扬声器共享噪声源，独立剧变场景罕见）。

### 前馈环路（每秒 16000 次）— "肌肉"

F-A 修复后，实时和离线使用独立路径，互不影响：

**实时路径**（`fxnlms_tick_rt`，ANC 带通 64tap 群延迟 2ms）：
```
ref → bp_anc(64tap) → x_ref ─┬─→ [Wc ⊗ x_ref] → anti (物理扬声器输出)
                              │
                              └─→ Ŝ ⊗ ref → Fx → 梯度更新
                                                 ↑
                                   err_meas = bp(err_mic) (实测误差直驱)
```

**离线路径**（`fxnlms_tick_rt`，与实时同信号链）：
```
ref → bp_anc(64tap) → Ŝ ⊗ ref → Fx → anti = Wc ⊗ Fx (Ŝ域, 仅写WAV)
              Pri(ref) → Dis → err = Dis + anti → 梯度
```

关键区别：实时 anti 输出是 `Wc ⊗ ref`（直接卷积 64tap 带通参考），梯度用实测误差麦信号直接驱动；离线 anti 是 `Wc ⊗ (Ŝ ⊗ ref)`（经模型滤波），用于 WAV 仿真评估。CNN 直接权重保留独立的 1024tap 带通（频率分辨率）。

### 三层架构

```
┌─ 慢速环路 (1Hz, 主线程) ─────────────────────────────────────┐
│                                                               │
│  ref → bp_fir(1024tap) → cnn_buf[2][16000] 双缓冲            │
│    │                         │                                │
│    │          CNN M5 直接权重 (30维回归, 运行时推导)           │
│    │                      ↓ tanh                             │
│    │       gain[30] (S×C, 带符号 [-1,1])                      │
│    │                      ↓                                  │
│    │       Wc = Σ gain[s,c]×sub_filter[(c,s),:]               │
│    │       RMS自动标定 (wc_rms_target), 取反                  │
│    │                      ↓                                  │
│    │        去场景层双模式 (gfanc_mode, v1.5)                 │
│    │         reset: cos(anchor_gains,cur)<0.8 → CrossFader    │
│    │         continuous: 仅首秒INIT, 永不重置                  │
│    │                                                         │
├─ 前馈环路 (16kHz, 音频回调) ─────────────────────────────────┤
│                                                               │
│  ref_filt → x_hist[1024] → anti = Wc ⊗ x_ref (直接卷积)      │
│    │            ↓                          ↓                  │
│    │         [Wc⊗ref]                  物理扬声器输出          │
│    │                                                         │
│    └→ sec_firs[6] (Ŝ,1024+dsp_delay) → Xd[E×S×L]             │
│                                     ↓                        │
│                              power = ΣXd²/(E·L)              │
│                                     ↓                        │
│  err_meas = bp(err_mic) ──→ ΔWc = -μ·err_meas·Xd/power       │
│                              (per-sample LMS)                 │
│                              + leak (5e-7, 自适应)             │
│                              + freeze_lms (max|Wc|>30×wc_init_max) │
│                                                               │
├─ 反馈环路 (规划中, B-1) ─────────────────────────────────────┤
│                                                               │
│  err_mic → IIR 8阶(20-200Hz) → anti_fb[s]                    │
│  (固定系数, 出厂烧录)                                         │
│                                                               │
├─ 辅助 ───────────────────────────────────────────────────────┤
│                                                               │
│  反馈抵消: fb_fir[2] FIR(256tap) 逐扬声器校准                  │
│  啸叫检测: DFT 256pt + IIR notch ×2, 可配阈值 (默认12)        │
│  在线Ŝ辨识: sec_online NLMS, μ=5e-6, 零探测噪声                │
│  冷启动保护: soft-release 前1s cap0.12(梯度冻结) 后1s cap→1.0  │
│  Ŝ环路延迟补偿: 自动加载 sec_bulk_delay.bin (dsp_delay)        │
│  NR指标: 分散采样250点 + ±30dB限幅                             │
│  anti_total = anti_ff + anti_fb → 限幅±1.0 → 线性内插×3 → DAC │
│                                                               │
└──────────────────────────────────────────────────────────────┘
## 反馈路径校准

反馈抵消功能需逐扬声器校准声学路径。详见 [快速开始 - 步骤 4](#4-校准反馈路径实时模式首次运行前必须执行)。

校准完成后运行 `./gfanc_realtime.exe`，启动日志显示：
```
Feedback spk0: 256 taps, RMS=0.0012
Feedback spk1: 256 taps, RMS=0.0011
```

文件缺失时反馈抵消自动禁用，日志显示 `Feedback cancel: disabled`，不影响降噪但可能引发啸叫。

## 次级路径测量（Python，Farina 扫频法）

提供 Python 指数正弦扫频测量工具，与 C 实时系统解耦：

```bash
# 从 GFANC_Scene 目录运行 (脚本用相对路径找 Primary and Secondary Path/)
cd GFANC_Scene
python ../export/measure_secondary.py --interactive   # 首次: 配置设备
python ../export/measure_secondary.py                 # 日常测量 (可加 --duration 10 --repetitions 6 --amplitude 0.95 提高SNR)
cd ..
python export/export_bin.py                           # 导出 .npy → data/secondary_path.bin
```

方法: Farina 2000 AES 指数扫频，5s 扫频 20-7500Hz，多次重复时域平均，自动反卷积提取脉冲响应。相比白噪声 NLMS 法，SNR 高 10-20dB，天然免疫时钟滑移。

**Ŝ 文件选择 + 环路延迟补偿（v1.4）**：
- 运行时**默认加载 `data/secondary_path.bin`**（扫频法产物）；`GFANC_SEC_FILE` 环境变量可强制指定其它文件（如 C 校准 `data/secondary_path_measured.bin`）。启动日志打印 `Ŝ file: ...`。
- **必须测环路延迟**：`calibrate_secondary.exe` 会生成 `sec_bulk_delay.bin`（总环路延迟 @16k），运行时自动换算 `dsp_delay` 补偿 FxLMS 对齐（启动日志 `Loop delay auto-loaded` / `Ŝ model delay`）。缓冲大小用 `GFANC_BUFFER` 调（默认 128 样本），实测 UMC ASIO + 128 帧环路 ≈ **12.4ms**（该值为 I/O+声学环路；控制路径另有 ANC 带通群延迟 2ms。旧 0.01s suggestedLatency 会把驱动顶到 512 样本 → 30ms，已修复）。
- ⚠️ **每次换安装位置/几何后必须重测**（管道/桌面/窗户声学不同），否则实时 NR 会下降。

## 在线次级路径辨识

系统启动后持续跟踪 Ŝ（扬声器→误差麦）的缓慢变化（温湿度、老化）：

- **算法**: NLMS，μ=5e-6（极慢，~0.1%/s），零探测噪声
- **激励**: 利用 ANC 自身 anti 输出作为宽带激励信号
- **更新**: 仅在正常模式（非 mute/fade/howling）下更新 sec_coeffs 原地
- **禁用**: `$env:GFANC_SEC_MU = "0"`

## 冷启动保护

首次进入某场景时，CNN 预设 Wc 可能与当前噪声不匹配，导致 anti 瞬时 overshoot（可闻嗡嗡/啸叫）。两层保护 + 软释放（v1.2 消除启动啸叫）：

| 机制 | 参数 | 作用 |
|------|------|------|
| Wc 衰减 | `GFANC_WC_COLD` (默认 0.3) | CNN 预设 × 衰减系数，LMS 从低向上收敛 |
| 冷启动软释放 | cold_hold 2s | **前 1s** anti 上限 ±0.12 且梯度冻结；**后 1s** 上限线性 0.12→1.0 且梯度活跃（Wc 在输出受界内自适应收敛）——避免硬释放时未收敛 Wc 的瞬态输出激起反馈啸叫 |

二次进入已收敛场景时跳过保护，直接恢复记忆 Wc，零延迟。

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
| 子滤波器 (C) | 15 | 预训练滤波器, 直接权重混合 (Wc 基) |
| CNN 输出维 (K) | 30 (=S×C, 运行时从 linear_weight 推导) | 直接权重回归: 2 扬声器 × 15 子带增益 (v1.6; 旧场景分类 K=3 已移除) |
| 滤波器长度 (L) | 1024 tap | 控制滤波器 Wc, 频域分辨率 ~15.6Hz |
| CNN 带通 / ANC 带通 | 1024 / 64 tap | 直接权重用 1024(分辨率), FxLMS 用 64(群延迟 2ms) |
| 带通频率 | 20-1500 Hz | ANC 有效频率范围 |
| 输入预增益 | 自适应 (env: GFANC_MIC_GAIN) | 自动标定到 TARGET_REF_RMS=0.03 |
| 步长 (μ) | 1e-7 (基准, Ŝ RMS 自动缩放; env: GFANC_STEP) | LMS 自适应步长 |
| 变步长 (VS-LMS) | 双 EMA 尖峰检测, 突发降步至 5% | 误差相对自身基线跳变→降步防反馈过冲; 平滑收敛全速 (2026-08-05) |
| 泄漏因子 | 5e-7 (基准, 自适应; env: GFANC_LEAK) | Wc 正则化 (2026-08-05 降档 5e-6→5e-7: 弱信号下 Wc 能长起来) |
| 输出限幅 | ±1.0 | DAC 满幅保护 + NaN/Inf 防护 |
| 模式 | reset=默认 / continuous (env: GFANC_MODE=reset\|continuous) | reset: OCG 簇索引变化 → 重置 Wc; continuous: 仅首秒 INIT, 永不重置 (v1.5 去场景层) |
| Reset 触发 (OCG) | OCG 多质心聚类闸门 (env: GFANC_OCG=0 关, 回退旧闸门) | v1.7 新增: 增益向量在线聚类, 簇索引变化才重置 (ICASSP 2026); 簇半径复用 switch_threshold (cos 相似度) |
| 聚类半径 τ | 0.8 (env: GFANC_RESET_THRESH) | cos(g', centroid) < τ → 新建簇; 簇内抖动/慢漂移被质心吸收不触发 |
| 质心漂移 α | 0.1 (env: GFANC_OCG_ALPHA) | 质心 EMA 跟随增益方向 (吸收慢漂移) |
| 簇上限 | 8 (env: GFANC_OCG_CLUSTERS) | LRU 淘汰最久未命中簇 |
| Reset 过渡 | 1600 样本 (100ms) | CrossFader, =20Hz×2 周期 |
| 嵌入式处理延迟 | 3ms 默认 (env: GFANC_EMBED_DELAY_MS) | 离线 pad Ŝ 模拟 ADC+DSP+DAC 因果缺口 (v1.5) |
| 冷启动 ramp | 400ms | 输出从 0 平滑渐入 |
| 冷启动 Wc 衰减 | 0.3 (env: GFANC_WC_COLD) | CNN 预设衰减系数 |
| 冷启动软释放 | 前 1s cap0.12(梯度冻结), 后 1s cap→1.0 | 消除启动啸叫 (v1.2) |
| Wc 发散救援 | anti_rms > 阈值持续 2s | 回滚 Wc + 重新 ramp |
| Wc 发散冻结 | max\|Wc\| > 30×wc_init_max | 自动冻结 LMS 梯度 (自适应基准) |
| 在线 Ŝ 辨识 | μ=5e-6 (env: GFANC_SEC_MU) | NLMS, 零探测噪声 |
| 音频缓冲 | 128 样本 (env: GFANC_BUFFER, 32-1024) | ASIO buffer; 越小延迟越低但易爆音 (128 稳定甜点, 实测环路 ~12.4ms) |
| Ŝ 环路延迟补偿 | 自动 (sec_bulk_delay.bin / GFANC_DSP_DELAY) | FxLMS 对齐, bench 实测 ~12.4ms (原 0.01s suggestedLatency 顶回 512 致 30ms, 已修复) |
| 反馈 FIR | 256 tap ×2 扬声器 | 逐扬声器独立校准 |
| 啸叫陷波 | DFT 256pt, IIR ×2 | 可配阈值 (env: GFANC_HW_THRESH, 默认 12) |
| NR 指标 | 分散采样 250 点 + ±30dB 限幅 | 防噪声基底虚高 (v1.2 BUG-1) |
| CNN 推理 | ~8ms/次 @1Hz | 静态缓冲, 无动态分配 |
| 回调预算 | ~30-45% (SIMD ~5-10%) | 已优化: 双段循环零取模, 含安全边际 |

### 参数 ↔ 降噪量实测速查

以下为**离线实测**（NR_true = Pri 模型真值；马路噪声 34s，窗口场景 预览3ms+延迟≤2ms，2026-08-05）。**数值随输入信号/时长变化，但趋势方向不变**。

**`GFANC_STEP`（步长）— 越大收敛越快、NR 越高，但实时易过冲/振荡**（当前默认 1e-7 为实时稳定底线）：

| `GFANC_STEP` | 离线 NR_true | 说明 |
|---|---|---|
| 1e-7（默认） | ~6 dB | 实时稳定底线；离线 15s 短文件仅 ~2dB（收敛慢） |
| 3e-7 | ~8 dB | VS-LMS 下实时曾稳定（无 MUTE 振荡） |
| 5e-7 | ~9 dB | |
| 3e-6 | ~12 dB | |
| 1e-5 | ~14 dB | ≈ 算法能力上限（对齐可行性文档 15dB） |

**`GFANC_LEAK`（泄漏）— 越小 Wc 长得越足、NR 越高，但太小时 Wc 漂移（实时风险）**：

| `GFANC_LEAK` | 离线 NR_true | 说明 |
|---|---|---|
| 5e-8 | ~16 dB | Wc 生长最足；实时需关注漂移/泄漏不足 |
| 5e-7（默认） | ~14 dB | 平衡点 |
| 5e-6 | ~10 dB | 弱信号下 Wc 被压死，收敛杠杆损失（旧默认） |

**`GFANC_WC_TARGET` — 不影响收敛后 NR**（0.003/0.01/0.03 实测均 ~13.9dB）。只影响冷启动 Wc 幅度（过大 → 开机"嗡"瞬态），收敛后自适应覆盖。

**文件时长**：步长不变时收敛需要时间——15s 文件默认参数 ~2dB、34s ~5-6dB、56s ~4.7dB（**均为理想正预览下**；默认 3ms 嵌入式延迟下受因果墙限制，见 [离线验证](#离线验证)）。

> ⚠️ 上表为**理想正预览**测量（无嵌入式处理延迟）。离线默认 `GFANC_EMBED_DELAY_MS=3` 后，NR 上限由三堵墙主导：**延迟**（因果缺口，见[窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）/ **相干** / **空间**（见[离线验证](#离线验证)）。实时 NR_est 还依赖 Ŝ 模型+误差麦灵敏度、误差麦降太狠会虚高——**唯一真值 = 离线 NR_true，实时以人耳为准**。

## 代码结构

C 实现已超越原始 Python 参考（新增实时 ASIO 音频栈、啸叫检测、反馈抵消、发散保护、双缓冲等模块），是独立的工程实现。

| 文件 | 功能 |
|------|------|
| `main.c` | 离线降噪: WAV 输入/输出, `fxnlms_tick_rt` + 64tap ANC 带通 (与实时同信号链) |
| `main_realtime.c` | 实时降噪: ASIO 音频, `fxnlms_tick_rt` 实时路径, **Reset/Continuous 双模式派发** (去场景层), Ŝ 环路延迟自动补偿 |
| `src/scene_controller.c` | CNN 直接权重 Wc 生产者 (v1.6): minmax → CNN 30 维回归 → `tanh` 增益 → `Wc=Σ gain×sub` → RMS 标定 + 取反 |
| `src/ocg.c` + `include/ocg.h` | OCG 多质心聚类闸门 (v1.7, ICASSP 2026): 增益向量在线聚类, 簇索引变化才重置 — 抑制簇内抖动/慢漂移导致的反复重置 |
| `src/fxnlms_mimo.c` | FxNLMS 自适应 (离线+实时双路径, anti-windup, 自适应 leak) |
| `src/cnn_m5_forward.c` | M5 CNN 前向推理 (实例化, 向后兼容单例宏) |
| `src/fir_filter.c` | FIR 滤波器 (gfanc_delay_t 双精度累加) |
| `src/howling_detect.c` | DFT 频谱峰值检测 + IIR 双二阶陷波 (逐扬声器独立状态) |
| `src/sec_online.c` | 在线 Ŝ NLMS 辨识 (零探测噪声, 原地更新 sec_coeffs) |
| `src/pa_loader.c` | PortAudio ASIO DLL 运行时加载 |
| `src/calibrate_feedback.c` | 反馈路径 NLMS 校准 (逐扬声器, 16k ZOH×3 激励) |
| `src/calibrate_secondary.c` | 次级路径 C 版白噪声校准 + 延迟/滑移诊断 |
| `src/binary_loader.c` | .bin 二进制权重文件加载 (v2 格式, GFNC 头+CRC32) |
| `include/gfanc_types.h` | 集中参数 + 分级日志 + 维度宏 |
| `include/scene_manager.h` | 共享纯函数 (main.c + main_realtime.c 共用); `sm_cos_sim`(reset 判定)/`sm_wc_max_abs`/`sm_check_divergence`/`sm_check_convergence`; v1.6 已删 `sm_scene_switch_execute`/`sm_first_sec_init`/`sm_check_scene_switch`/`sm_wc_rms` 死代码 |
| `include/sec_online.h` | 在线 Ŝ 辨识 API |
| `include/cnn_m5_forward.h` | CNN 实例化 API |
| `export/export_bin.py` | PyTorch → C .bin 导出 |
| `export/measure_secondary.py` | Python 次级路径测量 (Farina 扫频法) |
| `export/measure_primary.py` | Python 初级路径测量 |
| `export/measurement/` | 测量核心模块 (扫频生成/反卷积/质量检验) |
| `GFANC_Scene/` | Python 项目 (训练代码 + 模型权重 + 声学路径测量数据) |

## 训练管线（直接权重 CNN）

直接权重架构：CNN 对 1 秒带通噪声回归 **30 维子带增益**（2 扬声器 × 15 子带），`Wc = Σ 增益 × 子滤波器` 构造启动滤波器，交给 FxNLMS 自适应。训练全在 Python 项目 [GFANC_Scene](../GFANC_Scene) 内完成，产物经 `export/export_bin.py` 导出为 C 可用的 `.bin`。

### 数据与标签

- 数据源：`D:\Dataset\Real_world_Dataset`（真实居民区室外噪声，road / children / construction / railway 各 ~25%）
- 子滤波器基：`models/MIMO_Pretrained_Control_filters_broadband.mat` —— 宽带 FxNLMS 主滤波器 + sqrt-Hann DFT 分解为 15 个功率互补子带；**标注、导出、C 运行时共用同一基**
- 标签：`gain_0..gain_29`（LMS 最优化子带增益，带符号 [-7, +7.3]），由 `label_real_noise.py` 用**实测声学路径 + 子滤波器基**生成 —— 正是直接权重 CNN 的回归目标
- **不需要场景聚类**：`recluster_real.py`（旧场景架构的 K-means → scene_id / SoftLabels / band_）直接跳过

### 命令顺序

```bash
# 0) 一次性依赖
pip install numpy scipy pandas torch torchaudio

# 1) 生成子滤波器（宽带 FxNLMS 训练主滤波器 → sqrt-Hann DFT 分解为 15 个子带）
cd GFANC_Scene
python training/control_filters/Pre_training_broadband_and_decompose.py
#    → models/MIMO_Pretrained_Control_filters_broadband.mat
#      仅当子滤波器基变更时才需重跑（声学路径/分解参数更换后）

# 2) 重标标签（只重标已有 WAV，不重切）
python training/labeling/label_real_noise.py
#    → 覆盖 Index_real_{Training,Validate,Testing}_data.csv + Gains_real_*.npy
#      gain_* 列不变；旧 scene_id/band_ 列消失（场景聚类已不需要）

# 3) 训练直接权重 CNN（m5_scene → 30 维回归头，tanh + MSE）
python training/network/Train_validate.py
#    → GFANC_Scene/models/MIMO_M5_DirectWeight_Real.pth

# 4) 评估（可选：测试集整向量 cos / 逐扬声器 cos / MSE）
python training/network/evaluate.py

# 5) 导出 C 二进制（自动检测直接权重模型）
cd ..
python export/export_bin.py
#    → data/*.bin；检测到 DW 模型则直接权重模式（30 维 + tanh，跳过 scene_defs.bin）
```

### 说明

- **何时重跑子滤波器（第 1 步）**：声学路径、分解参数或部署基变更时。`Pre_training_broadband_and_decompose.py` 的 `USE_LOG_SPACING` 须保持 `False`（均匀间距，与部署/导出一致）。
- **何时必须重标（第 2 步）**：子滤波器更换后，或标注基与部署不一致时。标注必须用与部署（`export/export_bin.py`）**相同的子滤波器基**（broadband）——`label_real_noise.py` 的 `USE_LOG_SPACING` 须为 `False`。
- **导出模式切换**：`export_bin.py` 检测到 `MIMO_M5_DirectWeight_Real.pth` 存在 → 直接权重模式（`cnn_info.json`/`gfanc_config.json` 标 `mode=direct_weight`、`activation=tanh`）；不存在则回退旧场景分类器（K 维 softmax，向后兼容）。
- **超参**：`Train_validate.py` 顶部 `LR`（默认 0.01，MIMO 原配置），loss 发散可降到 0.001。
- **合成数据仅作可选增强**：MIMO 的 `band_*` 标签（0/1 频带激活）语义与直接权重不同，不能直接用来训练；若要加频谱覆盖，合成样本必须走自己的标注管线重新标成 `gain_*` 再混入。

## 离线验证

离线模式 (`main.exe`)：

| 指标 | 结果 |
|------|------|
| **算法能力**（正预览 + `GFANC_STEP=1e-5`） | **~13-15 dB**（详见 [窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)） |
| 平均 NR_true（嵌入式默认 3ms 延迟, road_noise-15, 15s） | ~0.3 dB（宽带→相干/空间墙, 见下方警告） |
| 平均 NR_true（嵌入式默认 3ms 延迟, mixed_7types_56s, 56s） | ~1.8 dB |
| 平均 NR_true（嵌入式 2ms + 预览 10ms → 净预览 +6ms, mixed_7types） | ~3.3 dB（稳态尾部 4-6dB） |
| CNN 输出维 (K) | 30 (=S×C, 从 linear_weight 推导, 直接权重回归) |
| 指标可信度 | NR_est 与 NR_true 逐秒偏差 <0.5dB |

> **默认 step=1e-7 是为实时稳定性调校的**（记忆/实测：VS-LMS 下 step 3e-7 实时已过冲，1e-7 才稳）。离线文件短（15-56s）时默认步长收敛偏慢、NR 偏低。要复现算法能力上限（13-15dB），用 `GFANC_STEP=1e-5` 跑离线。步长与离线 NR（34s 马路噪声, 正预览）实测关系：1e-7→6dB、5e-7→9dB、1e-5→14dB。

> ⚠️ **离线 NR_true 现在按嵌入式处理延迟 3ms 建模因果性**（默认 `GFANC_EMBED_DELAY_MS=3`，启动日志打印净预览时间），不再是"无因果缺口"的理想上限。关键实测（2026-08-08）：① 即使不加嵌入式延迟，64tap ANC 带通群延迟 1.97ms 已超过初级路径提前量 0.69ms → 基线净预览即 −1.9ms；② 纯宽带 road_noise 加多大预览都封顶 ~0.8dB（相干/空间墙，2 扬声器/3 误差麦几何）；③ 要净预览为正需 `τ_pri > τ_spk + τ_proc`（proc=2ms 时需参考麦声学提前量 >4.6ms ≈ 1.6m，参考麦移向噪声源/移离误差麦）。实时受 PC 控制路径延迟限制更重（净预览 ≈ −9ms；详见 [窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）。**实时实测（2026-08-03 bench，误差麦位置、相对安静）NR 平均 ~13dB**，多数秒 7-18dB；窗户安装态重测 Ŝ 后待进一步验证。NR 读数仅对误差麦位置有效（静音区 ≈ λ/10），人耳远离误差麦时降噪下降。

## 常见问题

**Q: 为什么离线运行 NR 只有 0.3-2dB？算法是不是有问题？**
A: **不是 bug**——三个因素叠加：① 默认步长 1e-7 为实时稳定性调校（VS-LMS 下 3e-7 实时已过冲），离线 15s 文件在 1e-7 下收敛慢；② 离线现在默认按嵌入式处理延迟 3ms 建模因果性（`GFANC_EMBED_DELAY_MS=3`），基线净预览已是 **−1.9ms**（64tap ANC 带通群延迟 1.97ms > 初级路径提前量 0.69ms）；③ 纯宽带噪声（road_noise）受**相干/空间墙**限制，与因果无关，加多大预览都封顶 ~0.8dB（2 扬声器/3 误差麦几何）。

| 配置 | NR_true |
|---|---|
| 默认参数, road_noise-15 | ~0.3 dB |
| 默认参数, mixed_7types_56s | ~1.8 dB |
| `GFANC_EMBED_DELAY_MS=2 GFANC_VIRT_PREVIEW_MS=10`（净预览 +6ms）, mixed_7types | ~3.3 dB（稳态 4-6dB） |
| 同上, Helicopter | 稳态 ~3.5 dB |

要复现算法能力上限（正预览 + 大步长），跑：
```bash
GFANC_EMBED_DELAY_MS=2 GFANC_VIRT_PREVIEW_MS=10 GFANC_STEP=1e-5 ./main.exe "Noise Examples/mixed_7types_56s.wav"
```
0.3-2dB 反而是"诚实"读数——反映真实因果约束 + 实时安全步长下的收敛结果，更接近嵌入式实际能拿到的量级（嵌入式还受参考麦声学提前量限制：要正预览需 `τ_pri > 4.6ms`，参考麦需移向噪声源/移离误差麦）。

**Q: 为什么 error_out.wav 听起来比原噪声还大？**
A: error_out.wav 是 3 声道文件（对应 3 个麦克风位置），播放器同时播 3 个声道叠加后音量更大。另外，ANC 只在 20-1500Hz 有效，高频部分反而增加了少量能量。降噪效果要看表格里的 NR_true（离线真值）或实时 NR 数字，不要用耳朵直接听 error_out.wav。

**Q: 实时模式怎么验证效果？**
A: 终端每秒输出 NR(dB)、err/anti RMS、啸叫状态。NR > 3dB 表示有效降噪。

**Q: 可以处理其他采样率的文件吗？**
A: 离线模式自动将输入重采样到 16000 Hz。支持 16-bit PCM WAV。

**Q: 实时版使用什么音频 API？**
A: PortAudio 运行时加载 (`libportaudio64bit-asio.dll`)，支持 ASIO / WASAPI / WDM-KS 后端，通过 `src/pa_loader.c` 动态加载 DLL。
