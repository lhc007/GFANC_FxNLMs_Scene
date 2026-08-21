# Phase-1 嵌入式验证 — 小白一步一步实现指南

> **写给谁**: 零嵌入式经验、想从 PC 原型走到能跑在 ARM 开发板上的人
> **目标**: 用最少的钱和焊接，按顺序验证三件事——
> ① **算力够不够**（RK3568 能不能在 62.5µs 内跑完 8×8 FxLMS）
> ② **算法在 ARM 上能收敛**（离线/实时 NR 和 PC 相当）
> ③ **低延迟能不能到**（进阶，涉及焊接）
> **配套规格**: [板级硬件定制需求_8S8E_闭环.md](板级硬件定制需求_8S8E_闭环.md)（最终定制板的目标）

---

## 0. 难度阶梯（先看这个再动手）

| 阶段 | 做什么 | 焊接 | 钱 | 时间 | 验证什么 | 谁适合 |
|---|---|---|---|---|---|---|
| **1-零** | 离线验证 + 算力微基准 | **无** | ¥0-800 | 1-3 天 | ① 算力 + 代码能否移植 | **小白从这里开始** |
| **1-A** | USB 声卡实时跑通 | **无** | ¥1.6k-2k | 1-2 周 | ② 算法在 ARM 收敛 | 小白进阶 |
| **1-B** | I2S 板低延迟链路 | **有** | ¥1k-3k + 示波器 | 2-6 周 | ③ 延迟 2-5ms | 有硬件基础者，**别一上来就做** |

> ⚠️ **最重要的一句话**: 你现在的代码是 Windows 专用（`InterlockedExchange` 等 Win32 API），**换嵌入式第一件事不是买硬件，是先把代码移植到 Linux/ARM**。这一步不花一分钱、不需要焊接，却是后面所有阶段的前提。所以从 **阶段 1-零** 开始。

---

## 1. 阶段 1-零: 离线验证 + 算力微基准（零焊接，1-3 天）

### 1.1 买什么（可选，但建议现在买）

| 件 | 型号建议 | 参考价 | 备注 |
|---|---|---|---|
| ARM 开发板 | **正点原子 RK3568** / Firefly RK3568 | ¥300-800 | 和最终定制板同平台（RK3568）。备选 Orange Pi 5（RK3588S，算力更强） |
| 供电 | 板子配套电源（看规格，多为 12V 或 5V/3A） | 随板 | |
| microSD | 32GB 以上 Class 10 | ¥30-60 | 烧系统用 |
| 键鼠/HDMI | 可有可无 | — | 会 SSH 就不需要 |

> 💡 **要不要现在买板子？——先别急。** 装个 WSL（Windows 自带 Linux），第 1.3、1.3b 步**全部免费**完成：Linux 编译 + QEMU 交叉编译验证 ARM 正确性 + 静态算力预算。**算力够不够 8×8 这个问题的答案，不买板子就能拿到。** 板子的唯一剩余用途是验证 NEON 向量化的**实际效率**（静态算不出那个 50-80% 的乘子）和真实 I/O 延迟——那属于阶段 1-A（接 USB 声卡）的活，到 1-A 再买，板子一到就干实事，不算白买。

### 1.2 给开发板烧系统

1. 官方镜像烧到 microSD（正点原子/Firefly 官网下载 Debian 镜像，用 balenaEtcher 写入）。
2. 插卡上电，SSH 登录（`ssh root@板子IP`，或接 HDMI 直接操作）。
3. 装工具链：
   ```bash
   sudo apt update && sudo apt install -y gcc make libc6-dev
   ```

### 1.3 编译离线版并跑基准（验证代码能移植）

离线版 `main.c` 只在代码里多带了一句 `#include <windows.h>`（第 18 行，实际只用 `clock()`，来自 `<time.h>`）——**Linux 上删掉这一行即可编译**。

