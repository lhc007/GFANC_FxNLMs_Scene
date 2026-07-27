# GFANC_FxNLMs_Scene 全面代码审查与架构分析报告

> **审查日期**：2026-07-26
> **审查基线**：branch `realtime-io` @ commit `7e37499`（工作区干净）
> **审查范围**：全部 C 源码（17 文件 / ~2700 行）、Makefile、README、export/export_bin.py、data/*.json 及 62 个 .bin 尺寸核验
> **审查方法**：逐文件完整阅读 + 数据流追踪 + 算法公式逐项推导 + 探针程序实测（PortAudio ABI）+ 资源定量估算
> **报告定位**：**完全自包含**。历史问题（F/S/B/§/CR/W 系列）在附录 A 中逐条复核当前状态；正文只呈现当前代码的事实与结论。
> **关联文档**：[COMPREHENSIVE_REVIEW.md](COMPREHENSIVE_REVIEW.md)（2026-07-23 审查）、[UPGRADE_ROADMAP.md](UPGRADE_ROADMAP.md)、[WINDOW_ANC_SUPPLEMENT.md](WINDOW_ANC_SUPPLEMENT.md)

**严重级别定义**：致命＝正在发生的内存破坏/崩溃路径/产品不可用；严重＝功能错误、鲁棒性缺口或阻塞下一阶段；一般＝有实际影响的偏差/债务；建议＝改进项。
**迁移标记**：`[Phase-1]` 当前 PC 阶段即有风险；`[Phase-2]` 树莓派 ARM Linux 原型时构成风险；`[Phase-3]` MCU/DSP 量产时构成风险。

---

## 0. 澄清阶段输出（追溯保留）

### 0.1 文件清单（实际读取）

| 类别 | 文件 | 行数 |
|------|------|------|
| 主程序 | main.c（离线）、main_realtime.c（实时） | 425 + 682 |
| 头文件 | include/{gfanc_types, fir_filter, fxnlms_mimo, scene_controller, howling_detect, binary_loader, pa_loader}.h | 329 |
| 实现 | src/{fxnlms_mimo, fir_filter, scene_controller, cnn_m5_forward, howling_detect, binary_loader, pa_loader, calibrate_feedback}.c | 1272 |
| 构建/工具 | Makefile、export/export_bin.py | — |
| 文档 | README.md、docs/{COMPREHENSIVE_REVIEW, UPGRADE_ROADMAP, WINDOW_ANC_SUPPLEMENT}.md | — |
| 配置 | data/{cnn_info, gfanc_config, sub_filters_info}.json | — |

**尺寸核验**（二进制权重，解析头 4 字节计数）：secondary_path 6144＝E·S·1024 ✓；primary_path 6144＝E·2·1024 ✓（R=2 个参考位，代码用 R=0）；sub_filters 30720＝C·S·L ✓；scene_defs 180＝**K·S·C＝6×30** ✓；bandpass 1024 ✓；feedback_path 256（旧版单文件）。

**明确未读取**：docs/micphone.md（用户确认不纳入）；62 个 .bin 的浮点内容（仅核验尺寸）；Python 训练工程 GFANC_Scene（不在本仓库）。
**确认不存在**：单元测试目录、CI 配置、CMakeLists.txt、链接脚本/DMA/HAL 代码、export_model.py（README 提及但缺失）、calibrate_secondary.c（在 git 历史 `162d357`）、data/feedback_path_{0,1}.bin（未生成→反馈抵消当前实际停用）。

### 0.2 架构理解摘要

1. **双速率双线程**：PortAudio 回调线程（48kHz、96 帧＝2ms 周期）内做 3:1 最近邻抽取得到 16kHz 处理流，逐样本执行：反馈抵消（双 fb_fir 扣除扬声器→参考麦串扰）→ tanh 软限幅 → 带通 FIR → Wc 直接卷积输出 anti；并行地 6 个 Ŝ FIR 生成滤波参考 Xd，以实测误差麦信号（带通后）直接驱动 MIMO FxNLMS 梯度（功率归一化＋泄漏＋发散冻结），输出经 ZOH×3 内插回 48kHz。
2. **慢速环路**：回调填充 cnn_buf 双缓冲并以 InterlockedExchange 原子交接；主线程 1Hz 取满帧 → M5 CNN → softmax → centroid max 归一化 blend 15 个子滤波器构造 Wc → 锚点余弦滞回判定切换 → CrossFader 100ms 过渡；Wc 经 wc_shadow＋序号提交回调，wc_snapshot 反向供主线程诊断，fx.wc 读写已双向隔离。
3. **隐式状态机**：first_sec / fade_cnt / ramp_cnt / mute_hold / freeze_timer 五组计数器驱动 INIT→正常→切换→冻结/永久冻结；安全层＝峰值快静音（10 样本）＋safety_mute（1s RMS）＋啸叫 DFT/IIR 陷波＋Wc 发散冻结四道防线。
4. **离线/实时算法双路径**：fxnlms_tick（Ŝ 域仿真，写 WAV）与 fxnlms_tick_rt（直接卷积＋实测误差直驱）并存，互不影响。
5. **参数/权重运行时加载**：62 个 .bin 经 fread 加载；E=3/S=2/L=1024 为编译期 #define；K（场景数）自 commit `0001362` 起运行时从文件尺寸推导——**该重构在实时路径留有未清扫的硬编码 `8`，见 R-1**。

### 0.3 信息缺口

1. calibrate_secondary.c 不在仓库（需 `git revert 162d357`）→ 次级路径校准逻辑无法审查；Ŝ 为 Python 仿真产物，无实测脉冲响应。
2. CNN 训练流水线与训练数据不在本仓库 → 场景覆盖度（窗户噪声）无法评估，仅能审查数据文件。
3. 无 Phase-2/3 目标平台代码（链接脚本/DMA/HAL）→ 移植评估基于代码模式标记与资源外推。
4. 无实测端到端延迟数据（ASIO buffer 96 帧为标称 2ms，驱动实际值未知）→ 因果性分析基于理论估算。
5. 反馈抵消实际状态：data/ 仅有旧版 feedback_path.bin，F-G 要求的逐扬声器文件不存在 → 该功能当前静默禁用（降级路径正确工作）。

### 0.4 用户确认结论（2026-07-26）

1. K 后续可能再变 → K 相关尺寸必须全运行时动态化或加上限断言。
2. 本报告完全独立、自包含重写全部结论。
3. Phase 2 ＝ 树莓派（ARM Linux, Cortex-A72）→ 资源估算以 A72 @1.5GHz 为基准。
4. export_bin.py 纳入审查；microphone.md 不纳入。

---

## 1. 系统架构概览

### 1.1 控制流与数据流（实测代码绘制）

```
                        ┌──────────────────────── PortAudio 回调线程 (ASIO, 2ms 周期) ────────────────────────┐
                        │                                                                                      │
 ADC 48k 6ch ──► 声道拆分 ──► 最近邻 3:1 抽取 ──► ref_buf[32]            err_buf[32×3]                          │
 (ch0=ref,                                       │                        │                                   │
  ch1-3=err)                                     ▼                        ▼                                   │
                        fb_est=Σ_s fb_fir[s](anti_spk[s])         err_meas[e]=bp_err[e](err×GAIN→tanh)        │
                        ref=(ref_raw-fb_est)×GAIN→tanh                     │                                  │
                        ref_filt=bp_fir(ref) ──────┬───────────────────────┤                                  │
                        (1024tap, 群延迟32ms)      │                       │                                  │
                        cnn_buf[fill][cnt++]=ref_filt                     │                                  │
                        (满16000→InterlockedExchange就绪)                  │                                  │
                        CrossFader(fade_cnt>0: Wc=a·wc_old+(1-a)·wc_cur)   │                                  │
                        Fx_arr[e,s]=sec_firs[e,s](ref_filt)  (6×1040tap)   │                                  │
                                                 ▼                       ▼                                  │
                        ┌──── fxnlms_tick_rt ─────────────────────────────────────────────┐                 │
                        │ Xd roll ← Fx_arr;  x_hist ← ref_filt                             │                 │
                        │ anti[s] = Σ_k Wc[s,k]·x_hist[k]        (物理输出, 直接卷积)        │                 │
                        │ power[s] = (1e-6 + Σ_e,k Xd²)/(E·L)                              │                 │
                        │ if(!freeze_lms): Wc -= μ·Σ_e err_meas[e]·Xd[e,s,k]/power[s]      │                 │
                        │                  Wc *= (1-leak)                                  │                 │
                        └───────────────────────────────────────────────────────────────┘                 │
                        anti → NaN防护/±1钳位 → 峰值快检测(10样本>0.95→peak_mute)                          │
                             → 啸叫陷波 howling_tick(err_avg, anti) → safety_mute/peak_mute 清零          │
                             → ramp 渐入 → anti_buf                                                      │
                        wc_snapshot ← fx.wc (主线程安全读取)                                              │
                        ZOH×3 内插 ──► DAC 48k 2ch                                                       │
                        └──────────────────────────────────────────────────────────────────────────────┘
                                                   ▲ cnn_buf 双缓冲 (1Hz 满帧交接)
                                                   ▼ wc_shadow+wc_seq / wc_snapshot
                        ┌──────────────────────── 主线程 (Sleep100 轮询, 1Hz) ───────────────────────────┐
                        │ scene_ctrl_process: minmax(denom>0.01) → CNN M5 → softmax → argmax            │
                        │ construct_wc: blend=centroid[sid]/max → Wc=Σ blend·sub_filter → 取反          │
                        │ INIT: wc_shadow←Wc, seq+=2, ramp=400ms, mute_hold=1500ms                      │
                        │ cos_sim(anchor,probs)<0.8 且 scene≠cur → 场景切换:                             │
                        │   保存 scene_wc[old] ← wc_snapshot; wc_cur ← scene_wc[new]或CNN预设;           │
                        │   fade_cnt=1600; mute_hold=1500ms; freeze 清零                                │
                        │ check_wc_divergence: max|Wc_snapshot| > 5×stub_rms → freeze_lms=1 (60s重试,   │
                        │   3s观察, 再犯→永久冻结)                                                       │
                        │ check_convergence: NR>3dB×3s → scene_wc[cur] ← wc_snapshot                    │
                        │ 日志: gfanc_log.csv (每秒 NR/场景/RMS + 事件)                                  │
                        └──────────────────────────────────────────────────────────────────────────────┘

   离线路径 (main.c, 单线程): WAV→峰值归一化→bp→[每秒: Pri(ref_filt)→Dis, CNN→Wc, 滞回/CrossFader,
   逐样本 Fx=Ŝ(ref_filt) → fxnlms_tick(err=Dis+Wc⊗Xd Ŝ域合成)] → anti_out.wav / error_out.wav
```

### 1.2 模块依赖与解耦评估

```
main_realtime.c ──► scene_controller.c ──► cnn_m5_forward.c ──► binary_loader.c
     │  ├──► fxnlms_mimo.c (算法核心, 仅依赖 fir_filter.h/gfanc_types.h)
     │  ├──► fir_filter.c
     │  ├──► howling_detect.c
     │  └──► pa_loader.c ──► libportaudio64bit-asio.dll (Windows 运行时绑定)
main.c (离线) ──► 同上算法模块 (无 pa_loader, 内联 WAV IO)
```

- **优点**：无循环依赖；算法核心（fxnlms_mimo / fir_filter / scene_controller / cnn_m5_forward / howling_detect）不调用任何硬件 API，可脱离音频栈做单元测试（离线 main.c 即为证明）；回调路径零动态分配；分层日志宏集中于 gfanc_types.h。
- **耦合问题**：
  1. 算法模块的维度常量（E/S/L/HW_S/SC_S/SC_C）分散硬编码于 6 个文件（见 R-29），1×5×4 扩展需多点同步修改；
  2. main_realtime.c 将通道映射（`in[(n*3)*6+ch]`）、采样率转换、ANC 编排、统计、日志全部内联在一个 682 行文件中，音频 I/O 未抽象为 HAL（见 R-21）；
  3. scene_controller.c 直接 `extern int cnn_m5_forward()`（main.c:157、scene_controller.c:13 各声明一次），无统一头文件（cnn_m5_forward.h 不存在）。

### 1.3 线程与实时模型

| 属性 | 现状 | 评估 |
|------|------|------|
| 处理粒度 | 逐样本（回调内 32 样本/2ms 块，块内逐样本循环） | 与嵌入式 DMA 双缓冲中断模型天然兼容：回调体可整体平移到半满/全满 ISR |
| 跨线程共享 | fx.wc 经 wc_shadow(w)+wc_snapshot(r) 双向隔离；计数器 fade/ramp/mute 用 Interlocked*；监控 float 用 volatile | x86 闭环（复核见 §4.19）；ARM 需将 Interlocked 映射为 C11 stdatomic（见 R-19） |
| 优先级反转风险 | 回调不取锁、不分配内存、不调用主线程函数 | 无锁设计，无反转风险 ✓ |
| 死锁风险 | 无互斥量 | 无 ✓ |
| 回调重入 | 单流单回调 | 无 ✓ |

**Phase-2/3 迁移难度：低-中**。回调体是纯函数式的样本处理（除全局 dft 表），平移到 ALSA period 回调或 I2S DMA ISR 时，只需替换「帧获取/ZOH 输出」两端。真正的迁移工作量在 R-21（HAL 抽象）与 R-12（xd 搬移优化）。

### 1.4 状态机与模式管理

```
                 ┌─────────┐   CNN首帧    ┌──────────┐  cos<0.8且异场景  ┌─────────┐
   上电 ───────► │ BOOT    │ ──────────► │ NORMAL   │ ───────────────► │ FADE    │ ──┐
   mute_hold=1.5s│ (Wc=0)  │ INIT/ramp   │ (自适应)  │                  │ (100ms) │   │ fade完
                 └─────────┘             └────┬─────┘                  └─────────┘   ▼
                                            │  ▲                                  ┌──────────┐
                max|Wc|>5×stub ────────────►│  └── NR>3dB×3s → 存scene_wc         │ NORMAL'  │
                 ┌─────────┐                  │                                    └──────────┘
                 │ FROZEN  │ ◄────────────────┘
                 │ 60s重试  │ ──► 解冻观察3s ──► 再发散 → PERMA_FROZEN (至下次场景切换)
                 └─────────┘
   叠加态(任意状态可触发): peak_mute(10样本>0.95), safety_mute(err_rms>ref_rms, mute_hold=0时),
                          啸叫 NOTCHING (独立微型状态机: CANDIDATE→NOTCHING→RELEASE)
```

- **评价**：状态转换条件完整，冷启动/切换的静音抑制时序正确（ramp 仅 INIT、mute_hold 覆盖收敛窗口）；但状态以 5 个计数器隐式编码，无枚举与转换表，新增状态（如未来的反馈环路标定态）容易遗漏互斥条件——建议 Phase-2 前重构为显式枚举（成本低、可测试性收益大，历史建议 A3 仍有效）。
- **两个实质性逻辑缺口**见 R-6（静音期间梯度开环）与 R-7（freeze 重试无 Wc 回滚）。

---

## 2. 物理可行性评估

### 2.1 欠定系统分析（1×3×2 与 1×5×4）

控制问题：min over W of  J = Σ_e |d_e + Σ_s S_es·(W_s⊗x)|²。E 个误差点的零残差要求 S·L 个自由度同时满足 E 个独立方程组——当 E>S（3>2、5>4），**对任意声场无法实现所有误差点独立归零**（系统对控制目标而言是超定的）。

代码实现的梯度（fxnlms_mimo.c:167-170）：

```
ΔWc[s,k] = −μ/power[s] · Σ_e err[e]·Xd[e,s,k]
```

恰为 J 对 Wc[s,k] 的最速下降方向（∂J/∂W[s,k] = 2·Σ_e e_e·x'_es[n-k]）。**结论：算法目标函数＝误差点总声压能量最小（等权最小二乘），与物理上唯一可实现的目标一致** ✓。不存在"期望各点独立归零"的目标设定错误。

两个附加观察：
- 各误差点等权。若产品定义中人耳位置对应特定误差麦，可加权 w_e（梯度改 Σ_e w_e·err[e]·Xd[e,s,k]），以非关键点的降噪量换取关键点 1-3dB——零成本可选优化。
- 多误差点多于扬声器在窗户开口场景是**正确拓扑**（空间采样靠 E 保证，能量最小化由算法自动权衡），但 E=3/S=2 能否覆盖 1m 级开口，只能由空间 NR 扫描实验回答（历史 W-1/W-5，代码无法给出）。

### 2.2 因果性量化分析（当前系统最硬的物理约束）

延迟链（16kHz 处理域）：

| 分量 | 数值 | 说明 |
|------|------|------|
| 声学超前（ref→err 50cm） | +1.5ms | 唯一可用的"提前量" |
| ADC+PA 输入缓冲 | −2.3ms | 96帧@48k + 转换 |
| **带通 FIR 群延迟** | **−32ms** | (1024−1)/(2·16000)，线性相位 FIR 固有 |
| Wc 最小延迟 | 0ms | FIR 不可超前，最好情况 k=0 |
| DSP 计算 | −0.07ms | 可忽略 |
| ZOH+DAC+输出缓冲 | −2.5ms | |
| **净因果缺口 Δτ** | **≈ −35.5ms** | 反噪声比扰动晚 ~35ms 到达误差麦 |

物理含义：对**宽带随机噪声**，参考信号只能提供 ~1.5ms 的声学提前，处理链却消耗 ~37ms；FIR 滤波器无法在时间上超前（Wc 的能量只能分布在 k≥0），所以 Wc 无法补偿 bp FIR 的 32ms。可对消的成分只剩：
1. **相关时间 > 35ms 的噪声成分**——即主导频率 ≲ 30Hz 的强相关成分（交通隆隆声、变压器嗡鸣），可完全对消；
2. **周期/窄带成分**（引擎谐波、风扇）——相位可按 2π 整数周期回绕对齐，任意频率可对消；
3. 20-200Hz 宽带成分——部分相关，残余 3-6dB 级；
4. **>500Hz 宽带随机成分——理论上不可对消**（与实测 NR 4-9dB 集中在低频一致；与离线 15dB 的差距主因即此：离线仿真中 Dis 与 anti 由同一 bp 信号驱动，因果缺口为 0，见 §6 归因）。

**这是系统级结论，不是代码 bug**：但代码结构放大了它——bp FIR 位于前馈前向通路（x_hist 与 xd 同源），32ms 成为不可绕过的地板。改进方案见 R-13。

### 2.3 空间采样覆盖（重述历史 W-1，代码侧确认）

代码中无任何空间采样建模；E=3/S=2 对 500Hz（λ≈0.69m）以上频率无法覆盖 1m² 开口的模态声场。当前 20-1500Hz 通带设计上届偏乐观，实际有效上限预计 500-800Hz——建议将空间 NR 扫描（网格法）列为 Phase-1 硬件实验，本报告不再展开（见 WINDOW_ANC_SUPPLEMENT §1.2，结论仍然成立）。

---

## 3. 算法正确性逐项核对（标准公式 vs 代码实现）

### 3.1 MIMO FxNLMS（fxnlms_mimo.c:139-175）

标准多误差 FxNLMS（Kuo & Morgan 式 7.2.x， leaky 变体）：

```
x'_es[n] = ŝ_es ⊗ x[n]                       （滤波-x 信号）
y_s[n]   = W_s ⊗ x[n]                        （控制输出）
W_s[n+1] = (1−γ)W_s[n] − μ·Σ_e e_e[n]·x'_es[n] / (ε + P_s[n])
```

代码逐行对照（fxnlms_tick_rt）：

| 步骤 | 理论 | 代码 | 结论 |
|------|------|------|------|
| 滤波-x | Ŝ⊗x | sec_firs 由回调驱动（Ŝ 含 DSP_DELAY=16 padding），xd 延迟线逐样本滚动 | ✓ 维度 [E][S][L] 正确 |
| 控制输出 | W⊗x | anti[s]=Σ_k wc[s,k]·x_hist[k]，x_hist 与 xd 同源（bp 后 ref_filt） | ✓ F-A 修复后结构正确：输出不经 Ŝ |
| 梯度符号 | −μ·e·x′ | `wc -= step·err_meas[e]·xd·inv_pwr` | ✓ 负号正确 |
| 交叉耦合 | Σ_e e_e·x′_es 全体误差驱动各扬声器 | 完全一致的嵌套循环 | ✓ 权值-通路对应正确（e·S+s 索引复核无误） |
| 归一化 | ε+P_s | power[s]=(1e-6+Σ_e,k xd²)/(E·L) | ✓ 数学上等价于 μ′=μ·E·L/Σxd²，被 step_size 调参吸收；epsilon 钳位有效步长 ≤μ/1e-6=100 ✓（TODO-4 已修，复核成立） |
| 泄漏 | (1−γ)W | 梯度后 `wc *= (1−1e-6)`，~1.6%/s 衰减 | ✓ F-D 修复有效（float32 可分辨）；梯度-泄漏顺序差异 O(μγ) 可忽略 |
| 冻结 | — | freeze_lms 同时跳过梯度与泄漏 | ✓ 冻结时 Wc 不衰减（期望行为） |

**梯度未按 E 归一化**（历史 CR-1 复核）：Σ_e 求和使有效步长 ∝ E（E=3→~√3 至 3 倍放大，视 xd 相关性）。当前 step_size 已按 E=3 标定，不改；**但 E 变更（→5）时必须重调 μ**，列入扩展风险（R-30）。

离线路径（fxnlms_tick:70-115）：err=dis+Wc⊗Xd（Ŝ 域合成）、梯度由合成 err 驱动——标准仿真语义 ✓。fxnlms_forward_only 的 err_out 仅含 anti_est（不含 disturbance）——历史 F-H，仅影响离线 fade 期 100ms 的 error_out.wav 内容，可忽略，维持现状。

### 3.2 CNN 前向（cnn_m5_forward.c）

架构 stem(k80,s4)→BN→ReLU→MaxPool(k4,s8)→2×2 ResBlock(64ch)→GAP→Linear(64,K)，与 data/cnn_info.json 完全一致；conv 边界条件（`pos>=0 && pos<in_len` 零填充）、BN（istd=1/√(var+1e-5)，与导出 bn_eps 一致）、GAP、fc 逐式核对 ✓。缓冲乒乓 b[0..3] 复用正确（每个 resblock 后 swap，pool 后 swap，无越界——各层长度 4000/500/125/31 均 ≤max_buf=64×4000）。
**一个前向兼容缺口**：ResBlock 的 projection 支路（in_ch≠out_ch 时）在 export_bin.py:120-122 有导出逻辑，但 C 端 `proj_weight=NULL` 硬编码（cnn_m5_forward.c:106），若未来模型改通道数，C 端静默算错而不报错——列入 R-35。

### 3.3 Blend 与 Wc 构造（scene_controller.c:121-156）

blend=centroid[sid]/bmax → clip[0,1] → Wc=Σ_c b·sub[c] → 取反。负权重截零 ✓（子滤波器相位已对齐，负权无物理意义）；取反符号约定与 LMS 梯度负号自洽 ✓（S-4 后无 RMS 强标，增益由 NLMS 自适应）；max 归一化的伪峰风险（历史 TODO-1）已有数据结论（7/8 场景 ratio≤1.6×），维持不修改。**minmax 归一化不减 min 与 Python 训练侧一致**（历史 S-5 复核：FALSE 成立）。denom≤0.01 弱信号跳过 CNN ✓（CR-18 已修，复核成立）。

### 3.4 啸叫检测（howling_detect.c）

DFT 23 bin（125-1500Hz）+ 汉宁窗 + 峰均值比 15dB + 4 帧确认/8 帧释放 + 逐陷波 512ms 最小保持 + 逐扬声器 IIR 状态——B-2/B-2b/CR-2 三项历史修复**全部复核成立**（状态 [HW_S][HW_MAX_NOTCHES] 维度、add/remove/compaction 三处状态搬运一致）。陷波器公式 b=[1,−2cosω0,1], a=[1,−2r cosω0,r²] 标准二阶陷波 ✓；r=0.96 极点半径距单位圆 0.04，float32 直接 I 型在当前参数下无极限环风险（历史 CR-14 复核：成立；仅当未来 HW_NOTCH_R>0.99 时需 DF2T/double）。**残留**：err 三通道平均会稀释单通道啸叫（R-17）；HW_S=2 硬编码（R-29）。

### 3.5 反馈抵消 NLMS 校准（calibrate_feedback.c:69-135）

NLMS 更新 `w += (0.2/Σx²)·e·x`，Σx² 不求均值 → μ=0.2 对应标准 β∈(0,2) 稳定域 ✓；16k 激励 ZOH×3 播放与运行时输出路径一致（F-F 复核成立）；逐扬声器辨识（F-G 复核成立）；固定种子 srand(42) 可复现 ✓。两点提醒：① 抽取 `ref_16k[i]=ref_hw[i*3]` 无抗混叠（与 R-14 同类，校准激励本身带限故影响小）；② 校准结果直接覆写 data/，无备份与质量门禁（FIR RMS/峰值位置合理性检查建议加入——历史 W-16/CRC-2 仍开放）。

### 3.6 场景切换 CrossFader

Wc=a·wc_old+(1−a)·wc_cur，a:1→0 线性，1600 样本=100ms=20Hz×2 周期 ✓（S-2/S-3 复核成立）；fade 期梯度冻结（forward_rt）——**这是正确设计**（过渡期混合 Wc 的梯度会污染两个场景的记忆，历史 CR-11 复核：成立）。fade 期间主线程跳过 CNN（main_realtime.c:604-606）✓ 避免 wc_cur 读写竞争。

---

## 4. 问题清单

> 编号 R-xx 为本次审查新发/复核结论；每条含：位置 / 级别 / 描述 / 影响 / 修复 / 迁移标记 / 验证方法。
> 级别排序：A 组（正确性）按严重程度降序。

### A. 正确性与健壮性

#### R-1 K 自适应重构残留硬编码 `8` — prev_probs 堆溢出（活跃）+ 栈垃圾污染 + 潜在栈溢出 · **致命** · [Phase-1]

- **位置**：main_realtime.c:429, 600, 605, 621, 630, 653；分配点 scene_controller.c:30
- **问题描述**：commit `0001362` 将 K 改为运行时推导（当前数据 K=6），但实时主循环仍有 6 处硬编码 `8`：
  - `:653` `memcpy(ctx->sc.prev_probs, probs, 8*sizeof(float))` — prev_probs 仅按 K=6 分配 **24 字节**，每秒写入 **32 字节** → **活跃堆缓冲区溢出，每秒 8 字节**，当前代码每次主循环迭代都在破坏堆；
  - `:605` `memcpy(probs, ctx->sc.prev_probs, 8*sizeof(float))` — 从同一 24 字节缓冲区读 32 字节 → 堆越界读；
  - `:600` `float probs[8]` — 若未来重训练 K>8（SC_K_MAX=16），scene_ctrl_process 写 K 个 float → **栈溢出**；
  - `:630-635` cos_sim 循环 `k<8` 与 `:429/:621` anchor memcpy — probs[6..7] 为未初始化栈垃圾，污染 dot/np/nc；若垃圾值在 anchor 与 probs 两侧一致（同栈位复用，高概率），cos 被偏向 1 → 场景切换阈值被隐性抬高。
- **造成的影响**：堆元数据/相邻分配块被每秒覆写 → 长时运行不可预知的堆损坏（无人值守设备不可接受）；场景切换判据被垃圾值偏移；K 增至 9-16 时直接栈破坏。离线 main.c 使用 SC_K_MAX/K 循环，无此问题 ✓。
- **修复方案**（全部改为运行时 K，并加零初始化与一致性断言）：
  ```c
  float probs[SC_K_MAX] = {0};                 /* :600, 清零 */
  const int K = ctx->sc.K;
  memcpy(probs, ctx->sc.prev_probs, K*sizeof(float));        /* :605 */
  memcpy(ctx->anchor_probs, probs, K*sizeof(float));         /* :429, :621 */
  for (int k = 0; k < K; k++) { ... }                        /* :630 */
  memcpy(ctx->sc.prev_probs, probs, K*sizeof(float));        /* :653 */
  ```
- **验证方法**：① sanitizer 验证——MSYS2/MinGW gcc 不支持 ASan，用 **WSL/Linux gcc `-fsanitize=address`**（或 Windows 上 clang-cl ASan / gflags PageHeap）编译实时版：修复前运行 10 秒必报 :653 heap-buffer-overflow，修复后无报告；② 构造 K=10 的伪造 scene_defs/cnn_linear 权重（export_bin.py 改 K），验证无栈破坏；③ 单测：K=6 下 cos_sim(anchor,probs) 与手算值一致到 1e-6。
- **修复状态**：✅ **已修复** (2026-07-27, commit `523b692`) — 6 处硬编码 `8` 全部改为运行时 `ctx->sc.K`，`probs[SC_K_MAX]={0}` 清零，完全按照上述方案实施。实测验证：`cos=-nan(ind)` 消除、场景跳变消失。

#### R-2 离线末块 CNN 输入越界读 · **严重** · [Phase-1]

- **位置**：main.c:284（调用）+ scene_controller.c:60-63（固定读 16000 样本）
- **问题描述**：WAV 长度非整秒时，末块 `len<16000`，但 scene_ctrl_process 的 minmax/CNN 无条件读 audio[0..15999] → 越过 noise_bp 堆缓冲区末（最多 ~64KB）。测试文件恰为整秒（56s/50s/15s）故从未暴露。
- **造成的影响**：堆越界读；轻则末秒场景分类基于垃圾数据（末秒 Wc 错误），重则段错误。
- **修复方案**：`if (len < chunk) break;` 之前跳过 CNN（沿用上一场景 Wc 处理末块），或将末块零填充至 16000 的独立缓冲：
  ```c
  if (len == chunk) new_scene = scene_ctrl_process(&sc, noise_bp+start, wc_cur, probs);
  /* else: 末块不足 1s, 保持当前 Wc（fx.wc 已是自适应后的值, 优于任何预设） */
  ```
- **验证方法**：构造 15.5s WAV 运行 main.exe，ASan 下修复前报越界、修复后干净；末秒 dB 输出无异常跳变。
- **修复状态**：✅ **已修复** (2026-07-27) — `len == chunk` 守卫：末块不足 1s 时复用上一帧 probs/scene，保持当前 Wc。

#### R-3 权重加载零校验 — 缺文件/截断文件 → NULL 解引用或越界 memcpy · **严重** · [Phase-1]

- **位置**：main_realtime.c:475-481（4 个 bin_load_float 返回值全丢弃；cnn_m5_init() 返回值未查）、:511（memcpy 不查 sec 长度）；main.c:146-151（只打印不校验）、:171-172（同）
- **问题描述**：bin_load_float 失败返回 −1 且 *data_out 不变（未初始化指针）；随后 `memcpy(..., sec_path + idx*SEC_LEN, SEC_LEN*4)` 直接解引用。文件存在但截断时（如下载中断），越界读堆缓冲区。cnn_m5_init 失败在实时版被忽略（离线版有检查）。
- **造成的影响**：data/ 任何一个文件缺失/损坏 → 立即崩溃或静默使用垃圾权重运行（后者更危险：扬声器输出不可预测）。
- **修复方案**：
  ```c
  #define LOAD_OR_DIE(path, pp, min_n) do { \
      int n_ = bin_load_float(path, pp); \
      if (n_ < (min_n)) { fprintf(stderr,"FATAL: %s load failed/too short (%d<%d)\n", path, n_, min_n); \
                          ret=1; goto cleanup; } } while(0)
  /* sec 需 ≥ E*S*SEC_LEN, sub 需 ≥ C*S*L_min, scene 需 ≥ S*C, bp 需 ≥ BP_LEN */
  if (cnn_m5_init() != 0) { fprintf(stderr,"FATAL: CNN\n"); ret=1; goto cleanup; }
  ```
  并在 bin 加载层加 magic/版本头（见 R-27）。
- **修复状态**：✅ **已修复** (2026-07-27) — main_realtime.c + main.c 全部 9 处 bin_load_float 返回值校验 + cnn_m5_init() 检查；L 一致性断言（sub 推导 vs 编译期宏）。

#### R-4 CNN 输出维度 K 与 centroids 维度 K 无交叉校验 · **严重** · [Phase-1]

- **位置**：cnn_m5_forward.c:122（g_K 由 linear 权重推导）vs scene_controller.c:20（sc->K 由 centroids 推导）；危险点 scene_controller.c:126 `blend = sc->centroids + scene_id*SC`
- **问题描述**：export_bin.py:51-56 在导出端校验 K 一致，但 C 端两个 K 独立推导、互不知晓。若 data/ 混入不同批次导出的文件（如 CNN 是 K=8 批次、scene_defs 是 K=6 批次），argmax 可能返回 scene_id=7 → centroids 越界读 → 垃圾 Wc。
- **造成的影响**：数据文件版本混配时输出不可预测的反噪声，无任何报错路径。
- **修复方案**：main 初始化处加 `if (cnn_m5_get_K() != sc.K) { FATAL }`；scene_ctrl_init 内加 `scene_id < sc->K` 防御性钳位（返回 −1 并保持旧 Wc）。
- **验证方法**：用两批不同 K 的文件组合启动 → FATAL 且退出；单测 scene_ctrl_process 注入 scene_id=K → 返回错误并保持旧 Wc。

#### R-5 回调假设 fcount 整除 3 — WASAPI 变长回调输出尾部垃圾 · **严重** · [Phase-1]

- **位置**：main_realtime.c:121（`c16k = c48k/3`）、:127-132（抽取相位每回调重置）、:299-307（ZOH 输出仅写 c16k*6 个 float）；main_realtime.c:94 `dec_phase` 声明后从未使用（死字段，说明跨回调相位补偿曾被规划但未实现）
- **问题描述**：ASIO 固定 96 帧时一切正确。但 cfe66e3 已放开 WASAPI/WDM-KS 设备枚举——WASAPI 事件驱动模式回调帧数可变（如 480/452/512…）。若 fcount%3≠0：① 每回调抽取相位从 0 重启 → 抽取采样点在 3 个相位间跳动（周期性抖动失真）；② 输出循环只写 c16k×6 = (fcount/3)×6 < fcount×2 个 float，**DAC 缓冲尾部残留未写内存直接播放**。
- **造成的影响**：WASAPI 路径下周期性爆音/杂音；抽取相位抖动引入边带。ASIO 用户不受影响。
- **修复方案**：短期——打开流后查询实际 framesPerBuffer，若非 3 的倍数则拒绝启动并提示；根治——实现 dec_phase 跨回调相位保持：
  ```c
  int p = ctx->dec_phase;
  for (unsigned long i = 0; i < fcount; i++) {
      if (p == 0) { ref_buf[w]=in[i*6+0]; err_buf[w*3+e]=in[i*6+1+e]; w++; }
      p = (p+1==3) ? 0 : p+1;
  }
  ctx->dec_phase = p;   /* 输出侧同理保持 ZOH 相位 */
  ```
- **验证方法**：WASAPI 设备上以 framesPerBuffer=0（可变）运行，输出接回环录制 → 频谱无周期性脉冲；单测注入 fcount=95/97/96 交替，验证相位连续。
- **修复状态**：✅ **已修复** (2026-07-27, `fb64bdf`) — dec_phase 跨回调相位保持 + 输出 while 补齐保证恰好写满 c48k×2 floats。

#### R-6 safety_mute / peak_mute 期间梯度开环继续更新 · **严重** · [Phase-1]

- **位置**：main_realtime.c:200-204（fxnlms_tick_rt 无条件执行）vs :273（mute 仅清零 anti_spk）
- **问题描述**：两种静音触发后，输出被强制为 0，但 LMS 仍由 err_meas（此刻＝未抵消的纯噪声或发散信号）驱动继续更新 Wc。输出被钳在 0 → 系统处于开环：Wc 朝"抵消一个它实际无法影响的误差"方向空转，直到 max|Wc| 触发 freeze（最长 1 秒延迟）。
- **造成的影响**：异常工况（正是 mute 的设计场景）下 Wc 被污染，mute 解除后 NR 恢复变慢，极端时加速冲向发散阈值。注：scene_wc 记忆保存有 NR>3dB 门，不会被污染 ✓。
- **修复方案**：
  ```c
  if (ctx->safety_mute || ctx->peak_mute || ctx->fade_cnt > 0)
      fxnlms_forward_rt(&ctx->fx, ref_filt, Fx_arr, err_meas, anti_spk); /* 冻结梯度 */
  else
      fxnlms_tick_rt(&ctx->fx, ref_filt, Fx_arr, err_meas, anti_spk);
  ```
- **验证方法**：仿真注入 err 突增（手拍误差麦）→ 触发 peak_mute 前后各保存 wc_snapshot，余弦相似度应 ≈1（修复前显著 <1）。
- **修复状态**：✅ **已修复** (2026-07-27, `f15b909`) — fxnlms_tick_rt 增加 safety_mute/peak_mute 前置检查，静音期间改用 forward_rt 冻结梯度。

#### R-7 freeze 重试无 Wc 回滚 — 以同一发散 Wc 解冻，机制上注定走向永久冻结 · **严重** · [Phase-1]

- **位置**：main_realtime.c:351-388（check_wc_divergence）
- **问题描述**：freeze 期间梯度与泄漏同时冻结 → Wc 分毫不变 → wc_max 永远 >5×stub_rms。60s 后代码无条件解冻（freeze_timer 倒数到 0），以**同一个已发散的 Wc** 恢复 LMS → 几乎必然在 3s 观察期内再次越限 → 永久冻结。即"重试"在机制上不可能成功，只是延迟了永久冻结 63 秒。
- **造成的影响**：瞬时扰动（风噪冲击）触发 freeze 后，系统在本可恢复的场景下被永久冻结至下次场景切换；无人值守时等于该场景 ANC 失效。
- **修复方案**：freeze 触发或解冻重试时回滚到已知良好 Wc：
  ```c
  /* 解冻前: 优先该场景已收敛记忆, 否则 CNN 预设 */
  if (ctx->scene_wc_valid[ctx->cur_scene_id])
      memcpy(ctx->wc_shadow, ctx->scene_wc[ctx->cur_scene_id], S*L*sizeof(float));
  else
      scene_ctrl_construct_wc(&ctx->sc, ctx->cur_scene_id, ctx->wc_shadow);
  InterlockedExchangeAdd(&ctx->wc_seq, 2);   /* 回调下帧原子应用 */
  InterlockedExchange(&ctx->fx.freeze_lms, 0);
  ```
- **验证方法**：离线注入 μ×100 的步长冲击使 Wc 发散 → 观察 freeze→回滚→NR 在 2-3s 内恢复 >3dB，且不进入永久冻结。
- **修复状态**：✅ **已修复** (2026-07-27, `f15b909`) — 解冻前回滚 Wc 到 scene_wc 收敛记忆（优先）或 CNN 预设，通过 wc_shadow+wc_seq 原子提交。

#### R-8 输入路径无 isfinite 防护 — NaN 进入 double 延迟线永久中毒 · **严重** · [Phase-1]

- **位置**：main_realtime.c:157-163（ref）、:193-198（err）
- **问题描述**：输出侧有 isfinite（:208），但输入侧没有。驱动层给出 NaN/Inf（热插拔、驱动异常、USB 抖动均可能发生）时：tanh(NaN)=NaN → bp_fir / bp_err / sec_firs 的 **double 延迟线被写入 NaN** → FIR 输出永远 NaN（延迟线无自恢复能力）→ anti NaN 被钳 0（无声）且梯度把 Wc 冲成 NaN。系统静默死亡且 freeze 机制检测不到（fabsf(NaN) 比较恒假）。
- **造成的影响**：一次驱动毛刺 → 设备永久无声直至重启，日志无任何线索。
- **修复方案**：
  ```c
  if (!isfinite(ref_raw)) { ref_raw = 0; ctx->nan_in_cnt++; }
  /* err 同理; nan_in_cnt 超阈值时 fir_reset(&bp_fir)/bp_err/sec_firs + 记日志 */
  ```
  另加看门狗：检测 anti/anti_est 连续 1s 非有限 → 全套 fir_reset + Wc 回滚（复用 R-7 路径）。
- **验证方法**：单测向回调注入 10 个 NaN 样本 → 系统继续运行且 nan_in_cnt=10，后续正常样本输出正常；注入前/后 bp_fir 输出有限。
- **修复状态**：✅ **已修复** (2026-07-27) — 输入 ref+err isfinite 守卫；输出 >1s 连续 NaN 看门狗自动复位全部 FIR 延迟线 + 主线程日志。

#### R-9 配置集中化半途而废 — 5 个 cfg 字段为死字段，env 覆盖无效 · **一般** · [Phase-1]

- **位置**：gfanc_types.h:30,36-37,71-73 vs main_realtime.c:29-35（宏）, :440（仅 LOG_INFO 打印）
- **问题描述**：`cfg.mic_clip_max`、`cfg.dsp_delay` 从未被使用；`cfg.ramp_ms/mute_hold_ms/fade_len` 被 env 写入后仅在启动日志中打印，实际生效的是 RAMP_SAMPLES/MUTE_HOLD_SAMPLES/FADE_LEN 宏。用户 `set GFANC_RAMP_MS=800` 后日志显示 800 而行为仍是 400——**日志撒谎**。另 main_realtime.c:30 的 MIC_PRE_GAIN 宏与 cfg.mic_pre_gain 并存，print_diagnostics(:336) 用宏显示，回调用 cfg——env 改增益后显示值≠实际值。main.c 加载了 env 但完全不用（MIC_PRE_GAIN 宏 1.0 写死）。
- **造成的影响**：调参工作流被隐性破坏（以为改了参数实际没改），现场调试误导。
- **修复方案**：二选一：① 删除死字段与对应 env 项，日志改打印实际生效宏值；②（推荐）回调初始化时用 cfg 计算 RAMP_SAMPLES/MUTE_HOLD_SAMPLES/FADE_LEN 存入 ctx，统一单一事实源。
- **验证方法**：`GFANC_RAMP_MS=800` 启动，日志值与示波器/录制实测 ramp 时长一致。
- **修复状态**：✅ **已修复** (2026-07-27, `16ec532`) — 删除 dsp_delay/mic_clip_max 死字段；FADE_LEN/RAMP_SAMPLES/MUTE_HOLD/MIC_PRE_GAIN 全部改用 cfg 运行时值，env 变量真正生效。

#### R-10 cleanup 中 use-after-free 读取 ctx->log_file · **一般** · [Phase-1]

- **位置**：main_realtime.c:661-679 — :672 `free(ctx)`，:675 `if (ctx && ctx->log_file)` 读已释放内存
- **问题描述**：释放 ctx 后解引用取 log_file 指针。UB；当前因堆块未复用而"能工作"。
- **修复方案**：`FILE *lf = ctx->log_file;` 在 free 前取出，free 后用 lf；或将日志关闭移到 free(ctx) 之前。
- **验证方法**：WSL ASan（或 clang-cl ASan）下 Ctrl+C 退出 → 修复前报 heap-use-after-free，修复后干净。
- **修复状态**：✅ **已修复** (2026-07-27, `16ec532`) — free(ctx) 前取出 log_file 指针，free 后通过局部变量关闭。

### B. 性能与实时性

#### R-11 回调内逐样本计算 anti_est 仅供 1Hz 显示 — 6144 MAC/样本（~16% 回调预算）浪费 · **严重[Phase-2/3] / 一般[Phase-1]**

- **位置**：main_realtime.c:243-252
- **问题描述**：为计算 nr_level/anti_est_rms 显示值，每样本执行 E·S·L=6144 次乘加（Wc⊗Xd 的 Ŝ 域投影），1 秒 16000 次全量计算只为每秒更新一次的统计量。这是回调内仅次于梯度+功率的第三大开销。
- **造成的影响**：Phase-2 树莓派上直接决定 CPU 余量；Phase-3 上该单项即可压垮 MCU 预算（见 §5）。
- **修复方案**（保语义，成本降 64 倍）：NR 统计改用每秒前 250 个样本（1/64 秒）的同窗口功率对——err 与 anti_est 在相同窗口内累计，其余统计（err_rms 等）仍用全秒：
  ```c
  if (ctx->acc_cnt < 250) {
      /* 原 anti_est 三重循环 → acc_anti_est_w += ...; acc_err_w += err_meas[e]²; */
  }
  /* 每秒结算: nr_level = 10log10((acc_err_w + acc_anti_est_w + ε)/(acc_err_w + ε)) */
  ```
  统计上 250 样本窗口对显示级指标（0.1dB 分辨率）足够。
- **验证方法**：修复前后同一段录制日志的 nr_level 序列偏差 <0.3dB；回调 CPU 占用按 §5.1 表下降 ~16%。

#### R-12 xd_roll_write / x_hist_push 每样本全量搬移 7168 次拷贝 · **严重[Phase-2/3] / 一般[Phase-1]**

- **位置**：fxnlms_mimo.c:29-47
- **问题描述**：xd[E·S·L] 每样本 memmove 式滚动 6144 floats + x_hist 1024 floats。16kHz 下 115M 次拷贝/秒，是纯内存带宽开销；梯度/功率循环随后又线性扫过同一块内存——搬移本身完全可用环形写指针消除。
- **造成的影响**：与 R-11 合计占回调预算 ~40%；Phase-3 上 Cortex-M 级内核直接因此不可行（§5.3）。
- **修复方案**：xd 改为环形缓冲＋写指针，梯度/功率循环改用双段线性访问（与 fir_tick:38-45 的成熟模式一致，编译器可 NEON 向量化）：
  ```c
  /* xd 布局不变, 增加 int xd_ptr; 写: xd_ring[p] = Fx; p=(p+1==L)?0:p+1;
     读 k 的历史: 两段 for(i=p; i>=0; i--) / for(i=L-1; i>p; i--) 与 wc[k] 对齐 */
  ```
  x_hist 同理（或直接与 sec_firs 的延迟线复用结构）。
- **验证方法**：① 离线 main.exe 处理同一 WAV，修复前后 error_out.wav 逐位一致（环形与滚动数学等价）；② perf/计时：fxnlms_tick_rt 单样本耗时应降 25-35%。

#### R-13 带通 FIR 群延迟 32ms 扼守前馈前向通路 — 因果带宽瓶颈 · **严重 · [Phase-1 物理约束，代码结构可改]**

- **位置**：main_realtime.c:163（bp_fir 输出同时供 x_hist 与 xd）、include/fir_filter.h
- **问题描述**：bp_fir(1024tap 线性相位) 群延迟 32ms，位于 Wc 的直接输入路径上；FIR Wc 无法时间超前 → 32ms 是前馈前向通路不可逾越的延迟地板（§2.2 量化：宽带随机噪声有效对消上限被压到 ~30Hz 完全对消 / 200Hz 内部分对消）。这是离线 15dB → 实时 4-9dB 差距的最大单一贡献者。
- **造成的影响**：降噪带宽与 NR 上限被延迟而非算法锁死。
- **修复方案**（按侵入度排序，需硬件 A/B 验证）：
  1. **bp 降阶**：ANC 通路 bp 1024→256tap（群延迟 8ms，频率分辨率仍 62.5Hz），CNN 通路保留 1024tap 独立滤波器（分类需要分辨率）。代价 +256 MAC/样本，因果缺口 35.5→11.5ms；
  2. **最小相位/IIR 带通**：4-6 阶 IIR 群延迟 <2ms（需 double 状态防极限环，与 B-1 反馈 IIR 共用结构）；
  3. 评估放宽通带下限（20→40Hz）换更短 FIR。
- **验证方法**：硬件 A/B——同噪声源分别运行 1024/256tap bp，比较 100-500Hz 段实测 NR；离线仿真无法暴露因果差异（勿用离线验证此项）。

### C. 信号链路

#### R-14 48k→16k 最近邻抽取无抗混叠 · **一般 · [Phase-1]**（历史 P-1 复核：成立）

- **位置**：main_realtime.c:127-132
- **问题描述**：>8kHz 分量折叠进 0-8kHz；14.5-16kHz 恰好折入 20-1500Hz ANC 通带 → 虚假参考/误差信号驱动梯度。缓解（ADC 模拟滤波、环境 HF 能量低）存在但不完备。
- **修复方案**：抽取前 48k 侧加 2 阶 IIR 低通（fc≈6-7kHz，4 通道，~60 MAC/16k 样本，<0.2% 预算）：
  ```c
  /* 每 48k 样本每通道 5 MAC 的 biquad, Q=0.707; 仅保留 x[n-3k] 相位的一致性 */
  ```
- **验证方法**：注入 15kHz 单音，修复前 err_meas 出现 ~1kHz 折叠峰，修复后低于底噪 20dB+。
- **修复状态**：✅ **已修复** (2026-07-27, `83ef232`) — 48k 侧 4 通道 biquad 低通 (2阶 Butterworth, fc=6.5kHz) 全速率滤波后 3:1 抽取。

#### R-15 16k→48k ZOH 内插无抗镜像 · **一般 · [Phase-1]**（历史 P-2 复核：成立）

- **位置**：main_realtime.c:299-307
- **问题描述**：ZOH 产生 16k/32kHz 镜像，1.5kHz 通带内容的镜像在 14.5kHz 处仅被 sinc 衰减 ~3.6dB；经 tweeter 重放、可能被人（儿童）或宠物感知，并占用扬声器冲程。
- **修复方案**：ZOH→线性内插（每 16k 样本 ~6 MAC）：镜像压制提升至 ~25dB；或 2 阶 IIR 抗镜像（fc=7kHz）。
- **验证方法**：输出 1kHz 单音，声谱仪测 15kHz 处镜像电平，修复后下降 ≥20dB。
- **修复状态**：✅ **已修复** (2026-07-27, `83ef232`) — ZOH→线性内插×3 + out_phase 跨回调相位追踪，镜像压制 ~25dB。

#### R-16 次级路径未实测校准（F-B）+ bin 文件无长度/版本防护 · **严重 · [Phase-1]**（历史 F-B 复核：仍开放）

- **位置**：Ŝ 数据来源 export_bin.py:29（Python 仿真 .npy）；使用点 main_realtime.c:502-518 / main.c:165-176
- **问题描述**：当前 Ŝ 是仿真脉冲响应（不含真实 I/O 延迟、扬声器/麦响应、相位），DSP_DELAY=16 为粗略补偿。Ŝ 与真实 S 的相位误差直接压缩 FxNLMS 的稳定步长域与收敛速度——这是 NR 上限的第二大贡献者（仅次于 R-13）。**本审查另发现**：.bin 无任何头部（magic/版本/长度/CRC），且 F-B 校准后 DSP_DELAY 需归零的路径依赖人工记忆。
- **造成的影响**：LMS 收敛域退化（理论：Ŝ 相位误差 >±90° 时任何 μ 都不稳定）；校准流程不可自动化。
- **修复方案**：① `git revert 162d357` 恢复 calibrate_secondary.c，ASIO 共时钟声卡上实测 E×S=6 条 Ŝ（含全部往返延迟），置 DSP_DELAY=0；② bin 格式加 16 字节头 `{magic, version, dims, crc32}`，bin_load_float 校验（与 R-3 同批实施）；③ 校准质量门禁：Ŝ 的 6 条 FIR 打印峰值位置/RMS，超差拒绝写入。
- **验证方法**：实测 Ŝ 与仿真 Ŝ 的互相关峰值对齐误差 <2 样本；替换后同噪声源实时 NR 提升可测（预期 +2-5dB @100-300Hz）。

#### R-17 啸叫检测输入为三误差通道平均 — 单通道啸叫被稀释 ~5dB · **一般 · [Phase-1]**

- **位置**：main_realtime.c:226-230（err_avg = Σerr/E）
- **问题描述**：啸叫通常是单一扬声器↔单一误差麦的窄带自激；三通道平均后若另外两通道有宽带噪声，峰均值比被压 ~4-6dB，15dB 阈值下边界啸叫漏检。
- **修复方案**：改为取三通道瞬时功率最大者（或逐通道独立检测，DFT 成本 ×3≈138 MAC/样本可接受）：
  ```c
  float err_sel = err_meas[0]; float best = fabsf(err_meas[0]);
  for (int e=1;e<E;e++) if (fabsf(err_meas[e])>best){best=fabsf(err_meas[e]); err_sel=err_meas[e];}
  ```
- **验证方法**：单通道注入 900Hz 缓升单音，其余通道播宽带噪声 → 检测延迟与修复前对比，漏检率降为 0。
- **修复状态**：✅ **已修复** (2026-07-27, `fb64bdf`) — 改取三通道中 `|err_meas[e]|` 最大者输入啸叫检测，替代算术平均。

#### R-18 离线工具链：24-bit WAV 实际不支持 + 重采样无抗混叠 + dB(Full) 每块瞬态 · **一般 · [Phase-1]**

- **位置**：main.c:44-76（wav_read_mono 读 bps 但未用，一律按 16-bit 解析）、:100-112（resample_mono 线性插值）、:263-268（full_buf 每 1s 块零态重启 pri_tmp2）
- **问题描述**：① README 声称支持 8/16/24-bit WAV，实际 24-bit 文件会被当 16-bit 错误解析（样本数算错、数据错位）——文档与实现不符，且无任何格式拒绝逻辑；② 任意采样率→16k 用线性插值，下采样时 >8kHz 折叠（离线评估引入虚拟通带内分量）；③ Dis_full 计算每个 1 秒块重建零态 FIR → 每秒前 64ms 含启动瞬态，dB(Full) 轻微低估（Dis_band 用状态拷贝延续 ✓ 正确，两条路径不一致）。
- **修复方案**：① 按 bps 分派 8/16/24/32-float 解析或显式报错；② 重采样前加简易低通（或 libsamplerate）；③ pri_tmp2 改为与 Dis_band 相同的状态延续模式（额外一组 delay_line）。
- **验证方法**：24-bit WAV 输入 → 明确报错或正确解析；48kHz 含 12kHz 成分的 WAV → error_out.wav 无折叠峰；dB(Full) 逐秒序列无周期性首块凹陷。

### D. 线程与并发

#### R-19 §6.2 线程安全复核：x86 闭环成立；ARM 移植需 stdatomic 重写 + 监控量撕裂残留 · **一般 · [Phase-2/3]**（历史 §6.2/TODO-5 复核）

- **位置**：main_realtime.c:136-142（wc_shadow 应用）、:295-296（wc_snapshot）、全文件 Interlocked*
- **复核结论**：写侧（wc_shadow+seq 序号）与读侧（wc_snapshot）隔离后，主线程不再直接触碰 fx.wc；InterlockedExchange/Decrement/ExchangeAdd 在 x86/x64 为全栅栏，wc_old/wc_cur 的"先写数据后置 fade_cnt"顺序成立 ✓。残留两类：① Interlocked* 为 Win32 API，ARM/Linux 需映射为 C11 `atomic_exchange/atomic_fetch_sub`（seq_cst 语义等价）；② nr_level/ref_rms 等 ~14 个 volatile float 监控量回调写/主线程读无原子保证（float 在 ARM 单字对齐写本身原子，但双字 double 或非对齐 float 不保证——当前全 float 故实际仅存在"显示撕裂"级别的风险，可接受）。
- **修复方案**：建立 os_atomic.h 抽象（Windows→Interlocked，GCC/Clang→__atomic_*，C11→stdatomic）；监控量维持 volatile float 即可，文档化"显示级撕裂无害"。
- **验证方法**：ARM 板（Phase-2）上 tsan/helgrind 或 24h 压力运行（高频场景切换注入）无 Wc 撕裂导致的爆音。

#### R-20 CNN 双缓冲在主线程超 1s 处理时静默丢帧 · **建议 · [Phase-1]**

- **位置**：main_realtime.c:166-174（回调覆盖 cnn_buf_ready 无消费检查）
- **问题描述**：主线程 1 秒内未消费（系统卡顿/GC 级别的调度延迟），回调写满第二块后 InterlockedExchange 直接覆盖就绪标记 → 整秒 CNN 输入静默丢失，场景跟踪出现 2s 空洞。
- **修复方案**：`if (ctx->cnn_buf_ready >= 0) ctx->cnn_drop_cnt++;` 并在日志输出。
- **修复状态**：✅ **已修复** (2026-07-27, `fb64bdf`) — cnn_drop_cnt 计数 + print_diagnostics 输出告警。
- **验证方法**：主线程注入 1.5s 阻塞 → 日志出现 drop 计数。

### E. 跨平台可移植性（Phase-2/3 迁移风险全清单）

#### R-21 无音频 I/O 抽象层（HAL）— Windows API 贯穿主程序 · **严重 · [Phase-2/3]**

- **位置**：main.c:14,133（windows.h/SetConsoleOutputCP）；main_realtime.c:12,12x（Sleep/Interlocked*/SetConsoleCtrlHandler）；calibrate_feedback.c:14；pa_loader.h:9（windows.h 直绑）
- **问题描述**：audio 获取/输出、线程同步、定时、控制台全部直调 Win32。Phase-2 树莓派需要：PortAudio 仍可复用（Linux 下直接链接 -lportaudio，pa_loader.c 的 LoadLibrary 层改为静态链接或 dlopen），但 Sleep→usleep/nanosleep、Interlocked→stdatomic、Ctrl handler→sigaction 均需映射。
- **修复方案**：新建两个 60 行以内的头文件：
  ```c
  /* os_atomic.h: gf_atomic_long / gf_atomic_exchange(i,p,v) / gf_atomic_decrement(p) */
  /* osal.h:      gf_sleep_ms() / gf_install_quit_handler(fn) / gf_now_ms() */
  ```
  音频 I/O 抽象 `audio_io_t {open, start, read_frames, write_frames, get_latency, close}`，PortAudio 为其实现之一；Phase-3 由 I2S DMA 实现替换。回调体（audio_cb 的 16kHz 处理段）提取为 `anc_process_block(ctx, in6, out2, n)` 纯函数——这一步同时让单元测试可以脱离声卡驱动回调。
- **验证方法**：Phase-2 交叉编译零 #ifdef 散落在业务代码（全部集中于 osal 两文件）；离线 main.exe 与 anc_process_block 单测共用同一二进制路径。

#### R-22 变长数组（VLA）— MSVC/IAR/Keil 不可编译 · **严重 · [Phase-3]**

- **位置**：fxnlms_mimo.c:79 `float anti_est[E]`、:96 与 :154 `float power[S]`（E/S 为运行时结构体字段 → C99 VLA）
- **问题描述**：PC 端 GCC 无碍；MSVC 全版本不支持 VLA，IAR/Keil 默认不支持。Phase-3 工具链直接编译失败。
- **修复方案**：改为编译期上限栈数组 `float power[GFANC_S_MAX]`（配合运行时断言 S≤GFANC_S_MAX），或移入 fxnlms_mimo_t 的预分配工作区。
- **验证方法**：`gcc -Wvla -Werror=vla` 全量编译零告警；IAR 试编译通过。

#### R-23 FIR 延迟线 double — Cortex-M 软浮点 10-30× 减速 + RAM 翻倍 · **严重 · [Phase-3]**

- **位置**：gfanc_types.h:13（`double *delay_line`）、fir_filter.c 全部
- **问题描述**：double 延迟线是为对齐 Python float64 的精度选择（1024tap 累加误差降 ~10³ 倍），在 x86/A72（硬件双精度）免费，但在 Cortex-M4/M7（仅单精度 FPU）上每次 MAC 是软件双精度运算（~20-30 cycle vs 1-2 cycle）。同时延迟线 RAM 翻倍：sec 6×1040×8=50KB、bp 4×1024×8=33KB、fb 2×256×8=4KB，共 ~87KB。
- **造成的影响**：Phase-3 若保留 double，任何 Cortex-M 直接出局（§5.3 预算 ×25）。
- **修复方案**：float32 延迟线 + 局部 float（或按需 double）累加器：
  ```c
  float dl[N]; ... double acc=0;  /* PC 上保持双精度累加, 嵌入式编 -DGFANC_FLOAT_ACC 切 float acc */
  for(...) acc += (double)c[k++]*dl[i];   /* M7 上 float×float→float 累加, 1024tap 误差 ~2^-19, ANC 可接受 */
  ```
  注意输入信号本身只有 24-bit ADC 精度，float32 存储无信息损失；精度差异仅在累加器。
- **验证方法**：离线 A/B——float32 延迟线版与现版处理同一 WAV，error_out.wav 差值 RMS 低于信号 −60dB；同时记录回调耗时变化（PC 上应基本持平）。

#### R-24 动态内存使用全清单（回调零分配 ✓，慢速路径 4 处需静态化） · **一般 · [Phase-2/3]**

- **位置**：① scene_controller.c:81 — 每秒 `malloc(64KB)` CNN 输入缓冲（应静态化）；② cnn_m5_forward.c:238-243 — 4MB 静态缓冲首次 lazy calloc（语义 OK，但 4MB 对 MCU 不可接受，见 R-25）；③ 全部 init 期 calloc（fxnlms/scene/fir/缓冲）——嵌入式可接受，建议改静态池以便链接期 RAM 审计；④ binary_loader.c 全部 .bin 经 malloc+fread（Phase-3 改内嵌数组后自然消除）
- **修复方案**：cnn_in 改 `static float cnn_in[16000]`（单调用者，无线程问题）；init 期分配集中到一个 `gfanc_arena_t` 静态池，链接脚本可定位。
- **验证方法**：`-finstrument` 或包装 malloc 计数：稳态运行 10 分钟 malloc 调用次数 = 0。

#### R-25 CNN 激活缓冲 4MB — Phase-3 RAM 的最大单项 · **一般 · [Phase-3]**

- **位置**：cnn_m5_forward.c:234-245（b_buf = 4×64×4000×4B = 4MB）
- **问题描述**：为层间乒乓预留的 4 块最大尺寸缓冲。实际仅需 2 块乒乓 + 2 块 scratch（scratch 可与乒乓复用）→ 可降至 2MB；进一步按层分块处理（stem 输出 4000→分段 500）可降至 <512KB；int8 量化后再 ÷4。
- **修复方案**：短期——b[2]/b[3] 与 b[0] 复用（resblock 的 tmp1/tmp2 长度 ≤ 输入长度，可证）；Phase-3——CNN 分块流式推理或 int8 量化（与 QCC5181 路线一致，历史 W-12）。
- **验证方法**：复用后同输入 logits 逐位一致；RAM 审计表降至目标值。

#### R-26 PaHostApiInfo2 与 PortAudio 真实 ABI 错位 — 靠 x64 对齐巧合工作 · **一般 · [Phase-1/2]**（探针实测）

- **位置**：pa_loader.h:45-49
- **实测证据**（探针程序直接读原始内存偏移，2026-07-26 运行）：

  | 偏移 | 真实语义（portaudio.h） | 实测值 | pa_loader.h 解读 |
  |------|----------------------|--------|------------------|
  | +0 | structVersion | 1 | structVersion ✓ |
  | +4 | **type** | 2,1,3,13,11（合法枚举） | （被 padding 吞掉） |
  | +8 | **name** | "MME","Windows DirectSound","ASIO","Windows WASAPI","Windows WDM-KS" | name ✓（巧合对齐） |
  | +16 | **deviceCount** | 11,11,2,9,17 | **type ✗（实为 deviceCount）** |

  代码仅用 `name`，x64 下指针 8 字节对齐使 name 恰好落在 +8 → 能工作。但结构体声明是错的：`type` 实际读到 deviceCount；32-bit 编译时 name 会落到 +4（读到 type 值当指针）→ 立即崩溃。
- **修复方案**：按 portaudio.h 改为 `{int structVersion; int type; const char *name; int deviceCount; int defaultInputDevice; int defaultOutputDevice;}`。
- **验证方法**：修复后设备列表输出与探针表一致；32-bit 编译（若需要）不崩溃。
- **修复状态**：✅ **已修复** (2026-07-27, `16ec532`) — 按 portaudio.h 修正: structVersion → type → name → deviceCount → ...

#### R-27 权重文件无格式头/manifest — 版本混配无防线 · **一般 · [Phase-2/3]**

- **位置**：binary_loader.c（裸 float 流）、export_bin.py（无 manifest 输出）
- **修复方案**：① 每个 .bin 加 16B 头（magic 0x47464E43 "GFNC"、version、dims、crc32）或单一 manifest.json 记录全部 sha256；② export_bin.py 同步输出 manifest 并校验收敛；③ Phase-3 内嵌数组时用同一脚本生成 .h（替代缺失的 export_model.py）。
- **验证方法**：篡改任一 .bin 一字节 → 启动 FATAL；manifest 与文件一一对应。

#### R-28 其他平台依赖项（集中登记，工作量均 <0.5 天） · **一般 · [Phase-2/3]**

| 项 | 位置 | Phase-2 (Linux) | Phase-3 (裸机) |
|----|------|----------------|----------------|
| getenv 参数覆盖 | gfanc_types.h:67-75 | 可用 ✓ | 删，改编译期/Flash 配置 |
| printf/fprintf 日志 | 全局 | 可用 ✓ | 重定向 UART/SWO，回调内已零打印 ✓ |
| time()/ctime 日志戳 | main_realtime.c:587,677 | 可用 ✓ | RTC 或删 |
| `volatile long freeze_lms` | fxnlms_mimo.h:14 | long=64bit on LP64 → 改 int32_t + atomic | 同左 |
| `##__VA_ARGS__` 日志宏 | gfanc_types.h:19-22 | GCC ✓ | IAR 支持，MSVC 2019+ ✓（低风险） |
| 类型宽度 | 全局 int 假设 ≥32bit | ✓ | 样本/系数改显式 int16_t/int32_t/float32_t 审计 |
| 大小端 | .bin 裸 float32 LE | ARM LE ✓ | ✓（DSP 若 BE 需转换层，罕见） |

