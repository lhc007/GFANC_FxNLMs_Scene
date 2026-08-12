# GFANC FxNLMS — 主动降噪系统 (ANC)

> **版本**: v1.9 (2026-08-11) | **分支**: gfanc-direct-weight | 完整变更史见 [变更记录](docs/变更记录_CHANGELOG.md)

## 这是什么？

一个**主动降噪（ANC）引擎**的纯 C 语言实现（Python 项目 [GFANC_Scene](GFANC_Scene) 的 C 移植）。原理和降噪耳机一样：

> **"听"到噪声 → 算出与噪声波形相反的声音（反噪声）→ 用扬声器播放 → 噪声和反噪声在空中抵消**，就像噪声没来过一样。

区别是不戴在耳朵上，而是用**独立的麦克风 + 扬声器**，可以消一整片区域的噪声——比如放在窗户开口处做**开窗降噪**。

它有**两种用法**：

| 模式 | 干什么 | 适合谁 |
|------|--------|--------|
| **实时降噪** | 接上麦克风和扬声器，实时抵消你房间/窗边的噪声 | 最终使用场景 |
| **离线降噪** | 输入一段噪声录音（WAV），输出降噪后的文件，并打印降噪量 | 评估效果、调参数 |

## 你需要什么（硬件清单）

### 实时模式必需

| 硬件 | 要求 | 本项目实测配置（参考） |
|------|------|------------------------|
| **音频接口** | 多通道声卡，≥4 进 2 出，支持 ASIO | BEHRINGER UMC 404HD（ASIO 设备号 `23`） |
| **参考麦克风** ×1 | 放在噪声源一侧，负责"听有什么噪声" | ECM8000 全指向测量麦 |
| **误差麦克风** ×3 | 放在听音区（降噪目标位置），负责"听剩多少噪声" | ECM8000 |
| **扬声器** ×2 | 播放反噪声 | 小书架箱 / 全频喇叭 |
| **电脑** | Windows 10/11 | — |

> ⚠️ **摆放几何决定能不能消**：参考麦朝向噪声源，误差麦+扬声器朝向听音区，摆成一条"前馈"线——**噪声先经过参考麦，再到达误差麦**，系统才有时间提前算出反噪声。本实测几何：**参考麦↔误差麦 64cm、扬声器↔误差麦 15cm**，建议装在窗户开口处（参考麦朝窗外、误差麦+扬声器朝室内）。
> ⚠️ **只能消"两个麦都能听到"的噪声**：只在参考麦听到、误差麦听不到的噪声，物理上对消不了。

### 软件

