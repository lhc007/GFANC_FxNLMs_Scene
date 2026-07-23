# GFANC FxNLMs 综合审查报告 — 架构·算法·逻辑·流程·升级方案

> 审查日期：2026-07-23 · **2026-07-23 合并 CODE_REVIEW.md** (原问题详述移入附录 A/B)
> 审查范围：全部 C 源码、头文件、Makefile、README
> 审查方法：逐文件完整阅读 + 数据流追踪 + 算法推导
>
> **应用场景**：窗户开口降噪系统 — 设备安装在窗户开口处，目标是将开口部分进入室内的声压降为零。当前处于 Windows 原型验证阶段，后续量产移植到嵌入式硬件。
>
> **关联文档**：
> - [UPGRADE_ROADMAP.md](UPGRADE_ROADMAP.md) — 升级路线图（已完成/待实现/量产方案）
> - [WINDOW_ANC_SUPPLEMENT.md](WINDOW_ANC_SUPPLEMENT.md) — 窗户降噪专项补充（声学约束、空间采样、因果性、量产差距）
> - ~~CODE_REVIEW.md~~ — 已合并入本文档附录 A/B

---

## 问题总览表

> **图例**: ✅ 已修复 · 🔶 需前置条件 · ⚠️ 待处理(需测试/验证) · ❌ 未实现 · 🟡 待实验/调查 · 🚫 误报(FALSE) · — 可忽略/无需修改
>
> 来源标记: [CR]=本文档审查新发现 · [W]=WINDOW_ANC_SUPPLEMENT · 其他=UPGRADE_ROADMAP 已有

### A. 前馈环路 (F系列)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ✅ | F-A | 🔴 高 | 误差信号双重计入 (disturbance含S×anti + 又叠加anti_est) | NR上限~6dB | fxnlms_mimo.c [CR] |
| 🔶 | F-B | 🔴 高 | 次级路径Ŝ未实测校准 (用Python仿真, 缺I/O延迟) | Ŝ模型不准, LMS收敛退化 | 需ASIO校准后 git revert `162d357` |
| ✅ | F-D | 🟡 中 | leak公式无效 (1-1e-9, float32不可分辨) | Wc无正则化, 长期漂移 | fxnlms_mimo.c |
| ✅ | F-E | 🟡 中 | anti_spk跨回调重置为0 | fb_fir ~3%错误样本 | main_realtime.c |
| ✅ | F-F | 🟡 中 | 反馈校准重采样失配 (48k播→近邻抽取→辨识) | NLMS辨识精度降 | calibrate_feedback.c |
| ✅ | F-G | 🟡 中 | 双扬声器反馈路径合并建模 (单FIR辨识H0+H1) | H0≠H1时抵消不准 | calibrate_feedback.c + main_realtime.c |
| — | F-H | 🟢 低 | fwd_only err语义不一致 (仅含anti_est) | 实时已不用, 离线100ms可忽略 | fxnlms_mimo.c |
| ✅ | F-I | 🟢 低 | acc_anti在mute/ramp前累积 | 控制台anti_rms偏高 | main_realtime.c |
| 🚫 | F-C | — | tanh软限幅断点 (声学影响<底噪) | 无 | FALSE |

### B. 反馈环路 (B系列)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ❌ | B-1 | 🟡 中 | 固定反馈IIR环路不存在 (仅前馈路径) | 20-200Hz低频无反馈补充 | 待新建 feedback_iir.c |
| ✅ | B-2 | 🔴 高 | 陷波IIR状态跨扬声器串用 | 通道间串扰失真 | howling_detect.c |
| ✅ | B-2b | 🟡 中 | 陷波硬释放振荡 (陷波→频谱干净→释放→啸叫重现) | 已加512ms最小保持时间, 逐频率独立释放 | howling_detect.c (已修复 2026-07-23) |
| ❌ | B-3 | 🟢 低 | remove_notch未被调用 (当前批量清除够用) | 无 | howling_detect.c |

### C. 场景/CNN (S系列)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ✅ | S-1 | 🟡 中 | 滞回检测渐变噪声失效 (cos比较相邻帧) | 场景记忆污染 | main_realtime.c |
| ✅ | S-2 | 🟡 中 | 场景切换ramp首样本0→输出消失400ms | 切换可感知消音 | main_realtime.c |
| ✅ | S-3 | 🟢 低 | CrossFader末帧6%跳变 (FADE_LEN=16) | 少量不连续 | main_realtime.c |
| ✅ | S-4 | 🟢 低 | Blend/Wc RMS强制对齐抹除增益 | 初始幅值不正确 | scene_controller.c |
| 🚫 | S-5 | — | MinMax不减min与训练不匹配 | 无, Python公式一致 | FALSE |
| — | S-6 | 🟢 低 | CNN输入延迟~1.0s (1s窗口设计特性) | 平稳场景无影响 | main_realtime.c |
| 🔶 | TODO-1 | 🟡 中 | Blend max归一化放大伪峰 (centroid离群值) | 子滤波器权重失真 | scene_controller.c |
| ✅ | CR-18 | 🟢 低 | 安静环境底噪被逐帧归一化放大→误分类污染scene_wc | 已改denom阈值: 1e-6→0.01, 弱信号跳过CNN保持当前场景 | scene_controller.c (已修复 2026-07-23) |
| 🟡 | CR-19 | 🟡 中 | CNN训练数据与窗户噪声场景覆盖度未知 | 场景分类退化 | data/scene_defs.bin [CR] |
| 🟡 | W-10 | 🟡 中 | 8类场景对窗户典型噪声(交通/施工/风声)的覆盖度待验证 | Wc初始值质量 | data/ [W] |

### D. 信号通路 (P系列)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| 🔶 | P-1 | 🟡 中 | 48k→16k抽取无抗混叠 (>8kHz折叠进通带) | 残余风险:15kHz→0-1.5kHz | main_realtime.c |
| 🔶 | P-2 | 🟡 中 | 16k→48k ZOH无抗镜像 | 16k/32k镜像送扬声器 | main_realtime.c |
| 🚫 | P-3 | — | 限幅±1.0非±0.5 (DAC满幅=±1.0) | 无 | FALSE |
| — | P-4 | — | 带通FIR群延迟32ms (线性相位FIR固有) | 物理事实, 非bug | main_realtime.c |
| — | CR-1 | 🟡 中 | MIMO梯度未按E通道数归一化 (有效步长放大√E≈1.7×) | E=3固定不变, step_size已补偿, 改了反而要重调参 | fxnlms_mimo.c (无需修改) |
| — | CR-3 | 🟢 低 | NR显示用Ŝ模型估计 ≠ 真实声学NR | 物理事实: Ŝ≠real_S时显示值≠真实值, 代码无法消除, 需声级计验证 | main_realtime.c (无需修改) |

### E. 线程/性能 (§系列)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ✅ | §6.1 | 🔴 高 | fir_tick取模idiv占142%回调预算 | 回调预算217%, 嵌入式丢帧 | fir_filter.c |
| 🔶 | §6.2 | 🔴 高 | 主线程/回调对fx.wc等无同步 (x86概率性安全, ARM必崩) | 跨平台最大阻塞项 | main_realtime.c |
| ✅ | §6.3 | 🟢 低 | CNN每秒calloc 4×1MB | 堆碎片化 | cnn_m5_forward.c |
| 🚫 | §6.4 | — | 功率正则被/(E·L)缩小 | 无, 数学推导证明eps未缩小 | FALSE |
| ✅ | §6.5 | 🟡 中 | 无Wc发散防线 | 系数暴涨无保护 | main_realtime.c |
| ✅ | §6.6 | 🟢 低 | rt_ctx_t ~211KB在main栈上 | 嵌入式栈<256KB危险 | main_realtime.c |
| 🔶 | TODO-4 | 🟡 中 | 功率epsilon=1e-10边界 (信号~1e-8时有效步长3000) | 极静时瞬时发散风险 | fxnlms_mimo.c |
| 🔶 | TODO-5 | 🟢 低 | volatile非原子 (10个监控变量) | 极低概率监控数据撕裂 | main_realtime.c |
| ✅ | CR-12 | 🟡 中 | safety_mute评估粒度1秒 (前100ms发散需等900ms) | 已加快检测通道: 连续10样本>0.95→0.6ms触发 | main_realtime.c (已修复 2026-07-23) |
| ✅ | CR-13 | 🟡 中 | Wc freeze无自动恢复 (瞬时扰动触发后永久冻结) | 已加60s超时重试+3s观察期+永久冻结机制 | main_realtime.c (已修复 2026-07-23) |

### F. 反馈抵消

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ✅ | FB | — | 双FIR逐扬声器校准+运行时抵消 | 反馈衰减~34dB | calibrate_feedback.c + main_realtime.c |
| 🔶 | TODO-2 | 🟡 中 | fb相位符号未验证 (减法可能变加法) | 需FIR峰值符号测量 | main_realtime.c |
| — | CR-15 | 🟢 低 | fb_fir输入为上一轮anti值 (1样本延迟, 62.5μs) | 256tap中占比0.4%, 声学上不可测 | main_realtime.c (无需修改) |
| — | B4 | 🟢 低 | 在线自适应反馈抵消 (补偿温度漂移) | 窗户ANC几何固定, 离线校准已足够; 此需求适用于耳机ANC(麦-扬声器距离可变) | 升级方案 (窗户场景不适用) |