### F. 可扩展性（1×3×2 → 1×5×4）

#### R-29 通道数/维度硬编码分布图 — 扩展需 6 文件同步修改 · **严重 · [Phase-2]**

- **位置与清单**：

  | 常量 | 当前值 | 位置 | 1×5×4 目标 |
  |------|--------|------|-----------|
  | E / S / L | 3 / 2 / 1024 | main.c:117-119, main_realtime.c:23-25 | 5 / 4 / 1024 |
  | SC_S / SC_C | 2 / 15 | scene_controller.h:11-12 | 4 / 15（子滤波器需重训练 C×S×L=15×4×1024） |
  | HW_S | 2 | howling_detect.h:27 | 4 |
  | 输入通道数/映射 | 6ch, ch0=ref,ch1-3=err | main_realtime.c:127-132, :566 | 6ch 够用（1+5），映射表化 |
  | 输出通道数 | 2 | main_realtime.c:567, ZOH 循环 | 4 |
  | fb_fir[2] / anti_spk[S] 等 | 2 | main_realtime.c:54, :153 `for s<2` | 4 |
  | err_buf 布局 n*3+e | 步长 3 写死 | main_realtime.c:129-131, :193, :236 | 步长 E |
  | probs[8] 等 K 残留 | 8 | （R-1） | 运行时 K |
  | wc_old[2048] | 2048 魔数 | main.c:237 | S·L |
  | export_bin.py S,C,E | 2,15,3 | export_bin.py:40 | 4,15,5 |