- **GCC 编译器**（Windows 通过 [MSYS2](https://www.msys2.org/) 安装）
- **Python**（仅当你需要**自己训练模型**或**重新测量声学路径**时才需要）
- **权重文件**：`data/` 目录下的 `.bin` 已随项目自带——**不训练也能直接跑**

---

## 快速开始

> 所有命令统一在下方 **完整命令流（训练 → 实时运行）**，按阶段 0 → A → B/C/D 顺序执行。这里先决定走哪条路：

| 你想做什么 | 走哪条路 |
|-----------|---------|
| 只想跑实时降噪（不训练） | 阶段 0 → B → C → D（跳过训练数据 A） |
| 自己训练 / 更换模型 | 阶段 0 → A → B → C → D 全流程 |

**要不要训练？** 仓库 `data/` 自带训练好的权重（含声学路径），**不训练也能直接跑实时降噪**。训练（阶段 A）以 `secondary_path.npy` 为输入（见完整命令流 A 说明）；换硬件/换摆放**不用重训模型**，只需重测声学路径。

**核心概念**：参考麦拾取噪声 → CNN 判定噪声类型 → 生成 30 维子带增益 → 构造反噪声 FIR（Wc）→ 扬声器播放抵消，误差麦反馈给 FxLMS 自适应调整。凡是"声音怎么在空气里走"的参数（扬声器→误差麦 Ŝ、扬声器→参考麦反馈、环路延迟）**取决于摆放几何，换了位置必须重测**。

**校准时注意**：增益旋钮调到 **SIG 常亮、CLIP 不亮**，且**校准和运行时必须用同一位置**（路径系数嵌入了模拟增益）。探针响度用 `GFANC_CAL_NOISE` 调（默认 0.9）：削波就调小（如 0.4），SNR 不足就调大。ERLE < 8dB 的弱耦合路径会被自动置零（属正常保护）。文件缺失时反馈抵消自动禁用，不影响降噪但可能啸叫。

**批次指纹（R-27）**：导出时对 [CNN 权重 + 子滤波器 + 两个带通] 算链式 crc32 写入 `data/batch_id.bin`；运行时重算比对，防止 CNN 与 sub_filters 来自不同训练批次导致 Wc 预设错位。不一致时启动打印 `[WARN] 批次混配检测……`（仅警告不阻断，FxLMS 会自适应纠正，只影响暖启动收敛）——**重跑一次导出（完整命令流 · 阶段 A5）即可修复**。声学路径不参与指纹（可单独替换）。

需要 `libportaudio64bit-asio.dll` 在同目录（项目自带）；编译也可用 `make` / `make realtime`（Makefile 已含全部模块）。换硬件或换摆放后从零到实时运行的完整清单见 [换硬件检查单](#换硬件检查单)。

> **想试场景切换（OCG 聚类闸门）？** 默认**关闭**（`GFANC_OCG=0`，稳定性最佳，日常开窗降噪用默认即可）。想验证"换噪声类型自动切换反相"时再开：
> - **PowerShell**：`$env:GFANC_OCG="1"; ./gfanc_realtime.exe`（关掉：重开终端即可，或 `Remove-Item Env:GFANC_OCG`）
> - **Git Bash / WSL**：`GFANC_OCG=1 ./gfanc_realtime.exe`
> ⚠️ OCG 会在检测到新噪声类型时重置 Wc 以跟随，纯音深对消下可能引入抖动（实测证伪后默认关，见 [CHANGELOG](docs/变更记录_CHANGELOG.md)）——验证用，不是默认稳定配置。

#### 怎么验证真的有效

**用 250Hz 纯音验证，别用宽带噪音**。系统受环路延迟限制，只能对消窄带/周期成分——纯音最能反映真实对消能力：

1. 手机/电脑放一段 **250Hz 纯音**，作为参考麦的噪声源
2. 看终端每秒一行的表格：**NR 应到 10dB 以上**，误差麦电平应明显下降
3. 拿开噪声源再放回，确认降噪跟随

> ⚠️ 别用马路噪音/宽带 WAV 验证"真实降噪"——宽带随机噪声物理上消不了（预览预算为负），纯音才见真章。降噪数字只在**误差麦位置**有效（静音区 ≈ 声波波长/10），人耳远离误差麦时效果下降。

关键操作要点：

- **先看声卡 SIG 灯，确认"噪声真进来了"**：SIG 是输入电平指示灯（约 -40dBFS），**只认电平、不认频率**。放 250Hz 纯音 SIG 常亮；马路噪音从笔记本外放出来时低频被扬声器滤掉，信号只比底噪高 ~20%（refFilt≈0.038 vs 底噪 0.030），SIG 不亮 → 系统"没听到"就谈不上降噪（ECM8000 平直到 20Hz，不是麦克风瓶颈）。**操作点：让噪声放得够响 / 靠参考麦近些，使 refFilt ≥ 0.05（2-3× 底噪）**，这是系统能健壮工作的前提。
- **模拟增益旋钮是灵敏度关键**：ECM8000 弱信号，输入增益旋钮要调到能清晰收音（启动日志 `refFilt≈0.03`、无「输入电平过低」警告）。**校准与运行必须用同一旋钮位置**（路径系数嵌入模拟增益）。
- **误差麦克风 = 安静区目标，不是测试拾音器**：在误差麦旁说话/拍手（参考麦预测不到的突发）会触发静音保护，属正常。噪声源应在参考麦上游（保持「参考麦 → 误差麦」的前馈几何）。
- **啸叫陷波（~125Hz）**：扬声器→误差麦反馈会在 ~125Hz 形成临界啸叫，由啸叫检测陷波压制。**不要调高 `GFANC_HW_THRESH`（默认 12）** —— 调高会放开反馈，导致周期性「收敛→爆炸→静音」循环。
- **几何限制**：ANC 带通已砍 256→64tap（群延迟 8→2ms），控制路径 ≈ 10.8ms（带通 2ms + ASIO 8.4ms + 声学 0.4ms）vs 参考→误差预览 ~1.9ms → **净预览 ≈ −9ms**（原 256tap −18.5ms，详见 [窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）→ 只能实时消 <~55Hz 宽带 + 周期/窄带成分（实测 NR 约 4-6dB）。要更高需拉大参考麦与误差麦距离，或降声卡缓冲。
- 可选调参：`GFANC_MIC_GAIN`（输入预增益）、`GFANC_STEP`、`GFANC_WC_TARGET` 等见[系统参数表](#系统参数)。

---

## 换硬件检查单

> 换了**音频接口 / 扬声器 / 麦克风**，或换了**摆放位置**（如从桌面移到窗边）后，按这个顺序从零跑到实时运行。**核心原则：凡涉及"声音怎么在空气里走"的参数全部重测，模型权重不用动。** 所有命令见上文 **完整命令流 · 阶段 B/C/D**，这里只列步骤与产物。

| 步骤 | 做什么 | 对应完整命令流 |
|------|--------|--------------|
| 0 | 接线 + 调增益旋钮（参考麦→输入 0，误差麦→输入 1-3，扬声器→输出 0-1） | SIG 常亮、CLIP 不亮，记住旋钮位置 |
| 1 | 编译校准程序（只做一次） | 阶段 B |
| 2 | **测次级路径 Ŝ（每次换位置/几何必测）** | 阶段 C1 → `secondary_path.bin`（运行时默认加载） |
| 3 | **测环路延迟（首次 / 换声卡·缓冲必测）** | 阶段 C2 → `sec_bulk_delay.bin`（运行时自动补偿） |
| 4 | 测反馈路径（防啸叫） | 阶段 C3 → `feedback_path_0/1.bin` |
| 5 | 编译实时版 | 阶段 D1 |
| 6 | 运行 + 纯音验证 | 阶段 D2（250Hz 纯音 NR ≥ 10dB、零 RESET） |

### 换硬件时**不需要**重做的

- **CNN 模型、子滤波器、权重导出**（`export_bin.py`）——除非你换的硬件改变了训练数据分布（一般不会）
- **主路径（Pri）测量**——实时版**不加载**主路径；只有离线评估 `main.exe` 算 NR_true 才用得上（`export/measure_primary.py`）

## 运行示例

```
PS D:\VSCodeRepository\GFANC_FxNLMs_Scene> ./main.exe "Noise Examples/road_noise_0-34.wav"
Loading weights...
  BP ANC: bandpass_anc.bin (64tap, gd=2.0ms)
  OK: sec=6144 pri=3072 sub=30720 bp=1024 L=1024
  CNN loaded.
  Ŝ: 原始增益 (默认, 与训练世界一致)
  PROC delay: 0 ms (0 samples) added to Ŝ — 嵌入式信号链处理延迟 (GFANC_EMBED_DELAY_MS=3ms 默认, 可覆盖)
  Causality: τ_pri=0.69ms τ_spk=0.62ms τ_proc(bp+emb)=1.97ms → 净预览=-1.91ms (<0 因果缺口 — 随机宽带对消受限, 只能消窄带/低频)
  System ready (CNN loaded).

Input: 16000 Hz, 1 ch, 557256 samples (34.8s)
Auto-gain: bandpass ref RMS 0.1100 -> mic_pre_gain 0.27 (目标 0.030, 实时版工作点)

 Sec |               TopBands | NR_est | NR_true |    err | refFilt |   anti | Note
---------------------------------------------------------------------------------------------------------
   1 |   2(10%) 17(9%) 14(8%) |    9.7 |    9.6 | 0.398 | 0.0287 | 0.0161 | INIT [C0/1] [FxRMS=0.0287]
   2 |   2(11%) 17(8%) 14(8%) |    9.9 |    9.9 | 0.400 | 0.0282 | 0.0160 | - [C0/1]
   3 |   2(10%) 14(9%) 17(8%) |    9.6 |    9.6 | 0.432 | 0.0323 | 0.0179 | - [C0/1]
  ...
  35 |    2(9%) 17(7%) 14(7%) |    0.0 |    0.0 | 0.000 | 0.0000 | 0.0000 | - [C0/1]
---------------------------------------------------------------------------------------------------------
  Avg |                           |        |    9.6 |

Processing: 14.3s for 34.8s audio (2.4x)
Output: anti_out.wav (2 ch), error_out.wav (3 ch)
Done.
```

> 上为 **R-58-10 默认参数**（`GFANC_STEP=0.005` sum 归一化、`GFANC_EMBED_DELAY_MS=0`、Ŝ 原始增益）的 34s 马路噪声结果。三个标准场景基准: mixed_7types_56s **+9.8dB** / road_noise_0-34 **+9.6dB** / road_noise-15 **+9.2dB**（R-58-10 修复梯度相位失配 + es 双 G 后，三个文件均稳定为正、无随时间衰减；旧 R-58 报的 +16.3/+15.2/−8.4 含 G² 口径伪影，真实值见 [离线验证](#离线验证)）。

## 效果解读

运行后会看到一张表格，每秒一行：

| 列 | 含义 | 举例 |
|----|------|------|
| `Sec` | 第几秒 | `1` |
| `TopBands` | CNN 30 维直接权重增益中 \|gain\| 占比最高的 3 个子带索引 + 占比（`带(百分比)`，0..29 = 扬声器×子带，仅诊断，不参与 Wc 切换）。占比 = \|gain[i]\|/Σ\|gain[j]\|；全 ~0 时显示 `-`。替代旧单值 argmax（CNN 输出层 bias 使 argmax 常钉死在低频带，单值信息量低） | `2(10%) 17(9%) 14(8%)` |
| `NR_est` | **估计降噪量**（与实时版同公式，数字越大越好） | `4.6 dB` = 估计压低了 4.6 分贝 |
| `NR_true` | **已知真值降噪量**（仅离线可用，Pri 模型精确计算扰动，最可信） | `4.4 dB` |
| `err` / `refFilt` / `anti` | 残差 / 带通参考 / 反噪声 RMS | |
| `Note` | 状态 | `INIT`(启动) / `-`(正常) / `RESET`(reset 模式 cos<0.6 → 重置 Wc) |

- **NR_true 是最可信的指标**（离线用 Pri 模型精确算出扰动，NR_est 与它逐秒偏差 <0.5dB）；10 dB 意味着噪声能量降到 1/10，20 dB 意味着降到 1/100
- 注意：**离线默认 `GFANC_EMBED_DELAY_MS=0`**（R-58-8：训练世界无此延迟，3ms 会造成 anti 相位错位 48 样本 → 自适应发散）。启动日志打印净预览时间（基线 ≈ −1.9ms：64tap ANC 带通群延迟 1.97ms > 初级路径提前量 0.69ms）；实时还受 PC 控制路径延迟限制（净预览 ≈ −9ms，见下文"离线验证"）


## 完整命令流（训练 → 实时运行）

> 从训练到实时降噪的**一套完整命令**，按先后顺序执行。整条流程分两段：
> - 🎯 **训练数据**（阶段 A）：纯 Python，产物落在 `GFANC_Scene/` 与 `data/`。**可选**——仓库自带训练好的权重，不训练可直接跳到阶段 B。
> - 🎯 **系统运行**（阶段 B/C/D）：编译 + 声学校准 + 实时运行。首次 / 换硬件 / 换摆放必做，之后每次运行只走阶段 D。

| 阶段 | 内容 | 必做？ | 何时做 |
|------|------|--------|--------|
| 🎯 准备 0 | 下载项目 + 装 Python 依赖 | 必做 | 首次 |
| 🎯 训练数据 A | 子滤波器 → 标签 → 训练 CNN → 导出 `data/*.bin` | 仅重训模型 | 换模型 / 模型效果差 |
| 🎯 系统运行 B | 编译两个校准程序 | 一次 | 首次 |
| 🎯 系统运行 C | 声学路径校准（次级路径 Ŝ / 环路延迟 / 反馈） | 必做 | 首次 / 换硬件 / 换摆放 |
| 🎯 系统运行 D | 编译实时版 + 运行验证 | 必做 | 每次 |

### 🎯 准备（阶段 0 — 必做，只做一次）

```bash
git clone https://github.com/lhc007/GFANC_FxNLMs_Scene.git
cd GFANC_FxNLMs_Scene

# 仅训练/导出需要 Python 依赖；只想跑系统（data/ 自带权重）可不装
pip install numpy scipy pandas torch torchaudio
```

### 🎯 训练数据（阶段 A — 可选）

> ⚠️ **次级路径 Ŝ 是训练与运行的共用输入**：A1 子滤波器训练、A2 打标签、A5 导出都读 `GFANC_Scene/Primary and Secondary Path/secondary_path.npy`（仓库自带一份）。**针对自家安装环境重训 → 先做阶段 C1 现场测次级路径（覆盖这份 .npy），再回本阶段**；只跑仓库自带模型 → 跳过本阶段，直接用自带的 .npy。

```bash
# A0. 一次性依赖（不训练跳过本阶段则无需执行）
pip install numpy scipy pandas torch torchaudio

# A1. 生成子滤波器基（宽带 FxNLMS 主滤波器 → sqrt-Hann DFT 拆 15 子带）
#    输入: Primary and Secondary Path/secondary_path.npy（Ŝ, MIMO FxNLMS 训练必需）
cd GFANC_Scene
python training/control_filters/Pre_training_broadband_and_decompose.py
#    → models/MIMO_Pretrained_Control_filters_broadband.mat
#      仅子滤波器基变更时重跑（声学路径/分解参数更换后）

# A2. 用实测声学路径 + 子滤波器基重标真实噪声标签（gain_0..29）
#    输入: Primary and Secondary Path/{primary,secondary}_path.npy + 子滤波器基
python training/labeling/label_real_noise.py
#    → 覆盖 Index_real_{Training,Validate,Testing}_data.csv + Gains_real_*.npy
#      gain_* 列不变；旧 scene_id/band_ 列消失（场景聚类已不需要）

# A2b. 合成数据增强（可选，治 CNN 输入失聪；默认 60000/7500/7500，打标签 ~5.5h）
python training/labeling/make_synthetic_dataset.py
#    → D:\Dataset\Synthetic_Dataset\{Training,Validate,Testing}_data\*.wav
#      + Index_synth_{split}_data.csv + Gains_synth_{split}_data.npy（LMS_MU=0.001, LMS_REPET=3）
#      --gen-only 只生成；--label-only 只打标签（重跑不用再生成）

# A3. 两阶段训练直接权重 CNN（合成预训练 → 真实微调，低 LR 防坍缩回低频处方）
python training/network/Train_validate_synth.py
#    → models/MIMO_M5_DirectWeight_Pretrain.pth（合成预训练，保留）
#      + models/MIMO_M5_DirectWeight_Real.pth（真实微调后，export 自动加载）
#      纯真实训练仍可用：python training/network/Train_validate.py

# A4. 评估（可选）
python training/network/evaluate.py
python training/network/verify_discrimination.py --model models/MIMO_M5_DirectWeight_Real.pth
#      基线对照：--model models/MIMO_M5_DirectWeight_Real_baseline_35pct.pth（应复现 ≈35.8%）

# A5. 导出 C 二进制（CNN 权重 + 子滤波器 + 声学路径 + 带通 + 批次指纹）
cd ..
python export/export_bin.py
#    → data/*.bin；检测到 DW 模型则直接权重模式（30 维 + tanh，跳过 scene_defs.bin）
#    默认自动查找同级目录的 GFANC_Scene；不同目录用 set GFANC_PYTHON_PROJ=D:\你的路径\GFANC_Scene
```

> 💡 **不训练的用户**：`data/` 已随仓库自带（`GFANC_Scene/Primary and Secondary Path/*.npy` 声学路径也在），跳过阶段 A，从阶段 B 开始即可。

### 🎯 系统运行（阶段 B/C/D — 必做）

```bash
# B. 编译两个校准程序（只需一次）
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 src/calibrate_feedback.c src/fir_filter.c src/binary_loader.c src/pa_loader.c -lm -lole32 -o calibrate_feedback.exe
gcc -O2 -Iinclude src/calibrate_secondary.c -lm -o calibrate_secondary.exe

# C1. 测次级路径 Ŝ（扬声器→误差麦，扫频法 SNR 最高，运行时默认加载其产物）
#     ⚠️ 覆盖 Primary and Secondary Path/secondary_path.npy —— 同是训练打标签/子滤波器的输入;
#        针对自家环境重训模型 → 先做本步再走阶段 A
cd GFANC_Scene && python ../export/measure_secondary.py && cd ..
python export/export_bin.py        # 重导，让新测的 secondary_path.bin 生效
#    → data/secondary_path.bin

# C2. 测环路延迟（首次 / 换声卡或 GFANC_BUFFER 后必测）
./calibrate_secondary.exe
#    → data/sec_bulk_delay.bin（运行时自动换算 dsp_delay 补偿 FxLMS 对齐）
#    该工具顺带测 NLMS 版 Ŝ 到 secondary_path_measured.bin, 但运行时默认不用;
#    想用它需 export GFANC_SEC_FILE=data/secondary_path_measured.bin

# C3. 测反馈路径（扬声器→参考麦，防啸叫）
./calibrate_feedback.exe
#    → data/feedback_path_0.bin / data/feedback_path_1.bin

# D1. 编译实时版
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 main_realtime.c src/scene_controller.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c src/howling_detect.c src/ocg.c src/sec_online.c src/pa_loader.c -lm -lole32 -o gfanc_realtime.exe

# D1b. 编译离线评估版（可选 — 处理 WAV 算 NR_true，见"离线验证"）
gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c src/howling_detect.c src/ocg.c -lm -o main.exe

# D2. 运行实时版 + 纯音验证
./gfanc_realtime.exe
#    设备号如 23；放 250Hz 纯音，NR 应 ≥10dB、零 RESET

# D2b. 运行离线评估版（可选 — 处理一段噪声录音，见"运行示例"）
./main.exe "Noise Examples/road_noise_0-34.wav"
```

> ⚠️ **换摆放后只重做 C1 + C3**；C2 仅在换声卡或 `GFANC_BUFFER` 时重做。模型权重（阶段 A 产物）不随摆放变化，无需重导。

## 训练管线（直接权重 CNN）

> 💡 本节是**训练/更换自己的模型**。训练完成后回到上文 **完整命令流 · 阶段 A（训练数据）** 的 A5 步，导出为 C 二进制。

直接权重架构：CNN 对 1 秒带通噪声回归 **30 维子带增益**（2 扬声器 × 15 子带），`Wc = Σ 增益 × 子滤波器` 构造启动滤波器，交给 FxNLMS 自适应。训练全在 Python 项目 [GFANC_Scene](../GFANC_Scene) 内完成，产物经 `export/export_bin.py` 导出为 C 可用的 `.bin`。

### 数据与标签

- 数据源：`D:\Dataset\Real_world_Dataset`（真实居民区室外噪声，road / children / construction / railway 各 ~25%）
- 子滤波器基：`models/MIMO_Pretrained_Control_filters_broadband.mat` —— 宽带 FxNLMS 主滤波器 + sqrt-Hann DFT 分解为 15 个功率互补子带；**标注、导出、C 运行时共用同一基**
- 标签：`gain_0..gain_29`（LMS 最优化子带增益，带符号 [-7, +7.3]），由 `label_real_noise.py` 用**实测声学路径 + 子滤波器基**生成 —— 正是直接权重 CNN 的回归目标
- 合成数据（可选增强）：`make_synthetic_dataset.py` 生成 4 族多样谱形（窄带/宽带/1-f^α/谐波，20-1500Hz 全子带覆盖）后走**同一套 LMS 管线**标成 `gain_*`。**治 CNN 输入失聪**——真实 4 类全低频主导、谱形接近，从零训练输出坍缩到同一低频处方（判别力 35.8% vs 输入谱 75%），合成数据逼 CNN 学输入再低 LR 微调
- **不需要场景聚类**：`recluster_real.py`（旧场景架构的 K-means → scene_id / SoftLabels / band_）直接跳过

### 命令顺序

训练/更换模型的完整命令见上文 **完整命令流 · 阶段 A（训练数据）**。本节补充训练特有的细节：

### 说明

- **何时重跑子滤波器（第 1 步）**：声学路径、分解参数或部署基变更时。`Pre_training_broadband_and_decompose.py` 的 `USE_LOG_SPACING` 须保持 `False`（均匀间距，与部署/导出一致）。
- **何时必须重标（第 2 步）**：子滤波器更换后，或标注基与部署不一致时。标注必须用与部署（`export/export_bin.py`）**相同的子滤波器基**（broadband）——`label_real_noise.py` 的 `USE_LOG_SPACING` 须为 `False`。
- **导出模式切换**：`export_bin.py` 检测到 `MIMO_M5_DirectWeight_Real.pth` 存在 → 直接权重模式（`cnn_info.json`/`gfanc_config.json` 标 `mode=direct_weight`、`activation=tanh`）；不存在则回退旧场景分类器（K 维 softmax，向后兼容）。
- **超参**：`Train_validate.py` 顶部 `LR`（默认 0.01，MIMO 原配置），loss 发散可降到 0.001。
- **合成数据管线**：`make_synthetic_dataset.py` 统一负责生成+打标签（MIMO 旧 `band_*` 场景标签语义与直接权重不同，**不能直接混用**——合成样本全走该管线标成 `gain_*`）。两阶段训练 `Train_validate_synth.py`：合成预训练逼 CNN 学「输入谱→增益」多样映射，再低 LR 真实微调适配真实统计量、防坍缩回低频处方。判别力 `verify_discrimination.py` 按原始最近均值协议(148 逐秒窗口/6 类)量 CNN 是否真的用输入，目标从 35.8% 提到 ≥70%

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

**去场景层双模式**（`gfanc_mode`，v1.5）——CNN 不再做"场景切换"（无场景记忆/滞回），只产 Wc：

| 模式 | CNN 行为 | Wc 行为 |
|---|---|---|
| **reset**（默认） | 每秒跑，输出新 30 维增益 + 候选 wc_cur | **cos(anchor,cur) < 0.6** → CrossFader 100ms 平滑过渡到新 Wc（默认；OCG 多质心聚类闸门 v1.7 实测证伪后默认关，`GFANC_OCG=1` 可开） |
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
                              └─→ Ŝ ⊗ ref → bp_anc(64tap) → Fx → 梯度更新 (R-58-11)
                                                                      ↑
                                                        err_meas = bp(err_mic) (实测误差直驱)
```

**离线路径**（`fxnlms_tick_rt`，与实时同信号链）：
```
ref → bp_anc(64tap) → Ŝ ⊗ ref → bp_anc(64tap) → Fx → anti = Wc ⊗ Fx (Ŝ域, 仅写WAV) (R-58-10)
              Pri(ref) → Dis → err = Dis + anti → 梯度
```

关键区别：实时 anti 输出是 `Wc ⊗ ref`（直接卷积 64tap 带通参考），梯度用实测误差麦信号直接驱动；离线 anti 是 `Wc ⊗ (Ŝ ⊗ ref)`（经模型滤波），用于 WAV 仿真评估。CNN 直接权重保留独立的 1024tap 带通（频率分辨率）。**R-58-10/11 后实时与离线的 Fx 均再过一次 64tap bp_anc（与 `err_meas` 的 bp 路径逐样本对齐）**，消除"误差带通而 Fx 不带通"的梯度相位失配（FxLMS 临界稳定 → Wc 慢漂移，实时被 cold_hold/adaptive-leak/safety_mute 掩盖）。

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
│  环境安静检测: 噪声停→freeze+衰减Wc, 弱噪声哨兵守卫 (P0-5 v1.9) │
│  NR指标: 分散采样250点 + ±30dB限幅                             │
│  anti_total = anti_ff + anti_fb → 限幅±1.0 → 线性内插×3 → DAC │
│                                                               │
└──────────────────────────────────────────────────────────────┘
## 反馈路径校准

反馈抵消功能需逐扬声器校准声学路径。校准命令见上文 **完整命令流 · 阶段 C3**（`calibrate_feedback.exe` → `feedback_path_0/1.bin`）。

校准完成后运行 `./gfanc_realtime.exe`，启动日志显示：
```
Feedback spk0: 256 taps, RMS=0.0012
Feedback spk1: 256 taps, RMS=0.0011
```

文件缺失时反馈抵消自动禁用，日志显示 `Feedback cancel: disabled`，不影响降噪但可能引发啸叫。

## 次级路径测量（Python，Farina 扫频法）

提供 Python 指数正弦扫频测量工具，与 C 实时系统解耦。**测量命令见上文 完整命令流 · 阶段 C1**（需从 `GFANC_Scene` 目录运行，脚本用相对路径找 `Primary and Secondary Path/`）。工具额外支持：

- `--interactive`：首次使用配置声卡设备
- `--duration <秒>` / `--repetitions <次数>`：延长扫频 / 多次重复时域平均，提高 SNR
- `--amplitude <0..1>`：调探针响度（默认 0.95，削波就调小）


方法: Farina 2000 AES 指数扫频，5s 扫频 20-7500Hz，多次重复时域平均，自动反卷积提取脉冲响应。相比白噪声 NLMS 法，SNR 高 10-20dB，天然免疫时钟滑移。

**三个校准工具的分工**（换位置/几何/声卡时按需重测，不重复）：
- **Ŝ 内容 → 扫频法**（`measure_secondary.py` + `export_bin.py` → `secondary_path.bin`）：每次换位置/几何/扬声器必测，运行时**默认加载**它。
- **环路延迟 → `calibrate_secondary.exe`**（→ `sec_bulk_delay.bin`）：首次 / 换声卡或 `GFANC_BUFFER` 后必测，运行时自动补偿。它顺带产出的 NLMS 版 Ŝ（`secondary_path_measured.bin`）默认不用，需 `GFANC_SEC_FILE` 指定。
- **反馈路径 → `calibrate_feedback.exe`**（→ `feedback_path_0/1.bin`）：防啸叫；换摆放后出现啸叫时重测。

**Ŝ 文件选择 + 环路延迟补偿（v1.4）**：
- 运行时**默认加载 `data/secondary_path.bin`**（扫频法产物）；`GFANC_SEC_FILE` 环境变量可强制指定其它文件（如 C 校准 `data/secondary_path_measured.bin`）。启动日志打印 `Ŝ file: ...`。
- **环路延迟只由 `calibrate_secondary.exe` 测**：生成 `sec_bulk_delay.bin`（总环路延迟 @16k），运行时自动换算 `dsp_delay` 补偿 FxLMS 对齐（启动日志 `Loop delay auto-loaded` / `Ŝ model delay`）。缓冲大小用 `GFANC_BUFFER` 调（默认 128 样本），实测 UMC ASIO + 128 帧环路 ≈ **12.4ms**（该值为 I/O+声学环路；控制路径另有 ANC 带通群延迟 2ms。旧 0.01s suggestedLatency 会把驱动顶到 512 样本 → 30ms，已修复）。
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
| 模式 | reset=默认 / continuous (env: GFANC_MODE=reset\|continuous) | reset: cos(anchor,cur)<0.6 → 重置 Wc; continuous: 仅首秒 INIT, 永不重置 (v1.5 去场景层) |
| Reset 触发 | 默认 cos(anchor,cur)<0.6 (env: GFANC_RESET_THRESH) | 场景真正切换才重置; 0.6 是实机纯音深对消验证值 (0.8 在深对消时误杀健康 Wc, 见 CHANGELOG) |
| OCG 聚类闸门 | 默认关 (env: GFANC_OCG=0) | v1.7 引入 (ICASSP 2026): 增益向量在线聚类, 簇索引变化才重置; **2026-08-10 实机证伪后默认关** — 纯音深对消下增益双模震荡致簇 0↔1↔2 翻转, 每~1000cb RESET (开≈18dB vs 关 27.5dB); 代码保留, 待簇判据更鲁棒后评估; 开启命令见上文快速开始「想试场景切换」框 |
| 聚类半径 τ | 0.8 (env: GFANC_OCG_TAU) | cos(g', centroid) < τ → 新建簇 (P0-1 解耦, 不再复用 GFANC_RESET_THRESH) |
| 质心漂移 α | 0.1 (env: GFANC_OCG_ALPHA) | 质心 EMA 跟随增益方向 (吸收慢漂移) |
| 簇上限 | 8 (env: GFANC_OCG_CLUSTERS) | LRU 淘汰最久未命中簇 |
| 聚类持续性 | 3 (env: GFANC_OCG_HOLD, 1Hz→≈秒数; <=1 立即切换) | P0-3 持续性判据: 候选簇连续命中 N 帧才切换 — 治抖动翻转, 实机 250↔1000 双向真切换的关键 |
| 增益时间平滑 | β=0.5, 旁路 cos=0.85 (env: GFANC_GAIN_SMOOTH) | P0-2: 纯音带跨秒翻转抖动被 EMA 吸收; 帧间 cos<0.85 (真场景切换) → β=1 立即跟随无延迟 |
| Reset 过渡 | 1600 样本 (100ms) | CrossFader, =20Hz×2 周期 |
| 嵌入式处理延迟 | 3ms 默认 (env: GFANC_EMBED_DELAY_MS) | 离线 pad Ŝ 模拟 ADC+DSP+DAC 因果缺口 (v1.5) |
| 冷启动 ramp | 400ms | 输出从 0 平滑渐入 |
| 冷启动 Wc 衰减 | 0.3 (env: GFANC_WC_COLD) | CNN 预设衰减系数 |
| 冷启动软释放 | 前 1s cap0.12(梯度冻结), 后 1s cap→1.0 | 消除启动啸叫 (v1.2) |
| Wc 发散救援 | anti_rms>0.25 且 err/ref>0.6 且 err_ref 逐秒上升 0.1, 连续 2s (env: GFANC_DIVERGE_ERR_RATIO) | P0-4 三重门控: 回滚 Wc + 重新 ramp; 防误杀健康深对消 (纯 anti 阈值会把 err_ref 达 1.3 的收敛中 Wc 误回滚) |
| Wc 发散冻结 | max\|Wc\| > 30×wc_init_max | 自动冻结 LMS 梯度 (自适应基准) |
| 在线 Ŝ 辨识 | μ=5e-6 (env: GFANC_SEC_MU) | NLMS, 零探测噪声 |
| 音频缓冲 | 128 样本 (env: GFANC_BUFFER, 32-1024) | ASIO buffer; 越小延迟越低但易爆音 (128 稳定甜点, 实测环路 ~12.4ms) |
| Ŝ 环路延迟补偿 | 自动 (sec_bulk_delay.bin / GFANC_DSP_DELAY) | FxLMS 对齐, bench 实测 ~12.4ms (原 0.01s suggestedLatency 顶回 512 致 30ms, 已修复) |
| 反馈 FIR | 256 tap ×2 扬声器 | 逐扬声器独立校准 |
| 啸叫陷波 | DFT 256pt, IIR ×2 | 可配阈值 (env: GFANC_HW_THRESH, 默认 12) |
| NR 指标 | 分散采样 250 点 + ±30dB 限幅 | 防噪声基底虚高 (v1.2 BUG-1) |
| 环境安静检测 (进入) | anti>0.02 && ref<0.045 && 曾有大噪声(20s 内), 持续 3s (env: GFANC_QUIET_ANTI/REF/HOLD/MEMORY) | v1.9 (P0-5): 噪声停后冻结梯度+衰减 Wc, 治"扬声器继续输出残余反相声/嗡嗡声"; **哨兵守卫**防宽带弱噪声 (ref≈0.038) 被绝对阈值误判"噪声消失"而砍掉反相 |
| 环境安静检测 (退出) | ref 重回 1.5× 或 err 重回 2.0× 安静基准 (env: GFANC_QUIET_EXIT/ERR_EXIT) | 噪声回归 → 重建 INIT; 纯音回归 ref 只高 20% 靠 err 通道兜底 |
| CNN 推理 | ~8ms/次 @1Hz | 静态缓冲, 无动态分配 |
| 回调预算 | ~30-45% (SIMD ~5-10%) | 已优化: 双段循环零取模, 含安全边际 |

### 参数 ↔ 降噪量实测速查

以下为**离线实测**（NR_true = Pri 模型真值；mixed_7types_56s，R-58-10 默认：sum 归一化 + EMBED=0 + Ŝ 原始增益 + 梯度相位对齐 + es 去双 G，2026-08-09）。**数值随输入信号/时长变化，但趋势方向不变**。

**`GFANC_STEP`（步长）— 离线默认 0.005（R-58-10 重标定）。离线为 sum 归一化（与训练世界一致），实时为 mean+cap（硬件标定语义），两者步长不可直接换算**：

| `GFANC_STEP` | 离线 NR_true (mixed) | 说明 |
|---|---|---|
| 0.05 | 发散 | 训练 mu 值，超出 C 端修复后链路的稳定上限 |
| 0.01 | +9.5 dB | 平台区上沿，开始回落 |
| **0.005（默认）** | **+9.8 dB** | 平台区（0.001-0.005），三文件均稳定最优（road-15 +9.2 / road_0-34 +9.6） |
| 0.001 | +9.5 dB | 平台区 |
| 0.0005 | +9.2 dB | 旧默认（R-58-8 在含双 G bug 链路上标定，已过时） |
| 0.00005 | +7.9 dB | 收敛偏慢 |
| µ=0（固定 Wc） | ~+7 dB | 仅 CNN 初值对消（road_0-34 开环 +7.1，Wc 初值健康） |

> 实时版 `GFANC_STEP` 语义不同：实时默认 1e-7 基准 + Ŝ RMS 自动标定（[main_realtime.c:951-980](main_realtime.c#L951-L980)），且归一化为 mean+cap —— 实时步长与上表不可比，保持默认即可。

**`GFANC_LEAK`（泄漏）— R-58-10 后敏感性大幅降低**（road_noise_0-34 实测 2026-08-09，修复后诚实口径）：

| `GFANC_LEAK` | 离线 NR_true | 说明 |
|---|---|---|
| 5e-8 | +9.6 dB | Wc 生长最足 |
| 5e-7（默认） | +9.6 dB | 平衡点，与 5e-8 无差 |
| 5e-6 | +9.1 dB | 略压 Wc，-0.5dB |

> 旧表（2026-08-05，~16/~14/~10dB）含 G² 口径虚高，绝对值作废，趋势（泄漏越小 NR 越高）不变。

**`GFANC_WC_TARGET` — 不影响收敛后 NR**（旧口径实测均 ~13.9dB，修复后诚实值 ~9dB 量级）。只影响冷启动 Wc 幅度（过大 → 开机"嗡"瞬态），收敛后自适应覆盖。

**文件时长**：步长不变时收敛需要时间——默认参数下 15s 文件收敛略欠、34s 稳、56s 最优（mixed 56s 实测 +9.8dB，15s road 文件 +9.2dB，差距 <1dB）。

> ⚠️ 离线 NR 上限由三堵墙主导：**延迟**（因果缺口 ≈ −1.9ms，见[窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）/ **相干** / **空间**。实时 NR_est 还依赖 Ŝ 模型+误差麦灵敏度、误差麦降太狠会虚高——**唯一真值 = 离线 NR_true，实时以人耳为准**。

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
| `src/calibrate_secondary.c` | 环路延迟/滑移测量（→ `sec_bulk_delay.bin`）+ 顺带 NLMS 版 Ŝ（默认不用） |
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

## 离线验证

离线模式 (`main.exe`)：

| 指标 | 结果 |
|------|------|
| 平均 NR_true, mixed_7types_56s（默认参数, R-58-10） | **+9.8 dB** |
| 平均 NR_true, road_noise_0-34（默认参数） | **+9.6 dB** |
| 平均 NR_true, road_noise-15（默认参数） | **+9.2 dB** |
| 平均 NR_true, road_0-34（µ=0 固定 Wc, 仅 CNN 初值） | +7.1 dB（Wc 初值健康） |
| CNN 输出维 (K) | 30 (=S×C, 从 linear_weight 推导, 直接权重回归) |
| 指标可信度 | NR_est 与 NR_true 逐秒偏差 <0.5dB |

> 三个文件均**稳定为正且无随时间衰减**。旧 R-58 的 +16.3/+15.2/−8.4 是 es 二次乘 G 的口径 bug（`es∝G²` → NR_true 偏移 ±20·log10(G)：G=0.27 虚高 +11.4dB、G=2.72 压低 -8.7dB + tanh 饱和梯度死亡）——R-58-10 去掉二次 G 后为真实值。road_noise-15 不是数据问题：µ=0 同数据开环即可对消，弱文件在 auto-gain 下曾因 G² 饱和学不动，修复后 G 无关、与强文件同样收敛。**离线 G 只是仿真工作点，与硬件旋钮无关；`GFANC_MIC_GAIN=1` 仍可看 G=1 口径**。

> ⚠️ 离线默认 `GFANC_EMBED_DELAY_MS=0`（R-58-8：训练世界无此延迟，3ms 加在 Ŝ 上 → anti 相位错位 48 样本 → 正反馈发散）。基线净预览 ≈ −1.9ms（64tap ANC 带通群延迟 1.97ms > 初级路径提前量 0.69ms）——随机宽带对消受限，只能消窄带/低频；纯宽带 road_noise 加多大预览都封顶 ~0.8dB（相干/空间墙，2 扬声器/3 误差麦几何）。实时受 PC 控制路径延迟限制更重（净预览 ≈ −9ms；详见 [窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）。**实时实测（2026-08-03 bench，误差麦位置、相对安静）NR 平均 ~13dB**，多数秒 7-18dB；窗户安装态重测 Ŝ 后待进一步验证。NR 读数仅对误差麦位置有效（静音区 ≈ λ/10），人耳远离误差麦时降噪下降。

## 常见问题

**Q: 为什么 offline 结果因文件而异（mixed/road_0-34 ~9.5dB+，road_noise-15 也曾是负值）？**
A: R-58-10 修复后三个标准场景用同一套默认参数，NR_true 收敛到 **+9.8 / +9.6 / +9.2dB**，差异 <1dB（主要是文件时长与噪声谱）。旧版 road_noise-15 的 -8.4dB 是**代码 bug 不是数据问题**：弱文件 auto-gain 补偿 G=2.72 → `es=(pri+anti)×G` 超 ±1 进 tanh 饱和（梯度≈0）学不动，且 G² 口径使读数 -8.7dB。R-58-10 去掉 es 二次乘 G 后步长/指标与 G 无关，弱文件与强文件同样收敛。**µ=0 同数据开环 +7.1dB** 证明数据与 CNN Wc 初值健康（若数据本身对消不掉，开环也应接近 0）。

| 配置 | NR_true |
|---|---|
| 默认参数（R-58-10）, mixed_7types_56s | +9.8 dB |
| 默认参数, road_noise_0-34 | +9.6 dB |
| 默认参数, road_noise-15 | +9.2 dB |
| `GFANC_MIC_GAIN=1`, road_noise-15 | ~+9 dB（G 无关, 口径不变） |
| 默认参数 + µ=0（固定 Wc）, road_0-34 | +7.1 dB（CNN 初值健康度） |

**Q: 为什么 error_out.wav 听起来比原噪声还大？**
A: error_out.wav 是 3 声道文件（对应 3 个麦克风位置），播放器同时播 3 个声道叠加后音量更大。另外，ANC 只在 20-1500Hz 有效，高频部分反而增加了少量能量。降噪效果要看表格里的 NR_true（离线真值）或实时 NR 数字，不要用耳朵直接听 error_out.wav。

**Q: 实时模式怎么验证效果？**
A: 终端每秒输出 NR(dB)、err/anti RMS、啸叫状态。NR > 3dB 表示有效降噪。

**Q: 放马路噪音为什么没降噪，声卡 SIG 灯也不亮？**
A: 分两层看。① **信号没进来**：马路噪音从笔记本外放出来，低频被扬声器滤掉，进参考麦只比底噪高 ~20%（refFilt≈0.038 vs 底噪 0.030），SIG 灯不亮 = 系统"没听到"。ECM8000 平直到 20Hz，不是麦克风瓶颈，瓶颈是播放源发不出低频。② **安静检测误杀**（v1.9 已修）：弱噪声 ref 低于绝对门槛会被误判"噪声消失"而砍掉反相，哨兵守卫要求"ref 曾在 20s 内高于门槛"才允许安静判定，弱噪声从启动就在则永不误判。**验证点：放噪声时 SIG 灯必须常亮、refFilt ≥ 0.05（2-3× 底噪），250Hz 纯音 NR ≥ 10dB 才说明系统正常**。

**Q: 可以处理其他采样率的文件吗？**
A: 离线模式自动将输入重采样到 16000 Hz。支持 16-bit PCM WAV。

**Q: 实时版使用什么音频 API？**
A: PortAudio 运行时加载 (`libportaudio64bit-asio.dll`)，支持 ASIO / WASAPI / WDM-KS 后端，通过 `src/pa_loader.c` 动态加载 DLL。

**Q: 启动时打印 `[WARN] 批次混配检测` 是什么？要紧吗？**
A: 表示 `data/` 里的 **CNN 权重 / 子滤波器 / 带通** 不是同一次 `export_bin.py` 导出的（比如只拷了某个旧的 `sub_filters.bin` 或 `cnn_*.bin` 进来）。直接权重架构下 `Wc = Σ 增益 × 子滤波器`，两者必须同源，混配会让启动 Wc 预设初值错位。**不要紧**——FxLMS 会自动收敛纠正，仅影响开机/切场景时暖启动慢一点；但建议**重跑一次 `python export/export_bin.py`** 让整批一致，指纹警告即消失。注意：`secondary_path.bin`、`feedback_path_*.bin` 等声学路径文件是**故意不参与**指纹的（按摆放可单独重测），替换它们不会触发此警告。
