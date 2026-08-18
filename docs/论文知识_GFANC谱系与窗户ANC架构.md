# 论文知识总结 — GFANC 谱系与窗户 ANC 架构

> **日期**: 2026-08-13 · **类型**: 第四类(论文知识: 各解决什么问题、用什么方式)
> **相关**: [窗户ANC可行性-因果限制_实测_方案](窗户ANC可行性-因果限制_实测_方案.md)(可行性/落地) · [SceneZone_综合审查报告_合并版](SceneZone_综合审查报告_合并版.md)(审查)

---

## 一、论文谱系（NTU 组: Luo Zhengding / Shi Dongyuan / Gan Woon-Seng）

**一条进化链**，每篇补前一篇的短板:

```
SFANC(选固定滤波器,离散) → GFANC(生成滤波器,连续) → GFANC-Bayes/Kalman(帧间平滑)
    → Unsupervised-GFANC(无监督训练) → GFANC-FxNLMS+OCG(混合+在线聚类)
```

| 论文 | 出处 | 解决什么问题 | 用什么方式 |
|---|---|---|---|
| **SFANC-Window** | MSSP 2024, 214:111364 | 固定滤波器不灵活(每种噪声一个)、FxLMS 收敛慢易发散 | 1参考麦+4扬声器(周边)+4误差麦(仅训练)；ShuffleNetV2 从 7 个固定滤波器里**选 1**；开环前馈,运行时无误差麦 |
| **GFANC** | ICASSP 2023, arXiv 2303.05788 | SFANC **离散选择**的局限(噪声连续变化时选不准) | 子滤波器库 + 1D CNN 生成**组合权重**,线性组合出**任意**滤波器——"生成"替代"选择" |
| **GFANC-Bayes** | TASLP 2023, 32:1048 | CNN 帧间权重**抖动** | 在 GFANC 上叠 **Bayesian 滤波**,用相邻帧相关性平滑权重 |
| **GFANC-Kalman** | SPL 2024, 31:276 | 同 Bayes(帧间平滑) | CNN-Kalman 滤波替代 Bayesian |
| **Unsupervised-GFANC** | ICASSP 2024, arXiv 2402.09460 | GFANC **监督训练需标签**(昂贵+标注误差) | 端到端可微系统,用**累计平方误差(误差麦 err²)直接当损失**,不要标签 |
| **GFANC-FxNLMS+OCG** | ICASSP 2026, arXiv 2601.15889 | 混合后 GFANC 每帧**重置 FxLMS 打断收敛**→不稳定 | GFANC 帧级给起点 + FxLMS 采样级自适应 + **在线聚类(OCG)**判断是否真切换,只真切换才更新 |

### 1.1 SFANC-Window 详解（本项目架构"母论文"）

