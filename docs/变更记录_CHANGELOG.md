# 变更记录 (CHANGELOG)

> **变更纪律**: 每次影响系统行为的代码变更，必须在本文档**顶部**插入一条记录。
> 既有记录**只增不改**（历史不可变）；格式字段固定，缺项不允许提交。
> 目标：让每一次变更留下"改了哪里、为什么改、影响了什么"的可追溯记录。

## 记录格式（模板）

新变更复制此模板，插到"记录列表"最上方（最新在上）：

```markdown
### [YYYY-MM-DD] <一句话标题>
- **状态**: 已提交 <commit> / 工作区未提交 / 已回退
- **基线**: <改之前的 git commit>
- **变更代码**:
  - 新增: <文件>
  - 修改: <文件 — 一句话说明>
  - 删除: <文件>
- **变更原因**: <为什么改：问题 / 需求 / 论文依据>
- **造成影响**:
  - 行为: <运行时行为变化，含默认配置下是否变化>
  - 配置: <新增/变更的环境变量、参数>
  - 测试/回归: <golden、单测、A/B 结果>
  - 性能/内存: <算力、内存、线程/锁影响>
  - 未验证项: <诚实列出尚未验证的部分>
- **验证方式**: <如何证明改对了且没改坏>
- **回退方式**: <如何恢复旧行为>
```

---

## 记录列表（最新在上）

### [2026-08-08] 重新引入 OCG 多质心聚类闸门 — 替代 cos 单锚点 reset 判定 (v1.7)

- **状态**: 工作区未提交
- **基线**: cbd08cd（chore: 删除冗余 MIMO 合成数据训练 notebook）
- **变更代码**:
  - 新增: `include/ocg.h`、`src/ocg.c` — OCG 多质心在线聚类闸门（ICASSP 2026 论文 §2.3, 适配增益域）
  - 修改: `main_realtime.c` — reset 模式派发改 `ocg_step()` 簇索引闸门（`GFANC_OCG=0` 可回退旧 cos 闸门）；rt_ctx 加 ocg 字段；INIT 时 `ocg_reset`；诊断行/CSV 加 `k_cluster,n_clusters` 列；apply_reset EVENT 行加簇信息
  - 修改: `main.c`（离线）— 同构接入 OCG + 每秒表加 `[Ck/n]` 簇诊断
  - 修改: `include/gfanc_types.h` — +`ocg_enable/ocg_alpha/ocg_max_clusters` 3 字段、默认值（1, 0.1, 8）、env 解析（`GFANC_OCG/GFANC_OCG_ALPHA/GFANC_OCG_CLUSTERS`）；τ 复用 `switch_threshold`（`GFANC_RESET_THRESH`）
  - 修改: `Makefile` — MODULES +`src/ocg.c`
- **变更原因**: 论文依据（Luo et al., ICASSP 2026）——双速率混合（1Hz CNN 产滤波器 + 采样率 FxNLMS 自适应）中，CNN 输出微小抖动反复触发滤波器更换会打断 FxNLMS 收敛。单锚点 cos 闸门（v1.6）只与"上次重置时增益"比较：慢漂移累计超阈值 → 反复重置；簇内抖动 → 锚点被抖动点反复覆盖 → 每帧重置。多质心闸门：质心跟随漂移/抖动（EMA α=0.1），仅簇索引变化才更换（论文式(4)）。
- **造成影响**:
  - 行为: reset 模式默认走 OCG 闸门；对稳定噪声（现测 mixed_7types_56s）行为与旧闸门一致（均无 reset，平均 NR 均 1.8dB）；对抖动/慢漂移噪声严格更少重置。continuous 模式不变
  - 配置: 新增 `GFANC_OCG`（默认开）、`GFANC_OCG_ALPHA`（0.1）、`GFANC_OCG_CLUSTERS`（8）；`GFANC_RESET_THRESH` 语义变为聚类半径 τ（cos 相似度）
  - 测试/回归: 三目标（main/gfanc_realtime/calibrate）编译零警告；`main.exe mixed_7types_56s.wav` 56s 跑通；OCG 机制 6 项单测全过（簇内抖动保持/突变新建簇/回已知簇复用/LRU 淘汰/慢漂移吸收/零增益保持）；A/B（OCG vs 旧闸门）同一文件结果一致（无回归）
  - 性能/内存: 每帧 O(簇数×K) 余弦比较，1Hz 主线程开销可忽略；ocg_t 栈内存 ~1.1KB
  - 未验证项: ① 真实重训 CNN（当前为合成冒烟检查点）下的抖动行为——OCG 的价值场景，待真实模型重训后复测；② τ=0.8/α=0.1 在增益域的标定依赖旧场景概率域的 0.8 经验值，如有抖动频发可调
