# SceneZone ANC — 主动降噪系统 (ANC)

> **版本**: v2.1 (2026-08-22) | **分支**: plan-c-dual-mode | 完整变更史见 [变更记录](docs/变更记录_CHANGELOG.md)
>
> **架构 = SFANC 硬选库（单一架构）**：误差麦只在**标定**用，部署态**无误差麦纯开环**。标定/离线收敛后存 N 条全频段成品滤波器到库 `data/wc_bank.bin`（**绝对增益烘焙**，不做 RMS 归一化）；部署态 = 1Hz CNN 分类（`cnn_bank`，K=N）argmax 硬选库槽 → 防抖 → crossfade 切换。**回归 CNN 线（直接权重/子滤波器/OCG/场景切换）已整体移除**，只保留硬选库需要的东西。详见下方「双模式」。

## 这是什么？

一个**主动降噪（ANC）引擎**的纯 C 语言实现（Python 项目 [SceneZone_Scene](SceneZone_Scene) 的 C 移植）。原理和降噪耳机一样：

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

> 本文档默认**从零开始、自己训练数据**。所有命令统一在下方 **完整命令流（训练 → 实时运行）**，按阶段 0 → 4 顺序执行。

| 你想做什么 | 走哪条路 |
|-----------|---------|
| 从头训练 + 实时降噪（默认） | 阶段 0 → 1 → 2 → 3 → 4 全流程 |
| 先用现成模型跑通（可选捷径） | 阶段 0 → 1 → 3 → 4（跳过训练 2；主路径测量可省） |
| **部署态硬选库（SFANC 无误差麦开环）** | 阶段 0 → 1 → 2 → 3 → 4（训练核心 = 阶段 2：2-⓪ 生成带通 + 2-⑧ N 条代表噪声生成库槽 → 打标签训分类 CNN → 导出） |

**为什么先测声学路径再训练？** 库槽生成（阶段 2-⑧：离线 FxLMS 收敛）以 `secondary_path.npy` 和 `primary_path.npy` 为输入，所以**阶段 1 测声学路径必须排在训练之前**；环路延迟、反馈路径是纯部署项（训练用不到，阶段 1 里一起测完）。仓库 `data/` 也带一份训练好的权重，想先跑通再训练可跳过阶段 2——此时换硬件/摆放后要回到导出步重导一次让新测 Ŝ 生效。

**核心概念**：参考麦拾取噪声 → 分类 CNN（`cnn_bank`，K=N）判定噪声类型 → argmax 硬选 N 条全频段成品滤波器库 `data/wc_bank.bin` 中的槽 → crossfade 切到槽内 Wc → 扬声器纯前向播放抵消。**误差麦只在标定/训练时用**——部署态没有误差麦，靠「N 条全频段成品滤波器库（离线 FxLMS 收敛或实机标定）+ CNN 分类 argmax 硬选」开环降噪（详见下方「双模式」）。凡是"声音怎么在空气里走"的参数（扬声器→误差麦 Ŝ、扬声器→参考麦反馈、环路延迟）**取决于摆放几何，换了位置必须重测**。

**校准时注意**：增益旋钮调到 **SIG 常亮、CLIP 不亮**，且**校准和运行时必须用同一位置**（路径系数嵌入了模拟增益）。探针响度用 `GFANC_CAL_NOISE` 调（默认 0.9）：削波就调小（如 0.4），SNR 不足就调大。ERLE < 8dB 的弱耦合路径会被自动置零（属正常保护）。文件缺失时反馈抵消自动禁用，不影响降噪但可能啸叫。

**批次指纹（R-27）**：导出时对 [分类 CNN 权重 `cnn_bank_*.bin` + 两个带通] 算链式 crc32 写入 `data/batch_id.bin`；运行时重算比对，防止 CNN 与带通来自不同训练批次导致决策错位。不一致时启动打印 `[WARN] 批次混配检测……`（仅警告不阻断）——**重跑一次导出（阶段 2-⑧ ④）即可修复**。声学路径不参与指纹（可单独替换）。

