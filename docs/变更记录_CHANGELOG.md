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

### [2026-08-14] calibrate_secondary 砍掉次级路径辨识 — C 端只测环路延迟
- **状态**: 工作区未提交
- **基线**: 51d090b
- **变更代码**:
  - 修改: `src/calibrate_secondary.c` — 删除 NLMS 次级路径辨识（`build_aligned`/`nlms_identify`/`print_ir_info` 三函数 + ERLE 门禁 + `secondary_path_measured.bin` 输出 + `sec` 缓冲 + `free(sec)`），只保留聚类投票探测 + `sec_bulk_delay.bin` 输出；同步删除常量 `E`/`SEC_TAPS`/`NLMS_MU`/`NLMS_PASSES`/`CHUNK`/`TRACK_SPAN`/`SEC_OUT_FILE`，更新头注释（v4→v5）、横幅、结尾提示
- **变更原因**: 次级路径 Ŝ 已由 Python 扫频产出 `secondary_path.bin`（正确默认）；C 端产出的 `secondary_path_measured.bin` 是死产物（S(e0,s1) 死路径 bug，2026-08-14 实测坐实后已弃用）。C 端唯一还需的输出是环路延迟（`sec_bulk_delay.bin`）。砍掉辨识后程序更快、测量管线职责清晰（Python=Ŝ，C=环路延迟，C=反馈路径）。
- **造成影响**:
  - 行为: `calibrate_secondary.exe` 不再产出 `data/secondary_path_measured.bin`；运行更快（去掉每只喇叭 2 遍 ×1024 抽头 NLMS）。环路延迟探测/保存逻辑不变
  - 配置: 删除 `GFANC_SEC_MIN_ERLE` 环境变量（仅用于被删的 ERLE 门禁）
  - 测试/回归: gcc 编译通过（零警告零错误）
  - 性能/内存: 更少（去掉 `sec` 缓冲 3×2×1024 float + NLMS 计算）
  - 未验证项: 实机未重跑 `calibrate_secondary.exe` 确认只出 `sec_bulk_delay.bin` 且值合理（≈195 样本）
- **验证方式**: 编译通过；实机跑一遍确认输出仅 `sec_bulk_delay.bin` 且 loop_delay≈195 样本
- **回退方式**: `git checkout src/calibrate_secondary.c`

### [2026-08-14] bandpass_anc 低截止 20→100Hz — 治低频扬声器失真滋滋声
- **状态**: 工作区未提交
- **基线**: 51d090b
- **变更代码**:
  - 修改: `export/export_bin.py` — `bp_anc_coeff = firwin(64, [20,1500])` 改 `[100,1500]`（P0-7）；重新生成 `data/bandpass_anc.bin`（备份 `bandpass_anc.bin.bak_20hz`/`.bak_50hz`）
- **变更原因**: 残留"滋滋声"随扬声器音量增减、噪声停后 1-2s 消失 → 低频 anti 内容驱动扬声器失真。实测 Ŝ 在 100Hz 以下滚降 25dB，20~100Hz 的 anti 既消不动（50Hz 发散 -29dB）又推扬声器低频失真。低截止 20→50Hz 后滋滋变小，再 50→100Hz 进一步收窄。
- **造成影响**:
  - 行为: anti 路径带通收窄到 [100,1500]；<100Hz 不再输出反相（该频段本就消不动，还省得驱动失真）
  - 配置: 无新增环境变量；需重跑 `export_bin.py` 重新生成 `bandpass_anc.bin`
  - 测试/回归: gcc 编译通过（gfanc_realtime.exe 零警告零错误）
  - 性能/内存: 无（带通系数同长度 64 tap）
  - 未验证项: ① [100,1500] 实机滋滋是否进一步减小尚未复验；② 100Hz 以下纯音（50Hz）放弃对消后 NR 归零属预期（本来 -29dB 发散）
- **验证方式**: 实机放噪声观察滋滋声随低截止抬升而减小（20→50Hz 已确认变小）
- **回退方式**: `export_bin.py` 改回 `[20,1500]` 并重跑导出，或恢复备份 `bandpass_anc.bin.bak_20hz`

### [2026-08-14] 场景切换软重锚定（去 cold_hold/mute）+ 阈值 0.7 + 迟滞 1s（治切换失效几秒）
- **状态**: 工作区未提交
- **基线**: 51d090b
- **变更代码**:
  - 修改: `main_realtime.c` — `apply_reset` 去掉 `cold_hold=2*FS_ANC` 与 `mute_hold` 两行（场景切换只 crossfade + 重锚定 + freeze_lms=0）；INIT 路径仍设自己的 cold_hold/mute（冷启动保护不变）
  - 修改: `include/gfanc_types.h` — `switch_threshold` 0.6→0.7、`reset_hyst` 2→1（含字段/注释同步）
- **变更原因**: 问题1 = 噪声切换后降噪失效几秒。根因: ① 实测 Ŝ 下 500Hz 切换 cos=0.63~0.67，旧阈值 0.6 永不触发 RESET → CNN 算好的 `wc_cur` 从不提交，只能靠 FxLMS 小步长(4.89e-8)硬爬 2~3s；② 即便触发 RESET，附带 2s `cold_hold` + 1.5s `mute_hold` 打断反相输出，比硬爬还慢。选方案 B（软重锚定）去根：场景切换直接提交 CNN 新滤波器，FxLMS 从正确起点微调。
- **造成影响**:
  - 行为: 场景切换从「硬爬 2~3s」变「1s CNN 检测 + 1s 迟滞 + crossfade ≈ 2s 内提交新滤波器」；RESET 不再附带 cold_hold/mute
  - 配置: `switch_threshold` 0.6→0.7（env `GFANC_RESET_THRESH` 可覆盖）、`reset_hyst` 2→1（env `GFANC_RESET_HYST` 可覆盖）
  - 测试/回归: gcc 编译通过（gfanc_realtime.exe 零警告零错误）
  - 性能/内存: 无
  - 未验证项: ① 实机未复验（需重跑 250↔500 切换确认切换延迟下降、无冷启动期误触发/发散）; ② 阈值 0.7 对 250Hz 深对消期 cos 自然滑落是否误触未回归
- **验证方式**: 待实机复验（跑 250↔500 切换，观察切换后 err_rms 收敛到位的秒数是否从 2~3s 降到 ~2s 内）
- **回退方式**: 恢复 `apply_reset` 两行 cold_hold/mute + `switch_threshold=0.6`/`reset_hyst=2`

### [2026-08-14] 新增 RESET 迟滞（cos 连续 2 秒 < τ 才触发）— 治 500Hz 簇 6/14 抖动误触发
- **状态**: 工作区未提交
- **基线**: 51d090b
- **变更代码**:
  - 修改: `include/gfanc_types.h` — 新增 `reset_hyst` 字段（默认 2, env `GFANC_RESET_HYST`），插入 switch_threshold 之后
  - 修改: `main_realtime.c` — `rt_ctx_t` 新增 `reset_pending` 连续秒数计数器；reset 派发逻辑改为「旧 cos 闸门 + 迟滞」：cos 连续 `reset_hyst` 秒 < τ 才 `apply_reset`，单帧跌破即回零；OCG 路径不变（已有 ocg_hold 持续性判据）；INIT 重建锚点时清零计数
- **变更原因**: 连续 5 次实机运行（同二进制、同命令、实测 Ŝ）翻车方式各不相同（RESET 循环 / Wc 发散 / 500Hz 对消失败），收敛出共同根因 = **增益方向抖动**：500Hz 场景簇 6/14、250Hz 场景簇 16/17 的 tanh 增益份额始终并列（差 1~2%），argmax 反复横跳 → cos(anchor,cur) 逐秒滑落跌破 0.60。旧逻辑 `cos<τ` 单帧即 RESET，而每次 RESET 附带 2s `cold_hold`（anti 硬限幅）+ 1.5s `mute_hold` + CrossFader 过渡，直接打断深对消收敛（NR 从 21dB 掉 0dB 再重建）。加迟滞后：单秒抖动（cos 掉一下又回）被过滤，真正场景切换（cos 连续 2s 以上低于 τ）仍能触发，代价仅是切换延迟 ~1s（期间 FxLMS 自主自适应，无副作用）
- **造成影响**:
  - 行为: 非 OCG reset 模式下，RESET 触发从「单帧 cos<0.6」改为「连续 2 秒 cos<0.6」；250Hz↔500Hz 合法切换仍会 RESET（延迟 ~1s），单帧抖动不再误触发
  - 配置: 新增 `GFANC_RESET_HYST`（默认 2 秒，`GFANC_RESET_HYST=1` 回退旧行为）
  - 测试/回归: gcc 编译通过（gfanc_realtime.exe 零警告零错误）
  - 性能/内存: 无（仅一个 int 计数器）
  - 未验证项: ① 实机未复验（需重跑 250→500→250 切换，确认 RESET 次数下降 + 深对消不被单帧抖动打断）; ② 迟滞值 2s 未扫最优（1s/3s 未对比）
- **验证方式**: 待实机复验（跑 250↔500 切换，观察 `-> RESET` 次数是否下降、NR 是否更稳）
- **回退方式**: `GFANC_RESET_HYST=1` 或恢复 `cos_sim < switch_threshold` 单帧判据

### [2026-08-14] quiet_hold 1→3 + quiet_ref_max 0.045→0.042（修实测 Ŝ 下 500→250Hz 切换 QUIET 误触发）
- **状态**: 工作区未提交
- **基线**: 51d090b
- **变更代码**:
  - 修改: `include/gfanc_types.h` — quiet_hold 默认 1→3, quiet_ref_max 默认 0.045→0.042（含字段/注释同步）
- **变更原因**: 换实测 Ŝ（`GFANC_SEC_FILE=secondary_path_measured.bin`）复验时，500→250Hz 切换瞬间 anti（仍是 500Hz 那套）失配 → refFilt 从 0.0464 瞬态 dip 到 0.0445，跌破 `quiet_ref_max=0.045` 门槛（正常工作 refFilt 0.046~0.048，余量仅 2~7%）→ 叠加 `quiet_hold=1`（上次 3→1）一秒即触发 → 误判"噪声消失"清空反噪声，之后噪声实际仍在（ch 0.09~0.10 甚至更大）却全程 ~2.4s 0dB 无对消
- **造成影响**:
  - 行为: 安静检测更保守——需 ref 更低（<0.042）且持续 3s 才判定噪声消失，场景切换瞬态 dip 不再误触发；代价是噪声真停后反相声残留消退体感稍慢（3s vs 1s）
  - 配置: quiet_hold 1→3, quiet_ref_max 0.045→0.042（env `GFANC_QUIET_HOLD`/`GFANC_QUIET_REF` 仍可覆盖）
  - 测试/回归: gcc 编译通过（gfanc_realtime.exe + main.exe 零警告零错误）
  - 性能/内存: 无
  - 未验证项: ① 实机未复验（需重跑 500→250 切换确认不再误触发 + 噪声真停后仍能正常进安静）; ② 0.042 门槛对"宽带弱噪声 ref≈0.038"是否仍被 quiet_ref_memory 守卫挡住未回归
- **验证方式**: 待实机复验（重跑 500→250 切换 + 噪声真停两种场景）
- **回退方式**: 恢复 quiet_hold=1 / quiet_ref_max=0.045 即恢复旧行为

### [2026-08-13] leak 固定不缩放 + quiet_hold 默认 1s（250/500Hz 发散根因 leak 不足的正式修复）
- **状态**: 工作区未提交
- **基线**: b750760
- **变更代码**:
  - 修改: `main_realtime.c` — 移除 Ŝ-RMS 自动缩放块中的 `cfg.leak *= s_scale`（leak 彻底不缩放）; `include/gfanc_types.h` — quiet_hold 默认 3→1