- **验证方式**: ① `make` 三目标零警告；② 机制单测 `build/ocg_selftest.c`（6 项全过）；③ 离线 A/B：`GFANC_OCG=0` 与默认对 mixed_7types_56s.wav 输出一致（NR 均 1.8dB）；④ 收紧 τ=0.92 压力测试两闸门均不误触发
- **回退方式**: `GFANC_OCG=0` 环境变量即时回退旧 cos 单锚点闸门；`git checkout` 删除 ocg 文件

### [2026-08-08] 设计决策留档 — tanh 增益域 vs 论文 [0,1] 非负权重 / CNN 路径解耦（均暂不改）

- **状态**: 记录留档，未实施（用户指示"先记录下来，以后在改"）
- **留档 1 — tanh [-1,1] 带符号增益 vs 论文 [0,1] 非负权重**:
  - 现状: `scene_ctrl_process` 对 CNN logits 做 `tanh` → 每扬声器每子带增益 ∈ [-1,1]（带符号，允许相位翻转），`Wc=Σ gain×sub` 后 RMS 标定 + 取反
  - 论文（GFANC 家族）: 权重向量 g' ∈ [0,1]^M 非负，CNN 回归 MSE=0.0031
  - 权衡: 带符号增益表达力强（可反相、逐扬声器独立），但标签求解域更宽 → CNN 回归更难、训练数据需求更大；[0,1] 非负更易学、有界更稳，但表达受限（只能幅度组合）
  - 后续行动（当 CNN 回归误差偏高/收敛不稳时）: ① 标签端加非负/稀疏约束；② CNN 输出层换 sigmoid；③ 保持 tanh 但在损失里加带内增益稀疏正则
- **留档 2 — CNN 与路径解耦（论文 §2.2: 新声学环境只重训子滤波器, CNN 直接迁移）**:
  - 分析结论: 当前标签由真实 MIMO 路径批量 LMS 求解（路径相关）。解耦 = 合成路径（带通）上求标签 → CNN 只学"噪声频谱 → 子带组合"，与路径无关
  - **为什么暂不改**: ① 当前单窗口固定声学环境，真实路径标签严格更优（初始 Wc 更准），解耦在本机不会提高降噪量、反而会略降初始质量；② 收益仅在**多窗型产品**（每台窗型=新路径，免重训 CNN）；③ 需重训 + 硬件 A/B
  - 触发条件（满足再实施）: 出现第二套窗型/开窗姿态需部署；或 CNN 迁移性测试失败
- **回退方式**: 无代码变更，仅文档

### [2026-08-08] 死代码清理 — 删除 OCG 聚类闸门 / scene_manager 死函数 / test 脚手架

