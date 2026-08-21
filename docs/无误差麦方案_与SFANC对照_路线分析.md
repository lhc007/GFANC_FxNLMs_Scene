# 无误差麦方案：与 SFANC-Window 对照 + 路线分析

> 2026-08-21 · 结论：**无误差麦 ≠ 只有 SFANC 一条路。你的代码已经具备 SFANC 的全部核心（CNN+固定滤波器组合），推荐"双模式"路线——日常 µ=0 开环（无误差麦、设备小、极稳），安装/维护时临时插误差麦跑一段闭环 FxLMS 刷新固定滤波器。比纯 SFANC 强在：刷新成本低一个量级，不需要重新训练滤波器库。**
>
> 本文整理自 8x8 实时性讨论的延续：PXI 用途澄清 → SFANC 架构对照 → 开环精准度拆解 → 硬选 vs 连续权重 → 无误差麦路线。

---

## 1. 讨论脉络（一页摘要）

| # | 问题 | 结论 |
|---|---|---|
| 1 | SFANC 算力轻为什么还买 PXI？ | PXI 只做**确定性实时 I/O + 固定 FIR 卷积**，不做重算力。重活（CNN）在笔记本上 1 秒一次。见 §2.2 |
| 2 | 我的架构和 SFANC 一样吗？ | **核心一样**：CNN 从固定滤波器库取权重。你的用连续增益组合 15 个子滤波器（30 维 tanh），他们硬选 7 选 1。差在闭环/开环。见 §3 |
| 3 | 他们的滤波器要求极其精准吗？ | 数值精度不重要；**次级路径必须精准且稳定**才要紧（开环无反馈兜底）。见 §4 |
| 4 | 为什么硬选而不是权重？ | 硬选 = 保证每次输出是"验过身的滤波器" + 可解释 + 实现极简。权重（生成式）是另一种被顶会验证的方案，且无跳变。见 §5 |
| 5 | 他们运行时没有 FxLMS 吧？ | 确认。运行时只有固定 FIR 卷积 + 每秒 1 次 CNN 选号；FxLMS 只在**训练阶段**用误差麦收敛滤波器。见 §6 |
| 6 | 我的架构不想用误差麦，怎么调？ | 三条路线，推荐**双模式**（§7 路线C）。你的 µ=0 就是路线A（= 生成式 SFANC）。见 §7 |

---

## 2. SFANC-Window 架构拆解

### 2.1 运行链路

```
参考麦(1) ──► 每秒采 1s 噪声 ──► 笔记本: CNN(ShuffleNetV2) 前向 ──► argmax ──► 整数 ID(1/7)
                                                                        │
                                                                        ▼ UDP
                               ┌─────────────────────────┐          (整数串)
   喇叭(4) ◄── 固定 FIR 卷积 ◄─┤  PXI (确定性实时 I/O)   │◄───────────────┘
                               │  快循环: 只有 FIR 卷积    │
                               └─────────────────────────┘
```

- **快循环（每采样，PXI）**：1 个固定 FIR 卷积 + 多通道同步采集/输出。**零自适应。**
- **慢循环（每秒，笔记本）**：CNN 对 1s 噪声分类，选 1 个滤波器。重活全在这，1Hz 无所谓算力。
- 误差麦（4 个）**只在训练时存在**（用于用 FxLMS 收敛 7 个滤波器），运行时装的是"无误差麦"形态。

### 2.2 PXI 的用途（消除"轻算力却用贵设备"的矛盾）

