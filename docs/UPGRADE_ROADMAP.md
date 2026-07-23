# GFANC FxNLMs — 后续升级路线

> 最后更新: 2026-07-22

---

## 总览

| 状态 | 项目 | 类别 | 问题 | 影响 | 方案/状态 |
|------|------|------|------|------|----------|
| ✅ | F-A | 前馈 | 误差信号双重计入 | NR 上限 ~6dB | fxnlms_tick_rt 独立实时路径 |
| 🔶 | F-B | 前馈 | 次级路径未实测 | Ŝ 不准, 收敛慢 | ASIO 校准后 git revert `162d357` |
| ✅ | F-D | 前馈 | leak 无效 (1-1e-9) | Wc 漂移 | 解耦, leak=1e-6 |
| ✅ | F-E | 前馈 | anti_spk 跨回调重置 | fb 抵消 ~3% 误差 | anti_spk_prev 持久化 |
| ✅ | F-F | 前馈 | 校准重采样失配 | NLMS 辨识精度降 | 16k ZOH×3 激励 |
| ✅ | F-G | 前馈 | 反馈路径合并建模 | H0≠H1 抵消不准 | 逐扬声器校准+双 FIR |
| ✅ | B-2 | 反馈 | 陷波 IIR 状态串用 | 通道间串扰失真 | 状态加 HW_S 维度 |
| ✅ | S-1 | 场景 | 滞回检测渐变失效 | 场景记忆污染 | anchor_probs 锚点 |
| ✅ | S-2 | 场景 | ramp 掩盖 CrossFader | 切换 400ms 消音 | 切换仅 CrossFader |
| ✅ | S-3 | 场景 | CrossFader 1ms 硬切 | 6% 末帧跳变 | FADE_LEN=1600 (100ms) |
| ✅ | S-4 | 场景 | RMS 强制对齐 stub_rms | 增益信息抹除 | 移除, LMS 自适应 |
| ✅ | §6.1 | 性能 | fir_tick 取模 idiv | 回调预算 217% | 双段线性循环 |
| 🔶 | §6.2 | 线程 | 跨线程数据竞争 | ARM 必崩 | 跨平台前修 (影子缓冲+原子) |
| ✅ | §6.3 | 性能 | CNN calloc 4MB/次 | 堆碎片 | 静态缓冲 |
| ✅ | §6.5 | 安全 | 无 Wc 发散防线 | 系数暴涨无保护 | max\|Wc\|>5×stub → freeze |
| ✅ | §6.6 | 内存 | rt_ctx_t ~211KB 栈 | 嵌入式危险 | heap calloc |
| ✅ | 反馈抵消 | 功能 | 扬声器→参考麦声泄漏 | 限制 MIC_PRE_GAIN | 双 FIR 逐扬声器校准 |
| ✅ | 啸叫检测 | 安全 | 窄带正反馈自激 | 啸叫破坏降噪 | DFT+IIR 陷波, 逐扬声器 |
| ✅ | Wc 发散检测 | 安全 | 系数暴涨 | 输出巨响 | max\|Wc\| 监控+freeze_lms |
| ✅ | 冷启动静音 | UX | INIT 瞬间冲击噪声 | 开机嘭声 | ramp 400ms + mute_hold |
| ✅ | 场景记忆切换 | 功能 | 重复收敛浪费时间 | 切换后 NR 恢复慢 | 每场景保存/恢复 Wc |
| ⚪ | 增益标定 | 标定 | 自动测量 MIC_PRE_GAIN | 上限在反馈非灵敏度 | 不实施 (NLMS 已补偿) |
| ⚪ | 子带处理 | 算法 | 特征值散布→收敛慢 | CNN 预设已消除 | 不实施 (延迟吃因果裕度) |
| ⚪ | 自适应步长 | 算法 | 动态调整 μ | NLMS 已做信号级自适应 | 不实施 (双环耦合风险) |
| 🟡 | 窗户 ANC | 实验 | 室内声学解相关 | NR 2-3dB (开放空间) | 参考麦伸窗外实验 |
| 🟢 | 双讲检测 | 功能 | 语音/音乐破坏自适应 | ANC 干扰期望信号 | 检测→冻结 LMS |
| 🟢 | 舒适噪声 | UX | ANC 伪影可闻 | 静音时反噪声残留感 | 成形噪声掩盖 |
| ❌ | 反馈 IIR (B-1) | 架构 | 无反馈 ANC 层 | 低频稳定性依赖前馈 | feedback_iir.c 待实现 |
| 🔶 | TODO-1 | Blend | max 归一化放大伪峰 | 子滤波器权重失真 | 需 centroid 数据选方案 |
| 🔶 | TODO-2 | 抵消 | fb 相位符号未验证 | 减法可能变加法 | 需 FIR 峰值符号测量 |
| 🔶 | TODO-3 | 参数 | 离线/在线 GAIN 不一致 | 参数不可比 | 需对比实验 |
| 🔶 | TODO-4 | 数值 | 功率 epsilon 边界 | 极静时有效步长过大 | 需在线 power 记录 |
| 🔶 | TODO-5 | 线程 | volatile 非原子 | 监控数据撕裂(极低) | 跨平台时改 _Atomic |

