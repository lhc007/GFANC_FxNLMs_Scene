# 代码审查报告 — GFANC FxNLMS 窗户开口降噪系统

> 审查日期：2026-07-16
> 审查范围：`main_realtime.c`、`src/fxnlms_mimo.c`、`src/scene_controller.c`、`src/fir_filter.c`、`src/howling_detect.c`、`src/cnn_m5_forward.c`、`src/binary_loader.c`、`src/calibrate_feedback.c` 及相关头文件
> 参考论文：*Real-time Implementation and Explainable AI Analysis of Delayless CNN-based Selective Fixed-filter Active Noise Control*
> 
> **状态更新 2026-07-22**：F-D (leak解耦) ✅ 已修复 · B-2 (陷波串状态) ✅ 已修复 · F-A/F-B 代码已回退待硬件验证恢复 · 新增 MinMax/CNN返回值/NaN保护 三项修复

---

## 1. 总体评估

代码整体模块划分清晰、离线仿真移植质量高，但**实时环路直接沿用了离线仿真的信号结构（合成误差 + 双重次级路径滤波），这在真实声学环境中是结构性错误**（理论 NR 上限被压到约 6dB）；此外架构描述中的"固定反馈 IIR 环路"（8 阶 IIR 20-200Hz）在代码中**并不存在**，且存在跨线程数据竞争、无抗混叠重采样、次级路径未实测等多个高危问题。

---

## 2. 架构一致性

### 2.1 慢速 CNN 环路 — ✅ 存在，但与描述规格不符

- 实现位置：主线程循环 `main_realtime.c:413-499`，每积满 1 秒（16000 样本）触发一次 `scene_ctrl_process`，轮询周期 `Sleep(100)`。
- **与描述的"17ms 异步"不符**：实际为 **1000ms 帧 + 最高 100ms 轮询抖动**。论文的 "Delayless" 指 CNN 决策不阻塞控制滤波（这点满足），但决策频率是 1Hz，不是 17ms。
- 数据交接机制：`cnn_buf`/`cnn_cnt` 采用"填满即停 → 主线程处理 → 清零重填"的握手（`main_realtime.c:168`、`main_realtime.c:497`）。逻辑上避免了读写重叠，**但 `cnn_cnt` 非原子非 volatile，形式上是数据竞争（C 标准 UB）**；且 CNN 推理 + 轮询期间的样本被直接丢弃，相邻分类窗之间存在 30-150ms 空洞（对场景分类影响不大，但应知晓——它不是环形缓冲，是"填满-丢弃"缓冲）。

### 2.2 实时前馈 FxNLMS 环路 — ⚠️ 存在，但信号结构是离线仿真的错误移植

- 实现位置：PortAudio 回调 `main_realtime.c:129-273`，逐样本 @16kHz（62.5μs）✅。
- **核心问题**：`fxnlms_tick` 的误差构造 `err = disturbance + anti_est`（`fxnlms_mimo.c:66-68`）是为**离线仿真**设计的——离线版 `dist` 来自初级路径卷积、不含反噪声（见 `main.c:354-361`）。实时版把**误差麦实测信号**（已经包含真实声学反噪声）当作 `dist` 传入（`main_realtime.c:185-197`），再叠加一次模型估计的 `anti_est` —— **反噪声被计入两次**。详见 §5.2-A。

### 2.3 固定反馈 IIR 环路 — ❌ 不存在

代码中**没有**任何"误差麦 → 固定 8 阶 IIR 带通(20-200Hz) → 扬声器"的反馈 ANC 控制器。实际存在的是两个不同的东西：

| 架构描述中的模块 | 代码中实际存在的 |
|---|---|
| 固定反馈 IIR 控制器（8 阶级联双二阶，20-200Hz 带通） | **无** |
| — | 反馈**路径抵消** FIR（256 taps，`main_realtime.c:153-159`）：消除扬声器→参考麦声泄漏，属于前馈环路的辅助，不是反馈控制器 |
| — | 啸叫陷波 biquad（`src/howling_detect.c`）：最多 2 个二阶陷波，是安全机制，不是带通控制器 |

同样，架构描述中的 2048/128 抽头、±0.5 限幅、17ms 均与代码不符（实际 L=1024×S=2 共 2048 系数、限幅 ±1.0、CNN 周期 1s）。**架构文档与代码已严重脱节，需先对齐再谈一致性。** 若目标是论文同等效果的三层结构，反馈 IIR 环路需要从零实现。

### 2.4 执行频率与实时约束划分