- **状态**: 已提交（与 v1.6 直接权重改动同一提交）
- **基线**: e09e946（chore: 重测次级路径 (14:07 窗位) + golden 基线更新）
- **变更代码**:
  - 删除: `src/ocg.c`、`include/ocg.h`、`include/os_atomic.h`、`test/gen_test_wav.c`、`test/test_ocg.c`、`test/test_fir.c`、`test/golden.sha256`、`test/run_tests.sh`（test/ 目录 v1.6 起失效，不再恢复脚手架）
  - 修改: `include/gfanc_types.h` — 删 `GFANC_K_MAX 16`、`ocg_enable/ocg_alpha/ocg_stay_thresh/ocg_rejoin_thresh/ocg_confirm_frames/ocg_max_clusters` 6 字段、`GFANC_CONFIG_DEFAULT` 中 OCG 默认值、`gfanc_config_load_env` 中 6 行 `GFANC_OCG_*` 解析
  - 修改: `include/scene_manager.h` — 全量重写为在用纯函数：`sm_cos_sim`/`sm_wc_max_abs`/`sm_check_divergence`/`sm_check_convergence`（保留原签名）；删 `sm_wc_rms`/`sm_scene_switch_execute`/`sm_first_sec_init`/`sm_check_scene_switch`
  - 修改: `Makefile` — MODULES 移除 `src/ocg.c`；删 `test`/`test-accept` 目标
  - 修改: `include/os_port.h` — 注释移除对已删 `os_atomic.h` 的引用
  - 修改: `README.md` — 参数表/代码结构的 OCG 引用改"已删除"，补判定粒度设计分析
- **变更原因**: OCG 在线聚类闸门（ICASSP 2026）在 v1.5 去场景层后运行时零调用，属留档死代码；直接权重（v1.6）彻底不再需要场景切换逻辑，连同 `test/`（`make test` 已失效）、`os_atomic.h`（仅 ocg.c 使用）一并清理，降低维护面
- **造成影响**:
  - 行为: 无运行时行为变化（删除对象均零调用；`GFANC_OCG` 相关 env 变量不再解析，但 v1.5 起已无 OCG 路径）
  - 配置: `GFANC_OCG`/`GFANC_OCG_ALPHA`/`GFANC_OCG_STAY`/`GFANC_OCG_REJOIN`/`GFANC_OCG_CONFIRM`/`GFANC_OCG_CLUSTERS` 移除（本就不生效）
  - 测试/回归: `make`/`make realtime` 零警告；main.exe 冒烟跑通 56s 无 FATAL、无越界；`make test`/`make test-accept` 目标移除
  - 性能/内存: 无（删除对象运行时不占用）
  - 未验证项: 无
- **验证方式**: ① `make clean && make all` 三目标零警告；② `./main.exe mixed_7types_56s.wav` 56s 跑通；③ 全仓 grep 确认无 `ocg`/`os_atomic`/`scene_defs`/死函数引用（仅文档历史记录）
- **回退方式**: `git checkout` 恢复 ocg/os_atomic/test（git 历史留存，ocg.h/scene_manager 死函数均有记录）

### [2026-08-08] C 运行时改直接权重模式 — CNN 回归 30 维子带增益（v1.6, 分支 gfanc-direct-weight）

- **状态**: 已提交
- **基线**: e09e946（chore: 重测次级路径 (14:07 窗位) + golden 基线更新）
- **变更代码**:
  - 新增: `export/make_synthetic_dw_ckpt.py` — 合成 30 维直接权重检查点（冒烟用，真实训练覆盖）
  - 修改: `include/scene_controller.h` — 去 `centroids/cur_scene/prev_probs`，加 `SC_DW_MAX 30`/`prev_gains[30]`；`scene_ctrl_init(sc, sub_filters, L)` 新签名
  - 修改: `src/scene_controller.c` — 全量重写：CNN 30 维 logits → `tanh` 增益 → `Wc[s,l]=Σ_c gain[s,c]·sub[(c,s),l]` → RMS 标定 + 取反；弱信号/CNN 失败保持上一秒增益
  - 修改: `src/cnn_m5_forward.c` + `include/cnn_m5_forward.h` — K 上限 16 → `CNN_M5_OUT_MAX 30`（`K=n_w/64` 推导不变）
  - 修改: `main.c` / `main_realtime.c` — 移除 `scene_defs.bin` 加载 + FATAL 检查 + centroids 交叉校验；数组 `prev_probs/anchor_probs/probs[16]` → `prev_gains/anchor_gains/gains[30]`；诊断列 Scene → Band（argmax |gain|）
  - 修改: `include/gfanc_types.h` — 注释更新（`GFANC_K_MAX 16` 仅为 ocg.c 死代码保留）
  - 修改: `README.md` — 架构/参数表/示例同步为直接权重（v1.6）