- **修复方案**：在 gfanc_types.h 集中 `#define GFANC_E/S/L`（编译期上限）+ 运行时实际值；各模块改从统一头取；`for (int s=0;s<2;s++)` 类字面量全局清查（grep `\bs *< *2\b` 可定位大部分）；子滤波器/centroid/主路径由训练侧重新导出（C 代码零改动即可加载新维度——前提是本组硬编码已清）。
- **验证方法**：以 E=5/S=4 重编译（数据文件用合成维度占位）→ 离线 main.exe 全链路跑通且 ASan 干净；矩阵乘法级单测核对 xd 索引。

#### R-30 梯度有效步长 ∝ E — E 变更必须重调 μ · **一般 · [Phase-2]**（历史 CR-1 复核：结论维持）

- **位置**：fxnlms_mimo.c:167-170（Σ_e 不归一化）
- **问题描述**：E=3→5 时同 μ 下梯度幅度 ~×5/3，发散阈值/收敛速度全部漂移。
- **修复方案**：扩展时同步做 μ 重标定实验；或在梯度中除 E（并一次性重调 μ，离线 NR 回归验证）。
- **验证方法**：E=5 仿真网格扫 μ ∈ [1e-5, 3e-4]，NR 峰值点 μ 与 E=3 时换算值一致。

