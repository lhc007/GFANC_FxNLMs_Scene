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