读了他们的 [UDP_pxie_connector.py](https://github.com/Luo-Zhengding/SFANC-Window/blob/main/UDP_pxie_connector.py) 和 [Acquired_sound.py](https://github.com/Luo-Zhengding/SFANC-Window/blob/main/Acquired_sound.py) 后的关键事实：

- PXI **不跑 CNN**。CNN 在笔记本上跑（PyTorch）。
- PXI 只做两件事：**固定 FIR 卷积**（≈4K MAC/采样，极轻）+ **多通道同步实时 I/O**。
- 买 PXI 是买它的**确定性实时音频 I/O**（多通道 ADC/DAC 同步、微秒级抖动、低延迟驱动），不是买算力。

对照你的实时循环：你的快循环 ~220K MAC/采样（8×8 时更多），**比他们重几十倍**，但这不说明你买不起——说明你更需要 NEON 优化，而不是 PXI。PXI 解决的是 I/O 实时性，你用一个低延迟 USB/多通道 CODEC 芯片就能拿到类似特性，不必上 PXI。

---

## 3. 你的架构 vs SFANC：逐项对照

### 3.1 核心机制相同

你的链路（[src/scene_controller.c:6-7](src/scene_controller.c#L6-L7)）：

```
1s 带通噪声 → minmax → CNN 前向(K=S*C=30) → tanh 增益 → Wc[s,l]=Σ_c gain[s,c]·sub[c,s,l]
```

SFANC 的链路：

```
1s 噪声 → CNN 前向(7类) → argmax → 选中 1/7 固定滤波器
```

**"CNN 从固定滤波器库取权重"这件事两边完全一致**——你是连续权重组合（生成式），他们是离散硬选（选择式）。核心不是谁对谁错，是两种流派（见 §5）。

### 3.2 差异对照表

| 维度 | SFANC | 你的现状 | 你转无误差麦后 |
|---|---|---|---|
| CNN 前向 | ShuffleNetV2 1Hz | m5 1Hz | 不变 |
| 滤波器库 | 7 个（4 通道窗口就地训练） | 15 个子滤波器 | 不变 |
| 取权重方式 | argmax 硬选 1/7 | Σ gain×sub 连续组合 | 不变（可保留连续组合） |
| 误差麦（运行时） | 无 | **有（FxLMS 在线用）** | 无 |
| 在线自适应 | 无 | FxLMS + 在线 Ŝ | 无 |
| 次级路径追踪 | 无（装好不动） | 有（实时补偿漂移） | 无 |
| 稳定性 | 极稳（无发散可能） | 需发散保护（已实现 freeze_lms） | 极稳 |
| 设备尺寸 | 小（无误差麦） | 大（误差麦常驻） | 小 |

### 3.3 一个关键事实：你的"µ=0"已经是 SFANC

你的 README 已记录：**µ=0（固定 Wc，仅 CNN 初值）在 road_0-34 开环 +7.1dB**（[README.md:635](README.md#L635)、[README.md:693](README.md#L693)、[README.md:712](README.md#L712)）。

代码里的对应开关就是 `freeze_lms`（[include/fxnlms_mimo.h:16](include/fxnlms_mimo.h#L16)），置 1 后梯度更新整段被跳过（[src/fxnlms_mimo.c:240](src/fxnlms_mimo.c#L240) `if (!fx->freeze_lms)`）。

**注意**：`freeze_lms` 今天在 main_realtime.c 里是**发散应急**用的（[main_realtime.c:763-824](main_realtime.c#L763-L824)），不是干净的"CNN-only 模式开关"。转无误差麦时要把"冻结"从"应急"升级为"可配置常驻模式"，并同步跳过误差麦读取和在线 Ŝ 更新（见 §7）。

---

## 4. 无误差麦（开环）意味着什么：三种"精准"拆解

SFANC 是开环——误差麦只在训练时用。**没有反馈去纠正任何误差**，滤波器定死后所有误差 100% 兑现成衰减损失。但这个"精准"要拆成三种：

### 4.1 系数数值精度 —— 不重要

FIR 系数量化 16-bit 有 ~96dB 信噪比，8-bit 也就 ~0.5% 带内幅度误差。这不是瓶颈。

### 4.2 对次级路径的精准 —— 硬要求

物理账本（残差 = |1 − H·e^(jδ)|，H=幅度比、δ=相位误差）：

| 目标降噪量 | 幅度误差容限 | 相位误差容限 |
|---|---|---|
| 10 dB（SFANC 实机水平） | ±30% | ±18° |
| 15 dB | ±20% | ±10° |
| 20 dB | ±10% | ±6° |

这是**整条链路**（滤波器 × 次级路径 × 喇叭线性度）的总容差，不是滤波器单件的容差。SFANC 拿 7-10dB 的代价是**就地实测次级路径 + 就地训练滤波器**。换环境必须重新获取滤波器（他们 README 原文），CNN 本身倒不用换。

### 4.3 次级路径的稳定性 —— 最脆的一环

开环不仅要求路径准，还要求路径**别变**。窗开大/关小、喇叭挪了、人站近了、温度变了 → 路径漂移 → 衰减**静默打折**，没有任何信号告诉你坏了。他们的兜底就一句话：**装好不动**。

**这是你最该在意的**——你选闭环就是因为开窗场景路径不密封、会漂移。无误差麦 = 放弃对漂移的实时追踪。

---

## 5. 为什么他们硬选，你的连续权重行不行

### 5.1 硬选的动机

1. **保证每次输出都是已知好的**：7 个滤波器都是就地 LMS 收敛验证过的成品。硬选 = 任何时刻播的都是验过身的。连续组合会造出**新滤波器**，两个子滤波器可能在某个频点相位打架，且从没在真机单独验过。
2. **argmax 鲁棒**：离散判决，正确类领先任意一点就输出对的。连续增益要求 CNN 数值恰好校准准确，偏一点合成滤波器就偏一点。
3. **可解释性**：他们论文卖点就是 XAI（LayerCAM），"选 3 号 = 检测到某频段噪声"好解释。连续权重没法讲这个故事。
4. **7 个宽带滤波器覆盖够粗**：真实噪声总能落在某一类覆盖内，类别间插值收益小；而开环真正卡你的是次级路径精度（§4.2），不是滤波器类别粒度。
5. **实现极简**：UDP 发一个整数 ID（[UDP_pxie_connector.py:13](https://github.com/Luo-Zhengding/SFANC-Window/blob/main/UDP_pxie_connector.py#L13)），PXI 直接加载对应 FIR。

### 5.2 你的连续权重不但行，还更强

- 你做的正是他们相关工作的**生成式流派**：GFANC-Kalman、Unsupervised-GFANC 都是 CNN 连续生成/组合滤波器，顶会验证过的正规方案。
- **无跳变**：硬选每秒换滤波器可能"咔哒"一声；你的连续增益 + 增益平滑（cos 闸门 + beta 自适应，[src/scene_controller.c:176-188](src/scene_controller.c#L176-L188)）是平滑过渡。
- 覆盖整个子滤波器空间，不受离散点限制。

**结论：µ=0 模式下的你 = 生成式 SFANC，不用改成硬选。**

---

## 6. 确认：他们运行时没有 FxLMS

代码铁证：

- [Control_filter_selection.py:51](https://github.com/Luo-Zhengding/SFANC-Window/blob/main/Control_filter_selection.py#L51) — `torch.argmax(prediction).item()`，纯推理
- [Acquired_sound.py:56-58](https://github.com/Luo-Zhengding/SFANC-Window/blob/main/Acquired_sound.py#L56-L58) — `stats.mode(id_vector)` 取众数
- [UDP_pxie_connector.py:13](https://github.com/Luo-Zhengding/SFANC-Window/blob/main/UDP_pxie_connector.py#L13) — 只发整数
- README — "laptop runs the CNN to transmit the index of the selected control filter"

**精确表述**：运行时无 FxLMS；FxLMS 在**训练阶段**用 4 个误差麦收敛那 7 个滤波器。也就是"用 FxLMS 训好 → 运行时摘掉误差麦和 LMS，只留固定 FIR + 每秒 CNN 选号"。

---

## 7. 无误差麦的三条路线

### 路线 A：纯开环（µ=0 常驻）—— 你的 µ=0 就是它

- **做法**：`freeze_lms=1` 常驻 + 跳过误差麦读取 + 跳过在线 Ŝ 更新。Wc 完全由 CNN→gain→组合驱动。
- **优点**：最简、最稳（无发散）、设备最小、和 SFANC 等价（你是生成式变体）。
- **缺点**：次级路径漂移无反馈；必须就地标定 + 装好不动；衰减静默退化（§4.3）。
- **改动量**：把 freeze_lms 从"发散应急"改为"配置可设常驻"，加条件编译跳过误差麦/在线 Ŝ。

### 路线 B：模型参考 FxLMS（虚拟误差）—— 保留自适应但赌模型

- **做法**：用离线识别的 Ŝ 和参考信号合成"虚拟误差" ê = d̂ − Ŝ⊗y 替代误差麦，FxLMS 照跑。
- **优点**：无误差麦，理论上能追慢漂移。
- **缺点**：**模型错 → 优化错误的代价函数 → 可能比不自适应更差**。本质是把"误差麦"换成"对 Ŝ 准确性的赌注"——而开窗场景恰恰是 Ŝ 最不可靠的地方。风险高。
- **改动量**：误差合成块 + 现有 FxLMS 接虚拟误差。**不推荐作主方案**，可作为研究项。

### 路线 C（推荐）：双模式 = 运营 µ=0 + 校准期临时误差麦 FxLMS

- **部署形态**：误差麦做成**可插拔/校准专用**（设备本体不含，配一套校准转接）。
- **运营模式（日常）**：µ=0 开环，无误差麦，CNN 连续增益 —— 就是路线 A。
- **校准模式（安装/维护/怀疑退化时）**：插上误差麦，跑**你现成的闭环 FxLMS**，收敛到当前环境的 Wc → **导出为新固定滤波器**（固化进 sub_filter 库）→ 拔掉误差麦。
- **节奏**：用"低频次的闭环"换"日常的开环稳定"。安装时标定一次；此后按需（怀疑衰减/环境大变）重标定。
- **优点**：
  - 日常无误差麦 → 设备小、极稳、无发散（和 SFANC 同等形态）。
  - 环境变了有**低成本刷新路径**——这是纯 SFANC 没有的：SFANC 换环境要重新训练整套滤波器库（他们 README 明说），你只要插一次误差麦跑一段闭环就能把全部滤波器刷新到当前路径，**不需要重训 CNN**。
- **缺点**：需要标定流程 + 误差麦硬件可拆；两次校准之间漂移仍会静默退化（但有刷新口子）。
- **改动量**：**最小**。三个点：(a) 配置区分运行模式；(b) 校准结束时把当前 Wc 导出/固化到 sub_filter 库；(c) 文档化流程。

### 路线 C 增强：被动漂移侦测（几乎零成本）

用现成的参考麦 + CNN：当参考麦频谱分布与 CNN 分类结果长期不匹配，或频谱统计显著漂移时，提示"环境变了，建议重标定"。这等于给开环装一个"退化提示灯"，不需要误差麦。

---

## 8. 路线 C 代码改动量评估（对照现有代码）

| 改动 | 位置 | 说明 |
|---|---|---|
| 运行模式配置 | main_realtime.c / 配置头 | 加 `MODE_FIXED`（µ=0 常驻）vs `MODE_ADAPT`（现状） |
| freeze_lms 语义升级 | [include/fxnlms_mimo.h:16](include/fxnlms_mimo.h#L16)、[src/fxnlms_mimo.c:240](src/fxnlms_mimo.c#L240)、[main_realtime.c:763-824](main_realtime.c#L763-L824) | 从"发散应急"改为"模式开关"；MODE_FIXED 下整段梯度跳过 |
| 跳过误差麦/在线 Ŝ | main_realtime.c 快循环、[src/sec_online.c](src/sec_online.c) | MODE_FIXED 下不读误差麦、不跑 sec_online_update |
| Wc 导出固化 | main_realtime.c + scene_controller | 校准模式退出时把收敛后的 wc 写回 sub_filter 库 |
| 标定流程 | 文档 + 上位机 | 安装/维护 SOP，误差麦转接即插即用 |

**已具备、不用动的**：CNN 前向、子滤波器库、连续增益组合链（[src/scene_controller.c:67-78](src/scene_controller.c#L67-L78) `scene_ctrl_construct_wc`）、增益平滑。

---

## 9. 验证阻塞项（上板实测，无法从代码推断）

1. **开窗场景次级路径漂移量**：µ=0 开环 vs 闭环 FxLMS 各跑一段，量化漂移导致的 dB 退化。决定日常该信谁、校准频率该多高。
2. **就地标定可复现性**：同一安装位置标定两次，固定滤波器差异是否在容差内（§4.2）。
3. **1m 相干性**：沿用 8x8 预算表 §4/§5 的实测项（500Hz 相干≥0.8）。
4. **切换瞬变**：连续增益下无此问题，但校准→运营切换时验证无爆音。

---

## 10. 参考

- 讨论起点：`docs/8x8_实时性验收预算表.md`、`docs/窗户ANC可行性-因果限制_实测_方案.md`
- SFANC 论文：Luo et al., *Real-time Implementation and Explainable AI Analysis of Delayless CNN-based Selective Fixed-filter ANC*, MSSP 2024. 代码：https://github.com/Luo-Zhengding/SFANC-Window
- 生成式流派（你的连续权重对应的正统方案）：GFANC-Kalman、Unsupervised-GFANC、Deep Generative Fixed-filter ANC（他们 related works 列表）

---

## 11. 实现状态（2026-08-21，分支 plan-c-dual-mode）

方案C 已实现，待硬件验证。**2026-08-21 收窄范围**：对标 SFANC（其无"标定→导出文件→加载"流程），砍掉 `fixed_source`/`snapshot_wc`（file 加载 + 标定快照导出），只保留 `anc_mode` 双模式。

- **config**：`gfanc_config_t` 新增 `anc_mode`（env `GFANC_ANC_MODE=adapt|fixed`）
- **实时（fixed 模式）**：err_meas 置 0（无误差麦）→ 派发恒走 `fxnlms_forward_rt`（无梯度/无Wc变更/无在线Ŝ）→ Wc 变更点（静音衰减、peak halve、冷启动 30% 软化）全 gate → 发散检测跳过；CNN 每秒仍产 Wc（生成式 SFANC，与 SFANC-Window 形态对齐）
- **遥测**：fixed 下 NR=n/a，CSV 事件列标 FIXED
- **验证**：实时/离线均编译通过；离线 adapt NR_true=14.7dB 与基线一致（无回归）；硬件实测（adapt vs fixed 开环对比 NR）待做

> 曾实现又移除：`fixed+file` 静态滤波器加载 + `GFANC_SNAPSHOT_WC` 标定快照导出（首版"首次收敛即导出"存浅滤波、后改"只存最优 NR"）。因用户对标 SFANC 决定不要 file 功能，整套移除，git 历史可回溯。