> **图例**: ✅ 已修复/已实现 · 🔶 需前置条件 · 🟡 待实验 · 🟢 P2 待评估 · ❌ 未实现 · ⚪ 不实施

---

## 已完成项

### F-A: 误差信号双重计入 ✅

**问题**：`fxnlms_tick` 的 `err = disturbance + anti_est` 被离线仿真和实时共用。实时版 `disturbance` 是实测误差麦（已含真实 S×anti），再叠加 `anti_est` → 反噪声被计两次，NR 理论上限 ~6dB。

**影响**：实时降噪量被系统性压制，离线仿真不受影响。

**修复** (2026-07-22)：
- 新增 `fxnlms_tick_rt` / `fxnlms_forward_rt`：anti=Wc⊗x_ref 直接卷积，梯度用 err_meas 直接驱动，不合成 err
- 新增 `x_hist[L]` 存储原始带通参考历史
- 离线 `fxnlms_tick` / `fxnlms_forward_only` 保留不动，两套路径互不影响
- 位置：`include/fxnlms_mimo.h:7-13,35-45`, `src/fxnlms_mimo.c:120-170`, `main_realtime.c:162-175`

### F-D: leak 因子无效 ✅

**问题**：原公式 `wc *= (1 - step_size × leak)` → `1 - 0.0001×1e-5 = 1 - 1e-9`。float32 精度下 `1.0f - 1e-9f = 1.0f`，完全无效，Wc 无任何正则化。

**影响**：长期运行 Wc 可能漂移/饱和，无内在机制限制系数增长。

**修复** (2026-07-22)：
- 公式改为 `wc *= (1 - leak)`，leak 与 step_size 解耦
- leak 参数从 `1e-5f` 调整为 `1e-6f`（~1.5%/秒衰减，float32 可分辨）
- 位置：`src/fxnlms_mimo.c:94`, `main.c:192`, `main_realtime.c:377`

### F-E: anti_spk 跨回调状态丢失 ✅

**问题**：`anti_spk[S] = {0,0}` 在每回调开头重置。fb_fir 首个样本馈入 0 替代上一回调末的真实 anti 值 → 256 tap 历史中 ~3% 错误样本。

**影响**：反馈抵消精度周期性退化，周期 = 回调频率 (~500Hz)。实际增益裕度充足未触发发散，但抵消精度下降。

**修复** (2026-07-22)：
- `anti_spk_prev[S]` 存入 `rt_ctx_t`
- 回调开头继承上轮回调末值：`anti_spk[0]=ctx->anti_spk_prev[0]`
- 回调末保存：`ctx->anti_spk_prev[0]=anti_spk[0]`
- 位置：`main_realtime.c:58,118-119,253-254`

### F-F: 反馈校准重采样失配 ✅

**问题**：原校准播 48k 白噪声 → 录制 48k → 两路都最近邻 3:1 抽取到 16k → NLMS 辨识。抽取后 48k 的多相分量表现为不可建模噪声，输入输出不再是纯 16k LTI 关系。