1. 拷贝仓库到板子（U 盘 / `git clone` / scp）。
2. 改一行：删掉 [main.c:18](main.c#L18) 的 `#include <windows.h>`。
3. 编译（注意没有 `-lole32`，那是实时版才需要）：
   ```bash
   gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c \
       src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c \
       src/howling_detect.c src/ocg.c -lm -o main
   ```
4. 跑三个标准基准，和 PC 的结果对比：
   ```bash
   ./main "Noise Examples/road_noise_0-34.wav"
   ./main "Noise Examples/mixed_7types_56s.wav"   # 若文件在仓库里
   ./main "Noise Examples/road_noise-15.wav"
   ```

**判定标准**:
- 三文件 NR_true 和 PC 一致（≈ +9.6 / +9.8 / +9.2dB）→ 代码移植无 bug ✅
- 处理速度：只要快于实时（处理时长 < 音频时长）就行，离线没实时约束
- 编译报错：绝大多数是 Windows 残留符号，逐个删对应 `windows.h` 依赖即可

> ✅ **已实测（2026-08-21）**: Linux ELF vs Windows PE，`road_noise_0-34` NR_true 平均 **14.7dB** 两侧一致，采样最大差 **1 LSB**（0.0031% FS），差异样本 anti 0.02% / err 0.3%——纯浮点库（MSVCRT vs glibc）舍入差异，无害。

### 1.3b 不买板子也能验证 ARM 正确性 — WSL 交叉编译 + QEMU（零成本）

想确认代码**在 ARM 上也能编译、结果正确**，不需要等板子。WSL 里装个 ARM 交叉编译器 + QEMU 用户态模拟器，直接跑出 ARM 版结果：

```bash
# 1) WSL 里装工具（root 直接装）:
apt update && apt install -y gcc-aarch64-linux-gnu qemu-user

# 2) 交叉编译 offline 版为 aarch64 ELF:
aarch64-linux-gnu-gcc -O2 -Iinclude main.c src/scene_controller.c \
    src/fxnlms_mimo.c src/fir_filter.c src/binary_loader.c \
    src/cnn_m5_forward.c src/howling_detect.c src/ocg.c -lm -o /tmp/main_arm

# 3) 用 QEMU 用户态跑（-L 指定 ARM sysroot）:
qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/main_arm "Noise Examples/road_noise_0-34.wav"
```

**判定标准**: ARM 版与 x86 Linux 版输出逐采样对比，差异 ≤ 1 LSB、差异样本 < 1%、NR_true 一致 → **ARM 正确性验证通过**。

> ✅ **已实测（2026-08-21）**: ARM(QEMU) vs x86(Linux)，`road_noise_0-34` NR_true 平均 **14.7dB** 两侧一致，采样最大差 **1 LSB**，差异样本 anti 0.018% / err 0.324%——与 x86↔Windows 差异同级，纯浮点舍入，数学上一致。
>
> ⚠️ **注意**: QEMU 用户态是**指令模拟**（跑 34.8s 音频要 ~168s，约 0.2x 实时），这**不是**真实 RK3568 的速度，只是验证"能在 ARM 指令集上正确运行"。真实速度测不出来，那是板子的活。另外记得确认产物是 ELF（`od -An -tx1 -N4 main_arm` 开头 `7f 45 4c 46`），别被 WSL interop 把 Windows PE 混进来。

### 1.4 算力微基准（回答"8×8 在 62.5µs 内能不能算完"——不花一分钱）

这是整个阶段 1 最值钱的一步。不用任何音频硬件，直接测 FxLMS 单 tick 耗时。

**先做静态 MAC 预算（不花一分钱，立刻知道数量级）**。FxLMS 是确定性 DSP，热路径 MAC 数从代码就能数出来（2026-08-21 已数）：

| 配置 | 每样本 MACs | 每秒 @16kHz | A55(1.8GHz) 标量 | A55 NEON 理论 | 结论 |
|---|---|---|---|---|---|
| **3×2**（PC 原型，E=3/S=2/L=1024） | ~42,000 | 0.67 GMAC/s | 37% 单核 | ~5% | ✅ 随便跑，单核标量都够 |
| **8×8**（目标，E=8/S=8/L=1024） | ~376,000 | 6.0 GMAC/s | 335%（3.4 核）| 42% 单核 | ⚠️ 必须上 NEON；Ŝ 在线辨识占 37%，可降频或移第二核 |

**结论（静态分析就能定）**:
- **3×2 可行性已确定**，任何 A55 单核都能跑，无需任何硬件验证。
- **8×8 必须上 NEON**——标量要 3.4 核，这决定"要不要为 8×8 写 NEON 内联"的答案是**必须写**。
- **优化杠杆白送**: Ŝ 在线辨识（`sec_online_update`）占 8×8 的 37%，降到慢循环（每 N 样本）或挪第二核，8×8 立刻从"紧"变"松"。

**这之后**，写个微基准上板跑，唯一目的是校准 NEON **实际效率**（静态算不出那个 50-80% 的乘子）。

**先扩编译上限**（当前代码限制 2S/3E，目标 8×8）：
- `include/scenezone_types.h`: `GFANC_E_MAX` 5→8、`GFANC_S_MAX` 4→8
- 直接权重相关：`SC_DW_MAX` 30→120（文档 §4.8 已列，纯代码）

**写一个微基准**（新建 `bench_fxlms.c`，示意）：
```c
#include <stdio.h>
#include <time.h>
#include "fxnlms_mimo.h"

int main(void) {
    const int E = 8, S = 8, L = 1024;
    fxnlms_mimo_t fx;
    fxnlms_init(&fx, E, S, L, 1e-7f, 5e-7f);

    float x[E], d[E], anti[S], err[E];
    /* 用随机/扫频信号跑 N 次，测平均每 tick 耗时 */
    clock_t t0 = clock();
    const int N = 100000;                    /* 10 万次 */
    for (int i = 0; i < N; i++) {
        for (int e = 0; e < E; e++) { x[e] = ...; d[e] = 0.001f; }
        fxnlms_tick_rt(&fx, x, x, d, anti);  /* 或对应的实时 tick */
    }
    double us = 1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / N;
    printf("8x8 avg tick: %.1f us  (预算 62.5 us)\n", us);
    return 0;
}
```

**判定标准**:
| 结果 | 结论 |
|---|---|
| `avg tick < 62.5µs` | ✅ 算力够，实时可行 |
| `62.5-125µs` | ⚠️ 偏紧——需优化（滑动和功率归一化 / 关在线Ŝ / L=512） |
| `> 125µs` | ❌ RK3568 不够，考虑 RK3588 或 L=512 + 优化 |

> 💡 跑微基准时加上 `-O2 -march=native`，且**测的是单个采样周期能不能算完**——这是"延迟不受算力拖累"的前提。

**做完阶段 1-零，你应该已经知道**: 代码能否移植 + RK3568 算力够不够 8×8。这两个答案不花钱就能拿到。**如果算力不够，直接换平台或降规格，别急着买后面的硬件。**

---

## 2. 阶段 1-A: USB 声卡实时验证（零焊接，1-2 周，软件为主）

> 目标：让现有实时代码在 RK3568 + Linux 上真的出声降噪。**注意：USB 声卡路径延迟 ~8ms，验证不了 2ms**——它验证的是"实时调度 + 算法在 ARM 上收敛"，不是低延迟。

### 2.1 买什么

| 件 | 型号 | 参考价 | 备注 |
|---|---|---|---|
| 开发板 | 阶段 1-零 已买 | ¥0 | |
| **USB 8×8 声卡** | **Behringer UMC1820**（18 入/20 出，实际 8×XLR 入） | ¥1550-1900（含运费） | 或先拿你已有的 UMC404HD（4 入/2 出）跑通流程再升级 |
| USB 线 | USB 3.0 屏蔽线 | ¥30 | 声卡需稳定供电，插主板/板载 USB |

### 2.2 软件移植（这是主要工作量）

项目审查报告已把移植拆成清单（R-21 HAL / R-19 atomic）：

| 改动点 | 现在（Windows） | 改成（Linux/ARM） | 涉及文件 |
|---|---|---|---|
| **原子操作**（~25 处） | `InterlockedExchange/Increment/Decrement`（Win32） | C11 `<stdatomic.h>` 的 `atomic_exchange` 等 | [main_realtime.c](main_realtime.c) 全文件 |
| **DLL 加载层** | `pa_loader.c` 运行时 `LoadLibrary` 加载 `libportaudio64bit-asio.dll` | 直接链接系统 `libportaudio`（`-lportaudio`），删加载层 | [src/pa_loader.c](src/pa_loader.c) |
| **平台 HAL** | `gf_sleep_ms` / `GFANC_RDTSC`（x86 rdtsc） / `CreateThread` | `usleep` / `clock_gettime` / `pthread_create` | include 公共头 |
| **类型** | `LONG`（Win32 int32） | `int32_t` / `atomic_int` | 公共头 |
| **编译** | `gcc ... -lole32 -o scenezone_realtime.exe` | `-lpthread -latomic -lportaudio`，去掉 `-lole32` | Makefile |
| 离线版 | `#include <windows.h>`（仅一行） | 删掉即可 | [main.c:18](main.c#L18) |

> ⚠️ 这是 1-2 周的工作量，对一个没写过 Linux 多线程的小白偏难。**如果卡住**，可以只做"原子操作 + DLL 加载"两个最小改动先跑通实时（其他用最简替代），或者请人做 BSP 移植（¥5-15k）。

### 2.3 步骤

1. Linux 装 PortAudio：`sudo apt install libportaudio2 libportaudio-dev`
2. 完成 2.2 的移植改动，编译通过
3. 插 USB 声卡，`arecord -l` / `aplay -l` 确认设备出现，配 ALSA 默认设备
4. 把实时程序里的设备号指向 UMC1820，跑起来，按 README 阶段 4 的方式验证（250Hz 纯音 NR ≥10dB）
5. 对比 PC 上同样的纯音/宽带结果

### 2.4 判定标准 + 诚实声明

- ✅ 实时能跑、纯音 NR 和 PC 相当 → 算法在 ARM 上没问题，**算力 + 实时调度这两件事验证完毕**
- ⚠️ **这一阶段验证不了 2ms 低延迟**——USB 声卡链路延迟 ~8ms，因果缺口还在。它只是"先跑通、先确认算法对"，是定制板之前必要的试错。
- 如果这一阶段发现实时 xrun / 爆音 / 调度不稳，**说明逐样本实时这条链路在 Linux 上需要专门工程处理**，这本身就是定制前的重要情报。

---

## 3. 阶段 1-B: I2S 低延迟路径（有焊接，进阶，2-6 周）

> ⚠️ **诚实警告：这是整个文档里最难的部分，不建议小白作为第一步。** 它需要示波器、看懂时钟/数据手册、调试设备树和 codec 寄存器。**先做完阶段 1-零和 1-A 再评估要不要碰这里。**

### 3.1 买什么

| 件 | 型号建议 | 参考价 | 备注 |
|---|---|---|---|
| I2S 引出板 | RK3568 开发板（需确认 I2S/SAI 引脚引出，看官方 pinout） | 已有 | 不是所有板都引出多路 I2S |
| **8ch I2S DAC 板** | CS4382 模块（8ch）或 ES9018 板 | ¥100-300 | 需 TDM/I2S 输入 |
| **8ch I2S ADC 板** | TLV320ADC6140EVM（TI）或 ADAU1772 板 | ¥200-800 | 需 TDM/I2S 输出 |
| 省事替代 | **ADAU1787 评估板**（自带低延迟多通道 codec） | ¥500-1500 | 低延迟 codec 直接给 |
| 接线 | 杜邦线 / 面包板 / 烙铁 | ¥50 | |
| **示波器**（必备） | 任意 100MHz 双通道 | ¥300-1500 | 没它没法对时钟 |
| 逻辑分析仪 | 8ch 即可 | ¥50-150 | 看 TDM 时隙 |

### 3.2 接线（时钟拓扑——成败关键）

ANC 对消要求**所有 ADC/DAC 共享同一时钟**，否则 8 路相位对不齐直接失效。标准拓扑：

```
RK3568 I2S/SAI (主控/时钟主)
  ├── MCLK   ──→ ADC 板  +  DAC 板     (系统主时钟)
  ├── BCLK   ──→ ADC 板  +  DAC 板     (位时钟 64×fs 或 32×fs)
  ├── LRCK   ──→ ADC 板  +  DAC 板     (帧时钟 = 采样率)
  ├── SDOUT  ──→ ADC 板  (参考/误差麦数据入)
  └── SDIN   ←── DAC 板  (反噪声数据出)
```

- **TDM8 时隙**：8 路误差麦占一个数据线上的 8 个时隙（slot 0-7）；参考麦可用第 2 根数据线
- **主从设置**：RK3568 做时钟主（master），两块 codec 板做从（slave），否则时钟打架
- **具体引脚**：取决于你买的开发板和 codec 板，**必须以各自数据手册 pinout 为准**，先量电压再插线

### 3.3 需要配置的软件

1. **设备树/DT overlay**：把 RK3568 的 I2S 控制器配成 TDM8、时钟主，把 codec 挂在上面
2. **codec 低延迟模式**：读 datasheet 寄存器映射，把抽取/插值滤波器切到最低群延迟档——**不配就是通用模式 1-2ms，吃光预算**
3. **裸机/RTOS**：Linux + ALSA 附加 5-10ms，用不了。这一步通常意味着脱离 Linux，进 RTOS（FreeRTOS）裸机逐样本中断

### 3.4 判定标准

- 环路延迟（ADC→DSP→DAC 往返）测到 **≤2-5ms**
- 8 路通道间相位差 <1 样本
- 实机纯音 NR 达到 / 超过 USB 路径

> 诚实预期：对小白，这段 2-6 周是乐观估计，且很可能中途发现需要更多工具/知识。**如果时间宝贵，跳过 1-B，直接跳阶段 4 定制**——你已在 1-零/1-A 验证了算法和算力，定制板把"时钟同步 + 低延迟 codec + 逐样本"这些难点打包给厂家解决。

---

## 4. 常见坑清单

| 坑 | 现象 | 对策 |
|---|---|---|
| 代码在 Linux 编不过 | `windows.h` 报错 | 删 `#include <windows.h>`（main.c 第 18 行）；其余文件逐个清 |
| 算力不够 | 微基准 tick > 62.5µs | 关在线Ŝ / 滑动和功率 / L=512 / 换 RK3588 |
| 板子没有 I2S 引脚 | 找不到 pinout | 买前先查开发板规格，确认 I2S/SAI 引出 |
| USB 声卡延迟大 | 宽带仍消不动 | 正常——USB 就是 ~8ms，这不是 bug |
| 时钟不同步 | 8 路对不齐、啸叫 | 检查 MCLK/BCLK/LRCK 是否共享、主从是否设对 |
| codec 延迟高 | 量出来 >1ms | 确认切了低延迟模式，别用通用模式 |

---

## 5. 下一步：什么时候该去定制

| 你的状态 | 建议 |
|---|---|
| 阶段 1-零 算力不够 | 换平台/降规格，**别定制** |
| 阶段 1-零 过了，但不想做 1-A/1-B | **直接去定制**（[板级规格](板级硬件定制需求_8S8E_闭环.md)），代码移植交给厂家 |
| 阶段 1-A 过了 | 算法/算力已验证，定制风险已大降，可下单定制或先做 1-B |
| 阶段 1-B 过了 | 低延迟链路已验证，定制板上"时钟+codec+逐样本"难点已有把握 |

> 一句话路线图：**先花 1-3 天零焊接验证算力和移植（¥0-800）→ 再花 ¥1.6k 用 USB 声卡验证算法（1-2 周）→ 算力算法都确认了，才值得花 ¥2w+ 定制或投入 1-B 焊接。** 前面每步都在降低后面定制的失败风险，且几乎不花钱。