| 环路 | 声称 | 实际 | 满足实时约束？ |
|---|---|---|---|
| CNN 慢速环路 | 17ms 异步 | 1s 帧 + 100ms 轮询，主线程执行 | ✅ 不阻塞回调 |
| 前馈 FxNLMS | 62.5μs/样本 | 回调内逐样本 @16kHz | ⚠️ 计算量逼近 2ms 回调预算上限（见 §6.1） |
| 反馈 IIR | 62.5μs/样本 | 不存在 | — |

---

## 3. 硬编码值

| 数值 | 位置 | 评估 | 建议 |
|---|---|---|---|
| `FS_HW=48000` / `FS_ANC=16000` | `main_realtime.c:20-21` | 结构性参数，编译期常量合理 | 保留宏，3:1 比率应派生（`FS_HW/FS_ANC`）并 `_Static_assert` 整除 |
| `E=3, S=2, L=1024` | `main_realtime.c:22-24` | 合理为编译期常量，但与 `SC_S`、`sub_len/(15*2)` 重复定义 | 与 `scene_controller.h:9-11` 统一到一个头文件；加载 `.bin` 后校验长度一致 |
| `BP_LEN=1024, SEC_LEN=1024` | `main_realtime.c:25-26` | **危险**：从文件加载却假设长度，无校验 | 用 `bin_load_float` 返回值校验，长度不符即报错退出 |
| `DSP_DELAY=16`（1ms） | `main_realtime.c:27`、`346-352` | **不合理**：WASAPI + 96 帧缓冲的真实 I/O 往返延迟远大于 1ms（典型 10-30ms） | 必须实测（可复用反馈校准的 NLMS 代码），存入配置文件；见 §5.2-B |
| `FADE_LEN=16`（1ms） | `main_realtime.c:28` | 1ms 交叉淡化对 1024 抽头滤波器近似硬切换 | 建议 160-1600（10-100ms），运行时可配 |
| `MIC_PRE_GAIN=10.0f` | `main_realtime.c:29` | 硬件相关整定值，换硬件即失效 | 移入配置文件；配合路线图中的自动增益标定 |
| `MIC_CLIP_MAX=1.0f` | `main_realtime.c:30` | 值合理，但配套 tanh 限幅实现有不连续 bug（§5.2-C） | 修 bug 后可保留 |
| `COLDSTART_MS=400` / `MUTE_HOLD_MS=1500` | `main_realtime.c:31-33` | 经验值，与 1s RMS 评估周期耦合（注释已说明） | 运行时可配；`MUTE_HOLD` 应表达为"覆盖 N 个 RMS 周期"而非独立毫秒数 |
| `FB_LEN=256` | `main_realtime.c:35` | 与 `calibrate_feedback.c` 的 `FB_TAPS=256` 重复且必须相等 | 提取到共享头文件，或把长度写入 `.bin` 文件头 |
| 输出/anti 钳位 `±1.0` | `main_realtime.c:203-204, 266-267` | 合理（DAC 满幅），但与架构文档的 ±0.5 不符 | 做成可配置 `output_limit`（路线图 config.json 已规划） |
| `0.0001f, 1e-5f`（μ、leak） | `main_realtime.c:385` | μ 量级合理；**leak 实际无效**（见 §5.2-D） | 移入配置；leak 语义修正 |
| `cos_sim < 0.8f`、`NR>3dB`、连续 3 帧 | `main_realtime.c:459-471` | 滞回阈值属整定参数 | 运行时可配 |
| `probs[8]`、`k<8`、`8*sizeof(float)` | `main_realtime.c:416, 432, 496` | 魔数，应为 `SC_K` | 全部替换为 `SC_K` |
| `15*2`（打印 L 用） | `main_realtime.c:331` | 魔数 = `SC_C*SC_S` | 替换 |
| `96`（framesPerBuffer）、`0.01`（latency）、`6`/`2`（通道数）、`paWASAPI=6` | `main_realtime.c:394-397, 318` | 硬件绑定值散落在调用点 | 集中为配置块；打开流前校验设备通道数/采样率支持 |
| `16000.0f` 字面量 ×4 处 | `howling_detect.c:135, 148, 208, 213` | 与 `FS_ANC` 重复 | 传参或引用统一宏 |
| `HW_*` 系列（256/2/24/15dB/4/8/0.96/2） | `howling_detect.h:18-25` | 编译期常量位置合理，注释充分 ✅ | 阈值类（THRESH/PERSIST/R）可提为运行时配置 |
| `CAL_SEC=4, NOISE_AMP=0.3, NLMS_MU=0.2, srand(42)` | `calibrate_feedback.c:23-26, 203` | 校准程序内合理 | 可保留 |
| CNN 结构常量（K/CH/STEM_* 等） | `cnn_m5_forward.c:15-35` | 与训练模型绑定，编译期合理 | 换模型需重编译——若要可替换模型，应把结构描述写入导出文件 |
| `Sleep(100)` | `main_realtime.c:414` | 引入最高 100ms 决策抖动 | 可用事件/条件变量替代轮询 |
| BN epsilon `1e-5f`、功率正则 `1e-10f`、各处 `1e-12f` | `cnn_m5_forward.c:158`、`fxnlms_mimo.c:79`、`main_realtime.c:229` | 数值防护，位置合理 | 保留；`fxnlms` 正则项量纲问题见 §5.2 |