### G. 代码质量/可维护性

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ✅ | CR-4 | 🟡 中 | 主循环~110行耦合5种逻辑 (CNN/发散/收敛/切换/统计) | 已拆为5个static函数: print_diagnostics/check_wc_divergence/check_convergence/check_scene_switch/cfg_load_env | main_realtime.c (已修复 2026-07-23) |
| ✅ | CR-5 | 🟢 低 | measure_drift.c重复PA加载代码(~30行) | 已复用pa_loader.h, Makefile已加drift目标 | measure_drift.c (已修复 2026-07-23) |
| — | CR-6 | 🟢 低 | gfanc_types.h太单薄, 核心类型散落各头文件 | C项目标准模式, 各模块头文件自包含, 已合理 | include/ (无需修改) |
| ⚠️ | CR-7 | 🟢 低 | cnn_m5_free空函数 (CNN权重永不释放~600KB) | OTA更新阻塞 | cnn_m5_forward.c [CR] |
| ✅ | CR-8 | 🟡 中 | fxnlms_init/scene_ctrl_init不返回错误码 (calloc无NULL检查) | 已改void→int, 失败时清理部分分配并返回-1 | fxnlms_mimo.c, scene_controller.c (已修复 2026-07-23) |
| ✅ | CR-9 | 🟡 中 | main_realtime.c初始化路径无失败回滚 (已分配资源泄漏) | 已加goto cleanup统一回滚, 所有资源NULL-safe释放 | main_realtime.c (已修复 2026-07-23) |
| — | CR-10 | 🟢 低 | 采样率硬编码(FS_HW=48000) vs 设备查询 | WASAPI设备几乎都是48k; 44100→16000非整数比增加复杂度>收益 | main_realtime.c (无需修改) |
| ⚠️ | CR-16 | 🟢 低 | 离线/实时接口重复 (_rt后缀, 共用xd_roll但路径不同) | 代码重复, 扩展困难 | fxnlms_mimo.c [CR] |
| ⚠️ | CR-17 | 🟡 中 | 零单元测试 (FIR/CNN/FxNLMS/Howling均无) | 回归靠手动 | 全局 [CR] |
| ✅ | CR-20 | 🟢 低 | 无分级日志框架 (全部printf) | 已加LOG_ERROR/WARN/INFO/DEBUG宏于gfanc_types.h | gfanc_types.h (已修复 2026-07-23) |
| ✅ | CRC-1 | 🟢 低 | 无运行时参数调整接口 (全部编译期#define) | 已加GFANC_MIC_GAIN/GFANC_STEP/GFANC_RAMP_MS/GFANC_MUTE_MS环境变量覆盖 | main_realtime.c (已修复 2026-07-23) |
| ❌ | CRC-2 | 🟢 低 | 无自动化校准流水线 (校准分散在多个exe) | 校准流程手动 | 全局 [CR] |

### H. 数值精度/稳定性

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ✅ | FIR-DBL | — | FIR延迟线double+系数float32混合精度 | 1024tap舍入误差降低10³倍 | fir_filter.c |
| — | CR-14 | 🟢 低 | IIR陷波float32状态在r→0.99时极限环风险 | 当前r=0.96安全; 仅当未来改HW_NOTCH_R>0.99时才需DF2T或double | howling_detect.c (当前无需修改) |
| ✅ | CR-2 | 🟡 中 | 啸叫检测硬释放振荡 (见B-2b) | 已加512ms最小保持+逐频率独立释放 (同B-2b) | howling_detect.c (已修复 2026-07-23) |

### I. 窗户ANC专项 (W系列)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| 🟡 | W-1 | 🔴 高 | 2S+3E空间采样是否覆盖窗户开口未知 | **阻塞量产决策** — 若不够需硬件改版 | 声学测量 [W] |
| 🟡 | W-2 | 🔴 高 | 因果性未定量测量 (32ms FIR延迟在500Hz+的因果裕度) | 有效带宽上限未知 | 互相关测试 [W] |
| 🟡 | W-3 | 🔴 高 | 离线15dB→实时4-9dB差距未归因 (因果/Ŝ误差/混响各自占比) | 优化方向不明确 | 系统测量 [W] |
| 🟡 | W-4 | 🟡 中 | 室内反射经墙壁→参考麦的间接串扰未建模 | 参考信号污染 | 声学设计 [W] |
| 🟡 | W-5 | 🟡 中 | 空间NR均匀性未测量 (窗户开口平面网格扫描) | 有效覆盖区域未知 | 空间扫描 [W] |
| 🟡 | W-6 | 🟡 中 | 因果性互相关测试未做 (参考→误差, 扬声器→误差) | 系统延迟未知 | 互相关 [W] |
| 🟡 | W-7 | 🟡 中 | 室内RT60混响未测量 (>100ms限制高频ANC) | 混响影响未量化 | 声学测量 [W] |
| 🟡 | W-8 | 🟢 低 | 扬声器/误差麦近场条件未验证 (<5cm @1kHz) | 反射声干扰梯度 | 物理布局 [W] |
| 🟡 | W-9 | 🟡 中 | 温度/湿度鲁棒性未测试 (5°C~40°C) | mic_pre_gain和Ŝ温度依赖性 | 环境测试 [W] |

### J. 量产移植专项

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| 🟡 | W-11 | 🔴 高 | QCC5181定点化(float32→int24)梯度截断/量化噪声 | 可能梯度消失 | 定点移植 [W] |
| 🟡 | W-12 | 🟡 中 | RK3566 NPU对M5 CNN(k=80大卷积核)兼容性未知 | NPU加速可能失效 | NPU移植 [W] |
| 🟡 | W-13 | 🟡 中 | ADC通道余量不足 (PCM1865 4ch=E=3+ref=1, 无扩展) | 若需增加E/S须换ADC | 硬件选型 [W] |
| 🟡 | W-14 | 🟡 中 | 次级路径校准代码在git历史 (需恢复+适配) | 量产校准缺关键环节 | git revert [W] |
| 🟡 | W-15 | 🟡 中 | 无统一自动化校准流水线 (FB+Fir+Sec+Pri四步分散) | 量产效率低 | 工具链 [W] |
| 🟡 | W-16 | 🟢 低 | 校准时的窗户状态(开/关)未记录 | 校准条件不可复现 | calibrate_feedback.c [W] |

### K. 升级建议 (深度)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| ❌ | A1 | 🟢 低 | 离线/实时接口统一 (提取共用原语, 消除_rt后缀) | 代码重复 | fxnlms_mimo.c [CR] |
| ❌ | A2 | 🟢 低 | 参数集中管理 (gfanc_config_t替代分散#define) | 调参不便 | 全局 [CR] |
| ❌ | A3 | 🟡 中 | 显式状态机 (STATE_INIT/FADING/SETTLING/CONVERGED等) | 可维护性差 | main_realtime.c [CR] |
| ❌ | B1 | 🟢 低 | 可变步长NLMS (误差自相关驱动μ动态调整) | 收敛速度次优 | fxnlms_mimo.c [CR] |
| ❌ | B3 | 🟢 低 | CNN在线微调 (实际环境样本→fine-tune最后一层) | 场景分类退化 | cnn_m5_forward.c [CR] |
| ❌ | C1 | 🟢 低 | 分级日志+运行时统计记录 | 诊断困难 | 全局 [CR] |
| ❌ | C2 | 🟢 低 | 运行时参数调整 (命令行/命名管道/共享内存) | 调参需重编译 | 全局 [CR] |
| ⚪ | — | — | 增益标定 (NLMS已补偿) | 不实施 | — |
| ⚪ | — | — | 子带处理 (延迟吃因果裕度, CNN预设已消除收敛需求) | 不实施 | — |
| ⚪ | — | — | 自适应步长 (NLMS已做功率归一化, 双环耦合风险) | 不实施 | — |

### L. P2 待评估 / 未来功能 (UPGRADE_ROADMAP 已列入)

| 状态 | ID | 严重度 | 问题 | 影响 | 位置/来源 |
|------|----|--------|------|------|-----------|
| 🟢 | P2-1 | 🟢 低 | 双讲检测 (语音/音乐时冻结LMS, 防ANC干扰期望信号) | 窗户ANC次要(无近端语音) | — |
| 🟢 | P2-2 | 🟢 低 | 舒适噪声注入 (成形噪声掩盖ANC伪影) | 改善静音时主观感受 | — |
| 🟢 | P2-3 | 🟢 低 | 输入过载保护增强 (已实现tanh基础, 可加限幅计数器+过载标志位) | 极端声压场景 | main_realtime.c |
| 🟢 | P2-4 | 🟡 中 | 硬件看门狗 (独立硬件监控, 异常断电) | 量产必需 | 硬件设计 |
| 🟢 | P2-5 | 🟢 低 | 通道间时延精确补偿 (当前固定DSP_DELAY=16) | 亚样本对齐精度 | main_realtime.c |
| 🟡 | P2-6 | 🟡 中 | 窗户ANC声学实验 (参考麦伸窗外, 验证开放空间NR提升) | 预期NR 10-15dB | 实验 |
| — | CR-11 | 🟢 低 | CrossFader期间梯度冻结 (Wc_old快照不再更新, 100ms无自适应) | 正确设计: 过渡期用混合Wc做梯度更新会导致旧场景梯度污染新Wc | main_realtime.c (无需修改) |

---

## 目录

1. [架构设计评估](#1-架构设计评估)
2. [算法正确性分析](#2-算法正确性分析)
3. [逻辑与状态机审查](#3-逻辑与状态机审查)
4. [数据流与信号通路](#4-数据流与信号通路)
5. [数值稳定性与精度](#5-数值稳定性与精度)
6. [性能与实时性](#6-性能与实时性)
7. [代码质量与可维护性](#7-代码质量与可维护性)
8. [测试与验证](#8-测试与验证)
9. [升级方案（深度）](#9-升级方案深度)
10. [总结与优先级](#10-总结与优先级)

---

## 1. 架构设计评估

### 1.1 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        main_realtime.c                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │ 慢速环路 1Hz  │  │ 快速环路 16kHz│  │ 辅助: 反馈抵消    │  │
│  │ (主线程)      │  │ (音频回调)    │  │ + 啸叫检测        │  │
│  │ CNN→Blend→Wc │  │ FxNLMS→anti  │  │ + Wc发散保护      │  │
│  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
│         │                 │                    │             │
│         └─────────┬───────┴────────────────────┘             │
│                   │                                          │
│     ┌─────────────┴─────────────┐                            │
│     │    shared: rt_ctx_t       │                            │
│     │    fx.wc[2048] (8KB)     │  ← 跨线程共享, 需注意      │
│     │    cnn_buf[2][16000]     │  ← 双缓冲+原子交接         │
│     │    scene_wc[8][2048]     │  ← 每场景记忆              │
│     └───────────────────────────┘                            │
└─────────────────────────────────────────────────────────────┘
```

**评分：8/10**

**优点**：
- 三层分离清晰：慢速决策（CNN）、快速执行（FxNLMS）、安全保护（啸叫/发散/FB抵消）各司其职
- 双速率设计（48k→16k→48k）有效降低计算量到 1/3
- 双缓冲 CNN 输入设计优雅，用 `InterlockedExchange` 实现了零样本丢失的线程间数据交接

**待改进**：
- 快速环路和慢速环路对 `fx.wc` 的访问在 x86 上概率性安全但无形式化保证（见 §6.2 线程安全分析）
- 主循环中 CNN 推理、Wc 构造、发散检测、收敛检测、场景切换逻辑全部耦合在一个 `while` 块中（~110 行），职责不清晰

### 1.2 模块依赖图

```
main_realtime.c
├── scene_controller.c  →  cnn_m5_forward.c  →  binary_loader.c
├── fxnlms_mimo.c
├── fir_filter.c
├── howling_detect.c
├── pa_loader.c         →  libportaudio64bit-asio.dll
└── binary_loader.c

main.c (离线)
├── scene_controller.c  →  (同上)
├── fxnlms_mimo.c
├── fir_filter.c
├── binary_loader.c
└── [内联 WAV 读写]
```

依赖层次清晰，无循环依赖。**但存在以下问题**：

1. **`gfanc_types.h` 过于单薄**：仅定义了 `fir_filter_t` 和 `gfanc_float_t`。`fxnlms_mimo_t`、`scene_ctrl_t`、`howling_detect_t` 等核心类型散落在各自的头文件中，缺乏统一的类型注册表
2. **`binary_loader.h` 暴露了 `bin_free` 但 `cnn_m5_forward.c` 中的 `cnn_m5_free` 是空函数**——CNN 权重加载后从不释放，内存泄漏约 2MB（设计选择，非 bug，但应文档化）
3. **`measure_drift.c` 重复 PA 加载代码而非复用 `pa_loader.h`**：约 30 行重复

### 1.3 接口设计评价

**fxnlms_mimo.h 接口**：

```c
// 离线路径
void fxnlms_tick(..., const float *disturbance, float *anti_out, float *err_out);
void fxnlms_forward_only(..., float *anti_out, float *err_out);

// 实时路径
void fxnlms_tick_rt(..., float x_ref, const float *err_meas, float *anti_out);
void fxnlms_forward_rt(..., float x_ref, const float *err_meas, float *anti_out);
```

**评价**：F-A 修复后的双路径设计是正确的架构决策。离线仿真需要 `disturbance` 和 `err_out`（用于 WAV 输出和 NR 评估），实时路径只需要 `x_ref` 和 `err_meas` 直驱梯度。四个函数的职责清晰，参数语义明确。

**待改进**：`_rt` 后缀暗示这是架构分歧点。如果未来离线也需要直接卷积路径，可能需要统一接口。建议将 `x_hist` 推入和 `anti` 卷积提取为独立函数，供两个路径共用。

---

## 2. 算法正确性分析

### 2.1 FxNLMS 算法

#### 2.1.1 实时路径 (`fxnlms_tick_rt`)

**算法流程**：
```
1. Xd roll: 将新的 Fx[E*S] 推入 Xd[E*S*L] 延迟线
2. x_hist push: 将新的 x_ref 推入 x_hist[L]
3. anti[s] = Σ_k Wc[s,k] · x_hist[k]          (物理输出，直接卷积)
4. power[s] = (ε + Σ_e,k Xd[e,s,k]²) / (E·L)  (功率归一化)
5. Wc[s,k] -= μ/power[s] · Σ_e err_meas[e] · Xd[e,s,k]  (梯度更新)
6. Wc[s,k] *= (1 - leak)                        (泄漏)
```

**正确性验证**：

- 步骤 3（物理输出）使用**直接卷积** `Wc ⊗ x_ref`，不经 Ŝ 模型。这是与离线路径的关键区别，也是 F-A 修复的核心——anti 信号经真实次级路径 S 到达误差麦，误差麦直接测量残差驱动梯度。**这是正确的**。
- 步骤 4 的功率归一化分母用 `(E·L)` 而非 Σ|Xd|² 直接值。这等价于 `μ' = μ / (E·L)` 重标定步长。ε=1e-10 与信号功率同比例缩放（因为除以了 E·L）。**数学上正确**。
- 步骤 5 的梯度使用 `Σ_e err_meas[e] · Xd[e,s,k]`——所有 E 个误差信号共同驱动 S 个扬声器的更新。每个扬声器 s 的梯度是所有误差通道的加权和。这与标准 MIMO FxLMS 公式一致：`ΔW[s,k] = -μ · Σ_e e[e] · Xd[e,s,k]`。

**⚠️ 潜在问题：梯度没有按误差通道数归一化**。标准 MIMO FxLMS 中，E 个误差信号的叠加可能造成有效步长放大 √E 倍。当前 E=3，影响因子约 1.7×，在经验调参中被 step_size 隐式吸收，但如果 E 改变（如增加到 6 通道），步长需要重新调整。**建议**：梯度中除以 `sqrtf(E)` 或使用 `μ/power[s]/E`。

#### 2.1.2 离线路径 (`fxnlms_tick`)

离线路径保留了仿真语义：
```
1. Xd roll
2. anti_est[e] = Σ_s,k Wc[s,k] · Xd[e,s,k]  (Ŝ域估计反噪声)
3. err[e] = disturbance[e] + anti_est[e]     (合成误差 = 初级噪声 + Ŝ域反噪声)
4. anti_out[s] = Σ_e,k Wc[s,k] · Xd[e,s,k]   (Ŝ域输出，仅写WAV)
5. 功率归一化
6. 梯度: Wc[s,k] -= μ/power[s] · err[e] · Xd[e,s,k]
```

**正确性**：这是标准的仿真 FxLMS——初级路径 Pri(ref) 生成 disturbance，反噪声经 Ŝ 模型滤波后与之叠加形成误差，梯度由合成误差驱动。**语义正确**，与 Python 参考实现一致。

#### 2.1.3 泄漏（Leak）分析

`Wc *= (1 - leak)`，leak=1e-6。

在 16kHz 采样率下，每秒衰减倍数为 `(1-1e-6)^16000 ≈ e^(-0.016) ≈ 0.984`，即 ~1.6%/秒。1024 tap 的 Wc 在 float32 下可分辨（最小可表示相对变化 ~1.2e-7）。

**合理**：这个衰减速率既能防止 Wc 长期漂移，又不会显著抵消 LMS 的收敛。相当于 Wc 的半衰期约 43 秒——如果 LMS 在这段时间内没有有效更新（信号极静），Wc 会逐渐归零，这是期望的安全行为。

### 2.2 CNN 场景分类

#### 2.2.1 M5 架构

```
Input(16000) → Conv1d(1→64, k=80, s=4) → BN → ReLU → MaxPool(k=4,s=8)
  → ResBlock(ch=64) ×2 → MaxPool(k=4,s=4)
  → ResBlock(ch=64) ×2 → MaxPool(k=4,s=4)
  → GlobalAvgPool → Linear(64→8)
```

总参数量估算：stem conv ~5K + 4×resblock ~148K + fc ~0.5K ≈ **~154K 参数**。单个 16000 样本输入的前向推理约 8ms（README 报告值）。

**评价**：
- 对于 1Hz 推理频率，~8ms 的开销仅占 1 秒窗口的 0.8%，非常充裕
- M5 架构（小卷积核+残差连接+全局池化）是音频场景分类的经典选择
- 输入是 20-1500Hz 带通信号，频谱信息集中在低频，80 样本的大卷积核提供了 ~5ms 的感受野，与 ANC 的有效频率范围匹配

#### 2.2.2 MinMaxScaler 实现

```c
denom = max(audio) - min(audio);
if (denom > 1e-6f) cnn_in[i] = audio[i] / denom;
else memset(cnn_in, 0, ...);  // 静音 → 零向量
```

**分析**：
- 这个实现与 Python 训练代码的 `audio / (max - min)` 完全一致（CODE_REVIEW S-5 已验证）
- **但存在信息丢失**：scaler 不保存训练时的全局 min/max，而是每帧独立计算。这意味着同一噪声在不同音量下的 CNN 输入完全相同（幅度不变性），但同时也意味着：
  - 低音量真实噪声和高音量静音（底噪）可能有相同的缩放后表示 → 场景误分类
  - 1e-6 阈值保护了静音情况，但边界情形（音量~10^-6 的极弱信号）会放大底噪

**建议**：考虑使用训练集的全局 min/max 做归一化（需保存在 `data/` 中），或者用 RMS 归一化替代 minmax。

#### 2.2.3 Softmax 实现

```c
float logit_mx = logits[0];
for (int k = 0; k < K; k++) if (logits[k] > logit_mx) logit_mx = logits[k];
for (int k = 0; k < K; k++) { probs_out[k] = expf(logits[k] - logit_mx); sum_exp += probs_out[k]; }
for (int k = 0; k < K; k++) probs_out[k] /= sum_exp;
```

**正确**：标准 log-sum-exp 技巧防止 exp 溢出。

### 2.3 Blend 与 Wc 构造

```c
// blend = centroid[scene_id]  →  shape [S, C] = [2, 15]
b = blend[s*C + c] / max(blend)  →  max 归一化到 [0, 1]
if (b < 0) b = 0; if (b > 1) b = 1;
Wc[s,l] = Σ_c b · sub_filters[c,s,l]
Wc = -Wc
```

**算法评价**：

1. **Max 归一化**：保证了每个场景至少有一个子滤波器的权重为 1.0。但 TODO-1 指出的问题是真实存在的——如果 centroid 中有一个异常峰值（训练噪声），其他 14 个权重会被系统性压低。需要检查实际 centroid 数据的 `bmax/bmax2` 分布来判断严重程度。

2. **负权重截断 `if (b < 0) b = 0`**：Python 训练中 blend 权重可能出现负值（线性回归无约束）。截断为 0 是正确的——负权重意味着"反向使用该子滤波器"，这在物理上没有意义（子滤波器已经是相位对齐的）。

3. **裁剪 `if (b > 1) b = 1`**：理论上 b 经过 max 归一化不会 >1，但浮点误差可能导致边界溢出。这条语句是防御性的，不会触发但无害。

4. **取反 `Wc = -Wc`**：因为 anti = Wc ⊗ ref 需要与噪声反相才能抵消。Python 训练时子滤波器已经编码了正确的相位，取反将"噪声预测"转为"反噪声"。**这是正确的**。

5. **S-4 修复（移除强制 RMS 对齐）**：之前 `Wc *= stub_rms / rms(Wc)` 会抹除 blend 权重隐含的增益信息。移除后 LMS 功率归一化自动适应增益。**这个改动是正确的**——LMS 的功率归一化本身就是增益自适应机制，额外的强制标定是冗余且有害的。

### 2.4 啸叫检测算法

**流程**：
```
1. 累积 256 样本 (16ms @ 16kHz)
2. 汉宁窗 → DFT 23 bins (125Hz~1500Hz, 步长 62.5Hz)
3. 找最大功率 bin → 峰均值比 = 10*log10(peak/mean)
4. peak/mean > 15dB → 候选频率
5. 连续 4 帧(64ms) 同一频率 → 确认啸叫 → 添加 IIR 陷波
6. 连续 8 帧(128ms) 无候选 → 释放所有陷波器
```

**算法评价**：

1. **DFT vs FFT**：使用 23 个独立 DFT bin（而非 256 点 FFT）在只关心窄带范围时是正确的选择——23 个 bin 的 DFT 计算量是 23×256≈6K 次乘加，远小于 256 点 FFT 的 ~2K 次（但 FFT 给出全频带）。这里 DFT 的针对性更强。

2. **汉宁窗**：正确使用，减少频谱泄漏。窗函数牺牲了约 1.8dB 的 SNR 但换来了更好的频率选择性。

3. **15dB 阈值**：峰均值比 >15dB 意味着候选频率的功率是平均功率的 31.6 倍。对于真实窄带啸叫（单音自激）这完全够用，但可能漏检多音啸叫（功率分散到多个 bin，单个峰不明显）。

4. **4 帧确认/8 帧释放**：这是合理的滞回设计。确认 64ms 排除了瞬时尖峰（如关门声），释放 128ms 防止啸叫短暂衰减时误释放。

5. **⚠️ 硬释放 `hw->active_count = 0`**：当 `release_timer >= HW_RELEASE` 时直接清空所有陷波器，不做逐频率管理。这意味着：如果啸叫 1 先被检测并被陷波，然后啸叫 2 出现，但啸叫 1 仍在（只是被陷波后频谱中不可见），当频谱中无候选频率时，release_timer 计数 → 清空 → 啸叫 1 可能重新出现。当前设计靠"陷波后啸叫不可见→无候选→release 递增"的循环是有问题的——陷波器自身掩盖了啸叫，导致 release 触发。**但实际影响可能有限**，因为陷波后啸叫确实被抑制了，即使释放后重新出现也会被重新检测并陷波。这是一个可接受的振荡行为（周期约 192ms 确认+释放），而非发散。

**建议**：后续可改为基于时间的陷波器寿命管理（每个陷波器独立 TTL），而非全局批量释放。

### 2.5 场景切换 CrossFader

```c
// fade_cnt 从 FADE_LEN=1600 递减到 0
a = fade_cnt / FADE_LEN;       // 1 → 0
Wc = a · Wc_old + (1-a) · Wc_new // 线性混合
```

**分析**：100ms（1600 样本）的线性 CrossFade 对于 Wc（1024 tap FIR 系数）的混合是合理的。在 FxNLMS 梯度持续更新的情况下，Wc 的平滑过渡可以避免输出跳变。20Hz 正弦波周期为 50ms，100ms = 2 个完整周期，确保最低有效频率的平滑过渡。

**⚠️ 注意**：CrossFader 期间梯度在混合后的 Wc 上更新（`fxnlms_forward_rt` 跳过梯度），但混合公式中的 `Wc_old` 是切换瞬间的快照（不会再被更新）。这意味着 CrossFade 期间 Wc 从旧的快照平滑过渡到新的 CNN 预设值，而 CNN 预设值是固定的。如果旧场景和新场景的 Wc 差异很大，100ms 内 FxNLMS 不做自适应——这是设计选择，避免过渡期间梯度混乱。

---

## 3. 逻辑与状态机审查

### 3.1 场景状态机

```
                    ┌──────────┐
          INIT ───→ │ NORMAL   │ ←─── 连续3帧NR>3dB → 保存scene_wc
          ramp=1    │ (稳定场景)│
          mute=1    └─────┬────┘
                          │ cos(anchor,cur) < 0.8 && new_scene != cur
                          ↓
                    ┌──────────┐
                    │ SWITCH   │
                    │ fade=100ms│
                    │ mute_hold │
                    └────┬─────┘
                         │ fade 完成后
                         ↓
                    ┌──────────┐
                    │ NORMAL   │  (新场景)
                    └──────────┘
```

**评价**：

1. **收敛检测的条件**：`nr_level > 3.0f && !safety_mute`——NR>3dB 且不在安全静音状态。3dB 阈值偏低（仅 2× 功率比），但配合连续 3 帧确认（3 秒），排除了瞬时波动。**合理**。

2. **⚠️ 收敛检测在保存后重置计数**：`converged_frames = 0` 在保存后重置。这意味着每 3 秒就会写一次 `scene_wc`（如果持续 NR>3dB）。这是有意为之——持续更新已收敛的 Wc。但频繁的 `memcpy 8KB` 对性能无影响（每秒最多 1/3 次），可接受。

3. **场景切换的 `anchor_probs` 更新**：INIT 和 SWITCH 都会更新 anchor。正确——每次进入新场景都需要新的锚点。

4. **⚠️ `memcpy(ctx->sc.prev_probs, probs, ...)` 在主循环末尾无条件执行**：即使 CNN 推理失败（回退到上一帧），prev_probs 也会被更新。但 cnn_m5_forward 的失败路径已经返回了 prev_probs，所以这是无操作。不会造成问题。

### 3.2 冷启动与安全状态

```
冷启动: ramp_cnt = RAMP_SAMPLES (400ms), mute_hold = MUTE_HOLD_SAMPLES (1500ms)
  0-400ms:  ramp 0→1, mute_hold 递减
  400-1500ms:  ramp 已退出, mute_hold 继续递减 (safety_mute 不触发)
  1500ms+:  完全正常运行

场景切换: fade_cnt = FADE_LEN (100ms), mute_hold = MUTE_HOLD_SAMPLES (1500ms)
  0-100ms:  CrossFade, mute_hold 递减 (safety_mute 不触发)
  100-1500ms:  CrossFade 结束, mute_hold 继续递减
  1500ms+:  完全正常运行
```

**评价**：mute_hold 的 1500ms 设计是合理的——CrossFade 后 FxNLMS 需要时间收敛到新场景的 Wc，这期间的 err_rms 可能很高。mute_hold 阻止了这期间的 false positive safety_mute 触发。400ms ramp 只在冷启动时使用，避免了 INIT 时 Wc=0 导致的瞬间冲击。

### 3.3 safety_mute 逻辑

```c
ctx->safety_mute = (ctx->err_rms > ctx->ref_rms && ctx->ref_rms > 0.0001f
                    && ctx->mute_hold == 0);
```

**分析**：
- `err_rms > ref_rms`：ANC 输出比输入噪声更大 → 正反馈/发散迹象
- `ref_rms > 0.0001f`：排除极静环境（NR 计算不可靠）
- `mute_hold == 0`：冷启动和场景切换期间抑制

**⚠️ 评估粒度**：每秒评估一次（FS_ANC 样本累计）。如果在前 100ms 已经发散，需要等 900ms 才能检测到。这是实时系统的固有折衷——更快的检测需要更短的平均窗口，但 RMS 估计的方差增大。

**建议**：可增加基于峰值/瞬时功率的快检测通道（如连续 10 个样本 anti>0.95 → 触发），与慢速 RMS 检测互补。

### 3.4 啸叫检测状态机

```
IDLE ──peak/mean>15dB──→ CANDIDATE(count=1)
  ↑                          │
  │                    freq≈prev? ──yes──→ count++
  │                          │no            │
  │                          ↓              ↓ count>=4?
  │                     count=1,new freq   确认啸叫→add_notch
  │                          │              │
  │                          │              ↓
  │                          │         NOTCHING
  │                          │              │
  │                          │   无候选 (啸叫被陷波抑制)
  │                          │              │
  │                          └──────→ release_timer++
  │                                        │
  └──────── release_timer>=8 ←────────────┘ (释放所有陷波)
```

**⚠️ 状态机缺陷**：如 §2.4 分析的，NOTCHING 状态下啸叫被陷波抑制后频谱变干净→无候选→release 递增→释放→啸叫重现。这是设计的固有振荡行为。建议：
- 陷波器加最小保持时间（如 500ms），防止快速释放-重新检测死循环
- 或使用 H∞ 鲁棒检测（考虑陷波器对频谱的影响）

### 3.5 Wc 发散保护

```c
if (max|Wc| > 5×stub_rms) → freeze_lms = 1  // 冻结梯度
// INIT/场景切换时: freeze_lms = 0
```

**评价**：
- stub_rms 是 15 个子滤波器等权求和后的 RMS，反映"正常"Wc 的量级。5× 是合理的阈值——Wc 系数不可能无原因地增长 5 倍
- 冻结后没有自动恢复机制。这意味着一旦触发，ANC 会以固定 Wc 运行直到场景切换。**可能过于保守**——如果 freeze 是由瞬时扰动触发（如风吹麦克风），冻结后的 Wc 仍是好的，但 anc 不再自适应

**建议**：增加 freeze 超时自动恢复（如 freeze 超过 60 秒后尝试解冻，若 3 秒内再次触发则永久冻结直到场景切换）。

---

## 4. 数据流与信号通路

### 4.1 48k→16k 抽取

```c
// main_realtime.c:113-116
ref_buf[n] = in[(n*3)*6 + 0];       // 每 3 个 48k 样本取 1 个
err_buf[n*3+e] = in[(n*3)*6 + 1+e]; // 最近邻抽取
```

**分析**：
- **无抗混叠滤波**（P-1）。最近邻抽取等价于在抽取前不做低通滤波。>8kHz 的频率分量会折叠进 0-8kHz 的奈奎斯特区间。
- **实际缓解因素**：
  - 硬件麦克风的模拟抗混叠滤波器（通常在 ADC 前）
  - 20-1500Hz 带通 FIR 截断了折叠到 0-8kHz 的大部分能量
  - 真实环境噪声在 >8kHz 的能量通常有限
- **残余风险**：14.5-16kHz → 折叠到 0-1.5kHz（正好在 ANC 通带内）。如果环境中存在 15kHz 强音（如 CRT 电视的行频），会在 ANC 通带内产生虚假信号。

**建议**：如果 CPU 预算允许，加一个简单的 2 阶 IIR 低通（fc≈6kHz）在抽取前。约 10 次乘加/样本，回调总开销增加 ~1.5%。

### 4.2 16k→48k 内插

```c
// main_realtime.c:261-269
for (int n = 0; n < c16k; n++) {
    for (int r = 0; r < 3; r++) { out[oi++] = a0; out[oi++] = a1; }
}
```

ZOH（零阶保持）——每个 16k 样本重复 3 次到 48k。频谱上产生 16kHz 和 32kHz 的镜像。**缓解**：DAC 内置重建滤波器 + 扬声器机械低通 + 人耳高频不敏感。

### 4.3 反馈抵消路径

```
anti_spk[s] → fb_fir[s] → fb_est[s] ──→ ref_sample = (ref_raw - Σ fb_est) × GAIN
```

**信号流分析**：
- fb_est 是从 anti_spk 经 FIR 估计在参考麦处的声反馈分量
- 减法 `ref_raw - fb_est` 试图消除参考麦接收到的扬声器串扰

**⚠️ 被减信号是 anti_spk（已钳位、已陷波、已 ramp/mute 处理后的值）还是原始 anti？** 当前代码中反馈抵消在 anti 计算后、但在钳位/陷波/ramp/mute 之前就使用了 `anti_spk[s]` 值。但 `fb_fir` 的 `tick` 调用在回调循环的最开始（127-132 行），用的是上一轮循环末尾的 `anti_spk` 值（通过 `anti_spk_prev` 继承）。而 `anti_spk` 随后在本轮更新为新的 FxNLMS 输出。

**这意味着 fb_fir 的输入总是上一轮循环的 anti 值（跨回调则从 anti_spk_prev 继承）**，而当前轮的新 anti 要到下一轮才被反馈抵消使用。这是一个**1 样本延迟**（62.5μs），在 256 tap FIR 中可忽略。

### 4.4 NR 计算路径

实时版：
```c
// 扰动 = err_meas[e]² (实测误差麦信号)
// anti_est = Σ_s,k Wc[s,k] · Xd[e,s,k]  (Ŝ模型估计)
// NR = 10·log10((pe + pa)/(pe + ε))
```

**⚠️ NR 计算使用的是模型估计 anti_est 而非实际声学反噪声**：
- `anti_est` 是 Wc 在 Ŝ 域的投影——即"如果 Ŝ 模型完美，反噪声在误差麦处的声学效果"
- 实际声学反噪声是 `real_S ⊗ anti_output`（真实次级路径滤波后的输出）
- 如果 Ŝ ≠ real_S，显示的 NR 不等于真实声学 NR

这在离线仿真中是合理的（Ŝ 域 NR 是标准评估方式），但在实时版中只作为参考指标。真实的声学 NR 只能通过外部测量验证。

---

## 5. 数值稳定性与精度

### 5.1 Float32 精度边界

| 操作 | 典型值 | Float32 精度 | 风险 |
|------|--------|-------------|------|
| `1.0f - leak` (leak=1e-6) | 0.999999 | 约 7 位有效数字 | ✅ 安全 |
| `1.0f - 1e-9f` | 1.0 | **不可分辨** | ✅ 已修复（原 leak 公式） |
| `power + 1e-10f` (power~10⁻⁶) | ~10⁻⁶ | 足够 | ✅ 安全 |
| `power + 1e-10f` (power~10⁻¹²) | ~10⁻¹⁰ | epsilon 主导 | ⚠️ 边界 (TODO-4) |
| `expf(logit - max)` | ~e⁰=1 | 指数可分辨 | ✅ 安全 |
| `sqrtf(pe + pa + 1e-12f)` (值~10⁻⁶) | ~10⁻³ | 足够 | ✅ 安全 |

### 5.2 FIR 延迟线精度

```c
typedef struct {
    gfanc_float_t *coeffs;   // float32
    double        *delay_line; // float64 ← 注意！
    int            n_taps;
    int            ptr;
} fir_filter_t;
```

**评价**：延迟线使用 `double`（float64）是正确的设计选择：
- FIR 的核心操作是 `y = Σ c[k] · x[n-k]`，涉及大量乘积累加
- float32 在 1024+ tap 的 FIR 中会积累舍入误差（每次乘法产生 ~0.5 ULP 误差，1024 次累加最多 ~10 bits 精度损失）
- double 延迟线将舍入误差降低了 ~10³ 倍，确保输出信噪比
- 系数保持 float32（与 Python 训练一致，且节省 50% 的内存带宽）

**但**：延迟线 double 和系数 float32 之间的乘法是 float32×float64 → float64，符合 IEEE 754 混合精度规则。累加器使用 double，截断为 float32 仅在最终返回时发生一次。**数值上最优**。

### 5.3 IIR 陷波器状态

```c
// howling_detect.c: 状态使用 float32
float x1[HW_S][HW_MAX_NOTCHES], x2[...], y1[...], y2[...];
```

对于 r=0.96 的陷波器，极点靠近单位圆。在极低频率（125Hz @ 16kHz），归一化频率 ω=0.049rad，cos(ω)≈0.999。直接形式 IIR 对极点靠近单位圆的滤波器容易出现极限环（limit cycle）——舍入误差在反馈环路中被放大。

**当前配置**：r=0.96 → 极点半径 0.96 → 距单位圆 0.04 → 相对安全。但如果需要更窄的陷波（r→0.99），建议切换到 DF2T（Transposed Direct Form II）结构或使用 double 状态。

---

## 6. 性能与实时性

### 6.1 回调计算预算

以 16kHz 处理速率、每次回调处理 ~32 个样本（48kHz framesPerBuffer=96, 16kHz = 32 样本）估算：

| 操作 | 每次调用 | 每样本 | 每回调(~32样本) |
|------|---------|--------|----------------|
| fb_fir ×2 (256tap) | 2 | 512 MAC | 16,384 |
| bp_fir (1024tap) | 1 | 1,024 | 32,768 |
| bp_err ×3 (1024tap) | 3 | 3,072 | 98,304 |
| sec_firs ×6 (1040tap) | 6 | 6,240 | 199,680 |
| FxNLMS fwd (Wc⊗x_hist, 1024tap×2) | 2 | 2,048 | 65,536 |
| FxNLMS grad (E×S×L) | - | 6,144 | 196,608 |
| crossfader (跨 Wc, fade期间) | - | 2,048 | 65,536 |
| 内插 (3×) | 2 | 6 | 192 |
| **合计** | | **~21,118 MAC** | **~676K MAC** |

以 2GHz CPU、4 MAC/cycle（SIMD）估算：676K / (2G × 4) ≈ 0.085ms / 回调。32 样本的实时窗口是 32/16000 = 2ms。**回调预算 <5%**（远低于 README 报告的 73%，可能因为 README 的 73% 是指在老旧硬件的单标量路径上的估算）。

**结论**：当前实现有充足的性能余量。

### 6.2 线程安全分析

**共享状态**（主线程 ↔ 音频回调线程）：

| 变量 | 访问模式 | x86 安全？ | ARM 安全？ |
|------|---------|-----------|-----------|
| `fx.wc[2048]` | 主线程: memcpy 读/写, 回调: 逐元素读/写 | ⚠️ 概率性 | ❌ 可能崩 |
| `fx.freeze_lms` | 主线程: 写, 回调: 读 | ⚠️ 概率性 | ❌ |
| `cnn_buf_ready` | 主线程: InterlockedExchange, 回调: InterlockedExchange | ✅ | ✅ |
| `fade_cnt` | 主线程: 写, 回调: 读 | ⚠️ 概率性 | ❌ |
| `ramp_cnt` | 主线程: 写, 回调: 读 | ⚠️ 概率性 | ❌ |
| `safety_mute` | 回调: 写, 主线程: 读 | ⚠️ 概率性 | ⚠️ |
| `nr_level` 等 volatile | 回调: 写, 主线程: 读 | ⚠️ 概率性 | ⚠️ |

**详细分析**：

1. **`fx.wc` 并发访问**（§6.2）：这是最严重的问题。主线程在场景切换时执行 `memcpy(fx.wc, wc_cur, 8KB)`，而回调同时在读 `fx.wc` 做卷积和写 `fx.wc` 做梯度更新。x86 的 TSO（Total Store Order）内存模型在大部分情况下提供隐式一致性，但没有任何形式化保证。在 ARM relaxed memory model 下，回调可能看到部分更新的 Wc，导致计算错误的 anti 输出。

2. **`fade_cnt` 的读-改-写**：回调中 `fade_cnt--` 不是原子的，主线程写 `fade_cnt=FADE_LEN` 也不是。在 x86 上对齐的 int 读写是原子的，但读-改-写（递减）不是。

3. **volatile 的正确用法**：`volatile` 确保每次访问都从内存读取（不缓存在寄存器中），但不提供原子性或多线程排序保证。当前对 nr_level 等监控变量的用法是"可接受的脏读"——数值可能短暂不一致但不影响控制逻辑。

**建议修复方案**（已在 UPGRADE_ROADMAP 中记录）：
```c
// 方案: 影子缓冲 + 序号
float wc_shadow[2][S*L];  // 双缓冲
volatile LONG wc_seq;      // 奇数=主线程在写, 偶数=可安全读取
// 主线程: InterlockedIncrement(&wc_seq); memcpy(wc_shadow[wc_seq%2], new_wc); InterlockedIncrement(&wc_seq);
// 回调:   seq = wc_seq; if (seq%2==0) use wc_shadow[seq/2]; else use old;
```

### 6.3 内存使用

```
rt_ctx_t:  ~12KB (堆分配)
  scene_wc[8][2048] = 64KB
  cnn_buf[2][16000] = 128KB
  wc_old/cur ×2 = 16KB
  sec_coeffs + delay_lines = ~50KB
  ref/anti/err buffers = ~480KB
  fb_fir delay_lines = ~4KB
  howling_detect_t = ~3KB

CNN 静态缓冲: 1MB (static)
CNN 权重: ~600KB (全局 + .bin 文件)
次级路径权重: ~25KB
其他 .bin: ~250KB

总计: ~2.5MB (运行时) + ~900KB (权重文件)
```

在 2026 年的任何平台上（包括嵌入式 ARM），2.5MB 内存都是完全可接受的。

### 6.4 回调内的动态分配

**审查结论：回调路径零动态分配** ✅

- `fir_tick`：纯栈操作
- `fxnlms_tick_rt`：纯栈操作
- `howling_tick`：局部数组 `frame[256]` 在栈上，其余纯栈

这是实时系统的正确实践。

---

## 7. 代码质量与可维护性

### 7.1 优点

1. **中文注释质量高**：每个宏、结构体字段、关键逻辑都有清晰的中文注释，阐释设计意图而非复述代码
2. **常量宏化**：所有魔法数字都是 `#define`（FS_HW, FADE_LEN, MIC_PRE_GAIN 等），调参方便
3. **头文件守卫**：所有 .h 都有 `#ifndef`/`#define` 保护
4. **编译整洁**：单个 Makefile 覆盖所有构建目标，无复杂构建系统
5. **模块内聚**：每个 .c 文件职责单一，依赖通过头文件显式声明

### 7.2 待改进

#### 7.2.1 主循环职责过重

[main_realtime.c:415-524](main_realtime.c) 的主循环约 110 行，混合了：
- CNN 双缓冲读取
- Wc 发散检测
- 收敛检测 + 场景记忆保存
- 场景切换检测 + CrossFade 触发
- 统计打印

**建议**：拆分为独立函数：
```c
static void handle_cnn_result(rt_ctx_t *ctx, const float *probs, int new_scene);
static void check_wc_divergence(rt_ctx_t *ctx);
static void check_convergence(rt_ctx_t *ctx);
static void check_scene_switch(rt_ctx_t *ctx, float cos_sim, int new_scene);
```

#### 7.2.2 错误处理不一致

- `fxnlms_init` 使用 `calloc` 但不返回错误码（假设成功）
- `scene_ctrl_init` 使用 `calloc` 但不检查返回值
- `main_realtime.c` 的初始化路径中，如果某个 `calloc` 失败，已分配的资源不会释放

**建议**：至少对关键分配添加 NULL 检查和回滚。

#### 7.2.3 代码重复

1. **`measure_drift.c` 重复 PA 加载代码**：约 30 行与 `pa_loader.c` 完全重复。应 `#include "pa_loader.h"` 并链接 `pa_loader.c`
2. **`main.c` 和 `main_realtime.c` 的权重加载**：`bin_load_float` 调用序列几乎相同

#### 7.2.4 缺少单元测试

当前无任何测试代码。对于信号处理系统，建议至少：
- FIR 滤波器脉冲响应测试
- FxNLMS 在已知输入下的收敛测试
- CNN 前向与 Python 参考的逐位对比测试
- 啸叫检测在合成啸叫信号上的测试

#### 7.2.5 `cnn_m5_free` 是空函数

[cnn_m5_forward.c:123-126](src/cnn_m5_forward.c) 的 `cnn_m5_free` 注释写"simplified — in production, track all allocations"但从未实现。CNN 权重（~600KB）在程序运行期间永不释放。对于长时间运行的系统（特别是嵌入式），如果将来需要动态重载模型（OTA 更新），这是一个技术债。

#### 7.2.6 硬编码采样率

FS_HW=48000, FS_ANC=16000 是硬编码的 `#define`。如果切换到 44100Hz 设备，所有抽取/内插逻辑需要手动修改。

**建议**：从 `PaDeviceInfo` 查询实际采样率，或至少做编译期验证。

---

## 8. 测试与验证

### 8.1 当前验证状态

| 测试类型 | 状态 | 说明 |
|---------|------|------|
| 离线 WAV 处理 | ✅ | main.c, 7 种混合噪声 56s 测试, 平均 NR 15dB |
| 实时音频流 | ✅ | ASIO 声卡实测, NR 4-9dB (窗户开口) |
| 反馈抵消校准 | ✅ | calibrate_feedback.exe 自检输出 RMS |
| 时钟漂移 | ✅ | measure_drift.exe 互相关测试 |
| 啸叫检测 | ⚠️ | DFT+陷波已实现, 无系统化的合成啸叫测试 |
| Wc 发散保护 | ⚠️ | 阈值检测已实现, 未测试触发和恢复路径 |
| 场景切换平滑度 | ⚠️ | CrossFader 已实现, 未测量切换期间的 NR 瞬态 |

### 8.2 建议的测试策略

**离线测试**（零硬件依赖）：
1. **回归测试**：固定输入噪声文件 + 固定权重 → 逐位对比 anti_out.wav 和 error_out.wav
2. **FIR 正确性**：脉冲响应 δ[n] 经过任一 FIR 应与系数完全一致
3. **CNN 正确性**：固定 16000 样本输入 → 对比 Python 推理输出的 logits
4. **啸叫检测**：合成单音信号（如 500Hz 正弦波）→ 验证检测和陷波

**硬件在环测试**（需要 ASIO 声卡）：
1. **次级路径验证**：播放宽带噪声→录制误差麦→互相关辨识真实 Ŝ→与模型中 Ŝ 比较
2. **端到端降噪**：已知噪声源 + 声级计 → 验证真实降噪量
3. **场景切换压力测试**：快速交替 2 种噪声（每 3 秒切换）→ 验证无冲击、无消音

---

## 9. 升级方案（深度）

以下方案超出了现有 UPGRADE_ROADMAP.md 的范围，是从架构和算法层面提出的深度改进建议。

### 9.1 架构级改进

#### A1. 离线/实时统一接口

**当前状态**：`fxnlms_tick`（离线）和 `fxnlms_tick_rt`（实时）是两个独立函数，共享 `xd_roll_write` 但输出路径不同。

**建议**：提取共用原语为独立函数：
```c
// 原语
void fxnlms_update_xd(fxnlms_mimo_t *fx, const float *Fx);     // Xd roll
void fxnlms_update_xhist(fxnlms_mimo_t *fx, float x_ref);      // x_hist push
void fxnlms_compute_anti_direct(fxnlms_mimo_t *fx, float *anti); // Wc ⊗ x_hist
void fxnlms_compute_anti_model(fxnlms_mimo_t *fx, float *anti);  // Wc ⊗ Xd (Ŝ域)
void fxnlms_compute_err_model(fxnlms_mimo_t *fx, float *err);    // anti_est (Ŝ域)
void fxnlms_update_gradient(fxnlms_mimo_t *fx, const float *err, const float *power); // 梯度
```

上层组装不同的处理流水线。消除 `_rt` 后缀和函数重复。

#### A2. 参数集中管理

```c
typedef struct {
    int fs_hw, fs_anc;
    int E, S, L;
    int bp_len, sec_len, fb_len;
    float mic_pre_gain;
    float step_size, leak;
    int fade_len, ramp_ms, mute_hold_ms;
    float switch_threshold;  // cos_sim 阈值
    float freeze_ratio;      // Wc 发散阈值倍数
    float nr_converge_db;    // 收敛判定 NR 阈值
} gfanc_config_t;
```

所有模块从统一的 config 结构读取参数，而非各自 `#define`。便于：
- 运行时从文件加载配置
- 不同硬件平台的参数切换
- 自动化参数扫描/调优

#### A3. 事件驱动的状态管理

当前状态转换（INIT → NORMAL → SWITCH → NORMAL）通过标志位（first_sec, fade_cnt, ramp_cnt, mute_hold）管理。建议改为显式状态枚举：

```c
typedef enum {
    STATE_INIT,        // 冷启动中
    STATE_FADING,      // 场景切换 CrossFade 中
    STATE_SETTLING,    // 切换后静默期 (mute_hold)
    STATE_CONVERGING,  // 正常自适应, 等待收敛
    STATE_CONVERGED,   // 已收敛, 定期保存 Wc
    STATE_FROZEN,      // Wc 发散, 梯度冻结
    STATE_MUTED,       // safety_mute 触发
} anc_state_t;
```

状态转换在统一的状态机函数中处理，每个状态有明确的进入/退出动作。提升代码可读性和可测试性。

### 9.2 算法级改进

#### B1. 可变步长 NLMS (VS-NLMS)

当前固定 step_size=0.0001。在平稳噪声下步长可以更大（收敛更快），在非平稳噪声下步长应更小（防止发散）。

**建议**：基于误差信号自相关的步长调整：
```
μ[n] = μ_max · (1 - |ρ[n]|)  +  μ_min · |ρ[n]|
ρ[n] = E[e[n]·e[n-1]] / E[e[n]²]   // 误差的一阶自相关系数
```
ρ≈0 时（白误差，未收敛）→ μ 大（快速收敛）
ρ≈1 时（高自相关，已收敛或发散）→ μ 小（精细调整/防发散）

注：UPGRADE_ROADMAP 标记为"不实施"，原因是 NLMS 已有功率归一化。但功率归一化是"信号级"自适应（响度变化），自相关步长是"状态级"自适应（收敛/未收敛），两者作用于不同维度，并不冲突。

#### B2. 子带 ANC（已评估不实施，但建议重新考虑）

**原评估**：延迟吃因果裕度

**重新考虑**：如果子带分解使用**零延迟滤波器组**（如 sliding DFT 或 polyphase IIR），延迟可控制在 2-3ms，比当前 32ms 的 FIR 群延迟小一个数量级。子带 ANC 的真正优势——不同频段不同步长（低频大步长、高频小步长）——在多音/谐波噪声（如引擎噪声）中有显著收益。

**适用范围**：引擎谐波、变压器噪声等窄带多音源。对于当前目标的白噪声/宽带道路噪声，子带优势有限。

#### B3. CNN 在线微调

当前 CNN 权重是固定的（Python 离线训练）。场景分类的正确率决定了 Wc 初始值的质量。如果实际使用环境与训练数据差异大（如窗户 ANC vs 车内 ANC），场景分类会退化。

**建议**：在保存 `scene_wc[scene_id]` 时同时保存该场景的最后一秒音频。积累足够样本后，用这些样本对 CNN 做在线微调（如 1 epoch fine-tuning）。对于嵌入式平台，可以用知识蒸馏（大 CNN → 小 CNN）或只更新最后一层（Linear 层）来降低计算量。

#### B4. 自适应反馈抵消

当前 `fb_fir` 系数是通过 `calibrate_feedback.exe` 离线校准的，固定不变。但声学反馈路径可能随温度、麦克风位置微调而变化。

**建议**：在线 NLMS 更新 fb_fir 系数（使用 anti_spk 作为参考，ref_sample 作为误差）：
```c
// 在 ref_sample 计算后
float fb_err = ref_sample - (ref_raw_bp + fb_est) * MIC_PRE_GAIN; // 仅反馈分量
// 用 fb_err 和 anti_spk 历史更新 fb_fir
```
需要在 anti_spk 和 ref_raw 之间对齐延迟。步长应极低（如 0.001）以防止反馈抵消和 ANC 前馈两个自适应环路耦合。

### 9.3 工程化改进

#### C1. 日志与诊断框架

当前所有诊断信息通过 `printf` 输出到 stdout。建议：
- 分级日志（INFO/WARN/ERROR/DEBUG）
- 运行时统计记录（NR 时间序列、场景切换日志、Wc 发散事件）
- 保存到文件以便离线分析

#### C2. 运行时参数调整接口

当前所有参数是编译期 `#define`。实时系统中调整参数（如 MIC_PRE_GAIN, step_size）需要重新编译。建议：
- 通过命令行参数或环境变量覆盖关键参数
- 实时版可通过命名管道或共享内存接收参数更新（无需重启音频流）

#### C3. 自动化校准流水线

当前反馈路径校准是独立程序 `calibrate_feedback.exe`。次级路径校准的代码在 git 历史中。建议：
- 将所有校准功能合并到一个 `gfanc_calibrate.exe`
- 校准流程自动化：播放→录制→辨识→保存，单命令完成全部校准
- 校准结果验证：检查 FIR RMS、峰值位置是否合理

---

## 10. 总结与优先级

### 10.1 总体评分

| 维度 | 评分 | 说明 |
|------|------|------|
| 架构设计 | 8/10 | 三层分离清晰, 双速率设计合理, 双路径修复正确 |
| 算法正确性 | 8/10 | FxNLMS/Core 算法正确, 啸叫检测存在设计折衷 |
| 数值稳定性 | 8/10 | double 延迟线 + float32 系数是最优混合精度 |
| 实时性能 | 9/10 | 零取模 FIR, 回调预算 <5%, 零动态分配 |
| 线程安全 | 5/10 | x86 概率性安全, 跨平台前必须修复 (最大短板) |
| 代码质量 | 7/10 | 注释优秀, 模块化良好, 但主循环过长, 缺少测试 |
| 可维护性 | 7/10 | 常量宏化, 单一 Makefile, 但参数分散, 缺少测试 |
| 文档完整性 | 9/10 | README + COMPREHENSIVE_REVIEW + UPGRADE_ROADMAP + WINDOW_ANC_SUPPLEMENT 覆盖全面 |

### 10.2 优先级排序

#### 🔴 高优先级（阻塞量产/跨平台）

| 项目 | 预计工作量 | 说明 |
|------|-----------|------|
| §6.2 跨线程同步 | 2-3 天 | ARM 移植前必须修复，影子缓冲+原子序号 |
| F-B 次级路径实测 | 1 天 | ASIO 校准，git revert 恢复源码 |

#### 🟡 中优先级（显著改善）

| 项目 | 预计工作量 | 说明 |
|------|-----------|------|
| A3 事件驱动状态机 | 2 天 | 提升可维护性和可测试性 |
| 啸叫检测硬释放改进 | 1 天 | 陷波器最小保持时间，防止振荡 |
| 离线/实时统一接口 (A1) | 2 天 | 减少函数重复，便于未来扩展 |
| 在线反馈抵消自适应 (B4) | 2 天 | 补偿温度/位置漂移 |
| P-1/P-2 抗混叠/抗镜像 | 0.5 天 | IIR 低通滤波器 |

#### 🟢 低优先级（锦上添花）

| 项目 | 预计工作量 | 说明 |
|------|-----------|------|
| A2 参数集中管理 | 1 天 | 运行时配置加载 |
| B1 可变步长 NLMS | 1 天 | 动态 μ 提升收敛速度 |
| C1 日志框架 | 1 天 | 结构化日志 |
| C2 运行时参数接口 | 0.5 天 | 命令行/环境变量覆盖 |
| 单元测试 | 3-5 天 | FIR / CNN / FxNLMS / Howling 模块测试 |

### 10.3 技术债清单

1. ~~`fir_tick` 取模性能~~ ✅ 已修复
2. ~~leak 公式无效~~ ✅ 已修复
3. ~~F-A 误差双重计入~~ ✅ 已修复
4. **跨线程数据竞争** ← **当前最大技术债**
5. **缺少单元测试** ← 回归测试基础
6. **`cnn_m5_free` 空函数** ← OTA 更新阻塞
7. **`measure_drift.c` PA 代码重复** ← 维护负担
8. **次级路径未实测校准** ← 精度天花板

---

> **结论**：GFANC FxNLMs 的 C 实现在架构设计、算法正确性和实时性能方面表现优秀。17 项已修复问题体现了迭代打磨的质量。当前最大的技术风险是**跨线程数据竞争**（§6.2），在 ARM 移植前必须解决。其次，**缺少自动化测试**使得回归验证依赖于手动运行，阻碍持续迭代。整体而言，这是一个工程质量较高的实时 DSP 系统，具备向嵌入式平台演进的坚实基础。

---

## 附录 A: 已修复问题详述

> 来源：原 [CODE_REVIEW.md](CODE_REVIEW.md)（已于 2026-07-23 合并入本文档）。
> 以下保留每个问题的完整详述（问题→后果→修复→位置），按系列排列。

### A.1 F 系列 — 前馈环路

#### F-A: 误差信号双重计入 ✅
- **严重度**: 🔴 高
- **后果**: `fxnlms_tick` 的 `err = dist + anti_est` 中 dist 是实测误差麦 (已含 S×anti), 再加 anti_est → 反噪声被计两次, NR 理论上限 ~6dB
- **修复**: 新增 `fxnlms_tick_rt` / `fxnlms_forward_rt` (2026-07-22): anti=Wc⊗x_ref 直接卷积, 梯度用 err_meas 直接驱动。离线 `fxnlms_tick` 保留不动
- **位置**: `include/fxnlms_mimo.h:35-45`, `src/fxnlms_mimo.c:120-170`, `main_realtime.c:162-175`

#### F-D: leak 因子无效 ✅
- **严重度**: 🟡 中
- **后果**: 原公式 `wc *= (1 - step×leak)` → 1-1e-9, float32 下等同 1.0, Wc 无正则化, 长期漂移/饱和风险
- **修复**: 解耦为 `wc *= (1 - leak)`, leak=1e-6 (~1.5%/秒), float32 可分辨
- **位置**: `src/fxnlms_mimo.c:94`, `main.c:192`, `main_realtime.c:377`

#### F-E: anti_spk 跨回调重置 ✅
- **严重度**: 🟡 中
- **后果**: 每回调开头 `anti_spk={0,0}`, fb_fir 首个样本馈入 0 → 256 tap 中 ~3% 错误样本, 反馈抵消精度退化
- **修复**: `anti_spk_prev[S]` 存入 `rt_ctx_t`, 回调首样本继承上轮回调末值
- **位置**: `main_realtime.c:58, 118-119, 253-254`

#### F-F: 反馈校准重采样失配 ✅
- **严重度**: 🟡 中
- **后果**: 原校准播 48k 白噪声 → 两路都最近邻 3:1 抽取 → NLMS 辨识 16k FIR。抽取后输入输出不再是 16k LTI 关系, 辨识精度下降
- **修复**: 改为生成 16k 白噪声 → ZOH×3 播放, 与运行时输出路径一致
- **位置**: `src/calibrate_feedback.c:29-34, 55-58, 160-210`

#### F-G: 双扬声器反馈路径合并建模 ✅
- **严重度**: 🟡 中
- **后果**: 原校准两声道播同一噪声, 辨识 H0+H1; 运行时 (anti0+anti1)/2 经单 FIR. H0≠H1 时产生 `(H0-H1)(a0-a1)/2` 误差
- **修复**: calibrate_feedback 逐扬声器两轮校准→feedback_path_0.bin/1.bin; 运行时双 `fb_fir[2]` 独立 FIR, `fb_est = Σ fir_tick(fb_fir[s], anti_spk[s])`. 自动降级: 仅加载到 1 个→单 FIR, 0 个→禁用
- **位置**: `src/calibrate_feedback.c`, `main_realtime.c:50-52, 127-131, 356-382`

#### F-I: acc_anti 累积时序 ✅
- **严重度**: 🟢 低
- **后果**: acc_anti 在 safety_mute/ramp 之前累积, 控制台显示值高于实际输出
- **修复**: 移至 mute/ramp 之后
- **位置**: `main_realtime.c:249`

### A.2 B 系列 — 反馈环路

#### B-2: 陷波 IIR 状态跨扬声器串用 ✅
- **严重度**: 🔴 高
- **后果**: s=0/1 共用 x1/x2/y1/y2, 陷波激活时通道间注入串扰失真
- **修复**: 状态数组从 `[HW_MAX_NOTCHES]` 扩展为 `[HW_S][HW_MAX_NOTCHES]` (HW_S=2), add_notch 初始化所有扬声器状态, remove_notch 搬运所有扬声器状态
- **位置**: `include/howling_detect.h:45-46`, `src/howling_detect.c:148-253`

### A.3 S 系列 — 场景/CNN

#### S-1: 滞回检测渐变噪声失效 ✅
- **严重度**: 🟡 中
- **后果**: cos_sim 比较相邻两帧 probs, 渐变场景每帧 cos≈0.95 但 argmax 已永久改变 → 切换永不触发 → 新场景 Wc 写入旧场景槽
- **修复**: 新增 `anchor_probs[8]`, 在 INIT/场景切换时保存当前 probs; `cos(anchor_probs, cur_probs)` 替代 `cos(prev_probs, cur_probs)`
- **位置**: `main_realtime.c:74, 431, 441-448, 514`

#### S-2: 场景切换 ramp 首样本为 0 ✅
- **严重度**: 🟡 中
- **后果**: 切换时 ramp=0 → 反噪声消失 400ms, CrossFader(100ms) 被掩盖
- **修复**: 场景切换仅走 CrossFader (FADE_LEN=1600, 100ms), ramp 仅用于冷启动 INIT
- **位置**: `main_realtime.c:431, 513-516`

#### S-3: CrossFader 末帧跳变 ✅
- **严重度**: 🟢 低
- **后果**: FADE_LEN=16 时末帧 6.25%→0 跳变
- **修复**: FADE_LEN=1600 (100ms = 20Hz×2 周期), 末帧跳变降至 0.0625%
- **位置**: `main_realtime.c:28`, `main.c:127`

#### S-4: Blend/Wc RMS 强制对齐抹除增益 ✅
- **严重度**: 🟢 低
- **后果**: 强制 `wc *= stub_rms/rms(wc)` 抹除 blend 权重隐含的增益信息, Wc 初始幅值不反映场景最优增益
- **修复**: 移除强制定标, 仅取反。LMS 功率归一化自动适应增益, 首次收敛后 scene_wc 记忆保存正确幅值
- **位置**: `src/scene_controller.c:110-124`

### A.4 § 系列 — 性能/安全

#### §6.1: fir_tick 取模消除 ✅
- **严重度**: 🔴 高
- **后果**: `(p-k+N)%N` idiv ×339k/回调 → 回调预算 217%, 嵌入式必丢帧
- **修复**: 双段线性循环 `i=p→0, N-1→p+1`, 零取模; `f->ptr = (p+1==N)?0:p+1` 替代 `(p+1)%N`. 预算降至 73%
- **位置**: `src/fir_filter.c:39-47`

#### §6.3: CNN calloc 静态化 ✅
- **严重度**: 🟢 低
- **后果**: `cnn_m5_forward` 每 1Hz 调用 `calloc(4×1MB)`, 分配+清零 4MB 后释放, Windows 堆可承受但碎片化
- **修复**: 静态缓冲单次 `calloc(4MB)`, 后续复用不释放
- **位置**: `src/cnn_m5_forward.c:223-231`

#### §6.5: Wc 发散防线 ✅
- **严重度**: 🟡 中
- **后果**: 仅被动 safety_mute (err>ref), 无系数监控/变化率检测/自动回退
- **修复**: `fxnlms_mimo_t` 新增 `freeze_lms` 字段; 主循环每秒检查 `max|Wc| > 5×stub_rms` → 跳过梯度; INIT/场景切换自动清除
- **位置**: `include/fxnlms_mimo.h:13`, `src/fxnlms_mimo.c:127`, `main_realtime.c:451-461`

#### §6.6: rt_ctx_t 堆分配 ✅
- **严重度**: 🟢 低
- **后果**: ~211KB 在 main 栈上, Windows 1MB 够用, 嵌入式栈 <256KB 则溢出
- **修复**: calloc 堆分配, 83 处 `ctx.`→`ctx->`
- **位置**: `main_realtime.c:324`

### A.5 功能模块

#### 反馈抵消 ✅
- **实现**: `src/calibrate_feedback.c` + `main_realtime.c`
- 校准程序: 逐扬声器两轮 NLMS 辨识 256 tap FIR → `data/feedback_path_{0,1}.bin`
- 运行时: `fb_est = Σ fir_tick(fb_fir[s], anti_spk[s])`, 从参考信号中减去
- 自动降级: 文件缺失时禁用, 不影响 ANC
- 实测效果: 反馈衰减约 -34dB, 10x 增益稳定 (无抵消时 6x 振荡)

#### 啸叫检测 ✅
- **实现**: `include/howling_detect.h` + `src/howling_detect.c`
- DFT 256 点频谱分析 (62.5-1500Hz), 汉宁窗
- 峰均值比 >15dB 候选, 4 帧确认 (64ms), 8 帧无峰释放
- IIR biquad 陷波 (r=0.96), 最多 2 路, 逐扬声器独立状态
- 与 safety_mute 互补: safety_mute 检测宽带反馈, 啸叫检测锁定窄带自激

#### Wc 发散检测 ✅
- 每秒检查 `max|Wc| > 5×stub_rms` → freeze_lms 跳过梯度
- INIT/场景切换自动清除

#### 冷启动静音 ✅
- ramp 400ms + mute_hold 1500ms + CrossFader 100ms
- 场景切换仅走 CrossFader, ramp 仅用于 INIT

#### 场景记忆切换 ✅
- 每场景保存/恢复已收敛的 Wc (`scene_wc[SC_K][S*L]`)
- 切换时优先恢复记忆, 无记忆时用 CNN 预设

### A.6 补充修复 (2026-07-22, 不在原始审查编号中)

| 编号 | 问题 | 后果 | 修复 |
|------|------|------|------|
| FIX-1 | MinMax scaler 阈值 1e-10 → 静默输入爆炸 | CNN 输入 Inf → NaN → Wc 随机 | 1e-6→0.01阈值 + 弱信号跳过CNN(保持场景) |
| FIX-2 | cnn_m5_forward 返回值未检查 | malloc 失败后 Wc 用垃圾数据 | 检查回退到上一帧场景 |
| FIX-3 | NaN/Inf 无声传播至 DAC | 可能损坏硬件 | isfinite 保护 |
| FIX-4 | Wc 退化至零无告警 | ANC 静默时无法排查 | stderr 告警 (限 3 次) |
| FIX-5 | CNN 双缓冲 + 原子交接 | 数据竞争 + 样本丢失 | InterlockedExchange 交接 |
| FIX-6 | pa_loader 共享层提取 | ~40 行 PA 样板重复 3 处 | pa_loader.h/.c |
| FIX-7 | FADE_LEN 16→1600 | CrossFader 1ms 硬切换 | 100ms 平滑过渡 |

---

## 附录 B: 待修复/待处理问题详述

> 以下为尚未修复的问题，按阻塞程度排列。每个问题包含当前状态、前置条件和建议方案。

### B.1 🔴 高优先级 — 阻塞量产/跨平台

#### §6.2: 跨线程数据竞争 🔶
- **严重度**: 🔴 高
- **后果**: 主线程 `memcpy` 8KB fx.wc 与回调梯度更新竞争, `fade_cnt`/`ramp_cnt` 读-改-写无同步. x86 TSO 下实测安全, ARM relaxed model 下概率大幅升高 → 回调可能看到部分更新的 Wc → anti 计算错误
- **前置条件**: 跨平台移植 (ARM)
- **建议方案**: 影子缓冲 + `InterlockedExchange` 序号:
  ```c
  float wc_shadow[2][S*L];   // 双缓冲
  volatile LONG wc_seq;       // 奇数=主线程在写, 偶数=安全
  ```
- **位置**: `main_realtime.c:414, 151, 453-484`

#### F-B: 次级路径未实测校准 🔶
- **严重度**: 🔴 高
- **后果**: 使用 Python 仿真 Ŝ (不含 I/O 延迟), 与真实 S 存在相位偏差, LMS 收敛条件退化. DSP_DELAY=16 为粗略 padding
- **前置条件**: ASIO 硬件校准 — 恢复 `calibrate_secondary.c` 源码
- **建议方案**: `git revert 162d357` 恢复校准源码, 实测 Ŝ 包含全部往返延迟后置 DSP_DELAY=0
- **位置**: `calibrate_secondary.c` (git 历史), `main_realtime.c` DSP_DELAY

### B.2 🟡 中优先级 — 显著改善

#### P-1: 48k→16k 抽取无抗混叠 🔶
- **严重度**: 🟡 中
- **后果**: 最近邻取样, >8kHz 折叠进 0-8k; 残余风险: 14.5-16k→20-1500Hz 通带
- **缓解**: 硬件 ADC 自带模拟抗混叠 + 带通 20-1500Hz FIR 截断 + 真实噪声 >8kHz 能量有限
- **前置条件**: CPU 预算更宽裕时实施
- **建议方案**: 48k 侧加 2 阶 IIR 低通 (fc≈7kHz), 约 10 MAC/样本, 回调开销 +1.5%
- **位置**: `main_realtime.c:141-146`

#### P-2: 16k→48k ZOH 无抗镜像 🔶
- **严重度**: 🟡 中
- **后果**: 16k 镜像频率送扬声器, 可闻性取决于扬声器/DAC 特性
- **缓解**: DAC 内置重建滤波 + 扬声器机械滚降 + 人耳 16kHz+ 不敏感
- **前置条件**: 与 P-1 一同实施
- **建议方案**: 零插值+低通
- **位置**: `main_realtime.c:265-272`

#### TODO-1: Wc Max 归一化低权重伪峰 🔶
- **严重度**: 🟡 中
- **后果**: `blend[i]/bmax` 强制最大权重=1.0. centroid 存在训练噪声/离群时, 伪峰压制其他 14 个子滤波器
- **前置条件**: 打印 8 个 centroid 各 30 维 blend 值, 分析 `bmax/bmax2` 比值分布
- **候选方案**: A) bmax 下限保护 B) softmax 替代 max C) L2 归一化
- **位置**: `src/scene_controller.c:94-96`

#### TODO-2: 反馈抵消符号未验证 🔶
- **严重度**: 🟡 中
- **后果**: `ref_sample = (ref_raw - fb_est) * MIC_PRE_GAIN` 假设扬声器正信号→参考麦正响应. 声学反相时减法变加法
- **前置条件**: 校准后检查 FIR 首峰值符号 + 对比启用/禁用 fb 时的 ref_rms
- **建议方案**: 反相时改减法为加法, 或运行时自动检测
- **位置**: `main_realtime.c:134`

#### TODO-3: 离线/在线 MIC_PRE_GAIN 不一致 🔶
- **严重度**: 🟡 中
- **后果**: `main.c: MIC_PRE_GAIN=1.0` vs `main_realtime.c: MIC_PRE_GAIN=10.0`. 离线调优参数不可直接用于在线
- **前置条件**: 用 main.c 分别在 GAIN=1.0 和 10.0 跑同一噪声文件, 对比 NR
- **位置**: `main.c:126`, `main_realtime.c:29`

#### TODO-4: 功率归一化 epsilon 边界调优 🔶
- **严重度**: 🟡 中
- **后果**: epsilon=1e-10. 信号功率 1e-8 级别时有效步长 `0.0001×3e7=3000`, 巨大单步更新可能触发瞬时发散
- **前置条件**: 在线记录 `power[s]` 最小值和典型范围
- **位置**: `src/fxnlms_mimo.c:76-84`

#### B-1: 固定反馈 IIR 环路不存在 ❌
- **严重度**: 🟡 中
- **后果**: 无反馈 ANC 控制回路 (误差麦→IIR→扬声器), 仅有前馈路径. 低频段 (20-200Hz) 完全依赖前馈
- **窗户场景评估**: 低频段前馈因果约束最宽松 (1.5ms << 10ms@100Hz), B-1 缺失影响可能比预想小. 但对室内突发噪声无控制能力
- **建议方案**: 独立模块 `feedback_iir.c`: 4 级双二阶级联 (DF2T, double 状态), 20Hz 极点 r→0.9998 需 double 防极限环. 输出与前馈 anti_ff 求和后统一限幅
- **位置**: 待新建

### B.3 🟢 低优先级 — 可延后

#### TODO-5: volatile 跨线程非原子 🔶
- **严重度**: 🟢 低
- **后果**: 10 个 `volatile` 变量 (nr_level, ref_rms 等) 回调写/主线程读, 无原子性保证. 极低概率监控数据撕裂 (显示跳变), 不影响音频处理
- **前置条件**: 跨平台移植时一同修复, 改为 `_Atomic`
- **位置**: `main_realtime.c:82-93`

#### B-3: remove_notch 未被调用 ❌
- **严重度**: 🟢 低
- **后果**: 无, 当前 `active_count=0` 批量清除够用
- **状态**: 保留 — 后续反馈环路需逐频率管理时直接启用
- **位置**: `src/howling_detect.c:162-179`

#### F-H: fwd_only err 语义不一致 —
- **严重度**: 🟢 低 (实时不适用)
- **分析**: 离线 `fwd_only` 的 err_out 只含 anti_est. F-A 修复后实时用 `fwd_rt` (不输出 err), 回调直接用 err_meas 统计. 离线 100ms fade 可忽略
- **位置**: `src/fxnlms_mimo.c:37-45`

#### S-6: CNN 输入延迟 ~1.0s —
- **严重度**: 🟢 低 (设计特性)
- **分析**: 场景分类结果对应 ~1.0s 前音频, 平稳场景无影响. 1s 窗口是分类正确设计, 双缓冲已消除样本丢失
- **位置**: `main_realtime.c:62-66, 137-143`

#### P-4: 带通 FIR 群延迟 32ms —
- **严重度**: 物理事实
- **分析**: (1024-1)/(2×16000)=32ms, 线性相位 FIR 的固有属性, 非 bug. 缩短 tap 牺牲频域分辨率
- **位置**: `main_realtime.c:317-322`