### G. 可维护性、测试与文档

#### R-31 零单元测试 / 零 CI — 回归靠人工 · **一般 · [Phase-1]**（历史 CR-17 复核：仍开放）

- **现状**：17 文件无任何测试。离线 main.exe 本身是事实上的集成测试，但无断言。
- **修复方案**（最小集合，按性价比排序）：
  1. **黄金回归**：固定 WAV+权重 → anti_out/error_out 的 sha256 基线（一条命令，任何改动立现）；
  2. fir_tick 脉冲响应 == 系数序列；
  3. fxnlms_tick_rt 收敛性：合成 Pri/Ŝ，100ms 内 err 功率下降 ≥10dB；
  4. CNN 与 Python 参考 logits 最大偏差 <1e-4（固定输入）；
  5. 啸叫检测：合成 850Hz 单音 → 64-128ms 内陷波激活，停音后按最小保持+释放时序退出；
  6. R-1/R-2 的 ASan 回归（并入 CI）。
- **验证方法**：`make test` 一条命令全绿；每次 commit 前本地 30 秒跑完。
- **修复状态**：✅ **已修复** (2026-07-27, `3c30f05`) — test/gen_test_wav.c 确定性WAV生成 + test/test_fir.c FIR/FxNLMS单测 + test/run_tests.sh 黄金回归编排 + test/golden.sha256 基线 + Makefile test/test-accept 目标。