- **变更原因**: 完成直接权重端到端闭环（Python 训练/导出已就绪，C 运行时仍为旧 scene-classifier + centroid blend 并硬依赖 `scene_defs.bin`；30 维检查点被 K≤16 拒绝）
- **造成影响**:
  - 行为: CNN 每秒回归 30 维增益（2 扬声器×15 子带），`tanh` → [-1,1] 带符号直接构造 Wc；不再需要场景 centroid；reset 判定改为 `cos(anchor_gains, cur_gains)<阈值`；`data/scene_defs.bin` 残留不影响运行（已不加载）
  - 配置: 无新环境变量；`GFANC_MODE`/`GFANC_RESET_THRESH`/`GFANC_WC_TARGET` 语义不变
  - 测试/回归: 合成检查点冒烟通过（export → main.exe 56s 跑通，reset 路径 K=30 无越界）；真实模型待用户重训后覆盖验证 NR
  - 性能/内存: 前向不变（同一 m5_scene 架构），仅输出维 16→30，logits/gains 数组各 +56B
  - 未验证项: 真实训练直接权重模型的端到端 NR 未验证（当前为随机权重冒烟）
- **验证方式**: `make`/`make realtime` 零警告；`python export/make_synthetic_dw_ckpt.py` → `python export/export_bin.py`（`cnn_info.json` mode=direct_weight/activation=tanh/fc_out=30，跳过 scene_defs.bin）→ `./main.exe` 56s 无 FATAL、无越界，reset 模式多秒触发
- **回退方式**: `git checkout` 恢复旧 scene-classifier（需同时回退 Python 导出为场景分类检查点）

### [2026-08-08] 去场景层改 Reset/Continuous 双模式 + 嵌入式处理延迟建模（v1.5, 分支 gfanc-direct-weight）

- **状态**: 工作区未提交
- **基线**: e09e946（chore: 重测次级路径 (14:07 窗位) + golden 基线更新）
- **变更代码**:
  - 新增: `include/gfanc_types.h` — `embed_delay_ms` 字段（默认 3ms）+ `GFANC_EMBED_DELAY_MS` env
  - 修改:
    - `include/gfanc_types.h` — 去场景层配置：+`gfanc_mode`（0=continuous, 1=reset）+ `GFANC_MODE`/`GFANC_RESET_THRESH` env
    - `main.c` — Ŝ 模型 pad 嵌入式处理延迟（`embed_delay_ms` 前补零，同时作用于 filtered-x 与误差合成）；启动打印因果报告 `净预览 = τ_pri − τ_spk − τ_proc`；删 OCG 分支/场景切换/收敛写回，改双模式派发
    - `main_realtime.c` — 场景状态机 → Reset/Continuous 双模式派发；`scene_wc`/`cur_scene_id`/`ocg` 字段 → 单一 `last_good_wc`（发散救援/freeze 回滚共用）；`check_scene_switch` → `apply_reset`（无场景记忆）
    - `README.md` — v1.5 文档同步（双模式语义/参数表/代码结构/离线验证/FAQ）
    - `test/golden.sha256` — 重接受（3ms 延迟改变默认输出）
  - 删除（运行时路径）: 场景记忆 `scene_wc`/`scene_wc_valid`、场景 ID `cur_scene_id`、滞回候选 `scene_cand/scene_cand_cnt`、OCG 调用；`sm_scene_switch_execute`/`sm_first_sec_init`/`sm_check_scene_switch`/`src/ocg.c` 留档为死代码