**影响**：NLMS 辨识的 FIR 精度下降，反馈抵消效果退化。

**修复** (2026-07-22)：
- 改为生成 16k 白噪声 → ZOH×3 播放 48k（激励源即带限）
- 仅 ref_hw 抽取为 ref_16k，noise_16k 直接用于 NLMS
- 与运行时输出路径（ZOH ×3）完全一致
- 位置：`src/calibrate_feedback.c:29-34,55-58,160-210`

### F-G: 双扬声器反馈路径合并建模 ✅

**问题**：原校准两声道播同一噪声 → 辨识 H0+H1 之和。运行时 `(anti0+anti1)/2` 经单 FIR。H0≠H1 时产生 `(H0-H1)(a0-a1)/2` 误差。

**影响**：两声道路径差异越大，抵消误差越大。

**修复** (2026-07-22)：
- `calibrate_feedback.c`：逐扬声器两轮校准（spk=0,1），每轮仅目标扬声器播音
- 输出 `data/feedback_path_0.bin` / `feedback_path_1.bin`
- `main_realtime.c`：`fb_fir[2]` 双 FIR 独立运行，`fb_est = Σ fir_tick(fb_fir[s], anti_spk[s])`
- 兼容降级：仅加载到 1 个 → 单 FIR，0 个 → 禁用
- 位置：`main_realtime.c:50-52,127-131,356-382`

### B-2: 啸叫陷波器 IIR 状态跨扬声器串用 ✅

**问题**：两个扬声器共用同一组 IIR 状态 `x1[i], x2[i], y1[i], y2[i]`。扬声器 0 处理后状态被污染，扬声器 1 用错历史继续滤波。

**影响**：多扬声器场景下陷波效果不可靠，通道间注入串扰失真。

**修复** (2026-07-22)：
- IIR 状态数组从 `[HW_MAX_NOTCHES]` 扩展为 `[HW_S][HW_MAX_NOTCHES]`（HW_S=2）
- `add_notch()` 初始化所有扬声器状态
- `remove_notch()` 搬运所有扬声器状态
- `howling_tick()` 两处 `notch_apply` 传入 `[s][i]` 索引
- 位置：`include/howling_detect.h:26,45-46`, `src/howling_detect.c:148-253`

### S-1: 滞回检测渐变噪声失效 ✅

**问题**：`cos_sim` 比较相邻两帧 probs。渐变噪声场景每帧 cos≈0.95 但 argmax 已永久改变 → 切换永不触发，新场景 Wc 写入旧场景记忆槽。

**影响**：场景记忆污染，切回旧场景时恢复的是错误的 Wc。

**修复** (2026-07-22)：
- 新增 `anchor_probs[8]`，在 INIT/场景切换时保存当前 probs
- `cos(anchor_probs, cur_probs)` 替代 `cos(prev_probs, cur_probs)`
- 渐变场景累积偏差终将 <0.8，正确触发切换
- 位置：`main_realtime.c:74,431,441-448,514`

### S-2: 场景切换 ramp 首样本为 0 ✅

**问题**：切换时 `ramp_cnt=RAMP_SAMPLES`，首样本 ramp=0 → 反噪声消失 400ms。CrossFader 的 100ms 平滑过渡完全被掩盖。

**影响**：每次场景切换有 400ms 消音窗口，用户可感知。

**修复** (2026-07-22)：
- 场景切换仅走 CrossFader（FADE_LEN=1600, 100ms），不再触发 ramp
- ramp 仅用于冷启动 INIT
- 切换过渡从"消音 400ms"变为"Wc 100ms 平滑过渡"
- 位置：`main_realtime.c:431,513-516`

### S-3: CrossFader 末帧跳变 ✅

**问题**：FADE_LEN=16 时 CrossFader 末帧 `a=1/16=6.25%` 后 `memcpy` 跳至 0%。

**影响**：6.25% 权重跳变（2048 系数），量级虽小但属可避免的不连续。