#### R-32 文档漂移集合 · **建议 · [Phase-1]**

| 项 | 文档 | 实际 |
|----|------|------|
| 场景数 | README "8 类"（多处） | K=6（cnn_info.json/scene_defs 实测） |
| fade_len | gfanc_config.json: 16 | 代码 FADE_LEN=1600（export_bin.py:179 是陈旧值源头） |
| export_model.py | README 项目结构列出 | 不存在 |
| 音频 API | README "ASIO 声卡"必需 | cfe66e3 已支持 WASAPI/WDM-KS 枚举（但见 R-5） |
| 24-bit WAV | README "支持 8/16/24 bit" | 仅 16-bit（R-18） |
| 校准编译命令 | README 无 -lole32/-D_WIN32_WINNT | Makefile 有（手工命令可能链接失败） |
| 回调预算 | README "~73%" | 分析估算标量 ~30-45%/SIMD ~5-10%（建议复测方法：回调内 rdtsc 统计直方图） |

#### R-33 cnn_m5_free 空函数 + 权重永不释放 · **建议 · [Phase-2/3]**（历史 CR-7 复核：仍开放）

- **位置**：cnn_m5_forward.c:132-135
- **影响**：当前进程生命周期内 ~600KB 权重 + 4MB 缓冲不释放（退出时 OS 回收，无实际泄漏）；阻塞未来 OTA/模型热切换。
- **修复方案**：实现真正的 free（遍历释放各层 + b_buf），或明确删除该函数并在头文件注释"进程级单例"。
- **验证方法**：连续 init/free 100 次，RSS 无增长。