需要 `libportaudio64bit-asio.dll` 在同目录（项目自带）；编译用 `make` / `make realtime`（Makefile 已含全部模块）。换硬件或换摆放后从零到实时运行的完整清单见 [换硬件检查单](#换硬件检查单)。

#### 怎么验证真的有效

**两种验证口径，别混**：
- **标定态（adapt，有误差麦）**：用 250Hz 纯音验证。系统受环路延迟限制，只能对消窄带/周期成分——纯音最能反映链路健康：放 250Hz 纯音 → 终端表格 **NR 应到 10dB 以上**，误差麦电平明显下降，拿开噪声源再放回确认跟随。
- **部署态（fixed，无误差麦，日常运行）**：**没有 NR 指标**（n/a）。验证靠两点：①放某类噪声，看每秒 `[BANK]` 类日志选的类对不对；②人耳听降噪有没有变安静。别用纯音——部署态播的是标定好的固定滤波器，纯音只测标定态链路。

> ⚠️ 别用马路噪音/宽带 WAV 验证标定态"真实降噪"——宽带随机噪声物理上消不了（预览预算为负），纯音才见真章。降噪数字只在**误差麦位置**有效（静音区 ≈ 声波波长/10），人耳远离误差麦时效果下降。

关键操作要点：

- **先看声卡 SIG 灯，确认"噪声真进来了"**：SIG 是输入电平指示灯（约 -40dBFS），**只认电平、不认频率**。放 250Hz 纯音 SIG 常亮；马路噪音从笔记本外放出来时低频被扬声器滤掉，信号只比底噪高 ~20%（refFilt≈0.038 vs 底噪 0.030），SIG 不亮 → 系统"没听到"就谈不上降噪（ECM8000 平直到 20Hz，不是麦克风瓶颈）。**操作点：让噪声放得够响 / 靠参考麦近些，使 refFilt ≥ 0.05（2-3× 底噪）**，这是系统能健壮工作的前提。
- **模拟增益旋钮是灵敏度关键**：ECM8000 弱信号，输入增益旋钮要调到能清晰收音（启动日志 `refFilt≈0.03`、无「输入电平过低」警告）。**校准与运行必须用同一旋钮位置**（路径系数嵌入模拟增益）。
- **误差麦克风 = 安静区目标，不是测试拾音器**：在误差麦旁说话/拍手（参考麦预测不到的突发）会触发静音保护，属正常。噪声源应在参考麦上游（保持「参考麦 → 误差麦」的前馈几何）。
- **啸叫陷波（~125Hz）**：扬声器→误差麦反馈会在 ~125Hz 形成临界啸叫，由啸叫检测陷波压制。**不要调高 `GFANC_HW_THRESH`（默认 12）** —— 调高会放开反馈，导致周期性「收敛→爆炸→静音」循环。
- **几何限制**：ANC 带通已砍 256→64tap（群延迟 8→2ms），控制路径 ≈ 10.8ms（带通 2ms + ASIO 8.4ms + 声学 0.4ms）vs 参考→误差预览 ~1.9ms → **净预览 ≈ −9ms**（原 256tap −18.5ms，详见 [窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）→ 只能实时消 <~55Hz 宽带 + 周期/窄带成分（实测 NR 约 4-6dB）。要更高需拉大参考麦与误差麦距离，或降声卡缓冲。
- 可选调参：`GFANC_MIC_GAIN`（输入预增益）、`GFANC_STEP`、`GFANC_BANK_HOLD` 等见[系统参数表](#系统参数)。

---

## 换硬件检查单

> 换了**音频接口 / 扬声器 / 麦克风**，或换了**摆放位置**（如从桌面移到窗边）后，按这个顺序从零跑到实时运行。**核心原则：凡涉及"声音怎么在空气里走"的参数全部重测，模型权重不用动。** 所有命令见上文 **完整命令流 · 阶段 1/3/4**，这里只列步骤与产物。

| 步骤 | 做什么 | 对应完整命令流 |
|------|--------|--------------|
| 0 | 接线 + 调增益旋钮（参考麦→输入 0，误差麦→输入 1-3，扬声器→输出 0-1） | SIG 常亮、CLIP 不亮，记住旋钮位置 |
| 1 | **测次级路径 Ŝ（每次换位置/几何必测）** | 阶段 1-② → `secondary_path.npy`；导出见阶段 2-⑧ ④ |
| 2 | **测环路延迟（首次 / 换声卡·缓冲必测）** | 阶段 1-④ → `sec_bulk_delay.bin`（运行时自动补偿） |
| 3 | 测反馈路径（防啸叫） | 阶段 1-⑤ → `feedback_path_0/1.bin` |
| 4 | 编译实时版 | 阶段 3-① |
| 5 | 运行 + 纯音验证 | 默认（标定态）跑 `.\scenezone_realtime.exe` + 250Hz 纯音 NR ≥ 10dB（部署态见阶段 4，无 NR 指标） |

### 换硬件时**不需要**重做的

- **CNN 模型、权重导出**（`export_bin.py`）——除非你换的硬件改变了训练数据分布（一般不会）
- **主路径（Pri）测量**——实时版**不加载**主路径；仅**库槽生成（阶段 2-⑧ ① 离线 FxLMS 收敛）、离线评估 `main.exe` 算 NR_true** 需要（`export/measure_primary.py`，见阶段 1-③）。若重生成库则需先重测

> ⚠️ **换摆放/换设备后，走部署态（fixed）需重标定填库**：SFANC 库槽是就地收敛成品（绝对增益嵌进了系数），换位置/几何后物理 P/S 解变了，开环降噪会静默下降。重跑一次标定（`GFANC_CAL_INDEX=k` 覆盖对应槽）即可恢复，分类 CNN（`cnn_bank`）**不用重训**——它是"谱形→类别"映射，跨环境可复用。

## 运行示例

```
PS D:\VSCodeRepository\SceneZone-ANC> ./main.exe "Noise Examples/road_noise_0-34.wav"
SceneZone-ANC — Offline WAV ANC Eval (SFANC 硬选库, 纯开环)
Loading weights...
  BP ANC: bandpass_anc.bin (64tap, gd=2.0ms)
  OK: sec=6144 pri=3072 bp=1024 L=1024
  Ŝ: 原始增益 (默认, 与训练世界一致)
  PROC delay: 0 ms (0 samples) added to Ŝ — 嵌入式信号链处理延迟 (GFANC_EMBED_DELAY_MS=3ms 默认, 可覆盖)
  Causality: τ_pri=0.69ms τ_spk=0.62ms τ_proc(bp+emb)=1.97ms → 净预览=-1.91ms (<0 因果缺口 — 随机宽带对消受限, 只能消窄带/低频)
  System ready.

Input: 16000 Hz, 1 ch, 557256 samples (34.8s)
Auto-gain: bandpass ref RMS 0.1100 -> mic_pre_gain 0.27 (目标 0.030, 实时版工作点)
  [BANK] 分类选槽: 整库 4 槽, CNN argmax+防抖 (K=4, 2帧) — 纯前向无梯度
  OPEN_LOOP: 固定库槽 Wc, 纯前向无梯度 (deploy 离线等价)

 Sec |             BankClass | NR_est | NR_true |    err | refFilt |   anti | Note
---------------------------------------------------------------------------------------------------------
   1 |       [BANK] 类 2/4 |    2.1 |    3.4 | 0.398 | 0.0287 | 0.0161 | INIT [FxRMS=0.0287]
   2 |       [BANK] 类 2/4 |    2.0 |    3.3 | 0.400 | 0.0282 | 0.0160 | -
   3 |       [BANK] 类 2/4 |    2.0 |    3.3 | 0.432 | 0.0323 | 0.0179 | -
  ...
  35 |       [BANK] 类 2/4 |    1.9 |    3.2 | 0.000 | 0.0000 | 0.0000 | -
---------------------------------------------------------------------------------------------------------
  Avg |                           |        |    3.3 |

Processing: 14.3s for 34.8s audio (2.4x)
Output: anti_out.wav (2 ch), error_out.wav (3 ch)
Done.
```

> 上为示例输出：**离线版 = 纯开环**（无梯度链），分类 CNN 选定库槽后整段播放该槽成品滤波器，NR_true 是"该槽在这段噪声上的真实对消量"（Pri 模型精确计算）。具体数值随库槽与输入噪声匹配度变化——匹配的槽 NR 高、错配的槽甚至可能反相变差（这正是部署态选类的意义）。类变时打印 `[BANK] 分类 2→3 (filter 3, slot 3, fade 1600)`。

## 效果解读

运行后会看到一张表格，每秒一行：

| 列 | 含义 | 举例 |
|----|------|------|
| `Sec` | 第几秒 | `1` |
| `BankClass` | 当前选定的库槽类：`[BANK] 类 k/N`（k = 槽号，N = 库槽总数）。静态槽（N=1）显示 `[BANK] 类 0/1` | `[BANK] 类 2/4` |
| `NR_est` | **估计降噪量**（与实时版同公式，数字越大越好；开环下仅作参考） | `2.0 dB` |
| `NR_true` | **已知真值降噪量**（仅离线可用，Pri 模型精确计算扰动，最可信） | `3.3 dB` |
| `err` / `refFilt` / `anti` | 残差 / 带通参考 / 反噪声 RMS | |
| `Note` | 状态 | `INIT`(启动) / `-`(正常) / `DIV!`(发散) |

- **NR_true 是最可信的指标**（离线用 Pri 模型精确算出扰动）；10 dB 意味着噪声能量降到 1/10，20 dB 意味着降到 1/100
- 离线版**只验证「选类正确 + 切换无爆音」**——真实开环降噪量以实机部署为准（部署态无 NR 指标，NR 显示 n/a）。`GFANC_BANK_SIM=1` 定时轮换槽、`GFANC_FORCE_CLASS=k` 强制静态槽，见 [离线验证](#离线验证)


## 完整命令流（训练 → 实时运行）

> 面向**从零开始、自己训练数据**的完整流程，按阶段 0 → 4 顺序执行。核心依赖关系：
> - **次级路径 Ŝ 必须先测（阶段 1）再训练（阶段 2）**——库槽生成（2-⑧ ① 离线 FxLMS 收敛）以它为输入（与仓库自带的那份 `secondary_path.npy` 是同一文件）；
> - 环路延迟、反馈路径是**纯部署项**，与训练无关（阶段 1 里一起测，训练用不到）；
> - 想先用现成模型跑通（可选捷径）：阶段 0 → 1 → 3 → 4，跳过阶段 2，但换硬件/摆放后需回导出步（2-⑧ ④）重导一次让新测 Ŝ 生效。

| 阶段 | 内容 | 必做？ | 何时做 |
|------|------|--------|--------|
| 🎯 准备 0 | 下载项目 + 装 Python 依赖 | 必做 | 首次 |
| 🎯 声学路径测量 1 | 编译校准程序 + 测次级路径/主路径/环路延迟/反馈路径 | 必做 | 首次 / 换硬件 / 换摆放 |
| 🎯 训练 2 | 生成带通 → 生成库槽 → 打标签 → 训练分类 CNN → 导出 `data/*.bin` | 必做（本文档面向从头训练） | 首次 / 换模型 |
| 🎯 编译 3 | 编译实时版/离线版 | 必做 | 首次 |
| 🎯 部署运行 4 | SFANC 硬选库部署（无误差麦开环） | 必做 | 每次 |

### 🎯 准备（阶段 0 — 必做，只做一次）

```bash
git clone https://github.com/lhc007/SceneZone-ANC.git
cd SceneZone-ANC

pip install numpy scipy pandas torch torchaudio
```

### 🎯 声学路径测量（阶段 1 — 必做）

> ⚠️ **四种声学路径一次测完**。其中**次级路径 Ŝ 和主路径 Pri 是训练输入**（库槽生成 2-⑧ ①、导出 2-⑧ ④ 都用），**环路延迟、反馈路径是运行部署项**（与训练无关，但同属"测量"、共用同一套硬件布置，一起测完最顺）。前置条件：硬件接好、摆放就位（参考麦朝噪声源、误差麦+扬声器朝听音区）、增益旋钮 SIG 常亮 CLIP 不亮。

```bash
# 1-① 编译两个校准程序（测环路延迟/反馈路径用，只需一次）—— 先编译，后面 1-④/1-⑤ 要调用
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 src/calibrate_feedback.c src/fir_filter.c src/binary_loader.c src/pa_loader.c -lm -lole32 -o calibrate_feedback.exe
gcc -O2 -Iinclude src/calibrate_secondary.c -lm -o calibrate_secondary.exe

# 1-② 测次级路径 Ŝ（扬声器→误差麦，Farina 指数扫频法，SNR 最高、免疫时钟滑移）
#     [从零必测] 训练 + 运行共用输入，必须先测、先于任何训练（2-⑧ 全都要它）
#     脚本用相对路径找 Primary and Secondary Path/，需从 SceneZone_Scene 目录运行
cd SceneZone_Scene
python ../export/measure_secondary.py --interactive   # 首次：配置声卡设备
python ../export/measure_secondary.py                 # 日常测量（--duration/--repetitions/--amplitude 见"次级路径测量"章节）
cd ..
#    → 覆盖 SceneZone_Scene/Primary and Secondary Path/secondary_path.npy

# 1-③ 测主路径 Pri（参考麦→误差麦）— 库槽生成/离线评估 NR_true 用
#     [从零建议测] 把扬声器从窗框拆下、搬到室外噪声源位置再接 (source-channel 对应其声卡输出通道)
#     实时运行不用它; 只想跑通现成模型可省
cd SceneZone_Scene
python ../export/measure_primary.py --source-channel 0 --duration 8 --repetitions 4
cd ..
#    → 覆盖 SceneZone_Scene/Primary and Secondary Path/primary_path.npy

# 1-④ 测环路延迟（首次 / 换声卡或 GFANC_BUFFER 后必测）
./calibrate_secondary.exe
#    → data/sec_bulk_delay.bin（运行时自动换算 dsp_delay 补偿 FxLMS 对齐）
#    注 (v5): 本工具只测环路延迟, 不再产出次级路径 — Ŝ 由 1-② 扫频法提供

# 1-⑤ 测反馈路径（扬声器→参考麦，防啸叫）
./calibrate_feedback.exe
#    → data/feedback_path_0.bin / data/feedback_path_1.bin
```

### 🎯 训练（阶段 2 — 必做，从零全量重训）

> 输入依赖：阶段 1 测好的 `secondary_path.npy` + `primary_path.npy`（2-⑧ 全都要）。**按顺序一条路走到底**：
> - **2-⓪ 生成带通**：分类 CNN 输入与导出的 `bandpass_fir.bin` 同源。
> - **2-⑧ 部署核心**：离线生成硬选库槽 + 打标签 + 训练分类 CNN（K=N argmax 选槽）+ 导出 `data/*.bin`，部署必做。

```bash
# 2-⓪ 生成带通 bandpass_fir.mat（CNN 输入 + 导出 bandpass_fir.bin 同源，50-1500Hz）
#     改降噪范围时改 --f-low/--f-high 重跑本步即可
python export/gen_bandpass_fir.py --f-low 50 --f-high 1500
#    → SceneZone_Scene/models/bandpass_fir.mat
```

**2-⑧ 离线生成硬选库 + 分类 CNN（走部署硬选库必做；只跑固定单滤波器时可跳过）**
部署态按噪声类型硬选库槽，需要 N 条全频段成品滤波器 + 一个 N 类分类器。滤波器**离线生成**（SFANC-Window / MIMO-SFANC 同款，无逐场景实机标定、无语义场景名）：**N 段代表噪声 → 逐段离线 FxLMS（经实测 Pri/Sec 路径）收敛 → 一条成品 = 库槽 k**；标签 = 每段噪声对 N 条候选滤波器算残差功率取 argmin（MIMO-SFANC 法）。若只需单一静态滤波器（N=1 库），跳过本节，部署自动回退静态槽。

> 📌 **`--filters` 的文件是你自己准备的噪声录音——脚本不生成、仓库不附带**（`f_road.wav` 只是占位名）。这条命令是"**吃噪声、吐滤波器**"：喂一段你部署时想消掉的噪声 → 收敛出专门消它的那条成品滤波器 → 写一个库槽。**想消什么噪声就喂什么**——正式用前拿目标窗户/房间录的真实噪声喂，比示例样本更贴合部署环境。
> 📌 **与 SFANC-Window 官方做法一致**（README 原话："7 broadband noises with different frequency ranges are used as primary noises to obtain the corresponding pre-trained control filters"；换环境时"obtain the corresponding pre-trained control filters in the new acoustic paths, the trained CNN can remain unchanged"）。**滤波器只需要 N 段代表噪声，不要上万条**——每条滤波器的未知数只有 S×L=2048 个系数，60s×16k≈96 万样本足以解出最优解（这是优化拟合，不是监督学习）；大语料（SFANC-Window 用 8 万条合成噪声）给的是**分类 CNN 判别器**学"谱形→槽号"，对应下边 ③。

```bash
# ⓪ 准备输入音频（脚本不生成任何音频——全是你自己的）:
#     --filters  每段 = 一类你要在部署时消掉的噪声录音（手机/录音笔录目标窗户的真实噪声; 可先用 Noise Examples/ 现成样本试跑）
#     --wav-dir  任意噪声语料目录（打分打标签用, 分类 CNN 的训练输入; 可用 Noise Examples/ 或 D:\Dataset 下现成噪声）
# ① 离线生成库（N 段代表噪声 → data/wc_bank.bin, N 槽）—— f_*.wav = 你自己的噪声录音路径
#    N 段建议覆盖你要对抗的噪声形态（马路/风扇/人声/…），一段 = 一条成品滤波器
python export/generate_bank.py --filters f_road.wav f_fan.wav f_voice.wav -o data/wc_bank.bin
#    马上能试: 用仓库自带的真实噪声样本（Noise Examples/ 里就是各形态噪声录音, 文件带空格要加引号）
python export/generate_bank.py --filters "Noise Examples/road_noise_0-34.wav" "Noise Examples/Helicopter.wav" "Noise Examples/Handheld drill.wav" -o data/wc_bank.bin
# ② 打分打标签（对任意噪声语料目录逐段 1s 算残差 argmin → 分类 CNN 训练标签；需先有库）
python export/generate_bank.py --labels --wav-dir D:\你的噪声语料目录 -o data/wc_bank.bin
#    （同一命令 A+B 一起跑: --filters ... --labels --wav-dir ...，先生成再打分，见 generate_bank.py 用法）

# ── 可选替代: 合成噪声打标签（SFANC-Window 同款 — 没有/不想用真实语料时，用它替代上面 ①+②）──
#    方法照搬 SFANC-Window："filtering white noise through various bandpass filters with randomly
#    chosen center frequencies and bandwidths"。类 = 频带（N 个对数分频频带覆盖 50-1500Hz）。
#    它同时产出 ① 的 --filters 素材 + ② 的标签 CSV，所以 ① 用它的 band_k.wav、② 直接跳过。
# ①' 生成合成噪声（类 k ↔ 频带 k ↔ 库槽 k 语义对齐；N=类数=库槽数=CNN 的 K）
python export/generate_synthetic_noise.py --n-classes 4 --clips-per-class 2000
#    → data/synth_noise/band_0..3.wav（每类代表性宽带）+ cls_k/*.wav（1s 训练样本）
#      + data/bank_labels_{train,valid}.csv（K=N 标签，train_real_bank_cnn.py 默认读）
# ②' 用每类代表性宽带生成库槽（顺序即槽序: band_0 → 槽 0 … 类 k 的噪声 → 部署选槽 k）
python export/generate_bank.py --filters "data/synth_noise/band_0.wav" "data/synth_noise/band_1.wav" "data/synth_noise/band_2.wav" "data/synth_noise/band_3.wav" -o data/wc_bank.bin
#    跳过上面 ②（标签已由生成器写好）
# ③ 训练 N 类分类器（CrossEntropy + 硬标签 + WeightedRandomSampler + early-stop）
#    大语料在这里——判别器要多而杂的噪声样本学会"这段谱形该选第 k 条滤波器"
#    ⚠️ 必须显式传 --train/--valid 指向仓库根 data/（脚本默认找 SceneZone_Scene/data/，会找不到 ② 的 CSV）
python SceneZone_Scene/training/network/train_real_bank_cnn.py \
  --train data/bank_labels_train.csv --valid data/bank_labels_valid.csv
#    → SceneZone_Scene/models/MIMO_M5_Scene_Bank.pth (K=N 从标签推导, 无语义类名)
# ④ 导出 C 二进制（分类 CNN 权重 cnn_bank_*.bin + 声学路径 + 带通 + 批次指纹 + 配置；K 从 ckpt 推导）
cd ..
python export/export_bin.py                       # → data/*.bin + cnn_bank_info.json (mode=classification)
#    默认自动查找同级目录的 SceneZone_Scene；不同目录用 set GFANC_PYTHON_PROJ=D:\你的路径\SceneZone_Scene
```
> ⚠️ 重计算（离线 FxLMS 收敛 + 打分）较耗时，脚本打印进度；60s 收敛 ≈ 15s/滤波器。**绝对增益已烘焙进库槽**（非归一化）——部署前用 `main.exe` 验证 NR_true，若整库幅度整体偏差用 `--gain-scale` 缩放重生成。

### 🎯 编译（阶段 3 — 必做）

```bash
# 3-① 编译实时版（含 scene_bank.c SFANC 库 I/O；也可直接用 make realtime）
gcc -O2 -Iinclude -D_WIN32_WINNT=0x0601 main_realtime.c src/scene_controller.c src/scene_bank.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c src/howling_detect.c src/sec_online.c src/pa_loader.c -lm -lole32 -o scenezone_realtime.exe

# 3-② 编译离线评估版（可选 — 处理 WAV 算 NR_true / SFANC 硬选验证，见"离线验证"）
gcc -O2 -Iinclude main.c src/scene_controller.c src/scene_bank.c src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c -lm -o main.exe
```

### 🎯 部署运行（阶段 4 — 必做，每次）

> 库已在 2-⑧ ① 生成好（`data/wc_bank.bin`）。日常运行就一条命令——**部署态 = 无误差麦纯开环**（µ=0、无梯度链；1Hz 分类 CNN argmax → 防抖 → crossfade 选库槽）。

```powershell
$env:GFANC_ANC_MODE='fixed'; .\scenezone_realtime.exe
#   启动日志应显示 "MODE = DEPLOY + 库 data/wc_bank.bin (N 槽)"（无库直接 FATAL 退出）
#   每秒打 [BANK] 类日志, 如 "分类 0→1 (filter 1, slot 1, fade 1600)"
#   验证: 放某类噪声 → [BANK] 类日志选的类对 + 人耳听降噪（部署态无 NR 指标, 见下注）
Remove-Item Env:GFANC_ANC_MODE -ErrorAction SilentlyContinue   # 跑完回默认（标定态）
```

> ⚠️ **部署态没有 NR 指标**（无误差麦，NR 显示 n/a）——验证靠两点：①`[BANK]` 类日志选的类对不对；②人耳听降噪有没有变安静。NR/纯音验证（250Hz 纯音 NR ≥ 10dB）只适用于**标定态**（adapt，有误差麦）——它是"标定/自适应链路健康"的验证，不是部署态验收。

> ⚠️ **换摆放后**：重做阶段 1-②（次级路径）+ 1-⑤（反馈路径）+ **重新填库**（2-⑧ ① 重跑 `generate_bank.py`）；1-④（环路延迟）仅换声卡或 `GFANC_BUFFER` 时重做；1-③（主路径）和训练（阶段 2）不随摆放变化——分类 CNN（`cnn_bank`）不用重训（"谱形→类别"映射跨环境复用）。

### 可选步骤（部署不需要，按需再看）

> 走完上面 **阶段 0 → 4 就部署完成**。下面两项只在特定场景需要，用不上就不用看。

- **实机标定填库（备选填库方式，需误差麦）**：2-⑧ ① 离线生成在实机效果不理想 / 换摆放后想就地精修某槽时用。放某噪声 → 闭环收敛自动 `[SAVE]` 存槽 k（槽序 == 分类 CNN 标签 filter_idx）：
  ```powershell
  $env:GFANC_CAL_INDEX='0'; .\scenezone_realtime.exe   # 放噪声形态 0, 收敛后 Ctrl+C
  $env:GFANC_CAL_INDEX='1'; .\scenezone_realtime.exe   # 换噪声形态 1, 收敛后 Ctrl+C（库自动扩到 2 槽）
  Remove-Item Env:GFANC_CAL_INDEX -ErrorAction SilentlyContinue
  ```
  前置：阶段 1 声学路径已测（尤其 1-② 次级路径 + 1-⑤ 反馈路径）；增益旋钮 SIG 常亮 CLIP 不亮，且标定与部署用同一旋钮位置；标定信号用稳态宽带噪声（白噪/粉噪/风扇/马路真实噪声）——**不要扫频、不要单音**。

- **离线评估**（处理一段噪声录音，SFANC 硬选算 NR_true，见"离线验证"）：
  ```bash
  ./main.exe "Noise Examples/road_noise_0-34.wav"
  ./main.exe "Noise Examples/road_noise-15.wav"
  ```

## 项目结构

```
SceneZone-ANC/
│
├── README.md              你正在看的文件
├── Makefile               编译脚本
│
├── main.c                 【离线降噪】主程序 — WAV 文件输入/输出
├── main_realtime.c        【实时降噪】主程序 — 麦克风输入/扬声器输出
│
├── include/               头文件（API 定义）
│   ├── scenezone_types.h      基础类型（FIR 滤波器）
│   ├── fir_filter.h       FIR 滤波器
│   ├── scene_controller.h CNN 分类决策层（argmax + 防抖 → 库槽）
│   ├── scene_bank.h       SFANC 硬选库 I/O（wc_bank.bin 读写）
│   ├── fxnlms_mimo.h      自适应降噪算法（闭环标定 + 开环纯前向 fxnlms_forward_rt_open）
│   ├── howling_detect.h   啸叫检测 + IIR 陷波
│   ├── binary_loader.h    模型加载器
│   └── pa_loader.h        PortAudio DLL 共享加载层
│
├── src/                   源代码（实现）
│   ├── fir_filter.c       FIR 滤波器（双段循环, 零取模）
│   ├── scene_controller.c CNN 分类决策层（minmax→CNN→argmax→防抖选类）
│   ├── scene_bank.c       SFANC 硬选库 I/O（库头校验 / 存槽 / 整库写）
│   ├── fxnlms_mimo.c      FxNLMS 自适应（标定闭环）+ 开环前向 fxnlms_forward_rt_open（部署）
│   ├── cnn_m5_forward.c   M5 分类 CNN 前向（cnn_init_base("cnn_bank") 加载分类权重集）
│   ├── howling_detect.c   啸叫 DFT 检测 + IIR 陷波（逐扬声器独立状态）
│   ├── binary_loader.c    从文件加载模型参数
│   ├── pa_loader.c        PortAudio 运行时 DLL 加载
│   └── calibrate_feedback.c  反馈路径校准（逐扬声器, 16k ZOH×3）
│
├── data/                  模型参数文件（运行时加载）
│   ├── cnn_bank_*.bin     分类 CNN 权重集（部署 argmax 硬选, K=N）
│   ├── wc_bank.bin        SFANC 硬选库（N 条全频段成品滤波器, 16B GFNC 头 + N×(S×L) float32）
│   ├── bandpass_fir.bin   分类 CNN 输入带通（1024tap）
│   ├── bandpass_anc.bin   FxLMS ANC 短带通（64tap, 群延迟 2ms）
│   ├── primary/secondary_path.bin  实测声学路径（可单独重测替换）
│   └── batch_id.bin      批次指纹（导出时写入, 运行时校验）
│
├── docs/                  文档
│   ├── SceneZone_综合审查报告_合并版.md  综合审查（唯一审查文档，七段式）
│   ├── micphone.md        麦克风数据手册
│   └── 2026-07-28_硬件调试记录.md  UMC404HD 面板操作指引 + 调试记录
│
├── tools/                 运维/调试工具
│   ├── coherence_test.py  相干性分析
│   └── b1_feedback_feasibility.py  反馈路径可行性分析
│
└── export/                工具脚本（Python → C 格式转换）
    ├── export_bin.py      导出 C .bin（分类 CNN cnn_bank_*.bin + 路径 + 带通 + 指纹）
    ├── gen_bandpass_fir.py 生成带通 bandpass_fir.mat（阶段 2-⓪）
    ├── generate_bank.py    离线生成 SFANC 硬选库 + 打分打标签（阶段 2-⑧）
    ├── generate_synthetic_noise.py  合成噪声（SFANC-Window 同款, 按频带定类, 阶段 2-⑧ 可选）
    └── measure_*.py        声学路径测量（阶段 1）
```

## 系统架构

系统由两个"环路"组成，协同工作：

> 🧭 **本节描述的是标定/自适应路径**（adapt 模式，误差麦在场）。**部署态（fixed）走 SFANC 硬选库**：决策层 = 「分类 CNN argmax → 防抖 → 选库槽」，前馈环路 = 库槽成品 Wc 纯前向（无梯度链），详见上方「双模式」。

### 慢速环路（部署态，每秒执行一次）— "大脑"

分类 CNN 每秒分析一次 1 秒窗口的带通噪声，输出 K 类 logits → argmax 得类号 → 防抖（`GFANC_BANK_HOLD` 帧连续命中才切换）→ 类变则 crossfade（100ms）到库槽 Wc。**标定态没有慢速环路**——标定 = 零启动闭环 FxLMS（无 CNN、无 warm-start），收敛后自动存库槽。双缓冲机制确保零样本丢失。

```
噪声 → 带通(50-1500Hz) → 1s 窗口 → minmax 归一化 → CNN(M5, K=N 分类) → argmax → 防抖 → 库槽 k
                                                             ↑ 1Hz, 双缓冲+原子交接
```

> 分类 CNN 是「谱形→槽号」判别器（跨环境可复用），库槽是「就地收敛成品滤波器」（绝对增益烘焙，换摆放要重标定填槽）。N=1 静态槽跳过决策层、永远停在槽 0。

### 前馈环路（每秒 16000 次）— "肌肉"

标定态走闭环 FxLMS（有误差麦梯度链），部署/离线走开环纯前向（µ=0、无误差麦、无梯度链）：

**标定闭环**（`fxnlms_tick_rt`，ANC 带通 64tap 群延迟 2ms）：
```
ref → bp_anc(64tap) → x_ref ─┬─→ [Wc ⊗ x_ref] → anti (物理扬声器输出)
                              │
                              └─→ Ŝ ⊗ ref → bp_anc(64tap) → Fx → 梯度更新 (R-58-11)
                                                                      ↑
                                                        err_meas = bp(err_mic) (实测误差直驱)
```

**部署/离线开环**（`fxnlms_forward_rt_open`，xd=NULL，纯前向）：
```
ref → bp_anc(64tap) → x_ref → anti = Wc_bank[slot] ⊗ x_ref → 扬声器输出 (WAV 仿真)
```

**离线评估**（main.exe，与部署同开环信号链）：分类选槽 → 槽内 Wc 纯前向；误差麦信号仍合成（Pri+Ŝ）**仅用于 NR_true 度量，不驱动任何更新**（R-58-10 修复：`es` 去掉二次乘 G 后 NR_true 口径诚实）。

**R-58-10/11 后实时与离线的 Fx 均再过一次 64tap bp_anc（与 `err_meas` 的 bp 路径逐样本对齐）**，消除"误差带通而 Fx 不带通"的梯度相位失配（FxLMS 临界稳定 → Wc 慢漂移，标定时被 cold_hold/adaptive-leak/safety_mute 掩盖）。

### 三层架构

```
┌─ 慢速环路 (1Hz, 主线程, 仅部署态) ────────────────────────────┐
│                                                               │
│  ref → bp_fir(1024tap) → cnn_buf[2][16000] 双缓冲            │
│    │                         │                                │
│    │          CNN M5 分类 (K=N, 运行时推导)                   │
│    │                      ↓ argmax                           │
│    │       class → 防抖 (GFANC_BANK_HOLD 帧连续命中)          │
│    │                      ↓                                  │
│    │       Wc_cur = wc_bank[slot k], 类变 → crossfade         │
│    │                                                         │
├─ 前馈环路 (16kHz, 音频回调) ─────────────────────────────────┤
│                                                               │
│  ref_filt → x_hist[1024] → anti = Wc ⊗ x_ref (直接卷积)      │
│    │            ↓                          ↓                  │
│    │         [Wc⊗ref]                  物理扬声器输出          │
│    │                                                         │
│    └─ 标定态: sec_firs[6] (Ŝ,1024+dsp_delay) → Xd[E×S×L]     │
│        err_meas = bp(err_mic) → ΔWc = -μ·err_meas·Xd/power   │
│        (per-sample LMS) + leak (5e-7) + freeze (max|Wc|>30×init)│
│        部署态: xd=NULL, 无梯度链 (fxnlms_forward_rt_open)     │
│                                                               │
├─ 辅助 ───────────────────────────────────────────────────────┤
│                                                               │
│  反馈抵消: fb_fir[2] FIR(512tap) 逐扬声器校准                  │
│  啸叫检测: DFT 256pt + IIR notch ×2, 可配阈值 (默认12)        │
│  在线Ŝ辨识: sec_online NLMS, μ=5e-6, 零探测噪声                │
│  冷启动保护: soft-release 前1s cap0.12(梯度冻结) 后1s cap→1.0  │
│  Ŝ环路延迟补偿: 自动加载 sec_bulk_delay.bin (dsp_delay)        │
│  环境安静检测: 噪声停→freeze+衰减Wc, 弱噪声哨兵守卫 (P0-5 v1.9) │
│  NR指标: 分散采样250点 + ±30dB限幅                             │
│  anti_total = anti_ff + anti_fb → 限幅±1.0 → 线性内插×3 → DAC │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

## 反馈路径校准

反馈抵消功能需逐扬声器校准声学路径。校准命令见上文 **完整命令流 · 阶段 1-⑤**（`calibrate_feedback.exe` → `feedback_path_0/1.bin`）。

校准完成后运行 `./scenezone_realtime.exe`，启动日志显示：
```
Feedback spk0: 512 taps, RMS=0.0012
Feedback spk1: 512 taps, RMS=0.0011
```

文件缺失时反馈抵消自动禁用，日志显示 `Feedback cancel: disabled`，不影响降噪但可能引发啸叫。

> ⚠️ **当前 PC 测试台暂弃反馈对消（2026-08-13, R-50）**：重标定的 512tap FIR 经 FFT 复核在 250Hz 增益 0.38/0.71、125Hz 0.86/0.99，比实测真实反馈增益 ~0.17 超估 ~4×（白噪声 NLMS 无逐块对齐/ERLE 门，把低频/直流烤进系数）→ 反馈开启实机发散。反馈文件已改名 `feedback_path_{0,1}.bin.disabled`，运行时自动跳过（= 当前稳定配置）。恢复：`mv data/feedback_path_0.bin.disabled data/feedback_path_0.bin`（0/1 各一次）。嵌入式样机反馈仍为刚需，待逐块对齐 NLMS + ERLE 门 + 窄带/白化激励 + 参考麦高通后重做。

## 次级路径测量（Python，Farina 扫频法）

提供 Python 指数正弦扫频测量工具，与 C 实时系统解耦。**测量命令见上文 完整命令流 · 阶段 1-②**（需从 `SceneZone_Scene` 目录运行，脚本用相对路径找 `Primary and Secondary Path/`）。工具额外支持：

- `--interactive`：首次使用配置声卡设备
- `--duration <秒>` / `--repetitions <次数>`：延长扫频 / 多次重复时域平均，提高 SNR
- `--amplitude <0..1>`：调探针响度（默认 0.95，削波就调小）


方法: Farina 2000 AES 指数扫频，5s 扫频 20-7500Hz，多次重复时域平均，自动反卷积提取脉冲响应。相比白噪声 NLMS 法，SNR 高 10-20dB，天然免疫时钟滑移。

**三个校准工具的分工**（换位置/几何/声卡时按需重测，不重复）：
- **Ŝ 内容 → 扫频法**（`measure_secondary.py` + `export_bin.py` → `secondary_path.bin`）：每次换位置/几何/扬声器必测，运行时**默认加载**它。
- **环路延迟 → `calibrate_secondary.exe`**（→ `sec_bulk_delay.bin`）：首次 / 换声卡或 `GFANC_BUFFER` 后必测，运行时自动补偿。v5 起只测环路延迟，不产出 Ŝ。
- **反馈路径 → `calibrate_feedback.exe`**（→ `feedback_path_0/1.bin`）：防啸叫；换摆放后出现啸叫时重测。

**Ŝ 文件选择 + 环路延迟补偿（v1.4）**：
- 运行时**默认加载 `data/secondary_path.bin`**（扫频法产物）；`GFANC_SEC_FILE` 环境变量可强制指定其它文件。启动日志打印 `Ŝ file: ...`。
- **环路延迟只由 `calibrate_secondary.exe` 测**：生成 `sec_bulk_delay.bin`（总环路延迟 @16k），运行时自动换算 `dsp_delay` 补偿 FxLMS 对齐（启动日志 `Loop delay auto-loaded` / `Ŝ model delay`）。缓冲大小用 `GFANC_BUFFER` 调（默认 128 样本），实测 UMC ASIO + 128 帧环路 ≈ **12.4ms**（该值为 I/O+声学环路；控制路径另有 ANC 带通群延迟 2ms。旧 0.01s suggestedLatency 会把驱动顶到 512 样本 → 30ms，已修复）。
- ⚠️ **每次换安装位置/几何后必须重测**（管道/桌面/窗户声学不同），否则实时 NR 会下降。

## 在线次级路径辨识

系统启动后持续跟踪 Ŝ（扬声器→误差麦）的缓慢变化（温湿度、老化）：

- **算法**: NLMS，μ=5e-6（极慢，~0.1%/s），零探测噪声
- **激励**: 利用 ANC 自身 anti 输出作为宽带激励信号
- **更新**: 仅在正常模式（非 mute/fade/howling）下更新 sec_coeffs 原地
- **禁用**: `$env:GFANC_SEC_MU = "0"`

## 冷启动保护

启动瞬间输出从 0 平滑渐入，防止零启动 Wc 的瞬时 overshoot（可闻嗡嗡/啸叫）：

| 机制 | 参数 | 作用 |
|------|------|------|
| 冷启动软释放 | cold_hold 2s | **前 1s** anti 上限 ±0.12 且梯度冻结；**后 1s** 上限线性 0.12→1.0 且梯度活跃（Wc 在输出受界内自适应收敛）——避免硬释放时未收敛 Wc 的瞬态输出激起反馈啸叫 |

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
| CNN 输出维 (K) | 分类 CNN: N (=库槽数, 从 ckpt linear weight 推导) | `cnn_bank_*.bin` 分类集, argmax 硬选库槽 (无激活) |
| 滤波器长度 (L) | 1024 tap | 控制滤波器 Wc, 频域分辨率 ~15.6Hz |
| CNN 带通 / ANC 带通 | 1024 / 64 tap | 分类 CNN 输入用 1024(分辨率), FxLMS 用 64(群延迟 2ms) |
| 带通频率 | 50-1500 Hz | ANC 有效频率范围 |
| 输入预增益 | 自适应 (env: GFANC_MIC_GAIN) | 自动标定到 TARGET_REF_RMS=0.03 |
| 步长 (μ) | 1e-7 (基准, Ŝ RMS 自动缩放; env: GFANC_STEP) | LMS 自适应步长 (仅标定态) |
| 变步长 (VS-LMS) | 双 EMA 尖峰检测, 突发降步至 5% | 误差相对自身基线跳变→降步防反馈过冲; 平滑收敛全速 (2026-08-05) |
| 泄漏因子 | 5e-7 (基准, 自适应; env: GFANC_LEAK) | Wc 正则化 (2026-08-05 降档 5e-6→5e-7: 弱信号下 Wc 能长起来) |
| 输出限幅 | ±1.0 | DAC 满幅保护 + NaN/Inf 防护 |
| ANC 运行模式 | adapt=标定闭环 / fixed=部署开环 (env: GFANC_ANC_MODE=adapt\|fixed) | fixed=无误差麦 SFANC 硬选: 加载 data/wc_bank.bin 整库 + CNN 分类 argmax 选槽, **无库直接 FATAL**; 详见下节「双模式」 |
| SFANC 库文件 | `data/wc_bank.bin` (env: GFANC_BANK_FILE) | 16B 头 {magic "GFNC", version, n_slots, slot_len=S×L} + N×(S×L) float32 成品滤波器; 槽序 == 分类 CNN 标签 filter_idx |
| 库槽数 N | 由文件推导 (n_floats/S×L) | 加载期校验 N == 分类 CNN K; 库槽 = 离线/实机 FxLMS 收敛成品 (绝对增益烘焙, 非归一化) |
| 分类防抖 | 2 帧 (env: GFANC_BANK_HOLD) | 候选类连续命中 N 帧才切换, 防噪声抖动误切 |
| 标定写槽 | 0 (env: GFANC_CAL_INDEX) | 闭环收敛自动保存写到库槽 k; 每槽标定一次填一槽, 或离线生成 (generate_bank.py) |
| 库轮换模拟 | 关 (env: GFANC_BANK_SIM=1 开, GFANC_BANK_SIM_SEC=3) | 部署态定时轮换库槽 (绕过 CNN), 验证切换无爆音 (实机/离线调试用) |
| 类变 crossfade | 1600 样本 (100ms) | CrossFader, =20Hz×2 周期 |
| 嵌入式处理延迟 | 3ms 默认 (env: GFANC_EMBED_DELAY_MS) | 离线 pad Ŝ 模拟 ADC+DSP+DAC 因果缺口 (v1.5) |
| 冷启动 ramp | 400ms | 输出从 0 平滑渐入 |
| 冷启动软释放 | 前 1s cap0.12(梯度冻结), 后 1s cap→1.0 | 消除启动啸叫 (v1.2) |
| Wc 发散救援 | anti_rms>0.25 且 err/ref>0.6 且 err_ref 逐秒上升 0.1, 连续 2s (env: GFANC_DIVERGE_ERR_RATIO) | P0-4 三重门控: 回滚 Wc + 重新 ramp; 防误杀健康深对消 (纯 anti 阈值会把 err_ref 达 1.3 的收敛中 Wc 误回滚) |
| Wc 发散冻结 | max\|Wc\| > 30×wc_init_max | 自动冻结 LMS 梯度 (自适应基准) |
| 在线 Ŝ 辨识 | μ=5e-6 (env: GFANC_SEC_MU) | NLMS, 零探测噪声 |
| 音频缓冲 | 128 样本 (env: GFANC_BUFFER, 32-1024) | ASIO buffer; 越小延迟越低但易爆音 (128 稳定甜点, 实测环路 ~12.4ms) |
| Ŝ 环路延迟补偿 | 自动 (sec_bulk_delay.bin / GFANC_DSP_DELAY) | FxLMS 对齐, bench 实测 ~12.4ms (原 0.01s suggestedLatency 顶回 512 致 30ms, 已修复) |
| 反馈 FIR | 512 tap ×2 扬声器 | 逐扬声器独立校准 (PC 测试台暂弃, R-50) |
| 啸叫陷波 | DFT 256pt, IIR ×2 | 可配阈值 (env: GFANC_HW_THRESH, 默认 12) |
| NR 指标 | 分散采样 250 点 + ±30dB 限幅 | 防噪声基底虚高 (v1.2 BUG-1) |
| 环境安静检测 (进入) | anti>0.02 && ref<0.045 && err/ref>1.5 && 曾有大噪声(20s 内), 持续 3s (env: GFANC_QUIET_ANTI/REF/ERR_REF/HOLD/MEMORY) | v1.9 (P0-5): 噪声停后冻结梯度+衰减 Wc, 治"扬声器继续输出残余反相声/嗡嗡声"; **哨兵守卫**防宽带弱噪声 (ref≈0.038) 被绝对阈值误判"噪声消失"而砍掉反相 |
| 环境安静检测 (退出) | ref 重回 1.5× 或 err 重回 2.0× 安静基准 (env: GFANC_QUIET_EXIT/ERR_EXIT) | 噪声回归 → 重建 INIT; 纯音回归 ref 只高 20% 靠 err 通道兜底 |
| CNN 推理 | ~8ms/次 @1Hz | 静态缓冲, 无动态分配 |
| 回调预算 | ~30-45% (SIMD ~5-10%) | 已优化: 双段循环零取模, 含安全边际 |

### 双模式（标定 / 部署）— SFANC 硬选库, 2026-08-22

设计文档: [docs/无误差麦方案_与SFANC对照_路线分析.md](docs/无误差麦方案_与SFANC对照_路线分析.md)

**架构（v2.0, Phase 1-3）**：误差麦只在**标定**用。标定态 = 现有闭环 FxLMS + 误差麦；部署态 = **纯开环**（µ=0、无误差麦、无梯度链、不加载误差通道），靠「N 条全频段成品滤波器库 + CNN 分类 argmax 硬选」降噪——这是"窗户开口 ANC + 部署无误差麦"的 canonical 形态（与 SFANC-Window / MIMO-SFANC 同构；运行时 FxLMS 因硬依赖误差麦而出局）。

| 模式 | 命令 | 行为 |
|------|------|------|
| **标定**（adapt，默认） | `.\scenezone_realtime.exe` | 闭环 FxLMS（零启动，无 CNN/warm-start）+ 误差麦 + 在线 Ŝ；**Wc 收敛稳定后自动保存** 写库 `data/wc_bank.bin` 槽 `GFANC_CAL_INDEX`（不用盯日志/掐 Ctrl+C） |
| **部署**（fixed） | `$env:GFANC_ANC_MODE='fixed'; .\scenezone_realtime.exe` | 开环 µ=0 无误差麦。加载 SFANC 库 `data/wc_bank.bin` 整库 → 1Hz CNN（`cnn_bank`，K=N 分类）argmax → 防抖 2 帧 → 类变则 crossfade（100ms）到库槽；**无库直接 FATAL 退出** |

**标定流程（单滤波器全自动，零环境变量）**：
1. 插上误差麦，跑闭环：`.\scenezone_realtime.exe`，放**稳态宽带噪声**（白噪/粉噪，或风扇·空调·马路等真实噪声都行）
2. **什么都不用管** —— 滤波器收敛后日志自动出现 `[SAVE] 标定完成! ... 库槽 %d 已自动保存` 即存好，之后 `Ctrl+C` 退出
3. 之后随时切开环：`$env:GFANC_ANC_MODE='fixed'; .\scenezone_realtime.exe`（误差麦拔不拔都行）

**多槽填库（SFANC 硬选库，Phase 3）**——给部署态分类选库填充 N 个槽（离线生成或逐槽实机标定，二选一；无语义场景名）：
```powershell
# A. 离线生成（推荐）— 阶段 2-⑧ ①: 每条代表噪声 → 一条成品 → 一个槽
#    f_*.wav 是你自己录的噪声（想消什么就喂什么），脚本不生成这些文件
python export/generate_bank.py --filters "你的噪声1.wav" "你的噪声2.wav" "你的噪声3.wav" -o data/wc_bank.bin

# B. 逐槽实机标定（可选）: 放某噪声形态 k → 收敛自动存槽 k（槽名 = 你放的噪声形态）
$env:GFANC_CAL_INDEX='0'; .\scenezone_realtime.exe    # 收敛后 [SAVE]，Ctrl+C
$env:GFANC_CAL_INDEX='1'; .\scenezone_realtime.exe    # 换噪声形态, 存槽 1
# ... 槽 k=0..N-1 逐个标定
Remove-Item Env:GFANC_CAL_INDEX   # 清环境变量
```
> **槽序 == 分类 CNN 标签 filter_idx == `cnn_bank_info.json` classes**（`filter_0..filter_{N-1}`，无场景名）。槽 k 对应 = 生成它的那段噪声（`generate_bank.py` 的 `--filters` 顺序或实机标定时放的噪声形态）。部署加载期校验 `N(库) == K(CNN)`，不符告警并把 argmax 钳到 N-1。

> 💡 **槽位 = 滤波器，无场景层**：槽 k 是一条全频段成品滤波器（`scene_bank_slot` 返回 `data + k×slot_len`，每条 = S×L 个 float）。SFANC-Window / MIMO-SFANC 的 "CNN 在 N 条固定滤波器里选" 与这里**完全同构**，区别只在条目来源：他们离线训练合成/实验室噪声得成品，你离线 FxLMS 收敛（或实机标定）得成品——**无语义场景，槽 k 就是"第 k 条滤波器"**。**N>1 才有"选"**：部署加载后只有 `bank_n_slots>1` 才走分类选槽（[main_realtime.c:1478](main_realtime.c#L1478)），N=1 时决策层被跳过、永远停在槽 0 = 静态单滤波器。所以**没填满库之前它只是个不挑的静态滤波器**——生成/标定填满 N 槽后，分类 CNN 才真正开始按噪声类型换滤波器。

**分类选库决策层（部署态慢循环，1Hz）**：带通噪声 → `cnn_bank` 分类 CNN（K=N logits，无激活）→ argmax → 候选类需连续 `GFANC_BANK_HOLD`（默认 2）帧命中才切换（防噪声抖动误切）→ 类变则 `wc_old`=当前播放、`wc_cur`=库槽 c、启动 crossfade（delayless，防爆音）。弱信号/CNN 失败保持当前类不换槽。库槽 Wc 为**离线/实机收敛成品（绝对增益烘焙）**，开环有效降噪的命门。

> **标定信号用稳态噪声，不要用扫频，也别用单音**：固定滤波器要的是覆盖全频段的 -P/S。扫频频率在动，最优滤波器跟着动，Wc 永远稳不下来——存出来的是"扫到那个频率时"的窄带滤波器，固定模式下只能消那一个频段，宽带降噪听不出来（这正是"怎么测都不行"的常见原因）。单音同理（只消单频）。稳态宽带噪声让 Wc 收敛到全频段固定滤波器，固定模式才能宽带降噪。音量适中防啸叫（反馈抵消需先跑 `calibrate_feedback.exe`）。

> **绝对增益烘焙（开环降噪命门）**：库槽保存的是**就地 FxLMS 收敛的成品 Wc 原样**（完整绝对振幅+相位，**绝不 RMS 归一化**）——这是 SFANC/MIMO-SFANC 离线收敛成品的形态，开环才真正可闻降噪。改动摆放/换设备后重跑一次标定即可。

部署态行为要点:
- 误差麦: err_meas 置 0（无误差麦语义）, NR 显示 n/a
- 决策层: 1Hz CNN argmax → 防抖 → 类变 crossfade；`GFANC_BANK_SIM=1` 定时轮换类（绕过 CNN），验证切换无爆音
- Wc 变更点全部 gate: 静音/peak 的 Wc 衰减、peak halve 均跳过（库槽成品不被瞬态削减）
- 输出安全保留: NaN 看门狗 + 软限幅 + cold-start ramp
- 发散检测跳过（无梯度不可能发散）

### 参数 ↔ 降噪量实测速查

> 离线版现在是**纯开环 SFANC 硬选**（无梯度链），不再有闭环离线收敛调参——`GFANC_STEP`/`GFANC_LEAK` 等只作用于**标定态**（实时闭环），保持默认即可（零启动标定不依赖步长微调）。开环降噪量 = 库槽成品与本段噪声的匹配度，用 `main.exe` 直接评估（见[离线验证](#离线验证)）。

> ⚠️ 离线 NR 上限由三堵墙主导：**延迟**（因果缺口 ≈ −1.9ms，见[窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）/ **相干** / **空间**。实时 NR_est 还依赖 Ŝ 模型+误差麦灵敏度、误差麦降太狠会虚高——**唯一真值 = 离线 NR_true，实时以人耳为准**。

## 代码结构

C 实现已超越原始 Python 参考（新增实时 ASIO 音频栈、啸叫检测、反馈抵消、发散保护、双缓冲等模块），是独立的工程实现。

| 文件 | 功能 |
|------|------|
| `main.c` | 离线降噪 (纯开环 SFANC 硬选唯一路径): WAV 输入/输出, 整库加载 + CNN 分类 argmax 选槽, 库槽 Wc 纯前向 (`fxnlms_forward_rt_open`) + 64tap ANC 带通 (与实时同信号链); `GFANC_FORCE_CLASS` 强制选槽量化错选代价, `GFANC_BANK_SIM` 轮换类验证切换 |
| `main_realtime.c` | 实时降噪: ASIO 音频, Ŝ 环路延迟自动补偿; **标定/部署双模式** (标定 = 零启动闭环 FxLMS + 收敛自动存槽; 部署 = SFANC 硬选: 整库加载 + CNN argmax 防抖选槽 + crossfade) |
| `src/scene_controller.c` | SFANC 分类决策层: minmax 归一化 → CNN 分类 (K=N) → argmax → `bank_hold_frames` 防抖 → 选库槽 |
| `src/scene_bank.c` + `include/scene_bank.h` | SFANC 硬选库 I/O: `scene_bank_load`/`save_slot`/`write_all` + 16B GFNC 头校验 |
| `src/fxnlms_mimo.c` | FxNLMS 自适应 (标定闭环, anti-windup, 自适应 leak) + `fxnlms_init_forward`/`fxnlms_forward_rt_open` 开环纯前向 (部署, xd=NULL) |
| `src/cnn_m5_forward.c` | M5 分类 CNN 前向推理 (单例宏); `cnn_init_base("cnn_bank")` 加载分类权重集, K 从 linear_weight 推导 |
| `src/fir_filter.c` | FIR 滤波器 (gfanc_delay_t 双精度累加) |
| `src/howling_detect.c` | DFT 频谱峰值检测 + IIR 双二阶陷波 (逐扬声器独立状态) |
| `src/sec_online.c` | 在线 Ŝ NLMS 辨识 (零探测噪声, 原地更新 sec_coeffs) |
| `src/pa_loader.c` | PortAudio ASIO DLL 运行时加载 |
| `src/calibrate_feedback.c` | 反馈路径 NLMS 校准 (逐扬声器, 16k ZOH×3 激励) |
| `src/calibrate_secondary.c` | 环路延迟/滑移测量（→ `sec_bulk_delay.bin`），v5 只测延迟不再产出 Ŝ |
| `src/binary_loader.c` | .bin 二进制权重文件加载 (v2 格式, GFNC 头+CRC32) + 批次指纹校验 |
| `include/scenezone_types.h` | 集中参数 + 分级日志 + 维度宏 |
| `include/scene_manager.h` | 共享纯函数 (main.c + main_realtime.c 共用): `sm_wc_max_abs`/`sm_check_divergence`/`sm_check_convergence` |
| `include/sec_online.h` | 在线 Ŝ 辨识 API |
| `include/cnn_m5_forward.h` | CNN 实例化 API |
| `export/export_bin.py` | PyTorch → C .bin 导出 |
| `export/measure_secondary.py` | Python 次级路径测量 (Farina 扫频法) |
| `export/measure_primary.py` | Python 初级路径测量 |
| `export/measurement/` | 测量核心模块 (扫频生成/反卷积/质量检验) |
| `SceneZone_Scene/` | Python 项目 (训练代码 + 模型权重 + 声学路径测量数据) |

## 离线验证

离线模式 (`main.exe`) 是**纯开环 SFANC 硬选唯一路径**：加载整库 `data/wc_bank.bin` + 分类 CNN `data/cnn_bank_*.bin`，CNN 分类 argmax → 防抖 → 选库槽，库槽 Wc 纯前向输出（无 FxLMS 闭环、无梯度）。**无库/无 CNN 直接 FATAL 退出**（无 `wc_fixed`/CNN 生成式兜底）。

> ⚠️ 离线 NR_true 需要误差麦模型（Pri）合成误差，仅能验证**选类正确 + 切换无爆音**；真实开环降噪量以实机部署为准（部署态无 NR 指标，NR 显示 n/a）。

| 指标 | 说明 |
|------|------|
| 开环降噪量 | = 库槽成品与本段噪声谱的**匹配度**（选对槽才有降噪，选错槽=反相更差） |
| 分类 CNN 输出维 (K) | 4 (=库槽数, 从 `cnn_bank_linear_weight.bin` 推导) |
| 验证手段 | 看日志 `[BANK] 类 k/N` 是否逐秒稳定 + 类变 crossfade 无爆音 |

> 标定态历史实测（2026-08，闭环 FxLMS 收敛后 NR_true）：mixed_7types_56s **+9.8 dB** / road_noise_0-34 **+9.6 dB** / road_noise-15 **+9.2 dB**（R-58-10 去掉 es 二次乘 G 的口径 bug 后为真实值）。离线已改开环硬选后不重复跑闭环基准——**离线 G 只是仿真工作点，与硬件旋钮无关；`GFANC_MIC_GAIN=1` 仍可看 G=1 口径**。

> ⚠️ 离线默认 `GFANC_EMBED_DELAY_MS=0`（R-58-8：训练世界无此延迟，3ms 加在 Ŝ 上 → anti 相位错位 48 样本 → 正反馈发散）。基线净预览 ≈ −1.9ms（64tap ANC 带通群延迟 1.97ms > 初级路径提前量 0.69ms）——随机宽带对消受限，只能消窄带/低频；纯宽带 road_noise 加多大预览都封顶 ~0.8dB（相干/空间墙，2 扬声器/3 误差麦几何）。实时受 PC 控制路径延迟限制更重（净预览 ≈ −9ms；详见 [窗户ANC可行性](docs/窗户ANC可行性-因果限制_实测_方案.md)）。**实时实测（2026-08-03 bench，误差麦位置、相对安静）NR 平均 ~13dB**，多数秒 7-18dB；窗户安装态重测 Ŝ 后待进一步验证。NR 读数仅对误差麦位置有效（静音区 ≈ λ/10），人耳远离误差麦时降噪下降。

### 离线 SFANC 硬选验证

部署态决策层（CNN 分类 argmax → 选库槽）是离线版**唯一路径**，与实时同构：

```bash
# 开环硬选: 加载 data/wc_bank.bin 整库 + cnn_bank_*.bin, 1Hz 分类 CNN argmax → 防抖 → 选槽 crossfade
./main.exe "Noise Examples/road_noise_0-34.wav"
#   日志逐秒打印 "[BANK] 类 k/N (filter k, slot k, fade)" 类号/槽号

# 强制选槽: 量化"选错库槽"代价 (开环错选=反相更差)
$env:GFANC_FORCE_CLASS='2'; ./main.exe "Noise Examples/road_noise_0-34.wav"

# 轮换类: 每 GFANC_BANK_SIM_SEC 秒轮换一槽, 验证切换无爆音
$env:GFANC_BANK_SIM='1'; ./main.exe "Noise Examples/road_noise_0-34.wav"
Remove-Item Env:GFANC_FORCE_CLASS, Env:GFANC_BANK_SIM -ErrorAction SilentlyContinue
```

## 常见问题

**Q: 为什么 offline 结果因文件而异（有些文件没降噪甚至更差）？**
A: 离线是**纯开环 SFANC 硬选**，降噪量 = 库槽成品与本段噪声谱的匹配度。库槽匹配的文件才有降噪，不匹配的（选错槽）反相更差——这正是硬选架构的特性：决策层的价值在于**选对槽**。选类是否正确看日志 `[BANK] 类 k/N` 是否逐秒稳定。标定态历史闭环基准（+9.8/+9.6/+9.2 dB）见[离线验证](#离线验证)，只作参考——离线已改开环后不重复跑闭环收敛。

**Q: 为什么 error_out.wav 听起来比原噪声还大？**
A: error_out.wav 是 3 声道文件（对应 3 个麦克风位置），播放器同时播 3 个声道叠加后音量更大。另外，ANC 只在 50-1500Hz 有效，高频部分反而增加了少量能量。降噪效果要看表格里的 NR_true（离线真值）或实时 NR 数字，不要用耳朵直接听 error_out.wav。

**Q: 实时模式怎么验证效果？**
A: 终端每秒输出 NR(dB)、err/anti RMS、啸叫状态。NR > 3dB 表示有效降噪。

**Q: 放马路噪音为什么没降噪，声卡 SIG 灯也不亮？**
A: 分两层看。① **信号没进来**：马路噪音从笔记本外放出来，低频被扬声器滤掉，进参考麦只比底噪高 ~20%（refFilt≈0.038 vs 底噪 0.030），SIG 灯不亮 = 系统"没听到"。ECM8000 平直到 20Hz，不是麦克风瓶颈，瓶颈是播放源发不出低频。② **安静检测误杀**（v1.9 已修）：弱噪声 ref 低于绝对门槛会被误判"噪声消失"而砍掉反相，哨兵守卫要求"ref 曾在 20s 内高于门槛"才允许安静判定，弱噪声从启动就在则永不误判。**验证点：放噪声时 SIG 灯必须常亮、refFilt ≥ 0.05（2-3× 底噪），250Hz 纯音 NR ≥ 10dB 才说明系统正常**。

**Q: 可以处理其他采样率的文件吗？**
A: 离线模式自动将输入重采样到 16000 Hz。支持 16-bit PCM WAV。

**Q: 实时版使用什么音频 API？**
A: PortAudio 运行时加载 (`libportaudio64bit-asio.dll`)，支持 ASIO / WASAPI / WDM-KS 后端，通过 `src/pa_loader.c` 动态加载 DLL。

**Q: 启动时打印 `[WARN] 批次混配检测` 是什么？要紧吗？**
A: 表示 `data/` 里的 **分类 CNN 权重（`cnn_bank_*.bin`）/ 带通（`bandpass_fir.bin` / `bandpass_anc.bin`）** 不是同一次 `export_bin.py` 导出的（比如只拷了某个旧的 `cnn_bank_*.bin` 进来）。SFANC 硬选下分类 CNN 与带通必须同源，混配会让特征失真、选错槽。部署态为纯开环，**没有 FxLMS 兜底**，混配建议**重跑一次 `python export/export_bin.py`** 让整批一致，指纹警告即消失。注意：`secondary_path.bin`、`feedback_path_*.bin` 等声学路径文件是**故意不参与**指纹的（按摆放可单独重测），替换它们不会触发此警告。