**修复** (2026-07-22)：
- FADE_LEN = 1600（100ms = 20Hz×2 周期，≥ FIR 长度 64ms）
- 末帧跳变降至 0.0625%，可忽略
- 位置：`main_realtime.c:28`, `main.c:127`

### S-4: Blend/Wc RMS 强制对齐抹除增益 ✅

**问题**：`construct_wc` 计算 `scale = stub_rms / rms(Wc)`，强制所有场景 Wc 幅值相同。预训练子滤波器和 blend 权重隐含的增益信息被抹除。

**影响**：不同场景的最优 Wc 幅值被拉平，LMS 需额外时间调整增益。场景记忆保存后无持续影响。

**修复** (2026-07-22)：
- 移除 `wc *= stub_rms/rms(wc)` 强制定标，仅取反 `wc = -wc`
- LMS 功率归一化自动适应增益
- 首次收敛后 `scene_wc` 记忆保存正确幅值
- 位置：`src/scene_controller.c:110-124`

### §6.1: fir_tick 取模消除 ✅

**问题**：`fir_tick` 内层 `(p-k+N)%N`，N 为运行时变量 → 编译器无法优化 → 每回调 ~339k 次 `idiv` 指令，取模一项占回调预算 142%。

**影响**：回调总预算 217%，嵌入式平台必丢帧，桌面靠超标量勉强撑。

**修复** (2026-07-22)：
- 双段线性循环：`i=p→0, N-1→p+1`，零取模，零额外分支
- `f->ptr = (p+1==N)?0:p+1` 替代 `(p+1)%N`
- 回调预算 217% → 73%
- 位置：`src/fir_filter.c:39-47`

### §6.3: CNN calloc 静态化 ✅

**问题**：`cnn_m5_forward` 每 1Hz 调用 `calloc(4×1MB)`，分配+清零 4MB 后释放。Windows 堆可承受但碎片化。

**修复** (2026-07-22)：静态缓冲单次 `calloc(4MB)`，后续复用，不释放。
- 位置：`src/cnn_m5_forward.c:223-231`

### §6.5: Wc 发散防线 ✅

**问题**：仅被动 `safety_mute`（err_rms > ref_rms），无 Wc 系数监控/变化率检测/自动回退。

**影响**：系数暴涨时无主动拦截，依赖每秒一次的被动检测。

**修复** (2026-07-22)：
- `fxnlms_mimo_t` 新增 `freeze_lms` 字段
- 主循环每秒检查 `max|Wc| > 5×stub_rms` → 设 freeze_lms 跳过梯度
- INIT/场景切换自动清除冻结
- 位置：`include/fxnlms_mimo.h:13`, `src/fxnlms_mimo.c:127`, `main_realtime.c:451-461`

### §6.6: rt_ctx_t 堆分配 ✅

**问题**：`rt_ctx_t` ~211KB 在 `main()` 栈上。Windows 1MB 栈足够，嵌入式栈 <256KB 则溢出。

**修复** (2026-07-22)：`calloc` 堆分配，83 处 `ctx.`→`ctx->` 机械替换。
- 位置：`main_realtime.c:324`

### 反馈抵消 ✅

**实现**：`src/calibrate_feedback.c` + `main_realtime.c`

1. 校准程序：逐扬声器两轮 NLMS 辨识 256 tap FIR → `data/feedback_path_{0,1}.bin`
2. 运行时：`fb_est = Σ fir_tick(fb_fir[s], anti_spk[s])`，从参考信号中减去
3. 自动降级：文件缺失时禁用，不影响 ANC

**实测效果**：反馈衰减约 -34dB，10x 增益稳定（无抵消时 6x 振荡）。

### 啸叫检测 ✅

**实现**：`include/howling_detect.h` + `src/howling_detect.c`

1. DFT 256 点频谱分析（62.5-1500Hz），汉宁窗
2. 峰均值比 >15dB 候选，4 帧确认（64ms），8 帧无峰释放
3. IIR biquad 陷波（r=0.96），最多 2 路，逐扬声器独立状态
4. 与 safety_mute 互补：safety_mute 检测宽带反馈，啸叫检测锁定窄带自激

### Wc 发散检测 ✅