**总原则**：结构维度（E/S/L/K/C）→ 编译期常量 + 加载校验；声学/硬件整定值（增益、延迟、阈值、μ）→ 外部配置文件。路线图 §六 的 config.json 方案方向正确，应落实。

---

## 4. 可维护性

1. **模块化良好**：CNN、场景控制、FxNLMS、FIR、啸叫检测均为独立编译单元，权重全部外置 `.bin` —— 替换 CNN/滤波器组无需改算法代码 ✅。但**加载几乎零校验**（`binary_loader.c:6-22` 无 magic/长度/维度检查；`main_realtime.c:326-329` 未检查返回值，文件缺失 → NULL 解引用崩溃）。
2. **`main_realtime.c` 承担过多**（~500 行）：PortAudio DLL 绑定、重采样、信号链、场景状态机、统计、UI 全在一个文件；回调函数 `audio_cb` 内联了整条信号链，**无法单独单元测试**。建议抽出 `anc_process_sample()` 纯函数。**→ 与 §6.2 (数据竞争修复) + 4.6 (耦合解耦) 合并处理，作为专项重构。**
3. **注释质量不均**：啸叫模块注释优秀（原理+用法+状态机说明）；~~FxNLMS 的步长归一化有注释但语义存疑~~ **FALSE (2026-07-22)**；~~`main_realtime.c:73` 残留注释~~ ✅ 已清理；~~`ref_48k`/`err_48k` 命名误导~~ ✅ 已修复 (2026-07-22, → `ref_buf`/`err_buf`/`anti_buf`)。
4. **死代码**（2026-07-22 清理）：
   - ~~`decimate_3to1`/`interpolate_1to3`~~ ✅ 已删除 — 单通道裸函数，回调版本含通道拆分+NaN保护，无法替代
   - ~~`fxnlms_update_wc`~~ ✅ 已删除 — 与 `set_wc` 字节级相同，零调用者
   - `remove_notch`（`howling_detect.c:162-179`）：**保留**，后续反馈环路需逐频率管理
5. ~~**重复的 PortAudio 绑定样板**~~ ✅ 已修复 (2026-07-22)。提取到 `include/pa_loader.h` + `src/pa_loader.c`，`main_realtime.c` 和 `calibrate_feedback.c` 各减少 ~40 行样板。`measure_drift.c` 保留独立简化版（独立工具）。
6. **过度耦合点**：回调直接读写 `ctx->fx.wc`（交叉淡化时整块覆写，`main_realtime.c:171-177`），FxNLMS 内部状态被外部模块直接操纵——建议由 `fxnlms_*` API 封装淡化。**→ 与 §6.2 (数据竞争修复) + 4.2 (重构) 合并处理。单纯加 API 封装不解决竞态问题，假安全不如不封装。**

---

## 5. 逻辑/算法错误

### 5.1 慢速环路