- **变更原因**: 场景层（K=3 分类 + centroid）是启动滤波器质量的瓶颈——Wc 被限制在 3 个 centroid 凸包内，离线启动仅 ~1.2dB；MIMO_GFANC 直接权重固定滤波器启动 ~6.14dB（目标启动 1.2→~6dB）。同时按用户决定，离线测试引入**嵌入式处理延迟 3ms**（ADC+DSP+DAC，典型 DSP 预算）建模因果缺口，供嵌入式移植前评估因果性上限。
- **造成影响**:
  - 行为: 双模式共用同一信号链，仅派发分支不同。**Reset**（默认 `gfanc_mode=1`）: 每秒 `cos_sim(anchor_probs, probs) < switch_threshold(0.8)` → CrossFader 平滑过渡到新 Wc 并刷新 anchor；**Continuous**（`GFANC_MODE=continuous`）: CNN 仅首秒初始化 Wc，FxNLMS 永不重置。离线默认按 3ms pad Ŝ，启动日志新增因果报告行。
  - 配置: 新增 `GFANC_MODE`（默认 reset）、`GFANC_RESET_THRESH`（默认 0.8）、`GFANC_EMBED_DELAY_MS`（默认 3）；`GFANC_OCG`/场景切换参数字段保留但运行时不再使用。
  - 测试/回归: 黄金回归重接受（anti_out/error_out 含 3ms 延迟，输出合理无 NaN）。**关键 A/B 发现**: 现有 3 类 CNN 的 softmax 对全噪声类型输出近恒定 probs（p≈[0.5,0.4,0.05]，cos 最低 0.833）→ Reset 永不触发，两模式输出逐位相同；强制触发（阈值 0.84）时 NR 反降 2.9→2.5（假重置浪费收敛）。结论: Reset 模式的价值只能在**直接权重 CNN 重训**（15 维权重回归替代 3 类 softmax）后显现；双模式外壳已就绪，重训后只替换 `scene_ctrl_process` 内部实现。
  - 性能/内存: 双模式共用一个派发分支；`last_good_wc` 单一数组替换 `scene_wc[K]`，内存略减；去 OCG/滞回路径，1Hz 主线程开销更小。
  - 未验证项: 直接权重 CNN 重训未开始；嵌入式延迟对 NR 的实际影响已在离线验证（基线净预览 −1.9ms、road_noise 相干墙 0.3-0.8dB、正预览下 mixed 恢复 3.3dB/稳态6dB）；实时实机 A/B 未做。
- **验证方式**: ① `make test` 全绿（golden 重接受后）+ 单元测试全过；② 离线 A/B reset vs continuous 输出逐位一致（CNN 恒定 probs 下两种模式无差异）；③ 因果报告行数值核对（τ_pri=0.69ms τ_spk + τ_proc → 净预览 −1.9ms）。
- **回退方式**: 恢复 `sm_scene_switch_execute`/`sm_first_sec_init`/`sm_check_scene_switch` 调用路径与场景记忆字段（git 历史留存），`GFANC_MODE=continuous` 等价于旧 Continuous 语义。

### [2026-08-07] 新增 OCG 在线聚类闸门（替代场景切换滞回）

- **状态**: 已提交 (feat: OCG 在线聚类闸门 — 替代场景切换滞回)
- **基线**: e00094e（docs: README 次级路径测量流程修正 + v1.4）
- **变更代码**:
  - 新增: `include/ocg.h`（ocg_t / ocg_reason_t + 4 API）、`src/ocg.c`（在线聚类闸门实现）、`test/test_ocg.c`（8 项单元测试）
  - 修改: `include/gfanc_types.h`（+6 个 OCG 配置字段/默认值/env）、`main_realtime.c`（rt_ctx_t + ocg 字段；切换决策改双路径；`check_scene_switch` 移除 `cos>=0.8` 冗余守卫；诊断 cos 改为活动簇相似度）、`main.c`（同构双路径 + action 后缀 /rj /nw）、`Makefile` 与 `test/run_tests.sh`（构建加 `src/ocg.c` + 运行 test_ocg）、`README.md`（参数表 +OCG 行）
  - 重接受: `test/golden.sha256`（原因见"测试/回归"）