#### R-34 export_bin.py 复核 · **建议 · [Phase-1]**

- **结论**：维度导出正确（sub_filters [C,S,L] C-order 与 C 端 `(c*S+s)*L+l` 一致 ✓；primary [E,R,L]、secondary [E,S,L] 一致 ✓）；K 自动检测 + CNN/场景一致性校验 ✓（好设计）；weights_only=True ✓。
- **问题**：① fade_len:16 陈旧值写入 gfanc_config.json（R-32 源头）；② proj_weight 条件导出但 C 端永不加载（R-35 关联）；③ 无 manifest/CRC（R-27）；④ PY_PROJ 硬编码绝对路径且**未读取任何环境变量**——README 声称的 `GFANC_PYTHON_PROJ` 覆盖方式在脚本中不存在，文档与脚本实现不符。

#### R-35 ResBlock projection 支路前向兼容缺口 · **建议 · [Phase-1]**

- **位置**：cnn_m5_forward.c:106 vs export_bin.py:120-122
- **问题描述**：导出脚本会在 in_ch≠out_ch 时导出 proj 权重；C 端 `proj_weight=NULL` 写死且 resblock_forward 的 projection 分支永远不触发。未来改 CNN 通道数 → C 端静默产出错误 logits，无任何报错。
- **修复方案**：load 时检查 proj bin 文件存在性，存在则加载并走 projection 分支（代码已具备，只差加载）；或在 cnn_info.json 校验 res_channels==64 并拒绝不匹配模型。
- **验证方法**：构造带 proj 的模型导出 → C 端明确行为（加载或拒绝），无静默。

