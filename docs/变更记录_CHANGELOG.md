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