- **变更原因**: CNN 每帧预测的 probs 有小幅抖动，旧静态滞回（冻结 anchor + cos<0.8 + 3 帧）在噪声缓慢漂移时会误触场景切换，每次切换都重初始化 FxNLMS（Wc 重载 + CrossFader + 保护重置），打断自适应造成不稳定。移植 Luo et al., *"A Stabilized Hybrid Active Noise Control Algorithm of GFANC and FxNLMS with Online Clustering"*, ICASSP 2026 的在线聚类思想：在 probs 空间维护自适应簇中心跟踪漂移，只在噪声真的进入新聚类时才确认切换，并复用已见过的场景（rejoin）。
- **造成影响**:
  - 行为: `GFANC_OCG=0`（默认）→ 与改动前**完全一致**（A/B 逐字节验证）；`GFANC_OCG=1` → 场景切换由在线聚类闸门决策，慢漂移不再误切、回归场景快速识别、单帧闪烁防抖、置信不足帧（argmax<0.5）不判定。切换**机制**（sm_scene_switch_execute + CrossFader + mute/cold/freeze 保护）未动。实时 CSV 新增 `# EVENT: ocg switch ... reason=rejoin|new`；离线 action 列新增 `RESET/rj`、`RESET/nw` 后缀。
  - 配置: 新增 `GFANC_OCG`(默认0)、`GFANC_OCG_ALPHA`(0.10)、`GFANC_OCG_STAY`(0.90)、`GFANC_OCG_REJOIN`(0.75)、`GFANC_OCG_CONFIRM`(3)、`GFANC_OCG_CLUSTERS`(4)。
  - 测试/回归: golden 哈希变化 —— **非代码所致**：数据 `secondary_path.bin`/`primary_path.bin` 于当天 14:07 重导，旧 golden 是旧数据产物，故重接受。改动本身经 A/B 证明零回归（当前二进制 OCG=0 vs git HEAD 输出逐字节一致）。新增 OCG 单测 8/8 通过。三个文件（mixed_7types/road_noise-15/road_noise_0-34）基线 vs OCG 平均 NR_true 完全持平（2.9/0.5/2.8 dB）。
  - 性能/内存: `ocg_t` ≈ 640B 加入 `rt_ctx_t`；1Hz 主线程调用，无锁、无动态分配；每帧至多 K=3 次 cos 计算，算力可忽略。
  - 未验证项: **实时**场景下的"减少误切"收益未实机 A/B（离线 wav 不触发切换路径，需 `GFANC_OCG=1 ./gfanc_realtime.exe` 对比真实噪声下的 `# EVENT: ocg switch` 次数与 NR 稳定性）。
- **验证方式**: ① git HEAD 参照二进制 A/B，OCG=0 输出逐字节一致；② `test/test_ocg.exe` 8 项单测覆盖 真实跳变→NEW、回归→REJOIN、慢漂移不切、闪烁防抖、置信不足、同场景子簇；③ `bash test/run_tests.sh` 全绿；④ 三文件 NR 基线 vs OCG 持平。
- **回退方式**: `GFANC_OCG=0`（默认）即旧滞回路径；如需彻底移除，删除 `include/ocg.h`、`src/ocg.c`、`test/test_ocg.c` 并从 Makefile/run_tests.sh 移除即可，`check_scene_switch` 守卫恢复 `cos>=0.8` 不影响旧路径。