---

## 5. 分阶段资源估算

### 5.1 算法计算复杂度基线（Phase 1，与平台无关）

每 16kHz 样本乘加（MAC）实测结构统计（1×3×2，L=1024）：

| 模块 | 计算式 | MAC/样本 | 备注 |
|------|--------|----------|------|
| 反馈抵消 fb_fir ×2 | 2×256 | 512 | |
| bp_fir | 1024 | 1,024 | double 累加 |
| bp_err ×3 | 3×1024 | 3,072 | double 累加 |
| sec_firs ×6 (Ŝ) | 6×1040 | 6,240 | double 累加 |
| anti 输出 Wc⊗x_hist | 2×1024 | 2,048 | |
| 功率归一化 | 3×2×1024 | 6,144 | |
| 梯度更新 | 3×2×1024 | 6,144 | |
| 泄漏 | 2×1024 | 2,048 | |
| anti_est 诊断（R-11） | 3×2×1024 | 6,144 | **可消除** |
| xd_roll + x_hist 搬移（R-12） | 6144+1024 | 7,168 copy | **可消除** |
| 啸叫 DFT 均摊+陷波 | (23×256×2)/256 + ≤20 | ~66 | |
| 抽取/ZOH/限幅/tanh/统计 | — | ~120 | |
| **合计（现状）** | | **≈33,600 MAC + 7,200 copy** | @16kHz → **≈537 MMAC/s + 115 Mcopy/s** |
| 修复 R-11+R-12 后 | | ≈27,400 MAC | → **≈440 MMAC/s**（−18%；copy 开销基本归零） |
| CNN（慢速环路） | ~52M MAC/次 × 1Hz | 摊薄 ~3,250/样本 | +52 MMAC/s |

**缩放公式**（L=滤波器长，sec_len≈L+16）：
```
MAC/样本 ≈ L·(4·E·S [Ŝ+梯度+功率+诊断] + S [输出] + E [bp_err] + 1 [bp]) + 256·S [fb] + 16·E·S [Ŝ padding]
修复 R-11 后首项系数 4→3；1×5×4 相对 1×3×2 的主导项比例 = E·S: 20/6 ≈ 3.3×
```

### 5.2 Phase 2：树莓派（Cortex-A72 @ 1.5GHz，NEON 4×fp32 FMA/cycle，峰值 6 GMAC/s/核）

| 配置 | 需求 | 有效算力假设 | 单核占用 | 结论 |
|------|------|-------------|----------|------|
| 1×3×2 现状（含 R-11/R-12 浪费） | ~590 MMAC/s（含 CNN 52） | -O2 -mcpu=cortex-a72 自动向量化 ~2-3 GMAC/s | **20-30%** | ✓ 充裕 |
| 1×3×2 修复后 | ~490 MMAC/s | 同上 | **16-25%** | ✓✓ |
| 1×5×4 现状 | ~1.63 GMAC/s | 同上 | **55-80%** | ⚠️ 临界，必须先修 R-11/R-12 |
| 1×5×4 修复后 | ~1.30 GMAC/s | 同上 | **43-65%** | ⚠️ 可行但偏紧（建议 NEON intrinsics 保底 + CNN 移第二核） |

- **调度**：2ms 回调周期对 stock Raspberry Pi OS 偏紧（定时抖动可达数 ms）。建议三选一：① PREEMPT_RT 内核；② 回调帧长 96→240/480（5-10ms 周期，注意每 +1ms 周期直接吞噬因果预算，与 R-13 权衡）；③ isolcpus + SCHED_FIFO + 锁定 CPU 核。Linux 下 PortAudio/ALSA 可复用现有回调结构，R-21 抽象后改动 <200 行。
- **RAM**：当前总量 ~6.6MB（§5.3 表）≪ 1GB ✓；**Flash**：权重 + 代码 <10MB ✓。
- **结论**：Phase 2 不需要 NEON 手工优化即可运行 1×3×2；1×5×4 需先完成 R-11/R-12（纯算法优化，无硬件成本）。

### 5.3 Phase 3：嵌入式 MCU/DSP

RAM 审计（当前 → Phase-3 瘦身目标）：

| 块 | 当前 | 瘦身后 | 手段 |
|----|------|--------|------|
| xd 环形 | 24.6 KB | 24.6 KB | — |
| x_hist / wc | 12 KB | 12 KB | — |
| FIR 延迟线 | 87 KB (double) | ~44 KB | R-23 float32 化 |
| scene_wc | 128 KB (K≤16) | 64 KB (K≤8) | 上限收紧 |
| cnn_buf | 128 KB | 32-64 KB | 0.5s 窗或分块 |
| CNN 激活 | 4 MB | ≤256 KB | R-25 复用+分块/int8 |
| CNN+子滤波器等权重驻留 | ~590 KB | ~590 KB (float32) / ~270 KB (CNN int8) | R-27 |
| 音频缓冲（ref/anti/err） | 1.1 MB | <2 KB | 按块分配（当前 99.9% 浪费） |
| **合计** | **~6.6 MB** | **~1.0 MB (fp32 CNN) / ~0.7 MB (int8)** | |

CPU 需求与芯片推荐（1×3×2，修复 R-11/R-12/R-23 后 ≈440 MMAC/s + 52 MMAC/s CNN ≈ 490 MMAC/s）：

| 芯片 | 有效 MAC 能力 | 1×3×2 占用 | 结论 |
|------|--------------|-----------|------|
| STM32H7 @480MHz（单精度 FPU，~1 MAC/cycle 乐观） | ~480 MMAC/s | >100% | ✗ 不可行；L=512 时 ~280 MMAC/s ≈ 58% — 临界（需 CNN int8/降频推理摊薄，低频分辨率降至 31Hz） |
| i.MX RT1170 (Cortex-M7 @996MHz) | ~1 GMAC/s | ~49% | ✓ 可行（2MB SRAM 满足瘦身 RAM） |
| TI C674x DSP @456MHz（8 MAC/cycle） | 3.65 GMAC/s | ~13% | ✓✓ 余量大，支持 1×5×4（~36%） |
| SHARC ADSP-21489 @450MHz（SIMD+FIR 加速） | >2 GMAC/s | <25% | ✓✓ 音频行业标准选择，支持 1×5×4 |
| QCC5181（Kalimba 定点 DSP） | 定点 Q31 | — | 需全定点移植（W-11：梯度截断/量化噪声需仿真先行） |

**Flash**：权重 fp32 ~578KB（CNN int8 后 ~257KB）+ 代码 ~200KB → **≥1MB Flash**（H7/RT1170 满足）。
**最低推荐规格**：1×3×2 L=1024 不降阶 → Cortex-M7 @~1GHz（i.MX RT1170）或 C674x/SHARC 级 DSP；接受 L=512（低频分辨率 31Hz）→ Cortex-M7 @600MHz 级可行；**1×5×4 量产推荐 SHARC/C674x 或维持 ARM-A 级**。

### 5.4 频域/子带迁移 break-even 分析

- 时域全路径（Ŝ+输出+梯度+功率）成本 ≈ 20.6k MAC/样本；全块频域（2L 点 FFT，块长 L）摊薄 ≈ (1+E·S+E·S+S) 次变换 × 2L·log2(2L)/L ≈ 22×15 ≈ **330 MAC/样本**（~60×节省），但块延迟 2L=128ms 对 ANC 完全不可接受。
- **分块卷积（partitioned-block, 分区 P=128）**：延迟 8ms，成本约为全块的 6-8 倍 ≈ **2-2.6k MAC/样本**，相对时域 20.6k 节省 **~8-10×**。
- **Break-even 判据**：当 ① E·S·L > ~10k（当前 6k，1×5×4 达 20k ✓ 越过阈值）且 ② CPU 是硬约束 且 ③ 可让渡 ≥8ms 算法延迟——三者同时成立才值得迁移。当前系统因果预算已被 32ms bp 群延迟耗尽（§2.2），任何附加延迟直接削减降噪带宽 → **结论：先实施 R-13 把链路延迟降至 <12ms 再评估；Phase-3 若选 SHARC/C674x（算力充裕）则时域即可，无需迁移**。子带方案维持历史"不实施"结论（延迟换收敛，本系统 CNN 预设已解决收敛问题）。

---

## 6. 降噪效果瓶颈归因（离线 15dB → 实时 4-9dB）

| 因素 | 估计占比 | 依据 | 对应项 |
|------|---------|------|--------|
| 因果缺口 35.5ms（bp 群延迟主导） | ~50-60% | §2.2 量化；离线 Dis/anti 同源驱动无此缺口 | R-13 |
| Ŝ 模型误差（未实测） | ~20-30% | 相位误差压缩稳定步长域 | R-16 |
| 室内混响/声学解相关（开放空间） | ~15-25% | 反射分量不可控（窗户实验待做，历史 W-2/W-3/P2-6） | 实验项 |
| 其余（抽取折叠/镜像/增益标定） | <5% | R-14/R-15 | |

相干性方面：反馈抵消（−34dB 实测）已压制扬声器→参考麦直接串扰；残余风险为室内反射间接串扰（W-4）与参考麦朝向工程约束——非代码问题。非平稳噪声：突发噪声由 peak_mute（0.6ms）+ tanh 软限幅 + freeze 链覆盖，建议补 R-6 防开环污染。

---

## 7. 总体评价与改进路线图

### 7.1 评分（10 分制）

| 维度 | 评分 | 说明 |
|------|------|------|
| 算法正确性 | 9 | FxNLMS/CNN/Blend/陷波公式级复核全部正确；控制目标与物理可实现目标一致 |
| 架构设计 | 8 | 三层分离清晰、无锁回调、双路径干净；HAL 缺失扣 2 |
| 数值稳定性 | 8 | double 累加 + epsilon 钳位 + 四道安全防线；R-6/R-7/R-8 三处逻辑缺口 |
| 实时性能 | 8 | 回调零分配、零取模；R-11/R-12 两处 ~40% 浪费 |
| 代码健壮性 | 5 | R-1 活跃堆溢出 + R-2/R-3/R-4 校验缺失——当前最大短板 |
| 可移植性 | 5 | 算法核心干净，但 Win32 直绑/VLA/double/CNN 4MB 四道 Phase-3 门槛 |
| 可扩展性 | 6 | K 已动态化（但有 R-1 残留）；E/S/L 仍六点硬编码 |
| 可测试性 | 3 | 零单测零 CI，回归靠人工 |
| 文档 | 8 | 三份文档覆盖全面；R-32 漂移项需同步 |

### 7.2 全部问题清单与修复状态

> 已完成 10 项 / 总计 35 项。按阶段分组，组内按严重度降序。

#### ✅ 已修复