| # | 严重程度 | 问题 |
|---|---|---|
| S-1 | **中** | **滞回检测在渐变噪声下失效 + 场景记忆污染**：`cos_sim` 比较的是**相邻两帧** probs（`main_realtime.c:431-437`，`prev_probs` 每秒无条件更新于 `:496`）。若场景缓慢过渡（每帧 cos≈0.95 但 argmax 已永久改变），切换永不触发，`cur_scene_id` 停留旧值；此时收敛保存逻辑（`:459-465`）会把**新场景收敛出的 Wc 写进旧场景的记忆槽**。修复：cos 应对比"当前帧 vs 进入当前场景时的锚点 probs"，或增加"argmax 连续 N 帧 ≠ cur_scene 也触发切换"。 |
| S-2 | **中** | **场景切换瞬间输出跳变为 0**：切换时 `ramp_cnt=RAMP_SAMPLES`（`:490`），而 ramp 公式 `1 - ramp_cnt/RAMP_SAMPLES` 首样本即为 **0**（`:248-253`）——反噪声瞬间消失再花 400ms 爬回，1ms CrossFader 完全被 ramp 掩盖，"无感切换"不成立；且期间自适应仍按"输出已全额播出"的假设更新（模型失配）。 |
| S-3 | 低 | **交叉淡化首尾**：`a` 从 16/16=1.0 递减到 1/16 后 `memcpy` 跳到 0，末帧有 6% 权重跳变（`:171-177`），量级无害。除零风险已由 `1e-10f` 防护（`:437`，softmax 保证 `nc>0`）✅。 |
| S-4 | 低 | **Blend/Wc 构造与论文偏离**：`construct_wc` 用 centroid 按 max 归一 + 截断 [0,1] 加权 15 个子滤波器，再**强制 RMS 对齐到"15 个子滤波器等权和"的 RMS** 并取反（`scene_controller.c:96-100`）。参考论文的 SFANC 是"每帧选择一个预训练固定滤波器"；本实现更接近 GFANC 软组合 + 自创 RMS 重标定 hack——重标定会抹掉预训练滤波器隐含的绝对增益标定，需用离线数据验证必要性。 |
| S-5 | 低 | ~~minmaxscaler 疑似不完整~~ **FALSE (2026-07-22 验证)**。Python 训练/推理三处 `minmaxscaler` 均使用 `data / (max - min)` 公式（不先减 min），C 端实现一致。阈值差异 (1e-6 vs 1e-10) 和静默填充策略 (全零 vs pass-through) 属安全强化，不影响语义。 |
| S-6 | 低 | CNN 输入延迟：分类结果对应的是"上一秒"音频，加上轮询 + 推理延迟约 1.1-1.2s 才生效——对准平稳场景分类可接受，但注释中应写明。缓冲为"填满-丢弃"而非环形，无衔接错位问题 ✅。 |

### 5.2 前馈环路（最严重问题集中区）