**已实现** (2026-07-22)：每秒检查 `max|Wc| > 5×stub_rms` → freeze_lms 跳过梯度。INIT/场景切换自动清除。

**待完善**（后续可加）：变化率检测 / 自动回退到 CNN 预设 / 退避策略。

### 冷启动静音 ✅

ramp 400ms + mute_hold 1500ms + CrossFader 100ms。场景切换仅走 CrossFader，ramp 仅用于 INIT。

---

## 🔶 需前置条件项

### F-B: 次级路径未实测校准 🔶

**问题**：当前使用 Python 仿真 Ŝ（不含 I/O 延迟），与真实 S 存在相位偏差。DSP_DELAY=16 为粗略 padding。

**前置条件**：ASIO 硬件校准 — 恢复 `calibrate_secondary.c` 源码（`git revert 162d357`），实测 Ŝ 包含全部往返延迟。

**修复**：ASIO 校准后置 DSP_DELAY=0。恢复方法：`git revert 162d357`。

### §6.2: 跨线程数据竞争 🔶

**问题**：主线程 `memcpy` 8KB fx.wc 与回调梯度更新竞争，`fade_cnt`/`ramp_cnt` 读-改-写无同步。

**前置条件**：跨平台移植 — ARM relaxed memory model 下概率性重合大幅升高，x86 上实测安全。

**修复**：影子缓冲 + `InterlockedExchange` 序号（见 COMPREHENSIVE_REVIEW.md 附录 B §6.2）。

### TODO-1: Wc Max 归一化低权重伪峰 🔶

**问题**：`blend[i] / bmax` 强制最大权重=1.0。centroid 存在训练噪声/离群时，伪峰压制其他 14 个子滤波器。

**前置条件**：打印 8 个 centroid 各 30 维 blend 值，分析 `bmax/bmax2` 比值分布。

**候选方案**：A) bmax 下限保护 B) softmax 替代 max C) L2 归一化。

### TODO-2: 反馈抵消符号未验证 🔶

**问题**：`ref_sample = (ref_raw - fb_est) * MIC_PRE_GAIN` 假设扬声器正信号→参考麦正响应。声学反相时减法变加法。

**前置条件**：校准后检查 FIR 首峰值符号 + 对比启用/禁用 fb 时的 ref_rms。

**修复**：反相时改减法为加法，或运行时自动检测。

### TODO-3: 离线/在线 MIC_PRE_GAIN 不一致 🔶

**问题**：`main.c: MIC_PRE_GAIN=1.0` vs `main_realtime.c: MIC_PRE_GAIN=10.0`。离线调优参数不可直接用于在线。

**前置条件**：用 main.c 分别在 GAIN=1.0 和 10.0 跑同一噪声文件，对比 NR。

### TODO-4: 功率归一化 epsilon 边界调优 🔶

**问题**：epsilon=1e-10。信号功率 1e-8 级别时有效步长 `0.0001×3e7=3000`，巨大单步更新可能触发瞬时发散。

**前置条件**：在线记录 `power[s]` 最小值和典型范围。

### TODO-5: volatile 跨线程非原子 🔶

**问题**：10 个 `volatile` 变量（nr_level, ref_rms 等）回调写/主线程读，无原子性保证。

**影响**：极低概率监控数据撕裂（显示跳变），不影响音频处理。

**前置条件**：跨平台移植时一同修复。

---

## ⚪ 已评估不实施

| 项目 | 原因 |
|------|------|
| 增益标定 | NLMS 功率归一化已自动补偿。上限在反馈边界非灵敏度。换硬件手动改一行 `#define` |
| 子带处理 | CNN 预设+场景记忆已消除收敛需求。2-5ms 延迟吃因果裕度。瓶颈在声学解相关 |
| 自适应步长 | NLMS 功率归一化已提供信号级自适应。自适应 μ 引入双环耦合/振荡风险 |

---

## 🟡 待实验

### 窗户 ANC 实验配置

**背景**：开放空间参考-误差声学解相关是 NR 主要瓶颈（室内 NR 2-3dB vs 离线 15dB）。