- **变更原因**: 实机两次 A/B 复现定位 250Hz 发散+滋滋 / 500Hz 无净降噪的**共同根因 = leak 不足**: leak=2.4e-7（曾随 s_scale×0.489 缩放）太小 → 梯度噪声累积 → Wc 无界膨胀 → 250Hz anti 冲 0.71 饱和削波=滋滋、500Hz Wc 在错误位置振荡压不动。leak 固定 5e-7 后两频点均真降噪（250Hz err→0.0225、500Hz→0.027, 均 < 自然基线 0.046）、anti 锁 0.25 零 peak_mute 零 rescue
- **造成影响**:
  - 行为: leak 固定 5e-7 不再随 Ŝ RMS 缩放（原会 ×0.489 → 2.4e-7）
  - 配置: quiet_hold 3→1s（噪声停后更快冻结衰减 Wc）
  - 测试/回归: 编译通过（`mingw32-make realtime`）; 实机验证通过（250/500Hz 真降噪 + anti 稳定 0.25 + 零 peak_mute 零 rescue, Auto gain 1.0x 正常输入）
  - 性能/内存: 无
  - 未验证项: ① `GFANC_QUIET_HOLD=1` 缩短噪声停后反相声残留的体感未验; ② leak 最优值未扫（5e-7 未做 3e-7/8e-7 扫描）
- **验证方式**: 实机验证 250/500Hz 真降噪 + anti 稳定 + 零 rescue（Auto gain 1.0x 正常输入）
- **回退方式**: 恢复 `cfg.leak *= s_scale` 即恢复原缩放语义

### [2026-08-13] R-50 延迟探测 signed 峰 + fb n_taps 用实际加载长度 — 对抗性复核后两处防御性修正
- **状态**: 已提交 <此提交>
- **基线**: b38fe21
- **变更代码**:
  - 修改: `src/calibrate_feedback.c` — `argmax_lag` 相关峰由 `fabs(xcorr)` 改带符号(正相关): 反馈路径正耦合, 真实延迟处 xcorr>0; fabs 会把负相关伪峰(反相串扰)也当候选锁错 lag; `best` 初值 -1→-1e300 防 signed 下全低于初值
  - 修改: `main_realtime.c` — 反馈 FIR `n_taps` 由硬写 `FB_LEN` 改实际加载长度 `n`(与 memcpy 一致, 短文件尾部不读防脏数据当系数); `delay_line` calloc 同步按 n
- **变更原因**: 对抗性复核 R-50 修订时发现两处防御性瑕疵(非阻断): ① fabs 锁负相关伪峰; ② n_taps=FB_LEN 在加载短文件时读未覆盖脏数据。当前 512tap 文件下 n==FB_LEN 且旧 256tap 截断文件被边沿拒收拦截, 两处均不触发; 修正为保证"未来合法短文件 / 反相串扰"场景语义正确
- **造成影响**:
  - 行为: 当前 512tap 反馈文件下无变化; 校准延迟探测在存在反相串扰时选正相关峰
  - 配置: 无
  - 测试/回归: gcc 语法检查 + 完整编译链接(calibrate_feedback.exe / gfanc_realtime.exe)零警告零错误
  - 性能/内存: 无
  - 未验证项: 反相串扰场景未实测(无硬件复现条件)
- **验证方式**: `gcc -fsyntax-only` 两文件 + 完整编译链接两目标
- **回退方式**: git revert 本提交

### [2026-08-13] 对抗性复核当日三条变更记录 — 数字全对, 两处修正: "Test A 稳定 0.044"断言存疑 + measured.bin 死路径遗漏
- **状态**: 已提交 <此提交>
- **基线**: b38fe21
- **变更代码**: 无（仅复核验证 + 记录修正, 不动运行时代码）
- **变更原因**: 用户要求对抗性检查当天三条 08-13 变更记录 → 逐条对照代码 diff / 数据文件 / 重跑工具:
  - ① 部署模型频率选择性: 重跑 `check_v2_direction.py`（分离度 0.79/0.65/0.10/0.06, cos 0.386/0.311/0.209 全精确复现）+ `sweep_deployed_model.py`（遮挡归因 98/233/553/984/1312 全命中; 250Hz 增益铺开 b16=0.66/b2=0.55/b29=0.32 精确匹配）。**且铺开实际比记录更严重**: 100/250/500/1500Hz 输入时 b0(47Hz) 增益 0.29-0.82 全程在场, 1500Hz 输入 top-6 中无任何音调带(±200Hz)内子带
  - ② QUIET err_ref 门: 代码/默认 1.5/`GFANC_QUIET_ERR_REF=0` 恢复旧判据语义全对; err_ref 分离量级自洽（深对消 0.044/0.048=0.92 ∈ 声称 0.7-1.1）
  - ③ 反馈 R-50: FIR 频点增益 FFT 实测精确匹配（spk0 @250Hz=0.382/@125Hz=0.861, spk1 @250Hz=0.709/@125Hz=0.986）; AGC 翻相位链条（main_realtime.c:276-282, fb_est 压过 raw_ref → 符号翻转 + 幅度被 agc 压低）结构成立; 0.058/0.35≈0.17 算术自洽
- **造成影响**:
  - 行为: 无
  - 配置: 无
  - 测试/回归: 复核确认三条记录的可验证数字全部精确; 发现两处记录需修正（见未验证项）
  - 性能/内存: 无
  - 未验证项: ① **"Test A 250Hz err 稳定 0.044 / 零 rescue"断言与 2026-08-13 晚 250-500-250 切换测试 Run A（同样反馈关）直接矛盾** — Run A 250Hz err 0.09-0.26 振荡、anti 涨到 0.71、peak_mute ×7、rescue ×2, 0.044 疑似 anti≈0 自然底噪（INIT 即 0.046）非收敛残差。待对账: Test A 是否更早代码状态/稳态 250Hz 单音; **250Hz 发散与反馈开/关无关, 指向模型-实机相位失配/step 余量**。② **当天重测的 `data/secondary_path_measured.bin` 含死路径 S(e0,s1)=全零**（calibrate_secondary 该路 NLMS 未收敛）→ Run B 500Hz err 上升根因, 该文件不可信已弃用
- **验证方式**: `git diff` 逐条对照; numpy FFT 复核反馈 FIR 频点增益; 重跑 `tools/check_v2_direction.py` / `tools/sweep_deployed_model.py`
- **回退方式**: 无代码变更, 无需回退