| # | 严重程度 | 问题 |
|---|---|---|
| ~~F-A~~ | ~~**高**~~ | ❌ **已回退 (2026-07-16)**。`fxnlms_tick_rt` 经 `tools/verify_fa.c` 验证正确 (SISO +11.4dB, MIMO +5.6dB)，但因 F-B 硬件阻塞连带 revert (`git revert 162d357`)。**2026-07-22 硬件已升级到 ASIO 共时钟声卡**，待 F-B 重新校准后可 `git revert 162d357` 恢复。原问题：(1) 实时版把误差麦实测信号作为 `disturbance` 传入，`fxnlms_tick` 再加一次 `anti_est` → `err ≈ d + 2·anti`，NR 理论上限 ~6dB。(2) 扬声器驱动信号被 Ŝ 模型二次滤波 + 对 3 条误差路径求和。 |
| F-B | ~~**高**~~ | ❌ **已回退 (2026-07-16), 🔧 硬件就绪待重新校准**。`calibrate_secondary.exe` 源码已在 revert 中移除（`git revert 162d357`，仅残留 .exe 二进制）。原阻塞原因：双独立时钟 USB 设备流滑移 1484~1917ppm 导致 NLMS 辨识 ERLE≈0。**2026-07-22 硬件已升级到 ASIO 共时钟声卡**，可重新运行 `calibrate_feedback.exe` 验证时钟稳定性后再恢复 `calibrate_secondary.c` 源码。恢复方法：`git revert 162d357`。 |
| F-C | **中** | **tanh 软限幅不连续**：`if (x > 1.0) x = tanhf(x)`（`main_realtime.c:161-162, 189-190`）——x 从 1.0⁻ 到 1.0⁺ 时输出从 1.0 跳到 tanh(1)≈0.762，**0.24 的硬跳变**恰在大信号时注入宽带毛刺，与"防失真"目的相反。应改为全程 `MIC_CLIP_MAX * tanhf(x / MIC_CLIP_MAX)`。 |
| F-D | ~~**中**~~ | ✅ **已修复 (2026-07-22, commit e8771b0)**。leak 公式从 `wc *= (1 - step_size*leak)` 改为 `wc *= (1 - leak)`，与 step_size 解耦。leak 参数从 `1e-5f` 调整为 `1e-6f`（~1.5%/秒衰减，float32 可分辨）。位置：[src/fxnlms_mimo.c:94](src/fxnlms_mimo.c#L94)、[main.c:192](main.c#L192)、[main_realtime.c:393](main_realtime.c#L393)。 |
| F-E | **中** | **反馈抵消的跨回调状态丢失**：`anti_spk[S]={0,0}` 在每次回调开头重置（`main_realtime.c:149`），每个回调的第一个样本把 0 而非上一回调末样本的真实输出推入反馈 FIR 延迟线——256 抽头历史中恒有 ~8 个错误样本（256/32），持续劣化反馈抵消精度。`anti_spk` 应存入 `ctx` 跨回调保持。 |
| F-F | **中** | **反馈路径校准与运行时的重采样不匹配**：校准播放 48k 白噪声、录音后两路都做最近邻 3:1 抽取（`calibrate_feedback.c:230-237`）——抽取后输入输出之间**不再是 16k 速率的 LTI 关系**（非整数倍相位分量表现为不可建模噪声，NLMS 只能辨识 1/3 的多相分量）。而运行时的等效反馈路径是 `decimate ∘ H ∘ ZOH`。校准激励应改为"16k 白噪声经 ZOH 上采样到 48k 播放"，与运行时输出路径完全一致。 |
| F-G | **中** | **双扬声器反馈路径被合并建模**：校准时两声道播同一噪声（`calibrate_feedback.c:93-98`），辨识出的是 H0+H1 之和；运行时用 `(anti0+anti1)/2` 过这一个 FIR（`main_realtime.c:156`）。当两路 anti 不相等时误差项为 `(H0−H1)(a0−a1)/2`。应分别校准两条路径、运行两个 FIR。 |
| F-H | 低 | `fxnlms_forward_only` 的 `err_out` 只含 `anti_est` 不含 dist（`fxnlms_mimo.c:40-43`），与 `fxnlms_tick` 语义不一致——淡化期间喂给啸叫检测与统计的 err 含义变了（仅 16 样本，影响小，但应统一）。 |
| F-I | 低 | 统计口径：`acc_anti` 在 ramp/safety_mute 置零**之前**累积（`main_realtime.c:225` vs `:245-253`），显示的 anti RMS 高于实际输出；~~NR 指标中 `dist` 是实测麦信号（ANC 生效后已含反噪声），"dist=误差麦处原始噪声"的注释在实机上不成立，NR 数值系统性失真（与 F-A 同源）~~ ← 此半边已随 F-A 修复解决（NR 现为 `d̂=e−anti_est` vs 实测 `e`）；`acc_anti` 累积时机问题仍待修。 |

### 5.3 反馈环路

| # | 严重程度 | 问题 |
|---|---|---|
| B-1 | — | **架构缺失**：8 阶 IIR 20-200Hz 反馈控制器不存在（见 §2.3），无从审查其稳定性/极限环。若按论文实现，建议：双二阶级联 + Direct Form II Transposed + float 下每级系数单独设计（不要高阶直接型），20Hz 极点在 fs=16k 下 `r→0.9998`，需用 double 存状态防极限环。 |
| B-2 | ~~**高**~~ | ✅ **已修复 (2026-07-22, commit e8771b0)**。IIR 状态数组从 `[HW_MAX_NOTCHES]` 扩展为 `[HW_S][HW_MAX_NOTCHES]`（HW_S=2）。`add_notch()` 初始化所有扬声器状态，`remove_notch()` 搬运所有扬声器状态，`howling_tick()` 两处 `notch_apply` 调用传入 `[s][i]` 索引。位置：[include/howling_detect.h](include/howling_detect.h#L26)、[src/howling_detect.c](src/howling_detect.c#L148-L253)。 |
| B-3 | 低 | 陷波器本身（r=0.96 二阶）稳定 ✅；检测跳过 bin0/1 避开直流与工频 ✅；`HW_MAX_BIN=24`（1500Hz）与带通上限匹配 ✅。`remove_notch` 是死代码。 |

### 5.4 信号通路汇总（重采样/限幅/输出映射)

| # | 严重程度 | 问题 |
|---|---|---|
| P-1 | **中** | **48k→16k 抽取无抗混叠滤波**：最近邻取样（`main_realtime.c:141-146`），8-24kHz 内容全部折叠进 0-8k；其中 14.5-16k 折叠到 20-1500Hz **恰好落入带通通带**，成为参考/误差信号中的虚假成分。 |
| P-2 | **中** | **16k→48k 内插为零阶保持**（`main_realtime.c:263-269`），16k 镜像频率成分直接送扬声器（仅受 ZOH sinc 微弱衰减），产生可闻镜像杂音，并进一步经反馈路径回灌参考麦。 |
| P-3 | 低 | 限幅为 ±1.0（非架构文档所述 ±0.5），anti 在 FxNLMS 后钳位一次（`:201-205`）、输出前再钳位一次（`:266-267`），逻辑正确；2 扬声器交织映射 `out[2n]=spk0, out[2n+1]=spk1` 正确 ✅。 |
| P-4 | 低 | 带通对 ref 与 err 使用同一 FIR（群延迟 512 样本=32ms 双方相同），Fx 与 dist 相对对齐不受影响 ✅——但绝对延迟叠加在因果裕度上，窗户场景需确认初级路径传播延迟 > 总处理延迟。 |

---

## 6. 实时性与数值稳定性风险

1. **【高】回调 CPU 逼近预算**：每 16k 样本约 41k 次 MAC（FxNLMS 4×6144 + 次级路径 6×1040 + 带通 4×1024 + 反馈 256）+ **~10.6k 次整数取模**（`fir_tick` 内层 `(p-k+N)%N`，`fir_filter.c:40-43`，N 为运行时变量无法优化为位与）+ `xd_roll_write` 每样本搬移 6144 个 float（`fxnlms_mimo.c:22-32`）。合计约 0.7 GMAC/s + 170M div/s，在 96 帧（2ms）回调预算内属于边缘状态——任何调度抖动即 xrun。必须消除取模（双段线性循环或 2 的幂掩码）并把 xd 改为环形索引（O(1) 写入）。
2. **【高】跨线程数据竞争**：主线程在回调运行期间直接 `fxnlms_set_wc` 写 `fx.wc`（`main_realtime.c:421`）、写 `wc_cur/wc_old` 后再置 `fade_cnt`（`:488-489`）、读 `fx.wc` 保存场景记忆（`:462, 473`）——全部无原子/屏障/锁。撕裂的 Wc 可能瞬时产生错误输出；`fade_cnt/ramp_cnt/mute_hold` 存在"主线程写 vs 回调递减"的丢失更新。建议：主线程只写"影子缓冲 + 序号"，回调在样本边界用原子序号检测并自行拷贝（单生产者单消费者无锁交接）。
3. **【中】CNN 推理在主线程执行 ✅ 不占回调**，但每次推理 `calloc` 4×1MB（`cnn_m5_forward.c:226-231`）——主线程可接受，仍建议一次性预分配。`Sleep(100)` 轮询引入决策抖动。
4. **【中】除零/定义域**：`log10f`、cos 相似度、`construct_wc` 的 RMS 缩放均有 epsilon 防护 ✅；~~`fxnlms` 功率正则项被 `/(E*L)` 缩小~~ **FALSE (2026-07-22 验证)**：`inv_pwr = 1/((eps+sum_x2)/(E*L)) = (E*L)/(eps+sum_x2)`，eps 和信号功率同比例缩放，分母中 eps 仍为 `1e-10`，正则效果不受除法影响。`(E*L)` 因子成为等效步长的一部分，不影响正则化强度。
5. **【中】无 Wc 发散防线**：仅有事后型 `safety_mute`（err_rms > ref_rms，每秒评估一次，`main_realtime.c:237-238`），且比较的是两个不同物理位置、不同含义的量；mute 生效期间自适应继续按"输出已播出"更新（模型失配加剧）。
6. **【低】回调内栈使用**：`float frame[256]`（howling）+ VLA 若干，量级安全；`rt_ctx_t` ~150KB 在 main 栈上，Windows 默认 1MB 栈内可用，但建议改堆分配。

---

## 7. 具体修改建议

### 7.1 P0 — ~~修正实时 FxNLMS 信号结构（对应 F-A）~~ ❌ 已回退 (2026-07-16), ASIO就绪可恢复

```c
/* fxnlms_mimo.h: 增加参考信号历史 */
typedef struct {
    float *wc;      /* [S*L] */
    float *xd;      /* [E*S*L] 滤波参考(梯度用) */
    float *x_hist;  /* [L]     原始带通参考(输出用) — 新增 */
    ...
} fxnlms_mimo_t;

/* 实时版 tick: e 直接用实测误差, 输出用 Wc⊗x */
void fxnlms_tick_rt(fxnlms_mimo_t *fx, float x_ref, const float *Fx,
                    const float *err_meas /*实测误差麦(带通)*/,
                    float *anti_out)
{
    xd_roll_write(fx, Fx);
    x_hist_push(fx, x_ref);

    /* 物理输出: anti[s] = Σ_k Wc[s,k] * x[n-k]  (不再经过 Ŝ, 不再对 e 求和) */
    for (int s = 0; s < S; s++) {
        anti_out[s] = 0;
        for (int k = 0; k < L; k++)
            anti_out[s] += fx->wc[s*L+k] * fx->x_hist[k];
    }

    /* 梯度: w[s,k] -= μ/power[s] * Σ_e err_meas[e] * xd[e,s,k] */
    ...
}
```

主回调中删除 `err_sig = dist + anti_est` 的合成，`err_meas[e] = fir_tick(&bp_err[e], mic_e)` 直接作为误差；NR 统计改为"ANC 开/关对比"或用收敛前基线估计 dist。

**⚠️ F-A 代码已于 `162d357` revert 移除，`tools/verify_fa.c` 亦随 revert 删除。恢复方法：`git revert 162d357`（需 F-B 先确认 ASIO 硬件校准可行）。**

### 7.2 P0 — ~~实测次级路径与系统延迟（对应 F-B）~~ ❌ 已回退 (2026-07-16), ASIO就绪可重新校准

新建 `calibrate_secondary.c`（复用 `calibrate_feedback.c` 的 NLMS 框架）：每只扬声器逐一播 ZOH 上采样的 16k 白噪声，用 3 只误差麦同时录音，NLMS 辨识 6 条 `Ŝ(e,s)`（含 I/O 往返延迟，**从而不再需要 `DSP_DELAY` 猜测值**），写入 `data/secondary_path_measured.bin`。运行时优先加载实测版本，缺失时回退仿真版并打印醒目警告。**⚠️ 源码已于 `162d357` revert 移除，可通过 `git revert 162d357` 恢复。**

### 7.3 P0 — ~~修复啸叫陷波状态串用（对应 B-2）~~ ✅ 已修复 (2026-07-22, e8771b0)

```c
/* howling_detect.h */
float x1[HW_MAX_NOTCHES][2], x2[HW_MAX_NOTCHES][2];  /* [notch][speaker] */
float y1[HW_MAX_NOTCHES][2], y2[HW_MAX_NOTCHES][2];

/* howling_tick 内 */
for (int s = 0; s < S; s++)
    for (int i = 0; i < hw->active_count; i++)
        anti_spk[s] = notch_apply(anti_spk[s], hw->b1[i], hw->a1[i], hw->a2[i],
                                  &hw->x1[i][s], &hw->x2[i][s],
                                  &hw->y1[i][s], &hw->y2[i][s]);
```

（`add_notch`/`remove_notch` 同步清零/搬移二维状态。）

### 7.4 P0 — 消除跨线程竞争（对应 §6.2）

```c
/* ctx 增加影子缓冲 + 原子序号 */
float          wc_pending[S*L];
volatile LONG  wc_seq;        /* 主线程发布, 回调消费 */

/* 主线程 (替代 fxnlms_set_wc / 直接写 wc_cur): */
memcpy(ctx->wc_pending, new_wc, sizeof(ctx->wc_pending));
InterlockedIncrement(&ctx->wc_seq);

/* 回调开头 (样本边界): */
LONG seq = ctx->wc_seq;
if (seq != ctx->wc_seq_seen) {
    memcpy(ctx->wc_cur, ctx->wc_pending, sizeof(ctx->wc_cur));
    memcpy(ctx->wc_old, ctx->fx.wc, ...);
    ctx->fade_cnt = FADE_LEN;
    ctx->wc_seq_seen = seq;
}
```

场景记忆保存（主线程读 `fx.wc`）改为回调侧周期性快照到另一影子缓冲。

### 7.5 P1 — FIR 性能重写（对应 §6.1）

```c
gfanc_float_t fir_tick(fir_filter_t *f, gfanc_float_t x)
{
    double *dl = f->delay_line;
    int N = f->n_taps, p = f->ptr;
    dl[p] = (double)x;
    double y = 0.0;
    const gfanc_float_t *c = f->coeffs;
    /* 两段线性循环, 无取模 */
    int k = 0;
    for (int i = p; i >= 0; i--) y += (double)c[k++] * dl[i];
    for (int i = N-1; i > p;  i--) y += (double)c[k++] * dl[i];
    f->ptr = (p + 1 == N) ? 0 : p + 1;
    return (gfanc_float_t)y;
}
```

`xd_roll_write` 改环形写指针（O(E·S) 而非 O(E·S·L)），点积循环按环形偏移读取；再考虑 `-O3 -march=native` 与 SIMD。

### 7.6 P1 — 重采样加抗混叠/抗镜像（对应 P-1/P-2）

48k 侧各加一个 ~63 阶 FIR 低通（fc≈7kHz）：输入端 4 通道滤波后再 3:1 取样；输出端先 3 倍零插值再低通（或多相实现，每 16k 样本 21 次 MAC/相位）。CPU 增量远小于现有 FxNLMS 负荷。

### 7.7 P1 — 其余定点修复

| 问题 | 修复 | 状态 |
|---|---|---|
| tanh 限幅不连续（F-C） | `x = MIC_CLIP_MAX * tanhf(x / MIC_CLIP_MAX);` 全程应用 | ❌ |
| anti_spk 跨回调重置（F-E） | 移入 `rt_ctx_t`，仅初始化时清零 | ❌ |
| 反馈校准激励失配（F-F） | 校准程序生成 16k 噪声 → ZOH ×3 播放；两扬声器分别校准（F-G），运行两条 FIR | ❌ |
| ramp 起点跳变（S-2） | 场景切换只走 CrossFader（加长到 10-100ms），不重触发 ramp；ramp 仅用于冷启动 | ❌ |
| 滞回失效（S-1） | 保存"场景锚点 probs"；`cos(anchor, cur) < 0.8` 或 argmax 连续 3 帧偏离才切换 | ❌ |
| ~~leak 无效（F-D）~~ | `wc *= (1-leak)`，leak=1e-6，与 step_size 解耦 | ✅ 2026-07-22 |
| ~~啸叫陷波串状态（B-2）~~ | IIR 状态扩展为 `[HW_S][HW_MAX_NOTCHES]` | ✅ 2026-07-22 |
| ~~MinMax不减min（S-5）~~ | Python 训练/推理均用 `data/(max-min)`，C 端一致 | ❌ FALSE |
| ~~功率正则量纲（§6.4）~~ | `inv_pwr = (E*L)/(eps+sum)`，eps 实际未被缩小 | ❌ FALSE |
| 功率正则量纲 | `power[s] = sum/(E*L) + 1e-10f;`（正则加在除法之后） | ❌ |
| 加载零校验 | `bin_load_float` 后逐一断言长度 == 期望值（BP_LEN、E*S*SEC_LEN、SC_K*SC_S*SC_C、…），失败即退出 | ❌ |
| 魔数 `8`/`15*2`/`16000.0f` | 统一替换为 `SC_K`/`SC_C*SC_S`/`FS_ANC` | ❌ |
| 死代码 | 删除 `decimate_3to1`/`interpolate_1to3`/`remove_notch`/`fxnlms_update_wc` 或改为实际调用 | ❌ |
| MinMax scaler 阈值/CNN返回值/NaN保护 | 见 commit e8771b0 中 FIX-1/2/3/6 | ✅ 2026-07-22 |

### 7.8 P2 — 工程化

1. 落实 `config.json`（路线图 §六），启动时加载：增益、μ、leak、阈值、淡化时长、设备名。
2. 抽出 `anc_pipeline.c`（纯函数信号链）+ `pa_loader.c`（DLL 绑定），使 `main_realtime.c` 缩至 I/O 与调度；信号链用 WAV 回灌建立回归测试（与离线版输出比对）。
3. 若确需论文架构中的"固定反馈 IIR 环路"（20-200Hz 反馈 ANC），作为独立模块 `feedback_iir.c` 新增：4 级双二阶级联（DF2T、double 状态）、误差麦平均输入、输出与前馈 anti 求和后统一限幅——并在架构文档中同步更新参数为实际值。

---

## 附：问题严重度速查

| 等级 | 编号 | 一句话 |
|---|---|---|
| ~~高~~ | ~~F-A~~ | ~~误差双重计入 + Ŝ 二次滤波~~ ❌ 已回退, ASIO硬件就绪可恢复 (`git revert 162d357`) |
| ~~高~~ | ~~F-B~~ | ~~次级路径延迟/校准~~ ❌ 已回退, ASIO硬件就绪可重新校准 (`git revert 162d357`) |
| 高 | §6.2 | 主线程/回调对 Wc、fade_cnt 等无同步的数据竞争 |
| 高 | §6.1 | fir_tick 取模 + xd 全量搬移，回调 CPU 逼近 2ms 预算 |
| ~~高~~ | ~~B-2~~ | ~~啸叫陷波状态跨扬声器串用~~ ✅ 已修复 (2026-07-22, e8771b0) |
| ~~中~~ | ~~F-D~~ | ~~leak 因子无效 (1-1e-9)~~ ✅ 已修复 (2026-07-22, e8771b0) |
| 中 | S-1, S-2, F-C, F-E~F-G, P-1, P-2, §6.5 | 滞回失效、ramp 跳变、tanh 不连续、反馈校准失配、无抗混叠、无发散防线 |
| 低 | S-3, S-4, S-6, F-H, F-I, B-3, P-3, P-4 | 淡化尾帧、Blend 重标定 hack、统计口径、死代码等 |
| ~~中~~ | ~~S-5, §6.4~~ | ~~MinMax不减min~~ / ~~功率正则量纲~~ **FALSE (2026-07-22 验证)** |
| 🆕 已修复 | FIX-1~3,6 | MinMax阈值/CNN返回值检查/NaN保护/Wc退化告警 (2026-07-22, e8771b0) |