**实验**：参考麦伸窗外 → 扬声器+误差麦窗内侧。窗户是噪声入口（低频 <500Hz 可视作点源），参考-误差高度相关。

**预期**：窗户场景 NR 有望提升至 10-15dB。

---

## 🟢 P2 待评估

| 项目 | 说明 |
|------|------|
| 双讲检测 | 检测语音/音乐→冻结 LMS，防止 ANC 干扰期望信号 |
| 舒适噪声 | 添加成形噪声掩盖 ANC 处理伪影，改善静音时主观感受 |
| 输入过载保护 | 已实现 tanh 基础保护，可加限幅计数器+过载标志位 |
| 硬件看门狗 | 独立硬件监控，异常断电（量产必需） |
| 时延补偿 | 精确补偿通道间采样延迟（当前固定 DSP_DELAY=16） |

---

## ❌ 未实现

### 反馈 IIR 环路 (B-1)

**问题**：架构中缺少误差麦→IIR 带通(20-200Hz)→扬声器的反馈 ANC 层。仅前馈路径处理全频段，低频段 FxNLMS 收敛慢。

**方案**：独立模块 `feedback_iir.c`：4 级双二阶级联（DF2T, double 状态），20Hz 极点 r→0.9998 需 double 防极限环。输出与前馈 anti_ff 求和后统一限幅。

---

## 量产硬件方案

### 推荐量产路径

| 量级 | 方案 | BOM | 工作量 | 周期 |
|------|------|-----|--------|------|
| 原型/展会 | Windows 迷你主机 | ~¥2200 | 零 | 即日 |
| 100-1000 台 | ARM Linux 定制板 (RK3568) | ~¥243 | 2-3 周单人 | ~1 月 |
| 10k+ 台 | Qualcomm AI ANC SoC (QCC5181) | ~¥120 | 3-6 月团队 | ~6 月 |

### 量产路径决策树

```
当前阶段: ASIO 声卡 + Windows PC (原型验证)
        │
        ├─ 需要脱离 PC?
        │   ├─ 否 → Windows 迷你主机, ¥2200, 零移植
        │   └─ 是 →
        │       ├─ 量 < 1000? → ARM Linux 定制板 (RK3568), BOM ¥240
        │       └─ 量 > 10k?  → Qualcomm AI ANC SoC, BOM ¥120
        │
        └─ 核心瓶颈: 声学设计 + ANC 调谐 > 芯片选型
```

### 方案 B: ARM Linux 定制板 BOM

| 元件 | 型号 | 单价 ¥ |
|------|------|--------|
| SoC | RK3566 (Cortex-A55 ×4, NPU) | 45 |
| RAM | LPDDR4 512MB | 15 |
| Flash | eMMC 8GB | 20 |
| ADC | PCM1865 (4-ch, 110dB) | 25 |
| DAC | TLV320AIC3254 | 15 |
| AMP | TPA2016D2 (2×1.7W) | 8 |
| 电源/晶振/PCB/被动 | — | 57 |
| 驻极体麦 ×4 | CMA-4544PF-W | 12 |
| 扬声器 ×2 | 20mm, 1W | 16 |
| 外壳 | 3D打印/开模 | 30 |
| **BOM 合计** | | **~243** |

软件移植：Linux BSP(1周) + ALSA(5天) + 交叉编译(1天) + 测试(1周) = 2-3 周单人。

### 方案 C: Qualcomm AI ANC SoC BOM (10k+)

| 元件 | 型号 | 单价 ¥ |
|------|------|--------|
| SoC | QCC5181 (DSP+NPU+BT+ADC/DAC) | 35-50 |
| Flash/Power/MEMS 麦/扬声器/PCB/外壳 | — | ~78 |
| **BOM 合计** | | **~110-130** |

软件：int8 量化 CNN(3周) + 定点移植(3周) + ADK集成(4周) + 调谐(2周) + 认证(8周) = 3-6 月团队。

---

## 参考资料

- [YDM6MIC 数据手册](micphone.md)
- Kuo & Morgan, "Active Noise Control Systems", Wiley, 1996
- PortAudio 文档: http://www.portaudio.com/docs/v19-doxydocs/