### [2026-08-13] 部署模型频率选择性验证 — 修正"部署 CNN 频率盲"错误结论 + 实机调试工具链
- **状态**: 已提交 <此提交>
- **基线**: b38fe21
- **变更代码**:
  - 新增: `tools/sim_step_sweep.py` — 离线 FxLMS 收敛仿真, 逐样本复现实机回调数学（step/leak 自动缩放、dsp 延迟补偿、AGC/cold_hold/ramp/自适应 leak/VS-LMS、块 LMS M=64, 数据全取自 data/）; 验证 step×反馈开关 → err 衰减时间常数, 对照实机日志 anti 平台
  - 新增: `tools/sim_seed_init.py` — 验证实机 ~30s 慢收敛根因 = CNN init Wc 错位（种子 Wc = 部署 CNN 纯音增益直接权重构造, 对照日志 7s err≈0.072 / 18s err≈0.0426 轨迹）
  - 新增: `tools/sweep_deployed_model.py` — 部署模型（data/*.bin 载权, 与实机一致）纯音扫频 + 频率遮挡归因
  - 新增: `Noise Examples/tones_250-500-250_10s.wav` — 250/500/250 切换纯音测试夹具
  - 修改: `docs/GFANC_综合审查报告_合并版.md` — 追加部署模型频率选择性实测结论
- **变更原因**: 用户质疑"CNN 是致盲的吗? 合成噪音训练不是解决了吗?" → 实测修正此前"部署 CNN 频率盲（方向单调）"的**错误**结论:
  - `check_v2_direction.py` 四模型方向区分（cos 250↔500/1000/1500）: **当前部署 `MIMO_M5_DirectWeight_Real.pth` 频率敏感（分离度 0.79, cos 0.386/0.311/0.209, 四者最高）**; 纯合成预训练 0.65 敏感; **v2（合成预训+微调）频率盲（0.10）— 真实微调把合成预训练的方向选择性洗掉了**; baseline_35pct 盲（0.06）
  - 纯音扫频遮挡归因（`sweep_deployed_model.py`, 挖频带重跑模型, 无梯度伪影）: 部署模型**决策驱动频率 = 真实音调**（100Hz→98 / 250Hz→233 / 500Hz→553 / 1000Hz→984 / 1500Hz→1312）— CNN 看得见音调
  - 但**输出增益仍铺开宽带**: 250Hz 输入时 b16/b17（906Hz）gain 0.66/0.55、b29（1812Hz）0.32, 比音调带 b2（188Hz）的 0.55 还高; 100Hz 输入时最高增益落在 b29（1812Hz）。"看得见 ≠ 增益集中" — 纯音训练损失不约束远离音调带的 Wc 增益（纯音在别处无能量, 损失不惩罚）→ 任意非零 → anti 经 Wc 高频放大参考底噪 → 滋滋声候选根因。此与"CNN 看不见音调"是两回事, 滋/浅的根因归属后续再闭环
- **造成影响**:
  - 行为: 无运行时行为变化（纯工具 + 文档 + 结论修正）
  - 配置: 无
  - 测试/回归: check_v2_direction 四模型分离度 0.79/0.65/0.10/0.06; sweep 5 频点遮挡归因全命中真实音调
  - 性能/内存: 无
  - 未验证项: ① `Real.pth` 文件名仍标"从零真实"但实测数字与 OCG 记忆"纯合成重训"完全一致（0.386/0.311/0.209）— 疑似文件已被纯合成重训覆盖、标签过期, 待 git/文件溯源确认; ②"增益铺开"是否直接导致滋滋声未闭环（LMS 稳态会收窄 Wc, 需区分瞬时 vs 稳态）
- **验证方式**: `python tools/check_v2_direction.py`（四模型 cos/分离度）; `python tools/sweep_deployed_model.py`（5 频点增益向量 + 遮挡归因）; `python tools/sim_step_sweep.py [250]`（对照实机 err 轨迹）
- **回退方式**: 删除三个工具文件即可; 结论修正不涉运行时代码

### [2026-08-13] QUIET 安静检测 err_ref 门（治参考麦近场饱和/污染 FIR 压低 ref → 确定性误触发）
- **状态**: 已提交 <此提交>
- **基线**: b38fe21
- **变更代码**:
  - 修改: `include/gfanc_types.h` — 新增 `quiet_err_ref` 字段（默认 1.5）; `GFANC_CONFIG_DEFAULT` 插入; `gfanc_config_load_env` 加 `GFANC_QUIET_ERR_REF` 解析
  - 修改: `main_realtime.c` — QUIET 进入判据加 `err_rms/(ref_rms+1e-6f) > quiet_err_ref` 门
  - 修改: `docs/GFANC_综合审查报告_合并版.md` — R-50 段记录 err_ref 门本次实机已生效（音源开着全程零误触发）
- **变更原因**: ref 门槛只有 ~7% 余量（纯音 ref 0.048 vs 门槛 0.045, 底噪 0.040）, 参考麦近场饱和（见反馈条目）或污染 FIR 把 ref 压低即**确定性误触发**（实机 cb≈12100 两次）。err_ref 门提供 ~2× 余量: **深对消纯音** err 被压到 ref 量级以下（err_ref≈0.7-1.1）→ 挡住误判; **噪声真停**时 anti 失去对消对象, err 被自身输出主导（err≈anti×G≈0.095 vs ref 塌底 0.040 → err_ref≈2.4）→ 通过。与"ref 塌底 + anti 大 + 曾有噪声（quiet_since_active≤20）"组合后两类才可分离
- **造成影响**:
  - 行为: 进安静判据 = anti>0.02 && ref<0.045 && err_ref>1.5 && quiet_since_active≤20（持续 3s）; 深对消纯音不再误进安静, 噪声真停仍正常 [QUIET]
  - 配置: 新增 env `GFANC_QUIET_ERR_REF`（默认 1.5, 0=关闭该门恢复旧判据）
  - 测试/回归: 实机 250Hz 全程零 QUIET 误触发, 噪声真停正常触发; 离线无安静路径未复跑
  - 性能/内存: 无（每 1s 一次标量除法）
  - 未验证项: 参考麦旋钮降半 + `GFANC_MIC_GAIN=2.0` 补偿后 err_ref 工作点需复测
- **验证方式**: 实机 250Hz 纯音 → 停 → `[QUIET] 噪声消失` + 静音; 深对消时零误触发
- **回退方式**: `GFANC_QUIET_ERR_REF=0` 即恢复旧判据

### [2026-08-13] 反馈对消 R-50 修订（512tap + 聚类投票 + 边沿拒收）→ 实机仍发散 → PC 测试台暂弃反馈
- **状态**: 已提交 <此提交>
- **基线**: b38fe21
- **变更代码**:
  - 修改: `src/calibrate_feedback.c` — R-50 修订: `FB_TAPS` 256→512（旧 256 只给响应尾留 ~18 样本 → 截断 → NLMS 不收敛 max|err| 平 0.42, FIR 全坏）; 峰位 sanity 由"11ms 硬上限"改为**聚类投票可复现 + 窗口边沿拒收**双检: 新增 `probe_vote()`（与 calibrate_secondary v4 同法, nsub 个子窗各自全程峰, ±150 样本聚类 ≥1/3 法定数才算真响应, 并报滑移 ppm）; 峰距窗口尾 <10% → 拒收不保存
  - 修改: `main_realtime.c` — `FB_LEN` 256→512; `fb_rms` 循环改用实际加载长度 n（原按 FB_LEN 算错）; 新增反馈 FIR 加载边沿门禁（峰贴窗口尾 → 截断响应 → 跳过不加载）
  - 修改: `data/secondary_path_measured.bin` / `data/sec_bulk_delay.bin` — 环路延迟/次级路径重测（bulk delay → 195 样本 = 12.2ms）
  - 删除: `data/feedback_path_0.bin` / `data/feedback_path_1.bin` → 改名 `.disabled`（Test A 反馈关 = 当前稳定配置, 运行时自动跳过）
  - 修改: `docs/GFANC_综合审查报告_合并版.md` — R-50 状态 → 🔒 PC 测试台暂弃反馈对消, 记录发散链、CLIP 发现、恢复方法、嵌入式 8spk/6mic 反馈刚需的 5 项待办
- **变更原因**: 反馈开启实机 250Hz 发散（4 rescue + 3 peak_mute + err 0.04-0.32 振荡 + anti 涨到 0.7 + 滋滋）。冒烟枪 = **FIR 低频超估 ~4×**: 新 512tap FIR（峰@236/240=14.75/15ms, 聚类 9/9 通过, 不再贴边）在 250Hz 增益 0.38/0.71、125Hz 0.86/0.99, 而运行时 raw_ref 0.058 / anti 0.35 → 真实 250Hz 反馈增益 ≈0.17 → 超估 4×。发散链: anti↑ → fb_est=0.71×anti↑ 压过 raw_ref(0.058) → AGC（main_realtime.c:276-282）误当"ref 过强" → ref_clean 衰减到 0.06 且翻相位 → err_ref 虚高到 4.57 → **rescue 全假触发**反复打断 → anti 涨到 0.7 → peak_mute → 滋滋=高驱动失真+rollback 宽带杂散。根因: 反馈 NLMS 本身没收敛（无逐块对齐、不报 ERLE, max|err| 平 0.33-0.41 是白噪声单样本最大峰非收敛指标）, 把低频/直流烤进系数 — **"截断修好 ≠ 系数修好"**。另: R-50 旧"11ms 物理上限"假设不成立 — USB 设备往返 ~12ms 就在反馈路径里（4 次实测定峰恒 ~238 样本可复现, 是真实延迟非噪声伪峰）。测试台暂弃反馈, 恢复方法已写入文档; 嵌入式样机反馈仍为刚需
- **造成影响**:
  - 行为: 默认配置（存在 feedback .bin）反馈抵消关闭 — Test A 250Hz 纯音 err 稳定 0.044 / 零 rescue / 零 QUIET 误触发; 反馈标定工具新增聚类投票 + 边沿拒收, 坏 FIR 不再保存/加载
  - 配置: 无新增 env; `GFANC_CAL_NOISE` 语义不变
  - 测试/回归: 重标定 512tap FIR 峰@236/240（14.75/15ms）聚类 9/9 通过、不再贴边; 但加载后实机仍发散 → 定位低频超估 → 暂弃反馈
  - 性能/内存: 反馈 FIR 512tap vs 256（+1KB/路）; 运行时反馈关则零开销
  - 未验证项: ① 参考麦 CLIP 饱和处理（旋钮降半 + `GFANC_MIC_GAIN=2.0` 数字补偿, 压低近场反馈分量）待复测 — 稳态 anti 扬声器输出把近场参考麦推饱和 → 限幅失真污染参考 → 慢燃发散源（cb 12067 err 0.074→0.193）, 与反馈开/关无关; ② 嵌入式样机反馈需逐块对齐 NLMS（移植 calibrate_secondary 的 build_aligned）+ ERLE 收敛门禁 + 窄带/白化激励 + 参考麦高通
- **验证方式**: 实机 250Hz Test A（反馈关）err 0.044 稳定; 反馈开启 A/B 复现发散链; FIR 250Hz 增益 vs 运行时实测对照（0.38/0.71 vs ~0.17）
- **回退方式**: `mv data/feedback_path_0.bin.disabled data/feedback_path_0.bin`（0/1 各一次）恢复反馈

### [2026-08-12] 训练脚本拆分 — 保留 Train_validate.py 纯合成训练, 拆出 finetune_real.py 真实微调, 删 Train_validate_synth.py
- **状态**: 已提交 <此提交>
- **基线**: 0a199aa（与上两条同基线, 实际基于打标签拆分后的工作区状态）
- **变更代码**:
  - 新增: `GFANC_Scene/training/network/finetune_real.py` — 真实数据微调(单功能): 加载 2-③ 纯合成产物 → 真实数据低 LR 微调(LR_FT=0.003 / EPOCHS_FT=25), 默认就地覆盖 `MIMO_M5_DirectWeight_Real.pth`; 训练循环同步 Train_validate.py 最新增强(GAIN_RANGE 幅度不变性 + LOG_EVERY 进度打印 + 逐扬声器 cos), 保留旧 train_phase 的 cos_max=-inf 修复; CLI --pretrain-pth / --out / --epochs-ft / --lr-ft / --batch
  - 删除: `GFANC_Scene/training/network/Train_validate_synth.py` — 两阶段二合一脚本(合成预训练 + 真实微调), 与 Train_validate.py 纯合成训练重叠(--no-finetune 分支 = 无 GAIN_RANGE/40 轮/写 Pretrain.pth 的降级变体); 全仓库无代码 import, 仅注释/文档引用
  - 修改: `README.md` — 2-⑤ 命令改 `finetune_real.py`, 删 Pretrain.pth 产物行与 --no-finetune 说明; 2-④ 注释"两阶段微调"→"真实微调"; 训练管线"说明"段两阶段路线措辞改 `finetune_real.py`
- **变更原因**: 延续"一功能一文件"重构: `Train_validate_synth.py` 的 `--no-finetune` 分支与 `Train_validate.py` 纯合成训练功能重叠但更弱; 用户确认保留 `Train_validate.py` 不动, 把唯一独有的真实微调拆成单功能文件
- **造成影响**:
  - 行为: 训练入口变化 — 纯合成训练仍走 `Train_validate.py`(未改动); 真实微调从 `Train_validate_synth.py` 改为 `finetune_real.py`, 输出默认就地覆盖 Real.pth(与旧两阶段一致, export_bin 自动加载); `MIMO_M5_DirectWeight_Pretrain.pth` 概念废弃(旧文件成孤儿, 不影响任何流程)
  - 配置: `finetune_real.py` 参数 --pretrain-pth/--out/--epochs-ft/--lr-ft/--batch 替代旧的 --epochs-pre/--lr-pre/--no-finetune 等
  - 测试/回归: 冒烟 1 轮微调循环跑通(输出临时路径, 未碰 Real.pth, 哈希不变); `Train_validate.py` 零改动
  - 性能/内存: 不涉及
  - 未验证项: 完整 25 轮真实微调未跑(数小时 GPU); 微调后判别力/降噪待验证(该路线当前未部署)
- **验证方式**: py_compile 两脚本; grep README/GFANC_Scene/docs 无 `Train_validate_synth` 残留(CHANGELOG 历史条目除外, 历史只增不改); 冒烟 `finetune_real.py --epochs-ft 1 --out <tmp>` 确认训练循环 + 检查点保存, 且 Real.pth 哈希不变
- **回退方式**: git revert 本提交; 旧 `Train_validate_synth.py` 可从 git 历史找回

### [2026-08-12] README 阶段 2 训练命令修正 — 纯合成训练为主流程, 两阶段真实微调降为可选
- **状态**: 已提交 <此提交>
- **基线**: 0a199aa
- **变更代码**:
  - 修改: `README.md` — ① 阶段 2 命令重排: 2-② = `make_synthetic_dataset.py`（**从零生成**合成数据 + LMS 打标签, 由原"可选增强"升为主流程必做, 注释明确 60000/7500/7500、4 类谱形、Pri/Sec 过路径、为何必须生成）; 2-③ = `Train_validate.py` 纯合成训练（主流程, 标注"脚本默认读 Index_synth_*"）; 2-④/2-⑤ = `label_real_noise.py` 真实打标签 + `Train_validate_synth.py` 两阶段训练（明确标注可选、当前流程没用、--no-finetune 等价纯合成）; 评估 → 2-⑥, 导出 → 2-⑦; ② 导出编号引用全局同步 2-⑥ → 2-⑦（快速上手选择表、批次指纹 R-27、换硬件检查单、完整命令流、训练管线导出提示）; ③ "训练管线·数据与标签"段数据源由真实为主改为**合成数据为主**（Synthetic_Dataset）, 补"为什么必须生成合成数据"（真实 4 类全低频坍缩、判别力 35.8% vs 75%）, 澄清合成是独立数据集不是增强; ④ 澄清 `MIMO_M5_DirectWeight_Real.pth` 名字带 Real 但实为纯合成训练产物
- **变更原因**: 用户对抗性审查阶段 2 三条命令: ① "我测试效果好的用的是合成数据, 但命令里没有生成合成数据的命令?"——生成命令就是 `make_synthetic_dataset.py`, 但原注释"合成数据增强（可选）"措辞误导, 让人以为是增强已有真实数据, 实为从零生成独立数据集; ② "合成数据增强怎么增强?做了什么?"——补 4 类谱形生成 + 同一 LMS 管线打标签机制说明; ③ "两阶段训练这也不对吧?我当前用的直接就是合成数据吧?"——核实 git commit 2ad4b69"合成数据全量重训 (Train_validate.py, cos=0.9706)"与 `Train_validate.py:56-58`(DATA_DIR=Synthetic_Dataset, 读 Index_synth_*)确认部署模型确为**纯合成训练、无真实微调**, 原文档 2-④"两阶段"与实际流程不符
- **造成影响**:
  - 行为: 纯文档修订, 无代码/运行时行为变化
  - 配置: 无
  - 测试/回归: 不涉及
  - 性能/内存: 不涉及
  - 未验证项: 无
- **验证方式**: grep 核对阶段 2 编号引用全自洽（2-①~2-⑦ 定义与引用一一对应, 无残留旧编号 2-⑥=导出）; 交叉核对数据流: `Train_validate.py:56-58` 读 Index_synth_*、`Train_validate_synth.py:61-69` 两阶段依赖 Index_real_*、`make_synthetic_dataset.py:77-78` 加载 broadband.mat
- **回退方式**: git revert 本提交

### [2026-08-12] 训练打标签拆分为"一功能一文件" — generate_synthetic / cut_real_noise / label_wavs, 删两个多合一脚本
- **状态**: 已提交 <此提交>
- **基线**: 0a199aa
- **变更代码**:
  - 新增: `GFANC_Scene/training/labeling/generate_synthetic.py` — 合成数据生成(4 类谱形 WAV + probe), 从 make_synthetic_dataset.py 抽出; 删死代码 generate_split
  - 新增: `GFANC_Scene/training/labeling/cut_real_noise.py` — 真实长录音切割(cut_config.json 状态机) + 首建分层拆分(0.8/0.1/0.1, BLOCK_SIZE 防泄漏), 新增 --max-per-category 类别平衡; 从 label_real_noise.py 抽出
  - 新增: `GFANC_Scene/training/labeling/label_wavs.py` — 统一 LMS 打标签(--wav-dir + --tag {real,synth}), 合并两脚本打标签段为唯一实现; category 列 real=类别前缀 / synth='synthetic'; 丢弃 real 旧 CSV 的 idx 空列
  - 删除: `make_synthetic_dataset.py` / `label_real_noise.py`(两多合一脚本; 全仓库无 import 依赖, 仅注释引用)
  - 修改: `recluster_real.py`/`Train_validate.py`/`verify_discrimination.py` — 注释引用改新文件名(输出格式不变)
  - 修改: `README.md` — 阶段 2-② 拆为 generate_synthetic + label_wavs --tag synth; 2-④ 拆为 cut_real_noise + label_wavs --tag real; 训练管线"数据与标签/说明"段同步
- **变更原因**: 用户诉求"每个功能单独文件, 需要时直接运行单独文件"——原两个脚本各耦合两件事(生成+打标签 / 切割+打标签), 打标签核心循环在两处逐行重复; 用户理想是消除重复
- **造成影响**:
  - 行为: 命令变化 — 旧 `make_synthetic_dataset.py --gen-only/--label-only` → 新 `generate_synthetic.py` + `label_wavs.py --tag synth`; 旧 `label_real_noise.py` → 新 `cut_real_noise.py` + `label_wavs.py --tag real`。输出文件格式不变(Index_{tag}_{split}.csv 列 File_path/category/gain_*, Gains_{tag}_{split}.npy), 下游 Train_validate/noise_dataset/verify_discrimination/recluster_real 无缝
  - 配置: 新增 --max-per-category(类别平衡); 旧 TOTAL_SEGMENTS=80000 采样预算改由 --max-per-category 表达(复现旧规模=20000/类); cut_config.json 状态文件兼容复用
  - 测试/回归: 待小样本冒烟验证
  - 性能/内存: 切割脚本不再加载 Pri/Sec + 子滤波器(GPU 资源), 纯 CPU; 打标签/生成与旧等价
  - 未验证项: 全量 60000 样本未跑(打标签 ~5.5h GPU); 真实切割对真实 raw 数据未跑(无样本)
- **验证方式**: 小样本 CPU 冒烟 — generate_synthetic --n-train 5/--n-val 2/--n-test 2 + label_wavs --tag synth → CSV 列结构 + MyNoiseDataset 加载; cut_real_noise 用临时 raw 假 wav + --max-per-category → 段数/分层断言
- **回退方式**: git revert 本提交

### [2026-08-12] README 声学路径测量阶段扩为四项 — 主路径/环路延迟/反馈路径并入阶段 1, 阶段 3 只剩编译
- **状态**: 工作区未提交
- **基线**: c5ffcbb
- **变更代码**:
  - 修改: `README.md` — ① 阶段 1"声学路径测量"扩为全部四项测量: 1-① 编译校准程序, 1-② 次级路径 Ŝ(measure_secondary.py), 1-③ 主路径 Pri(measure_primary.py --source-channel, 扬声器搬到室外噪声源位置), 1-④ 环路延迟(calibrate_secondary.exe), 1-⑤ 反馈路径(calibrate_feedback.exe); ② 主路径补入训练输入标注(子滤波器 2-①/打标签 2-②/导出 2-⑥ 都用 primary+secondary .npy, 与 Pre_training_broadband_and_decompose.py:411-414 加载 Pri_path+Secon_path 一致); ③ 阶段 3 改为仅编译(实时版 3-①/离线版 3-②), 环路延迟/反馈路径移出; ④ 快速开始/换硬件检查单/次级路径测量/反馈路径校准章节引用同步(阶段 1-②/1-④/1-⑤/3-①/4-①); ⑤ 修三层架构图代码块缺闭合(└┘ 后无 ```, 吞掉"反馈路径校准"标题至反馈日志块)
- **变更原因**: 用户两点对抗性审查: ① 环路延迟/反馈路径同为"测量"动作且共用硬件布置, 应从"编译部署"归入"声学路径测量"阶段; ② 记得有单独的主路径录制命令(measure_primary.py)——核实确存在且是训练输入(打标签/子滤波器必需), 原文档只在训练阶段提了一句"仓库自带", 阶段 1 只测了次级路径, 主路径漏测
- **造成影响**:
  - 行为: 纯文档修订, 无代码/运行时行为变化
  - 配置: 无
  - 测试/回归: 不涉及
  - 性能/内存: 不涉及
  - 未验证项: 无
- **验证方式**: 逐文件核对: measure_secondary.py 写 secondary_path.npy(L318), measure_primary.py 写 primary_path.npy(L139, source-channel 必填), Pre_training_broadband_and_decompose.py:411-414 同时加载两路径; grep 确认阶段引用一致、代码块围栏配对(26 偶数)
- **回退方式**: git revert 本提交

### [2026-08-12] README 完整命令流重构为阶段 0-4 — 次级路径测量前移到训练之前, 默认从头训练
- **状态**: 工作区未提交
- **基线**: 7f0d885
- **变更代码**:
  - 修改: `README.md` — ① 完整命令流阶段重排: 原 准备0→训练数据A→系统运行B/C/D 改为 准备0 → 声学路径测量1(测次级路径 Ŝ, 先于训练) → 训练数据2(子滤波器→打标签→合成→训练→评估→导出) → 编译部署3(环路延迟+反馈路径+编译实时/离线版) → 系统运行4(运行+纯音验证); ② 快速开始默认路径改为"从头训练"全流程(阶段0→1→2→3→4), "先用现成模型跑通"降为可选捷径(阶段0→1→3→4); ③ 训练管线章节/换硬件检查单/次级路径测量章节/反馈路径校准章节的阶段引用全部同步为新编号(阶段2/2-⑥/阶段1/阶段3-③); ④ 换摆放说明重写(重做阶段1+3-③+4, 训练数据阶段2不随摆放变化, 效果差可重训)
- **变更原因**: 用户要求文档默认读者都要自己训练数据、从头开始——原编排把次级路径测量(C1)放在子滤波器(A1)之后, 但 A1 子滤波器生成与 A2 打标签都硬依赖 secondary_path.npy(Pre_training_broadband_and_decompose.py:411-414 / label_real_noise.py:94-95), 从头训练者必须先测次级路径才能训练, 次序反了
- **造成影响**:
  - 行为: 纯文档修订, 无代码/运行时行为变化
  - 配置: 无
  - 测试/回归: 不涉及
  - 性能/内存: 不涉及
  - 未验证项: 无
- **验证方式**: grep 确认无旧阶段引用残留(阶段A/B/C/D/C1/C2/C3/D1/D2/A5 全部清零); 新阶段依赖链与代码核对一致(阶段1 Ŝ → 阶段2-①子滤波器/2-②打标签/2-⑥导出, 阶段3-②环路延迟/3-③反馈路径为纯部署项)
- **回退方式**: git revert 本提交

### [2026-08-12] README 明确次级路径 Ŝ = 训练/运行共用输入 — 重训场景 C1 先于阶段 A
- **状态**: 工作区未提交
- **基线**: abd6617
- **变更代码**:
  - 修改: `README.md` — ① 完整命令流阶段 A 新增 ⚠️ 说明: A1 子滤波器 / A2 打标签 / A5 导出都读 `GFANC_Scene/Primary and Secondary Path/secondary_path.npy`(仓库自带), 针对自家环境重训须先做阶段 C1 现场测次级路径再回阶段 A; ② A1 注释补输入(secondary_path.npy, MIMO FxNLMS 训练必需), A2 注释补输入(primary/secondary_path.npy + 子滤波器基); ③ C1 注释补 ⚠️ 覆盖同一份 .npy(=训练输入), 重训先做本步; ④ 快速开始"要不要训练"句改为"阶段 A 以 secondary_path.npy 为输入, 换硬件不用重训"
- **变更原因**: 用户对抗性质疑"生成子滤波器/次级滤波器怎么在系统运行阶段? 不然怎么训练子滤波器、打标签?"——核实代码依赖链 (Pre_training_broadband_and_decompose.py:365-414 读 secondary_path.npy; label_real_noise.py:94-95 读 primary/secondary .npy; export_bin.py:46-47 同槽位; measure_secondary.py:41+318 覆盖写同一 .npy) 确认次级路径 Ŝ 同时是训练输入与运行测量项; README 原 A→C 编排未说明"重训要先测路径"
- **造成影响**:
  - 行为: 纯文档修订, 无代码/运行时行为变化
  - 配置: 无
  - 测试/回归: 不涉及
  - 性能/内存: 不涉及
  - 未验证项: 无
- **验证方式**: 逐文件核对依赖链 (Pre_training_broadband_and_decompose.py / label_real_noise.py / export_bin.py / measure_secondary.py 均指向同一 secondary_path.npy 槽位); 文档新增说明与代码实际读取路径一致
- **回退方式**: git revert 本提交

### [2026-08-12] README 命令去重 — 完整命令流=唯一命令源, 快速开始/换硬件检查单改文字引用
- **状态**: 工作区未提交
- **基线**: 36f9fc6
- **变更代码**:
  - 修改: `README.md` — ① 完整命令流补全: 总览表加"🎯 准备 0"行 + 新增准备块(下载项目+装 Python 依赖); 系统运行块补 D1b(编译离线评估 main.exe)/D2b(运行 main.exe 处理 WAV); C2 补注释(该工具顺带测 NLMS 版 Ŝ 默认不用, 需 GFANC_SEC_FILE 指定); A5 补注释(默认找同级 GFANC_Scene, 否则 set GFANC_PYTHON_PROJ); ② 去重: 快速开始五个命令块(git clone/导出权重/编译/校准/运行)删除, 改为决策表+要不要训练+核心概念+校规+批次指纹文字, 指向完整命令流; 换硬件检查单"详细命令"块删除, 表格命令列改阶段引用(B / C1-C3 / D1 / D2); 次级路径测量章节命令块精简为文字引用 + 参数选项(--interactive/--duration/--repetitions/--amplitude)文字化; ③ 修失效锚点: 反馈路径校准章节 `[快速开始-步骤4](#4-校准反馈路径…)` → 纯文本引用完整命令流 C3; 系统参数表 OCG 行 `[§5 运行](#5-运行)` → 引用快速开始"想试场景切换"框; ④ 删除"关键操作要点"后残留的重复运行句
- **变更原因**: 用户指出"完整命令流"与快速开始、换硬件检查单的命令重复, 选择"完整命令流=唯一命令源"——命令只保留一份, 其余章节改文字引用, 避免同一命令散落三处、改一处漏两处
- **造成影响**:
  - 行为: 纯文档修订, 无代码/运行时行为变化
  - 配置: 无
  - 测试/回归: 不涉及
  - 性能/内存: 不涉及
  - 未验证项: 无
- **验证方式**: grep 全库确认 gcc -O2 / calibrate_feedback.exe / gfanc_realtime.exe 仅完整命令流一处; 被删命令块内容逐条核对已并入完整命令流(阶段0 / D1b / D2b / GFANC_SEC_FILE / GFANC_PYTHON_PROJ)无信息丢失; 两处失效锚点(#4-校准反馈路径 / #5-运行)已改纯文本引用
- **回退方式**: git revert 本提交

### [2026-08-12] README 新增"完整命令流"章节 — 训练 → 实时运行 全链路按阶段 A-D 顺序编排
- **状态**: 工作区未提交
- **基线**: bc5b247
- **变更代码**:
  - 修改: `README.md` — ① 在训练管线前插入 `## 完整命令流（训练 → 实时运行）`: 🎯训练数据(阶段A: 子滤波器→标签→训练→导出) + 🎯系统运行(阶段B编译校准→C声学校准→D编译运行), 附必做性总览表; ② 训练管线章节的"命令顺序"代码块删除, 改为引用完整命令流阶段A(命令只保留一份, 独有细节注释并入完整命令流); ③ 快速开始校准表格旧"次级路径+环路延迟"一项拆为"次级路径Ŝ/环路延迟/反馈路径"三行
- **变更原因**: 用户反馈命令散落快速开始/换硬件检查单/训练管线三处, 无统一先后顺序, 也分不清哪是训练数据哪是系统运行; 命令重复三份易看乱
- **造成影响**:
  - 行为: 纯文档修订, 无代码/运行时行为变化; 命令全部核对与快速开始/检查单/运行时代码一致
  - 配置: 无
  - 测试/回归: 不涉及
  - 性能/内存: 不涉及
  - 未验证项: 无
- **验证方式**: 新增完整命令流 vs 快速开始(L101-114)/换硬件检查单(L187-215)/main_realtime.c 加载逻辑(L870/L988) 逐一核对命令与产物路径一致
- **回退方式**: git revert 本提交

### [2026-08-12] README 校准工具分工澄清: 扫频法=Ŝ 内容、calibrate_secondary=环路延迟、calibrate_feedback=反馈路径
- **状态**: 工作区未提交
- **基线**: c4c512e
- **变更代码**:
  - 修改: `README.md` — 修正快速上手/换硬件检查单/详细命令/扫频法章节中"calibrate_secondary 是主测量、扫频法是可选替代"的错误定位: 运行时默认加载 `secondary_path.bin`(扫频法, [main_realtime.c:870](/main_realtime.c#L870)), 环路延迟 `sec_bulk_delay.bin` 只由 calibrate_secondary 测([main_realtime.c:988](/main_realtime.c#L988)); 三步校准命令重排 (②Ŝ→③环路延迟→④反馈), 文件表补充"顺带 NLMS 版 Ŝ 默认不用"
- **变更原因**: 用户追问三个校准工具分工时发现 README 描述与运行时实际加载逻辑矛盾——详细命令把 calibrate_secondary 标为主测量, 但运行时默认加载的是扫频法产物; 照 README 只跑 calibrate_secondary 会导致 Ŝ 内容不生效
- **造成影响**:
  - 行为: 纯文档修订, 无代码/运行时行为变化
  - 配置: 无
  - 测试/回归: 不涉及
  - 性能/内存: 不涉及
  - 未验证项: 无
- **验证方式**: 对照 [main_realtime.c:868-870](/main_realtime.c#L868-L870)(默认 Ŝ 文件) 与 [main_realtime.c:986-1006](/main_realtime.c#L986-L1006)(环路延迟来源) 逐条核对 README 描述
- **回退方式**: git revert 本提交

### [2026-08-12] R-18 离线抗混叠升级 biquad + R-50 反馈标定峰位 sanity 门禁 + 审查报告按"待办/归档"重排
- **状态**: 已提交 6c72e4b
- **基线**: 34f94a6
- **变更代码**:
  - 修改: `main.c` — `resample_mono` 下采样抗混叠从"2 样本移动平均"升级为 2 阶 Butterworth biquad（与实时版 `main_realtime.c` R-14 逐字一致; 新增 `biquad_t`/`biquad_init_lpf`/`biquad_tick`, fc=0.40625×sr_out≈6.5k@16k 输出）
  - 修改: `src/calibrate_feedback.c` — R-50 峰位物理 sanity 门禁: `FB_MAX_PEAK_MS=11.0f`（spk→ref 反馈环路上限, 依据: 全 ANC 环路实测 12.4ms, spk→ref 为其子集必更短; calibrate_secondary 聚类法实测 4.9-7.9ms）; 峰位超限打 ⚠ WARN（**不拒收**, 运行时已加载同类文件, 硬拒会清空反馈抵消）; 输出打印加 PNR
  - 修改: `docs/GFANC_综合审查报告_合并版.md` — ① 按"待办/归档"重排: 设计权衡（原三）移到行动路线图后、归档区开始, 物理层四→三、路线图五→四; ② R-18/R-50 状态更新
  - 删除: `src/calibrate_secondary.c` — 移除 c==0 参考麦反馈路径辨识 + `feedback_path_s*.bin` 死输出（运行时只加载 `feedback_path_{0,1}.bin`=calibrate_feedback 产物, 该文件从未被读; NLMS 循环改从 c=1 起只测误差麦次级路径）
- **变更原因**: R-18"简易 2 点移动平均"抗混叠截止仅 ~fs/4、折叠镜像压制不足, 且与实时抗混叠链（R-14 biquad fc=6.5k）不一致 → 离线 NR 不能完全反映实时行为。R-50"spk0 峰@tap224=14ms 疑似噪声伪峰"缺物理 sanity 检查。
- **造成影响**:
  - 行为: 离线 main.exe 44.1k/48k→16k 重采样抗混叠从 2 点平均改 biquad（与实时版同款）; calibrate_feedback.exe 峰位 >11ms 打印 ⚠ 提示、并打印 PNR; calibrate_secondary.exe 不再测/存反馈路径（输出文件集变为只含 secondary_path + sec_bulk_delay）
  - 配置: 无新增 env
  - 测试/回归: 离线 road_noise-15（44.1kHz→16k, 实际走新 biquad 路径）平均 NR_true=9.3dB 与基线一致; main.exe / calibrate_feedback.exe 编译零新警告（-Wall 仅既有 unused 警告）
  - 性能/内存: 离线重采样每样本多 ~5 MAC, 一次性处理, 可忽略; calibrate_feedback 仅标定工具, 无运行时影响
  - 未验证项: R-50 WARN 门禁需实机重跑校准才能观测（现有 feedback_path_0/1.bin 峰位 13.6/14.4ms 会触发新 ⚠ — 预期行为, 提示流对齐残留）
- **验证方式**: 离线 NR 回归（road_noise-15 = 9.3dB 基线一致）; -Wall 零新警告; 对现有反馈 FIR 峰位/PNR 数据分析确认门限合理
- **回退方式**: git revert 本提交

### [2026-08-12] R-27 批次指纹落地（防 cnn/sub_filters/bandpass 跨批混配）+ 审查报告第七节补记 v1.6 DW 架构切换
- **状态**: 已提交 34f94a6
- **基线**: 3248221
- **变更代码**:
  - 新增: `src/binary_loader.c` — `bin_crc32_chain`（链式 crc32, 匹配 Python `zlib.crc32(data, prev)` 续算语义）+ `bin_batch_crc()`（对排序后 `data/cnn_*.bin` + `sub_filters.bin` + `bandpass_fir.bin` + `bandpass_anc.bin` 原始字节折入链式 crc, 用 `_findfirst`/`qsort` 枚举排序）+ `bin_check_batch()`（读 `data/batch_id.bin` hex 比对, 不一致打 `[WARN] 批次混配检测`, 缺文件跳过）
  - 修改: `include/binary_loader.h` — 新增 `bin_batch_crc`/`bin_check_batch` 原型 + 说明
  - 修改: `main.c` / `main_realtime.c` — CNN 加载成功后调 `bin_check_batch()`（WARN 不阻断）
  - 修改: `export/export_bin.py` — 新增第 5c 段: 对批内文件算链式 crc32 → 写 `data/batch_id.bin`（hex）+ `data/batch_info.json`（溯源清单, 含 batch_id + 文件列表）
  - 修改: `docs/GFANC_综合审查报告_合并版.md` — R-27 状态更新为"批次指纹已做, manifest/sha256 留 Phase-3"; 第七节补记 2026-08-06~08-08 架构切换缺口（BUG-8 Ŝ 选择定案 / v1.6 DW / OCG v1.7）+ 2026-08-12 批次指纹记录; ADV-B5 / CNN 置信度条目加"已被 v1.6 移除"标注
- **变更原因**: R-27"版本混配无防线"剩最后缺口。DW 架构（v1.6）下 `Wc = Σ gains(CNN 30维) × sub_filters`，CNN 与 sub_filters 必须同源；单文件 v2 头 crc32 只能防单文件损坏，防不了"CNN 重导但 sub_filters 还是旧批"的**跨文件静默混配**（K=30/L=1024/单文件 CRC 全合法, 现有 R-3/R-4 检查全过）。声学路径（secondary/primary/feedback）不入指纹——它们是安装态可替换的测量值, 换 Ŝ 属 R-16-①/BUG-8 设计行为, 入指纹会误报。
- **造成影响**:
  - 行为: 启动加载权重后多打印一行 `[batch] 批次指纹一致 0x……`（默认）或 `[WARN] 批次混配检测……`（混配时, 仅警告不阻断, 与"损坏数据好过拒绝启动"哲学一致）; 旧 data/ 无 `batch_id.bin` 时打印跳过提示（向后兼容）
  - 配置: 无新增 env（指纹由 export_bin.py 每次导出自动更新）
  - 测试/回归: 离线 `main.exe` 指纹一致 `0x94b9b20c`（C 重算 == Python 生成）; 篡改 batch_id → `[WARN] 批次混配检测` 且 exit=0; 删 batch_id.bin → 跳过提示; road_noise-15 NR_true 9.3dB 无回归; 三目标零警告编译
  - 性能/内存: 启动时多读 ~59 个 cnn 文件一次（合计 ~数百 KB, 毫秒级, 一次性）
  - 未验证项: 真实"跨批混配"（整文件替换为另一批 crc 合法文件）未做端到端实测——已用篡改 batch_id 等价覆盖比对逻辑; 实时版未实机跑（批次校验代码路径与离线共享, 编译通过）
- **验证方式**: 见上"测试/回归"; C 端链式 crc 与 Python `zlib.crc32` 语义对齐已用真实 data/ 逐位比对验证
- **回退方式**: 删除 `data/batch_id.bin` 即恢复旧行为（校验自动跳过）; 或 git revert 本提交

### [2026-08-11] P0-5 环境安静检测（治"噪声消失后反相声残留/嗡嗡声" + 宽带弱噪声误杀守卫 + leak 连续化治滋滋）v1.9
- **状态**: 已提交 <此提交>
- **基线**: 2ad4b69
- **变更代码**:
  - 修改: `include/gfanc_types.h` — P0-5 新增安静检测参数组（`quiet_anti_rms=0.02` / `quiet_ref_max=0.045` / `quiet_hold=3` / `quiet_exit=1.5` / `quiet_err_exit=2.0` / `quiet_ref_memory=20`; `quiet_nr_db`/`quiet_err_max` 标记弃用, 仅 env 兼容）+ env `GFANC_QUIET_*` 加载
  - 修改: `main_realtime.c` — 环境安静状态机（进入=anti>0.02 && ref<0.045 && quiet_since_active≤20 持续 3s → 冻结梯度 + 逐样本衰减 Wc 至静音; 退出=ref 重回 1.5× 或 err 重回 2.0× 安静基准 → 重建 INIT）; **quiet_since_active 哨兵守卫**（启动初始化 0x7fffffff, 不算"刚有大噪声"; ref>门槛清零, 否则累计——弱噪声从启动就在则永不触发安静）; leak 离散分档改连续映射 + `leak_ema` 慢 EMA
  - 新增: `Noise Examples/tone250_30s.wav` / `tone1000_30s.wav` / `tone500_30s.wav` / `exam_tone_road_tone_60s.wav` / `test_ocg_250_1000_250_45s.wav` 等测试纯音夹具（250Hz-only 隔离滋滋声 + 弱噪声复现用）; 根目录旧 tone wav 移入 `Noise Examples/`
- **变更原因**: 治实机两个听感问题。① **噪声停后扬声器继续输出残余反相声**（嗡嗡 5-7s 不消）: 安静检测判据三阶段演进——首版 anti>0.05&&NR<6 够不着残余（0.03）; 阶段③ 实测证伪 NR 门（噪声停后 NR 保持 8-12dB 不塌, 反相声学上仍在抵消底噪）→ 改 **ref 塌底判据**; 阶段⑤ 暴露**宽带弱噪声误杀**（马路噪音 ref≈0.038 < 绝对门槛 0.045, 被误判"噪声消失" → 反相砍到 0 → 30s 全程 0dB, 根因=绝对阈值照 250Hz ref=0.048 标定, 同响度宽带 ref 更低）→ 加哨兵守卫。② **运行期滋滋声**: leak 离散分档（1/2/5/10×）每秒硬跳 → anti 1Hz 泵动, 改连续映射 + EMA
- **造成影响**:
  - 行为: 默认配置下噪声消失 → ~3s 后 `[QUIET]` 冻结梯度 + 衰减 Wc → 1-2s 内静音; 噪声回归 → `[QUIET] exit` + 重建。宽带弱噪声（ref<0.045）不再被误判为"噪声消失", 反相持续生长
  - 配置: 新增 `GFANC_QUIET_ANTI`(0.02) / `GFANC_QUIET_REF`(0.045) / `GFANC_QUIET_HOLD`(3) / `GFANC_QUIET_EXIT`(1.5) / `GFANC_QUIET_ERR_EXIT`(2.0) / `GFANC_QUIET_MEMORY`(20)
  - 测试/回归: 离线 NR_true 9.8/9.6/9.3 **无回归**（改动只加门, 不动自适应路径）; 实机 [QUIET] 触发→衰减→exit→重建全通
  - 性能/内存: 无（安静检测为 1Hz 标量运算, 无新增线程/锁）
  - 未验证项: 阶段⑤ 哨兵守卫版实机复测待跑（弱噪声不再被误杀 / 噪声停→[QUIET]+静音 / 回归→exit+重建）; 滋滋声是否根除待 250Hz-only 测试隔离（可能含扬声器硬件成分）
- **验证方式**: 实机跑马路噪音（ref<0.045）确认反相持续生长不再被砍; 放 250Hz（ref>0.048）→ 停 → `[QUIET] 噪声消失` + 静音 → 再放 → `[QUIET] exit` + 重建; 离线三文件回归
- **回退方式**: `GFANC_QUIET_MEMORY=9999` 关闭哨兵守卫（恢复阶段③ 行为）; 或 git revert 本提交

### [2026-08-10] 论文改进逐项落地 v1.8: τ解耦 + 自适应增益平滑 + OCG 定案关闭 + 发散救援三重门控 + fade 清理 + LayerCAM 诊断
- **状态**: 已提交 <此提交>
- **基线**: bdf7639（fix: 实机验证闭环 — reset 闸门 0.6 + OCG 默认关 + safety_mute 判据修正）
- **变更代码**:
  - 新增: `tools/layercam_diagnose.py` — P2 LayerCAM 离线诊断（从 `data/*.bin` 载权复现运行时 CNN 前向, 频率遮挡归因为主判据）; `tone250/1000/250to1000/250to500.wav` — 测试纯音夹具（P2 验证 + 复现用）
  - 修改: `include/gfanc_types.h` — P0-1 新增 `ocg_tau`（簇半径独立于 switch_threshold, 默认 0.8）+ env `GFANC_OCG_TAU`; P0-2 新增 `gain_smooth_beta/switch`（默认 0.5/0.85）+ env `GFANC_GAIN_SMOOTH`; P0-4 新增 `diverge_err_ratio`（默认 0.6）+ env `GFANC_DIVERGE_ERR_RATIO`; 默认参数注释同步 OCG 定案关闭结论
  - 修改: `include/scene_controller.h`/`src/scene_controller.c` — P0-2 自适应增益 EMA 插入 `scene_ctrl_process`（帧间 cos<switch→β=1 立即跟随, 否则 β=0.5 慢速平滑）; 新增 `scene_ctrl_set_gain_smoothing` 接口
  - 修改: `main_realtime.c` — P0-4 发散救援三重门控（anti 超限 且 err/ref>diverge_err_ratio 且 err_ref 逐秒上升 0.1, 连续 2s 才回滚）; P1-1 CrossFader 末帧 memcpy 移除（自然结束, 冻结期无 LMS 状态可救）; apply_reset 增 `by_ocg` 诚实标注触发源; 模式派发注释定案 OCG 关闭结论
  - 修改: `main.c`/`include/ocg.h` — 离线与实时一致: ocg_init 传独立 `ocg_tau` + 增益平滑参数; 修正过期 τ 复用注释
- **变更原因**: 论文改进逐项应用（目标=开窗降噪, 终极=稳定性降噪）。① OCG τ 复用 switch_threshold 方向耦合（降阈值治 cos 闸门却让 OCG 更敏感）; ② CNN 增益逐秒抖动（纯音 bands 2/6/14/17/19 跨秒翻转）未治, OCG/cos 闸门受害; ③ OCG 重开实机证伪需定案关闭; ④ diverge 救援纯 anti 阈值误杀健康深对消（本硬件 err 麦比 ref 热, 收敛中 err_ref 可达 1.3）; ⑤ fade 末 memcpy 硬覆盖丢 LMS 状态语义隐患; ⑥ 需要 CNN 决策归因诊断工具
- **造成影响**:
  - 行为: 默认配置下发散救援不再误杀健康深对消（实测 anti>0.25 持续 9s 零救援、26dB 深对消保持、零 RESET, 修复了 P0-3 的锯齿震荡）; 增益平滑吸收纯音带抖动、真场景切换（帧间 cos<0.85）β=1 无延迟; OCG 保持默认关闭（验证稳定配置 = OCG 关 + THRESH 0.6 + 平滑 + 三重门控, 代码保留待簇判据更鲁棒后评估）
  - 配置: 新增环境变量 `GFANC_OCG_TAU`(0.8)、`GFANC_GAIN_SMOOTH`(0.5, 1=关平滑)、`GFANC_DIVERGE_ERR_RATIO`(0.6)
  - 测试/回归: 实机 250Hz 深对消 26dB 零 RESET（P0-4 三重门控生效）; 旧exe 日志 A/B 证明 OCG 簇震荡独立于救援误杀（OCG 开≈18dB 每~1000cb RESET vs 关 27.5dB 零 RESET）; P2 频率遮挡归因 250Hz→233Hz / 1000Hz→984Hz 子带, 干净纯音下增益 std/mean≈0.00 → 实时抖动根因在实况信号非 CNN 固有
  - 性能/内存: 无（平滑与三重门控均为每帧 O(SC) 标量运算, 无新增线程/锁）
  - 未验证项: P1-1 实机回归（改动无功能影响, 预期零 RESET, 待用户硬件复跑确认）; 平滑在"半抖动半切换"中间态（cos≈0.85 附近）可能延迟一次切换 <1s（已接受）
- **验证方式**: 实机纯音/路噪双场景日志对比（RESET 次数 / err 锯齿 / 深对消保持）; `python tools/layercam_diagnose.py tone250.wav` 归因验证; 离线 main.exe 与实时同构回归
- **回退方式**: OCG 重开 `GFANC_OCG=1`; 关平滑 `GFANC_GAIN_SMOOTH=1`; 救援还原纯 anti 门 `GFANC_DIVERGE_ERR_RATIO=0`; 或 git revert 本提交

### [2026-08-10] 实机验证闭环: reset 闸门灵敏度 0.8→0.6 + OCG 默认关 + safety_mute 判据修正 + 重校准数据入库
- **状态**: 已提交 <此提交>
- **基线**: bf52a2e
- **变更代码**:
  - 修改: `include/gfanc_types.h` — `switch_threshold` 默认 0.8→**0.6**；`ocg_enable` 默认 1→**0**（OCG 需 `GFANC_OCG=1` 显式开启）
  - 修改: `main_realtime.c` — `safety_mute` 判据 err_rms>2×ref 改 **8×ref + anti_rms>0.05 门**（防 rig 结构性误杀）
  - 修改: `data/secondary_path_measured.bin`、`data/sec_bulk_delay.bin`、`data/feedback_path_s0/s1.bin` — v4 校准程序重测入库
  - 修改: `docs/GFANC_综合审查报告_合并版.md` — 补离线 NR 天花板=次级路径低频滚降的分析与路线图项
- **变更原因**: 实机纯音/路噪验证中, 默认 reset 闸门(0.80)在深对消时误杀健康 Wc(cos 自然跌破 0.8) → 每 ~1000cb 震荡循环; OCG 聚类在 cos 0.99-1.00 也抖动触发, 且 τ 复用 switch_threshold, 降 0.6 反而加剧 → 验证有效配置为 **OCG 关 + THRESH 0.6**。safety_mute 旧 2× 判据对 err/ref≈7-9 的 rig 结构性误杀(anti 仅 0.01 时无反馈可护, err 跳变全为路噪)。
- **造成影响**:
  - 行为: 默认配置下 reset 仅在 cos<0.6 触发(场景真正切换), 深对消不再被误打断; safety_mute 仅在 err>8×ref 且 anti>0.05 时冻结(anti 极小时物理不可能啸叫)
  - 配置: `GFANC_RESET_THRESH` 默认语义 0.8→0.6；`GFANC_OCG` 默认关
  - 测试/回归: 实机 250Hz/500Hz 纯音深对消 err 0.10→0.015-0.018 (~17dB) 并保持 40s+ 零 RESET; 路噪稳态 err/ref≈0.30 (~10dB) 持续 2 分钟零发散; 新默认=验证有效配置
  - 性能/内存: 无
  - 未验证项: 500Hz 收敛较慢(CNN 增益与 LMS 最优对齐差, 属架构特性非 bug); OCG 关闭后场景切换场景(scene change)回归未测
- **验证方式**: 实机 GFANC_SEC_FILE=secondary_path_measured.bin + 新默认(无 RESET_THRESH/OCG 环境变量), 250Hz/500Hz 纯音 + 路噪 WAV 各跑 ~90s, 检查 err/anti/RESET/NOTCH
- **回退方式**: `GFANC_RESET_THRESH=0.8` + `GFANC_OCG=1` 环境变量还原旧行为; 或 git revert 本提交

- **状态**: 已提交 97b3eb8（feat: 两阶段训练后验证 — Wc-only NR 对齐 C 端 RMS 标定, v2 达成 5-8dB）
- **基线**: e389483（feat: verify_discrimination.py 加 Wc-only NR 实测）
- **变更代码**:
  - 修改: `training/network/verify_discrimination.py` — Wc-only NR 对齐 C 运行时 RMS 标定（`scene_ctrl_construct_wc` 的 `Wc *= stub_rms/wc_rms`）。此前测的是未标定裸 Wc，低估约 3.3 dB，造成"CNN Wc 比全 1 滤波器差"的误判
  - 新增: `models/MIMO_M5_DirectWeight_Real_v2.pth` — 两阶段训练产物（部署候选，未部署）；`models/MIMO_M5_DirectWeight_Pretrain.pth` — 合成预训练中间检查点
- **变更原因**: Checkpoint 3 目标（Wc-only 5-8 dB + 判别力≥70%）训练后验证。两个关键事实：① "Wc-only 只有 2.5-4.1 dB"是度量伪影——C 端每秒做 RMS 标定，Python 侧此前未对齐；② 对齐后**两模型都达成目标，且输出几乎相同**
- **造成影响**:
  - 行为: 无运行时行为变化（verify 脚本度量口径修正；部署模型 `MIMO_M5_DirectWeight_Real.pth` 未更换，仍与 baseline 一致）
  - 测试/回归（全文件逐窗 7 文件 148 窗）:
    - v2 标定后 Wc-only NR 全体 +6.54±1.04 dB（首窗 5.0-7.9）→ **Checkpoint 3 目标 5-8 达成**
    - base 标定后同样 +6.54±1.07 dB（首窗 4.9-8.0）；逐窗配对差 v2−base = **−0.007±0.160 dB → 训练对 Wc-only 降噪提升≈0**
    - 增益 cos(base,v2)=0.997（148 窗），每带幅值差≤0.02 → 两模型输出几乎相同
    - cos(base,label)=cos(v2,label)=0.86-0.99 → 两模型增益方向都近 LMS 最优；标签本身跨类型近共线 → 这批基准文件增益空间近似一维，CNN 学到的都是同一平均方向
    - 判别力 37.2→42.6%（排除 mixed 52.2→64.1%），但两模型增益空间对类型都不可分（类型内≈类型间 cos 0.98）→ 判别力提升是近共线空间的小角度偏移，弱信号，非"输入失聪修复"
  - 性能/内存: 无
  - 未验证项: ① v2 部署（copy→export_bin→rebuild→离线全系统回归）未做，需用户确认；② 合成数据对未见过噪声类型的鲁棒性收益（判别力提升的实际价值场景）未硬件验证
- **验证方式**: `diag_fulllen_nr.py` 全文件逐窗配对对比（148 窗，差≈0）；`verify_discrimination.py` 判别力（base 复现 37.2%）；`diag_both.py` cos 到标签（两模型均 0.86-0.99）
- **回退方式**: verify 脚本 `git checkout`；v2/Pretrain 模型删除即无影响（部署模型未动）

### [2026-08-09] 合成数据生成+打标签管线 & 两阶段训练（合成预训练→真实微调）— 治 CNN 输入失聪

- **状态**: 工作区未提交
- **基线**: 5266e9f（fix: 实时版同步梯度相位修复 R-58-11）
- **变更代码**:
  - 新增: `GFANC_Scene/training/labeling/make_synthetic_dataset.py` — 合成数据生成+打标签入口：4 族信号（窄带 0.40 / 宽带 0.30 / 1-f^α 倾斜 0.15 / 谐波 0.15）覆盖 20-1500Hz 全子带谱形，走 LMS 标成 `gain_*`；CLI `--gen-only`/`--label-only`/`--probe`，默认 60000/7500/7500
  - 新增: `GFANC_Scene/training/network/Train_validate_synth.py` — 两阶段训练：合成预训练 40ep（LR 0.01）→ 真实微调 25ep（LR 0.003）；复用 m5_scene+带通+minmax+MSE+Adam+StepLR；输出仍写 `models/MIMO_M5_DirectWeight_Real.pth`（export 自动加载，部署路径不变）。**关键修复 `cos_max=-inf`**：回归训练首轮 valid_cos 可能为负，从 0 起则永不如 0 → 一个检查点都不存 → 重载崩溃（冒烟测试捕获）
  - 重写: `GFANC_Scene/training/network/verify_discrimination.py` — Checkpoint 3 判别力验证，复现原始最近均值协议（7 基准录音 → 148 逐秒窗口 → 6 类；指标 = 最近均值分类准确率 + 类型内/间 cos 间隙），附排除 mixed 的 5 类变体
  - 新增: `models/MIMO_M5_DirectWeight_Real_baseline_35pct.pth` — 训练前基线模型备份（判别力对照用）
  - 修改: `README.md` — 数据与标签/命令顺序/说明补合成数据管线（2b 生成+打标签、步骤 3 两阶段训练、4b 判别力验证）；`docs/变更记录_CHANGELOG.md` — 本条记录
- **变更原因**: 2026-08-09 诊断确认 CNN「输入失聪」——输出增益判别力仅 35.8%，而输入谱可分 75.0%、真实标签 76.7%（带通后探针 gap=+0.036 证明输入含全部区分信息，瓶颈在模型）。根因：真实 4 类（道路/儿童/施工/铁路）全低频主导、谱形接近，从零在真实数据训练 → 输出坍缩到同一低频处方。合成数据用多样谱形逼 CNN 必须用输入，再低 LR 微调适配真实统计量、防坍缩回
- **造成影响**:
  - 行为: 训练产物路径不变，`export_bin.py` 自动加载 → 部署流程零改动。**当前 CNN 行为未变**（重训+重导出前仍是旧模型）
  - 配置: 无新增（全部 CLI 默认；`LMS_MU=0.001`、`FX_NOISE_DB=-30`、`LMS_REPET=3`、`BATCH_SIZE=128`）
  - 测试/回归: 训练脚本冒烟通过（1 epoch 预训练，cos_max bug 已修复）；判别力脚本基线复现（CNN 输出 37.2%≈35.8%、输入谱 gap +0.036 精确、真实标签 80%≈76.7%）
  - 性能/内存: 打标签 ~5.5h（60000×Repet=3，GPU）、训练 ~3.5h（40+25ep）、合成数据集磁盘 ~6GB（`D:\Dataset\Synthetic_Dataset`）
  - 未验证项: **Checkpoint 3 判别力（目标 ≥70%）待训练完成后验证**；重导出 .bin 后离线 NR 是否提升待实测；合成数据对实时降噪的实际增益待硬件验证
- **验证方式**: 打标签 `make_synthetic_dataset.py --label-only`；训练 `Train_validate_synth.py`；验证 `verify_discrimination.py --model models/MIMO_M5_DirectWeight_Real.pth`（基线对照传 `--model ..._baseline_35pct.pth` 应复现 37.2%）
- **回退方式**: 不重训/不重导出即无行为影响；删 `D:\Dataset\Synthetic_Dataset` 即移除数据；仍可走纯真实训练 `Train_validate.py`

### [2026-08-09] Band 日志改 TopBands — 诊断输出 top-3 子带占比替代 argmax 单值

- **状态**: 工作区未提交
- **基线**: 5266e9f（fix: 离线降噪发散根因修复 R-58-7/8/9 — 路径统一 + EMBED/步长默认 + 归一化分离）
- **变更代码**:
  - 新增: `include/scene_manager.h` — 纯函数 `sm_fmt_top_gains()`：计算 30 维直接权重增益中 |gain| 占比最高的 3 个子带，格式 `2(10%) 17(9%) 14(8%)`（占比 = |gain[i]|/Σ|gain[j]|，全 ~0 时 `-`）
  - 修改: `main.c` — 表格列 `Band`→`TopBands`（列宽 5→22），行打印用 `sm_fmt_top_gains(gains, K, ...)`；删除不再使用的 `new_scene` 局部变量
  - 修改: `main_realtime.c` — `print_diagnostics` 的 `s=%d max=%.2f`→`top=%s`；INIT 单次打印 `scene=%d max=%.2f`→`top=%s`；`new_scene` 参数保留但标记 `(void)`（CSV 机器日志仍直接用，未动）
  - 修改: `README.md` — 输出示例与列含义表同步 TopBands
- **变更原因**: 诊断列 argmax 单值信息量低 — CNN 输出层 bias[2]=+0.970 恒占优使 argmax 常钉死在低频带 2（用户疑问"Band 为什么一直是 2"），但真实信息在整套增益向量分布。top-3 占比同时看到"哪个带最重"和"各带如何分配"（road_noise 稳定 top=2；mixed 场景 top-1 翻到 14、出现 band 6，证明 CNN 输入自适应）
- **造成影响**:
  - 行为: 仅控制台/离线表格诊断输出格式变化（Band 单值 → TopBands 三值+占比）；ANC 处理逻辑与 Wc 构造**零改动**。实时版 CSV 机器日志列序/语义不变
  - 配置: 无
  - 测试/回归: 离线 road_noise_0-34 +9.6dB / mixed_7types_56s +9.8dB 与基线一致；两二进制零警告编译通过
  - 性能/内存: `sm_fmt_top_gains` 每行 O(K=30) 一次, 可忽略
  - 未验证项: 实时版输出需硬件上电肉眼确认
- **验证方式**: `gcc` 零警告编译 main.exe + gfanc_realtime.exe；离线跑 `Noise Examples/road_noise_0-34.wav` 与 `mixed_7types_56s.wav`，NR 与 R-58-10 基线一致
- **回退方式**: 恢复 `git checkout 5266e9f -- main.c main_realtime.c include/scene_manager.h README.md`

### [2026-08-09] 实时版同步梯度相位修复 R-58-11 — Fx 过 bp_anc（落地 R-58-10 未验证项①）

- **状态**: 已提交（2026-08-09, commit: fix: 实时版同步梯度相位修复 R-58-11 — Fx 过 bp_anc）
- **基线**: 6d303ad（fix: 离线时间衰减 + 一正一负双根因修复 R-58-10）
- **变更代码**:
  - 修改: `main_realtime.c` — 梯度 Fx 过 64tap bp_anc: 结构体新增 `bp_fx[E*S]`（**每条 (e,s) 路径独立 FIR**，与离线 R-58-10① 同构）; 初始化（`bp_err` 分配循环后，同样 `bp_anc_ok ? bp_anc_coeff : bp_coeff` 回退）; 主循环 `Fx_arr[e*S+s] = fir_tick(&ctx->bp_fx[e*S+s], Fx_arr[e*S+s])`; cleanup 补 `free(bp_fx[i].delay_line)` + NaN 恢复 `fir_reset(&ctx->bp_fx[i])`。err_meas 过 bp_err（64tap 群延迟 31.5 样本）而 Fx 不过 → 与离线 R-58-10① **同构的梯度相位失配**，实时被 cold_hold/adaptive-leak/safety_mute/howling 掩盖（无显性衰减但降噪被压在收敛上限之下）。修复后 `∂err_meas/∂Wc = bp(Ŝ⊗x)` 与 eg 逐样本对齐。**陷阱同离线**: 必须 E×S 独立 FIR，若共享则 s=1 的 tick 用 s=0 污染的延迟线 → 第二扬声器滤波参考被交叉污染 → Wc[1] 梯度错位
- **变更原因**: R-58-10 未验证项① 落地。实时版与离线同构的梯度相位失配根因此前仅被保护层掩盖
- **造成影响**:
  - 行为: 实时无离线仿真路径，需硬件重验证。预期: 梯度诚实后收敛上限 ↑、保护层对漂移的对抗减弱; 实时 step 旧标定（针对失配链路）可能需重调
  - 配置: 无新增 env。BP_ANC_LEN 仍 64tap（`main_realtime.c` 中 256tap 注释为过期描述，实际代码已用 64，见 R-13 定义）
  - 性能/内存: bp_fx 内存 E*S×64×8B ≈ 3KB; 逐样本多 E*S×64=384 次乘加，16k 单帧实时开销可忽略
  - 未验证项: ① 硬件实机验证（重测 SIG/CLIP 校准、Ŝ 重估、step 重调、啸叫检测阈值复核）; ② 训练侧无 bp_anc 级差异仍待训练管线重训时评估（R-58-10 未验证项②）
- **验证方式**: 编译零新增告警（gcc -Wall 无 bp_fx 相关告警）。需硬件: 重测校准后实机 A/B，对比修复前后实时 NR 轨迹与啸叫/发散事件数
- **回退方式**: `git checkout main_realtime.c` 恢复 R-58-10 状态（或手动移除 bp_fx 段）

### [2026-08-09] 离线时间衰减 + 正负口径双根因修复 R-58-10 — 梯度相位对齐 + 去双 G

- **状态**: 已提交（2026-08-09, commit 6d303ad: fix: 离线时间衰减 + 一正一负双根因修复 R-58-10 — 梯度相位对齐 + 去双 G）
- **基线**: 0aeeeb8（fix: 离线降噪发散根因修复 R-58-7/8/9）
- **变更代码**:
  - 修改: `main.c` — R-58-10① 梯度 Fx 过 64tap 带通: 新增 `bp_fx[E*S]`（**每条 (e,s) 路径独立 FIR**），逐样本 `Fx_arr[e*S+s] = fir_tick(&bp_fx[e*S+s], ...)`。err_meas = bp_err(es) 带 31.5 样本群延迟而 Fx = Ŝ⊗ref_anc 不过 bp → 梯度与误差错位 → FxLMS 临界稳定 → Wc 相位慢漂移 → 降噪随时间衰减。修复后 `∂err_meas/∂Wc = bp(Ŝ⊗x)` 与 eg 逐样本对齐。**陷阱**: 若每 e 只建一个 FIR 共用两条扬声器路径，s=1 的 tick 用 s=0 污染的延迟线 → 第二扬声器滤波参考被交叉污染 → Wc[1] 梯度错位 → 慢漂移仍在（该残留曾使 road_0-34 从 8.4 衰减到 4.4）
  - 修改: `main.c` — R-58-10② es 不再二次乘 G: `es = pri_raw + anti_at_mic`（原 `×(pri_raw+anti_at_mic)*cfg.mic_pre_gain`）。pri_raw/anti_at_mic 已含 G（经 ref_anc=bp(G·x)）→ 原 es∝G²: 有效步长∝G + NR_true 口径 ±20log10(G) 伪影。G=2.72 road-15 → tanh 饱和(87%)梯度死亡 → 负 NR；G=0.27 road_0-34 → +11.4dB 虚高。修复后步长/指标与 G 无关
  - 修改: `main.c` — R-58-10③ 离线默认 step 0.0005→0.005。R-58-8 的 0.0005 标定在旧链路（es∝G² 饱和 + 梯度错位）下得出，修复后重新扫描（三文件）: 0.00005→7.9 / 0.0001→8.4 / 0.0005→9.2 / 0.001→9.5 / 0.002→9.6 / 0.005→9.8 / 0.01→9.5（mixed），平台区 0.001-0.005，默认取 0.005（三文件均稳定最优，裕量 2×）
  - 删除: `main.c` — `[DBG-a4f2]` 发散机理诊断插桩（累加器/逐秒 stderr 打印/清零）、`GFANC_FX_BP_ERR`/`GFANC_ES_NO_DOUBLE_G` getenv 测试开关
- **变更原因**: 用户报告离线降噪"越运行越差"且两文件"一正一负"。实验定位: ①梯度相位失配（Fx 未过 bp_err，与错误路径差 31.5 样本）→ 时间衰减；②es 双 G → 文件间符号相反 + 指标伪影；③bp_fx 共享 FIR → 残留慢漂移。µ=0 固定 Wc 同数据开环 +7.1dB → 数据/Wc 初值健康，问题全在自适应链路代码
- **造成影响**:
  - 行为: road_noise-15: −8.4 → **+8.4dB 稳定**；road_noise_0-34: +15.2（伪影虚高, 真实~3.8 衰减）→ **+9.0dB 稳定**（8.7→10.5 缓慢改善, 33/34s 回落为输入静默+突发瞬态）；mixed_7types_56s: +16.3（伪影虚高）→ **+9.2dB 稳定**。三个文件自适应均超过 µ=0 开环（road_0-34: 9.0 vs 7.1）→ 梯度真正收敛
  - 配置: 无新增 env。`GFANC_STEP`/`GFANC_LEAK`/`GFANC_MIC_GAIN` 覆盖语义不变（R-58-10② 后步长与 G 无关，GFANC_MIC_GAIN=1 不再需要）
  - 测试/回归: 三场景默认配置跑通且无单调衰减；µ=0 对照确认输入时变特性（混合文件逐秒 8.1-10.9 波动）≠ 系统漂移；leak=0 对照排除泄漏
  - 性能/内存: bp_fx 内存 E*S×64×8B ≈ 3KB；逐样本多 E*S×64=384 次乘加，离线可忽略
  - 未验证项: ① 实时版 main_realtime.c 存在同构梯度相位失配（bp_err 在误差路径、Fx 未过 bp）但被 cold_hold/adaptive-leak/safety_mute 掩盖，未同步修改（硬件标定工作点需重验证）；② 训练侧无 bp_anc 级（Disturbance_generation 直接把原始噪声送 Pri/Sec，C 端先 64tap bp）——次要差异，不影响本次结论，待训练管线重训时评估
- **验证方式**: ① 时间衰减: road_0-34 默认(衰减到 12) vs 修复后(9.0, 轨迹无单调下降)；② 正负符号: road-15 −8.4→+8.4；③ 残留漂移: 修复前 bp_fx 共享 FIR → road_0-34 8.4→4.4，修复后独立 FIR → 8.7→10.5；④ 数据健康: µ=0 同数据开环 +7.1
- **回退方式**: `git checkout main.c` 恢复 R-58-8 状态（或手动移除 bp_fx 段 + 恢复 es 乘 G）

### [2026-08-08] 离线降噪发散根因修复 R-58-7/8/9 — 路径统一 + EMBED/步长默认值 + 归一化模式分离

- **状态**: 已提交（2026-08-09, commit: fix: 离线降噪发散根因修复 R-58-7/8/9）
- **基线**: dfdf23a（feat: OCG 多质心聚类闸门 v1.7）
- **变更代码**:
  - 修改: `include/fxnlms_mimo.h`、`src/fxnlms_mimo.c` — R-58-9: `fxnlms_mimo_t` 新增 `sum_norm` 字段 + `fxnlms_set_norm()`。`fxnlms_tick_rt` 功率归一化按开关分支: sum=1 时 `power=ΣXd²+1e-6`、inv_pwr 无 cap（离线，与训练逐样本数学一致，R-58-5 收益保留）；sum=0 时恢复 mean+cap1000（实时硬件标定语义，R-48）。注意: main.c 离线仿真**走 fxnlms_tick_rt 路径**（非 fxnlms_tick，后者已无调用方），故必须由调用方显式置 1
  - 修改: `main.c` — R-58-9: `fxnlms_init` 后调用 `fxnlms_set_norm(&fx, 1)`（离线 sum 归一化）。`main_realtime.c` 不调用 → 实时保持默认 mean+cap，与旧硬件二进制数值语义一致（实测 12-15dB 工作点）
  - 修改: `export/export_bin.py` — R-58-7: 主路径导出裁剪 `Pri[:, :1, :]` → (E,1,L)。npy 实测 (3,2,1024) 且第二维是复制占位数据（3 误差通道完全相同，corr=1.0），训练端 `Disturbance_generation.py::_multi_channel_filter_pri` 写死用 `pri_path[:,0,:]`（第 0 参考），裁剪后 C 端 `e*PRI_LEN` 布局与训练语义逐样本一致
  - 修改: `data/primary_path.bin`、`data/secondary_path.bin` — 恢复为当前系统真实路径（git checkout 还原 HEAD 版本，与 `GFANC_Scene/Primary and Secondary Path/*.npy` corr=1.0）。此前被我误用 MIMO_GFANC npy 覆盖，用户明确要求只使用当前系统内路径
  - 修改: `include/gfanc_types.h` — R-58-8: `embed_delay_ms` 默认 3→0（原 3ms=48 样本 pad 进 Ŝ，训练世界无此延迟 → anti 相位错位 → 自适应正反馈发散；需评估嵌入式目标时 `GFANC_EMBED_DELAY_MS` 显式开启）
  - 修改: `main.c` — R-58-8: 离线默认 step 0.05→0.0005。训练 mu=0.05 在纯线性 float64 世界收敛，C 端链路含 `mic_pre_gain G×tanh×bp_err`，同 step 把 Wc 从 0.01 推到 0.8 → anti 过量正反馈（发散程度 ∝ G 已验证：road-15 G=2.72→-45dB，mixed G=0.21→-12dB）；实测 step 扫描 0.0005 最优（+16.3dB，µ=0→+10.4）
- **变更原因**: 用户报告离线降噪效果差（~1.8dB 甚至负值）且 road_noise-15 发散。逐层定位: ①路径不一致（C 端 bin 与训练 npy 不同源）→ 已统一；②3ms 嵌入延迟训练/运行时不一致 → 已归零；③step=0.05 在 C 端饱和链路过冲 → 已降 100 倍；④NR_true 口径含 G² 项制造假象（anti≈0 时 NR_true=-20·log10(G)：G=0.21→+13.6dB 假象、G=2.72→-8.7dB 假象）——本次未改口径，仅记录
- **造成影响**:
  - 行为: mixed_7types_56s: 1.8dB→**+16.3dB**；road_noise_0-34: **+15.2dB**；road_noise-15: 发散(-45)→-8.4dB（读数含 G=2.72 的 -8.7dB 口径偏差，G=1 时实测真实对消 +3.3dB）。µ=0 固定 Wc 时 mixed +10.4dB（auto-gain G=0.21 口径）确认 Wc 初值健康
  - 配置: `GFANC_EMBED_DELAY_MS` 默认 3→0（语义不变，默认行为变）；离线 `GFANC_STEP` 默认 0.05→0.0005
  - 测试/回归: 三场景默认配置跑通；µ=0/G=1/step 扫描/EMBED=0/RAW_ERR=1/2 共 ~20 组对照实验定位（见验证方式）
  - 性能/内存: 无变化
  - 未验证项: ① road-15 的 G=2.72 使 es 进 tanh 饱和区 → 梯度失效，离线仿真中 auto-gain 是模拟"实时工作点"，是否应在离线评估固定 G=1 待用户决策；② 训练管线 (Pre_training_broadband_and_decompose.py 在当前仓库) 需确认用当前系统路径重训的 sub_filters 与运行时 bin 同源（用户已更新 .mat，µ=0 对消验证通过）；③ 实时版 main_realtime.c 的 step/归一化同步检查
- **验证方式**: ① 路径: `np.corrcoef(bin_payload, npy.flatten())=1.0`；② 发散定位: µ=0 时 anti_mic≈pri（0.235 vs 0.265）量级精确匹配 → Wc 初值正确，发散来自自适应；③ EMBED=0 时 es 4.59→1.25（mixed, G=1）→ 延迟错位确认；④ step 扫描 0.05/0.005/0.0005/0.00005/µ=0 → -23.1/-8.4/+16.3/+11.1/+10.4；⑤ G=1 消除口径假象后 road-15 真实对消 +3.3dB
- **回退方式**: `GFANC_EMBED_DELAY_MS=3 GFANC_STEP=0.05` 环境变量恢复旧行为；`git checkout data/primary_path.bin data/secondary_path.bin`（恢复后即旧路径版本）

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