| 序 | 项 | 级别 | 阶段 | 修复版本 |
|----|----|------|------|----------|
| 1 | **R-1** K 残留硬编码 8 → 堆溢出 | 致命 | Phase-1 | `523b692` |
| 2 | **R-3** 权重加载零校验 + **R-4** CNN/Scene K 交叉校验 | 严重 | Phase-1 | `6dddf85` |
| 3 | **R-2** 离线末块 CNN 输入越界读 | 严重 | Phase-1 | `6dddf85` |
| 4 | **R-8** 输入 isfinite + FIR 中毒看门狗 | 严重 | Phase-1 | `b649d74` |
| 5 | **R-6** 静音期梯度开环 + **R-7** freeze Wc 回滚 | 严重 | Phase-1 | `f15b909` |
| 6 | **R-10** UAF + **R-9** cfg 死字段/env 无效 + **R-26** PaHostApiInfo2 ABI | 一般 | Phase-1 | `16ec532` |
| 7 | **R-31** 黄金回归测试基线 + FIR/FxNLMS 单测 | 一般 | Phase-1 | `3c30f05` |

#### 🔴 Phase-1 待修复（纯软件，零硬件依赖）

| 序 | 项 | 级别 | 工作量 |
|----|----|------|--------|
| 8 | ~~**R-5** WASAPI 变长回调 `fcount%3≠0`~~ ✅ 已修复 (`fb64bdf`) | 严重 | 0.5 天 |
| 9 | **R-11** anti_est 逐样本计算 6144 MAC/样本（~16% 回调预算）— NR 统计降采样优化 | 一般(Phase-1) / 严重(Phase-2/3) | 0.3 天 |
| 10 | **R-12** xd_roll_write/x_hist_push 全量 memmove 7168 拷贝/样本 — 改环形缓冲 | 一般(Phase-1) / 严重(Phase-2/3) | 0.5 天 |
| 11 | ~~**R-14** 48k→16k 最近邻抽取无抗混叠~~ ✅ 已修复 (`83ef232`) | 一般 | 0.2 天 |
| 12 | ~~**R-15** 16k→48k ZOH 内插无抗镜像~~ ✅ 已修复 (`83ef232`) | 一般 | 0.1 天 |
| 13 | ~~**R-17** 啸叫检测三通道平均稀释单通道啸叫 ~5dB~~ ✅ 已修复 (`fb64bdf`) | 一般 | 0.2 天 |
| 14 | **R-18** 离线工具链: 24-bit WAV 不支持 + 重采样无抗混叠 + dB(Full) 瞬态 | 一般 | 0.3 天 |
| 15 | ~~**R-20** CNN 双缓冲主线程超 1s 未消费静默丢帧~~ ✅ 已修复 (`fb64bdf`) | 建议 | 0.1 天 |
| 16 | ~~**R-32** 文档漂移~~ ✅ 已修复 (`c6c1570`) | 建议 | 0.2 天 |
| 17 | **R-34** export_bin.py: fade_len 陈旧值 + GFANC_PYTHON_PROJ 文档声称但未实现 | 建议 | 0.1 天 |
| 18 | **R-35** ResBlock projection 支路: export 有导出但 C 端 NULL 硬编码 → 改通道数时静默算错 | 建议 | 0.2 天 |

#### 🟡 硬件窗口期（需声卡/扬声器/麦克风实测）

| 序 | 项 | 级别 | 工作量 |
|----|----|------|--------|
| 19 | **R-13** bp FIR 群延迟 32ms 扼守前馈通路 — 降阶 1024→256tap (8ms) 或 IIR | 严重 | 1 天 |
| 20 | **R-16** Ŝ 次级路径未实测校准 — `git revert 162d357` + 实测 E×S 条 Ŝ + bin 格式头 | 严重 | 1 天 |

#### 🟠 Phase-2 前必须重构（树莓派 ARM Linux）

| 序 | 项 | 级别 | 工作量 |
|----|----|------|--------|
| 21 | **R-21** 无音频 I/O HAL — Win32 API 贯穿主程序 (Sleep/Interlocked*/CtrlHandler) | 严重 | 1 天 |
| 22 | **R-29** 通道数 E/S/L 硬编码分布 6 文件 — 1×5×4 扩展需多点同步 | 严重 | 0.5 天 |
| 23 | **R-19** Interlocked* → stdatomic 映射 (x86 闭环成立, ARM 需 C11 atomic) | 一般 | 0.3 天 |
| 24 | **R-30** 梯度有效步长 ∝ E — E 变更必须重调 μ | 一般 | 0.2 天 |
| 25 | **R-24** 动态内存需静态化 — 慢速路径 4 处 malloc 改静态池 | 一般 | 0.3 天 |
| 26 | **R-27** 权重 .bin 无格式头/manifest — 版本混配无防线 | 一般 | 0.5 天 |

#### 🔵 Phase-3 前必须重构（MCU/DSP 裸机）

| 序 | 项 | 级别 | 工作量 |
|----|----|------|--------|
| 27 | **R-23** FIR 延迟线 double — Cortex-M 软浮点 10-30× 减速 + RAM 翻倍 | 严重 | 0.5 天 |
| 28 | **R-22** 变长数组 VLA — MSVC/IAR/Keil 不可编译 | 严重 | 0.2 天 |
| 29 | **R-25** CNN 激活缓冲 4MB — Phase-3 RAM 最大单项, 需分块/复用降至 <512KB | 一般 | 1 天 |
| 30 | **R-28** 其他平台依赖: getenv/printf/time()/`##__VA_ARGS__`/大小端等 | 一般 | 0.5 天 |
| 31 | **R-33** cnn_m5_free 空函数 + 权重永不释放 — 阻塞 OTA/模型热切换 | 建议 | 0.2 天 |

### 7.3 建议修复路线

```
Phase-1 收尾 (本周):  R-5 → R-17 → R-14/R-15 → R-11/R-12 → R-18 → R-20 → R-32/R-34/R-35
硬件可用时:           R-16 Ŝ实测 → R-13 bp降阶 A/B
Phase-2 启动:         R-21/R-19 → R-11/R-12 → R-29 (1×5×4) → RPi 部署
Phase-3 评估:         芯片选型 → R-23/R-22/R-24/R-25 → 定点化仿真 (QCC 路线)
```

---

## 附录 A：历史问题登记复核表（自包含）

> 对 UPGRADE_ROADMAP / COMPREHENSIVE_REVIEW 全部登记项在当前代码（7e37499）上的逐条复核。

**已修复且复核成立（代码证据确凿）**：
F-A（fxnlms_tick_rt 独立路径，fxnlms_mimo.c:139-175）· F-D（leak 解耦 1e-6，:113,172）· F-E（anti_spk_prev，:69,144,292-293）· F-F（16k ZOH×3 校准激励，calibrate_feedback.c:56-58）· F-G（逐扬声器双 FIR，:174-216 + main_realtime.c:520-546）· B-2/B-2b/CR-2（陷波逐扬声器状态+最小保持，howling_detect.c 全量复核）· S-1（anchor_probs，:85,429,621,630-635）· S-2/S-3（ramp 仅 INIT、FADE_LEN=1600）· S-4（construct_wc 仅取反，scene_controller.c:143-155）· §6.1（fir_tick 双段零取模，fir_filter.c:38-47）· §6.2（wc_shadow/wc_snapshot 双向隔离，复核见 R-19）· §6.3（CNN 静态缓冲）· §6.5（freeze_lms，fxnlms_mimo.h:14）· §6.6（rt_ctx_t 堆分配，:486）· TODO-4（power epsilon 1e-6，fxnlms_mimo.c:98,156）· TODO-5（计数器 Interlocked 化）· CR-8/CR-9（错误码+goto cleanup）· CR-12（峰值快检测，:213-223）· CR-13（freeze 60s 重试——机制有效但见 R-7）· CR-18（denom 0.01 弱信号跳过）· CR-20（分级日志宏）· C1（gfanc_log.csv）· A2（gfanc_config_t——但见 R-9 死字段）· F-I（acc_anti 移至 mute/ramp 后，:287）· **R-1（K 残留堆溢出，commit `523b692`，2026-07-27）**

**复核为 FALSE/物理事实（维持原结论）**：F-C（tanh 断点三层 FIR 平滑后不可测）· S-5（minmax 与训练一致）· P-3（±1.0 限幅正确）· P-4（群延迟是 FIR 物理属性——但其位置成为瓶颈见 R-13）· §6.4（epsilon 未缩小）· CR-1（步长按 E=3 标定，扩展时见 R-30）· TODO-2（NLMS 辨识含极性，减法恒为抵消）· TODO-3（离线/在线 GAIN 差异是信号链差异非 bug）· CR-11（fade 期冻结梯度是正确设计）· CR-14（r=0.96 安全）· CR-15（fb 输入 1 样本延迟无害）

**仍开放（本报告对应项）**：F-B→R-16 · P-1→R-14 · P-2→R-15 · TODO-1（centroid 数据已验证低风险，维持不改）· TODO-5 监控量→R-19 · B-1 反馈 IIR 环路（未实现，窗户场景影响有限，维持排队）· B-3 remove_notch 死代码（保留）· CR-7→R-33 · CR-17→R-31 · CRC-2/W-15 校准流水线（开放）· A1/A3/B1/B3/C2（升级建议，排队）· W-1~W-16 窗户声学/量产实验项（非代码，维持）· CR-19/W-10 场景覆盖度（训练侧，无法在本仓库验证）

**新发现（本报告首发）**：R-2（离线末块越界）· R-3/R-4（加载与 K 校验）· R-5（fcount%3）· R-6（静音开环）· R-7（freeze 无回滚）· R-8（输入 NaN 中毒）· R-9（cfg 死字段）· R-10（UAF）· R-11/R-12（回调浪费）· R-13（bp 位置瓶颈的量化与方案）· R-17（啸叫稀释）· R-18（离线工具链三项）· R-20（CNN 丢帧）· R-22（VLA）· R-23（double 延迟线移植代价量化）· R-25（CNN 4MB）· R-26（PA ABI 实测错位）· R-27（manifest）· R-29（扩展硬编码分布图）· R-34/R-35（export 侧三项）

## 附录 B：关键验证测试设计

| 测试 | 方法 | 通过判据 |
|------|------|---------|
| ASan 全链路 | gcc -fsanitize=address 编译离线+实时，跑 15.5s WAV 与 60s 实时回环 | 零报告（当前必报 R-1/R-2/R-10） |
| K 鲁棒性 | export_bin.py 生成 K=10 数据，启动 | R-4 FATAL 或正常运行无溢出 |
| 收敛性 | 合成 Pri/Ŝ，fxnlms_tick_rt 驱动 | err 功率 100ms 内降 ≥10dB，Wc 有界 |
| freeze 恢复 | 注入 μ×100 冲击 | freeze→R-7 回滚→3s 内 NR>3dB，不进永久冻结 |
| NaN 注入 | 回调注入 10 个 NaN | 无崩溃、FIR 复位后输出正常、计数正确 |
| 因果 A/B（硬件） | bp 1024 vs 256tap 同噪声源 | 100-500Hz 段 NR 提升可测 |
| Ŝ 校准 | revert 162d357 后实测 vs 仿真互相关 | 峰值对齐 <2 样本 |
| 黄金回归 | 固定输入输出 sha256 | 任何重构后哈希一致（算法等价改动除外，需逐位等价证明如 R-12） |

---

> **总结**：GFANC_FxNLMs_Scene 的算法内核（FxNLMS/CNN/陷波/校准）经公式级复核全部正确，线程模型在 x86 上闭环，工程质量整体较高。当前最紧迫的是 **R-1 活跃堆溢出**（K 自适应重构的清扫死角，每秒腐蚀堆）与一组健壮性校验缺口（R-2/R-3/R-4/R-8）；最大的性能杠杆是 **R-13**（bp 群延迟扼守前馈通路，NR 带宽的第一约束）与 **R-11/R-12**（回调逾 1/3 无效开销）；Phase-2 树莓派按 §5.2 可顺利承载 1×3×2，1×5×4 需先完成算法侧优化；Phase-3 推荐 i.MX RT1170 / SHARC / C674x 档位，STM32H7@480 需 L 降阶至 512 方可达临界可行。