- **硬件**: 1 参考麦 + 4 扬声器(窗户四周相对,方形环绕) + 4 误差麦(仅训练) + NI PXIe 实时单元。
- **训练**: 7 种不同频段宽带噪声 → 让 4 误差麦声压最小化 → 反解 7 个固定滤波器;8 万条合成带通噪声 → 训 CNN 分类"选哪个滤波器"。
- **实时(开环)**: 参考麦采 1s → Mel 语谱图 [1,64,32] → ShuffleNetV2 → 选 ID → UDP 发 PXIe → 固定滤波器前馈 → 4 扬声器。**误差麦不参与**。
- **delayless 精髓**: CNN(慢链路,帧率) 与实时控制器(快链路,采样率) **两条链路并行**,选择结果只在帧边界交接,不打断实时控制。
- **降噪数值**: 摘要**未给具体 dB**(只说"宽带+真实低频噪声满意降噪"),数值在付费正文图表。
- **代码**: [SFANC-Window](https://github.com/Luo-Zhengding/SFANC-Window) **只开源推理**(CNN 选滤波器 + UDP),**训练固定滤波器/次级路径测量/结果评估都未开源**。

### 1.2 GFANC 原论文（本项目的直接架构来源）

- **核心**: 完美重构滤波器组把预训练宽带滤波器分解成**子滤波器库(sub-filter bank)**;1D CNN 输出**组合权重**,线性组合生成任意滤波器。
- **关键性质**: **路径(子滤波器) 与 噪声(CNN 组合权重) 解耦** → 换环境只重标定子滤波器,CNN 不变(迁移性好)。
- **双速率**: CNN 帧率(协处理器) + 控制器采样率,delayless。
- 本项目 direct-weight 架构(CNN 生成 K=30 子带 gains → 组合子滤波器 → Wc)就是 GFANC。

### 1.3 GFANC-FxNLMS + OCG（本项目当前架构的论文版）

- **混合动机**: GFANC 快但不稳 / FxLMS 稳但慢,互补。
- **混合的坑**: GFANC 每帧重新生成权重,微小变化也**重置 FxLMS** → 自适应被打断 → 震荡。
- **解法**: **在线聚类(OCG)**——CNN 预测权重做多质心聚类,**只有簇索引真变(噪声类别真切换)才更新滤波器**,簇内抖动不触发重置。
- **结果**: 飞机噪声 + 20-2000Hz 宽带下,有聚类版更平滑、误差更低;CNN 仅 0.21M 参数,只需 1 个预训练宽带滤波器。
- **对应本项目**: `ocg.c`(tau/alpha/max_clusters/hold) 即此模块,已实机验证(250↔1000 双向真切换+簇复用+零 rescue)。

### 1.4 Unsupervised-GFANC（开环转型最省标签的路）

- 监督 GFANC 需"标签=该噪声的最优组合权重",标签要离线让误差麦最小化来算(昂贵+偏差)。
- Unsupervised 把协处理器+控制器拼成**端到端可微系统**,用 err² 直接当损失,**不要标签**。
- **训练时仍要误差麦(损失靠它),运行时照样可开环拆掉**。
- 对开环的意义: 若走开环,CNN 训练目标需从"初始 Wc"改成"最优 W",Unsupervised 是最省标签的那条路。

---

## 二、本项目在谱系中的位置

**已实现(超出论文)**:

| 组件 | 实现 |
|---|---|
| GFANC generative(子滤波器库+CNN 组合权重) | `cnn_m5_forward.c` + direct-weight |
| FxLMS 闭环自适应 | `fxnlms_mimo.c`(VS-LMS/自适应 leak/anti-windup) |
| OCG 在线聚类 | `ocg.c` |
| 在线次级路径辨识 | `sec_online.c` |
| 反馈对消 | `calibrate_feedback.c`(实机发散暂弃) |
| 啸叫检测/安静检测/发散救援 | `howling_detect.c` + P0-5 等(**论文没有**的工程加固) |

**还差的(及其对实际降噪的作用)**:

| 差的部分 | 作用 |
|---|---|
| Unsupervised 无监督训练(现在打标签监督) | 小:省标签工程+可能小幅提升,不改推理架构 |
| GFANC-Bayes/Kalman 帧间平滑 | 无:OCG 已替代(同一问题不同解法) |
| GFANC-RL 强化学习 | 小:训练范式,不直接改降噪量 |
| **多通道窗户扩展(8spk/8mic MIMO)** | **决定性**:窗户开口降噪需空间采样,当前 E=3 S=2 只验证了算法 |
| **开环转型(CNN 训练目标: 初始W→最优W)** | **决定性**:拆 FxLMS 闭环后 CNN 须直接生成最优解,否则降噪掉 |

---

## 三、窗户 ANC 架构核心知识

### 3.1 开口尺寸 → 频率上限 → 通道数(两条法则,互相印证)

**法则① λ/2 空间采样**: `f_max = c / (2 × 间距)`;周边布置 `间距 = 周长 / 扬声器数`。

**法则② 矩形波导模式数**: 透过开口的声场展开为模式,控制声场=控制主导模式。模式截止频率
`f_c(m,n) = (c/2)·√((m/a)² + (n/b)²)`,a/b 为开口两边。独立次级源数 ≈ 主导传播模式数。

**123×45cm 窗**(c=343):

| 频率 | λ | λ/2 间距 | 4 源(周长间距84cm) | 8 源(42cm) | 传播模式数 |
|---|---|---|---|---|---|
| 139Hz | 247cm | 123cm | ✅ | ✅ | 2 |
| 200Hz | 171cm | 86cm | ⚠️ | ✅ | ~3 |
| 380Hz | 90cm | 45cm | ⚠️刚好 | ✅ | 4 |
| 500Hz | 69cm | 34cm | ❌ | ⚠️刚好 | ~10 |
| 1000Hz | 34cm | 17cm | ❌ | ❌ | ~24-30 |

**结论**: 4 源 ≈ 380Hz、8 源 ≈ 500Hz、1000Hz 需 20-24 源。误差麦数须 ≥ 扬声器数(MIMO 解耦)。

### 3.2 边缘环绕 vs 铺面(两种路线)

| | 边缘环绕(SFANC-Window 4 大扬声器) | 铺面(Lam 2020 24 小扬声器) |
|---|---|---|
| 源数 | 少(4-8) | 多(20-24) |
| 优势频段 | **低频**高效(低阶模式极值在边缘) | **中高频**(800-1000Hz) |
| 高频上限 | 低(受边缘间距约束) | 高(面内采样密) |
| 低频效率 | 高(大口径大位移) | 差(小喇叭推不动 <300Hz) |

**论文 4 个够的原因**: 只做**低频** + 宿舍小窗(47×47cm),不是宽带;大扬声器是为低频体积位移,非为采样。

### 3.3 开环 vs 闭环

| | 开环(SFANC) | 闭环(本项目 FxLMS) |
|---|---|---|
| 误差麦 | 仅训练,运行时拆 | 运行时持续自适应 |
| 稳态降噪量 | = 闭环稳态(固定滤波器=收敛解) | 同左 |
| 路径漂移(窗户开合/家具/温度) | **无法自适应**,失配 | 能追,鲁棒 |
| 部署 | 简单(不留误差麦) | 要误差麦常驻 |

**折中**: 误差麦**按需标定**(安装时测Ŝ+训滤波器,标完拆),设备装好后不动 → 兼顾"不留"与"抗漂移"。

### 3.4 量产模式

- **每家都要标定**(次级路径 P/Ŝ 每家不同),但可**自动化成"安装自标定"**(几分钟)。
- **CNN 可复用**(噪声分类/组合权重映射与路径解耦),只重标定路径相关部分(固定滤波器/子滤波器库)。

### 3.5 关键物理结论速查

1. **8 扬声器 + 8 误差麦 → 20-500Hz 务实**,覆盖马路噪音主体(轮胎-路面噪声峰值 100-400Hz)。
2. **开窗场景玻璃不起隔声作用**: >500Hz 噪声直接通过开口漏入,ANC 采样不足又无玻璃兜底 → 纯漏。
3. **低频要大口径扬声器**(体积位移需求);中频小口径(4.5-8cm)高效。
4. **严格"整面声压=0"不可达**(需无限密集源+全频段),但控主导模式即可大幅降低透过开口的总辐射声功率。
5. **参考麦相干性**: 距离越远预览↑但相干↓;高频需参考阵列(间距~λ/2)。

---

## 参考资料

- Lam, Shi, Gan, Elliott & Nishimura, "Active control of broadband sound through the open aperture of a full-sized domestic window", *Scientific Reports* 10, 2020. https://link.springer.com/article/10.1038/s41598-020-66563-z
- Luo, Shi, Ji, Shen & Gan, "Real-time implementation and explainable AI analysis of delayless CNN-based selective fixed-filter active noise control", *MSSP* 214, 2024. 代码: https://github.com/Luo-Zhengding/SFANC-Window
- Luo et al., "Deep Generative Fixed-filter Active Noise Control", ICASSP 2023. arXiv:2303.05788
- Luo et al., "Delayless Generative Fixed-Filter ANC Based on Deep Learning and Bayesian Filter", *IEEE/ACM TASLP* 32, 2023.
- Luo et al., "Unsupervised Learning Based End-to-End Delayless Generative Fixed-Filter ANC", ICASSP 2024. arXiv:2402.09460
- Luo, Ma et al., "A Stabilized Hybrid ANC Algorithm of GFANC and FxNLMS with Online Clustering", ICASSP 2026. arXiv:2601.15889
