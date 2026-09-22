# 双核示波器深存储采集/显示 实施计划

> **给执行者：** 本计划按任务逐个执行，每个任务末尾都有「怎么验证」。验证不通过不要往下走。
> 步骤用 `- [ ]` 复选框跟踪。推荐用「子代理逐任务执行」（superpowers 的 subagent-driven-development：
> 每个任务派一个全新的子代理，任务之间我来复核）的方式跑；也可以在当前会话里按「分批执行并在检查点停下」
> （executing-plans）的方式跑。

**目标：** CM4 采集 + 触发检测，CM7 用 MDMA 把半帧搬进 SDRAM 深存储环，CM7 做 min/max 抽取并把 512 点推给 TouchGFX 的两个动态折线图。

**架构：** CM4 的 ADC 循环 DMA 每 655 µs 产生一个半帧，在 ADC 的 HT/TC 回调里扫描触发点、更新 `osc_ctrl`、用 HSEM 通知 CM7。CM7 的一个高优先级任务在收到通知后按 `seq` 补搬半帧（MDMA，轮询等待），另一个低优先级任务做 min/max 抽取并双缓冲发布给 UI 线程。深存储环在 SDRAM，CH1 16 MB / CH2 8 MB，各 4,194,304 word，时间跨度同为 1.342 s。

**技术栈：** STM32H747 (CM7 @480 MHz / CM4 @240 MHz)、STM32 HAL、FreeRTOS (CMSIS-RTOS2)、TouchGFX 4.26、MDMA、HSEM、CMake + Ninja。

**设计文档：** [docs/superpowers/specs/2026-09-19-osc-deep-acquisition-design.md](../specs/2026-09-19-osc-deep-acquisition-design.md)

**本计划对设计文档的修正（先读这一段）：**

| # | 设计文档原文 | 实际情况 | 本计划 |
|---|---|---|---|
| 1 | §6.1 「设 `hadc1.DMA_Handle->XferHalfCpltCallback = OSC_Ch1_HalfCplt`」 | **不行**。`HAL_ADC_Start_DMA`（`stm32h7xx_hal_adc.c:2169-2175`）和 `HAL_ADCEx_MultiModeStart_DMA`（`stm32h7xx_hal_adc_ex.c:1085-1091`）会**无条件覆盖**这三个回调 | 覆写 HAL 的弱回调 `HAL_ADC_ConvHalfCpltCallback()` / `HAL_ADC_ConvCpltCallback()` |
| 2 | §5 `osc_ctrl_t` 字段表末尾有 `_pad[3]`，标注 64 B | 按该字段表算是 19 word = **76 B**，不是 64 B | 删掉 `_pad[3]`，正好 16 word = 64 B |
| 3 | §7.1 用 `HAL_MDMA_RegisterCallback` + 中断 | 采集任务每个半帧有 655 µs 预算，MDMA 实际只用 ~40 µs，中断回调没有收益 | 改用 `HAL_MDMA_Start` + `HAL_MDMA_PollForTransfer` 轮询 |
| 4 | §8 「档 13 一帧读 24 MB，约 160 ms」 | 只算了 CH1。CH1 4.19 M word + CH2 4.19 M halfword = 8.39 M 次访问 × 18.93 cyc / 480 MHz = **331 ms** | 按 `T_sweep ≈ step × 20.2 µs` 重算，档 13 是 331 ms（≈3 fps） |
| 5 | §10.5 「LTDC 当前只使能了行中断，加 `LTDC_IER_TERRIE`」 | **两处都不对**：`HAL_LTDC_Init`（`stm32h7xx_hal_ltdc.c:304`）已经使能 `LTDC_IT_TE \| LTDC_IT_FU`；缺的是 `LTDC_ER_IRQn` 的 NVIC 使能和 `LTDC_ER_IRQHandler` | 任务 6 里补 NVIC + 自己写 handler |
| 6 | §9.3 「min/max 抽取在 Model 里做」 | Model 的 `tick()` 跑在 TouchGFX 任务里，档 13 一次抽取 331 ms，会把界面卡死 331 ms | 抽取放独立任务，Model 只读结果 |
| 6b | §9 「Presenter 重写 `tick()`」 | **`touchgfx::Presenter` 里根本没有 `tick()` 这个虚函数**（`framework/include/mvp/Presenter.hpp` 只有 `activate` / `deactivate`），写在子类里也没人调 | 挂到 `OscillographView::handleTickEvent()`（`Screen.hpp:144`，由 `Application` 对当前屏幕每帧调用） |
| 7 | §7.2 状态机「FROZEN 等重臂」 | 没说谁重臂，实现出来就是第一次触发后永久冻结 | 搬完冻结帧后自动重臂（≈Normal 触发模式），另留 `OSC_Acq_Rearm()` 给将来的 Single 模式 |
| 8 | §10.4 「两个 `CMakeLists.txt` 只把 `Common/Src/...` 列为源文件」 | 那行在 `CM4/mx-generated.cmake:44` 和 `CM7/mx-generated.cmake:57` | 只改 `CMakeLists.txt`（`mx-generated.cmake` 是生成文件） |
| 9 | 隐含：`seq` 只表示「有新数据」 | CM7 必须知道该读哪一半 | 约定 `seq` 的奇偶 = 半帧下标；落后 > 2 个半帧就记账并跳到最新 |

---

## 执行中的变更记录（2026-09-20：触发电平逻辑重做）

任务 1 的 ABI 片段、任务 3 步骤 1 的 `osc_publish_half()`、任务 4 的触发扫描片段**都已被下面的版本取代**，不要照抄旧片段。

| # | 原计划 | 改成 | 为什么 |
|---|---|---|---|
| A1 | `osc_ctrl_t` 64 B，`cfg_trig_level` 单字段 | **96 B**（magic 换 `'OSC2'`），电平拆成 `cfg_trig_level_ch1` / `_ch2` | CH1(ADC1+ADC2，交替采同一路) 与 CH2(ADC3) 幅度可能差很远，共用一个电平必然有一路定歪 |
| A2 | 电平只能由 CM7 写 | 加 `OSC_LEVEL_AUTO` 哨兵值，**CM4 闭环**自适应定电平 | 用户不必手工找电平；跨核往返会让自适应变慢 |
| A3 | 迟滞对称：武装 `v < level-hyst`、触发 `v > level+hyst` | **迟滞只压"武装"侧，触发阈值就是 `level`** | 对称写法报出的触发点整体偏后 `hyst` 个采样，慢信号上肉眼可见 |
| A4 | 触发扫描在 HT/TC 中断内 | **中断只 +1 计数，扫描移到主循环 `OSC_CH_Poll()`** | 那两个中断走 `DMA1_Stream0/1`、抢占优先级为 0，扫 2048 word 会把 CM4 上所有其它中断挡住 200 µs 量级 |
| A5 | `diag_miss` = CM7 来不及搬 | `diag_miss` = **CM4 主循环来不及扫** | 语义随 A4 一起变 |
| A6 | 无 | 新增 `diag_min/max/level_ch1`、`_ch2` | 自适应模式下 CM7 不知道实际电平（画不出电平标记线）；min/max 顺带供 CM7 做垂直量程 |
| A7 | — | **直流判定不做** | 示波器限定只测周期变化信号。直流输入下 `trig_flags` 恒为 0，CM7 据此走自由运行即可，不需要额外字段 |
| A8 | 头文件 `Common/Inc/osc_shared.h` | 实际是 `Common/Inc/sharedconfig.h` | 文档笔误，代码一直用的是后者 |
| A9 | `main()` 主循环 `OSC_CH_DiagPrint(); HAL_Delay(1000);` | 改成 `OSC_CH_Poll()` + tick 限速的每秒一次诊断打印 | `HAL_Delay(1000)` 会让扫描丢掉 99.8% 的半帧（A4 的硬约束） |
| A10 | 单趟扫描命中就 `return i` | 命中只**锁存**位置，循环继续跑完 | 提前返回会让窗口统计只覆盖到触发点，`min/max` 不完整，自适应电平失准 |
| A11 | 有 `OSC_TRIG_CONSEC` 连续确认 + `TRIG_BACK` 位置补偿 + 自适应电平一阶平滑 + `seeded` 标志 | **全部砍掉**（2026-09-20 二次简化） | 目标是"原迟滞 + 自适应电平"的最小改动。连续确认带来半个 word 的取整误差且不是当前要解决的问题；平滑只在"每窗口都更新"时有意义，改成"连续两窗口失败才动"后直接落位更好；`seeded` 用 `no_trig` 初值替代。代价：慢速含噪正弦可能触发不稳 —— 先加大 `cfg_trig_hyst` 顶，不够再加回来 |

**主循环硬约束（A4 的代价）**：`OSC_CH_Poll()` 必须在半个帧周期 **655 µs**（2048 word × 320 ns）内被调到，循环里不许出现阻塞调用。跟不上会记进 `diag_miss`，不静默丢数据。

> 备选方案，改动更小：保留扫描在中断内，只把 `DMA1_Stream0/1` 的抢占优先级从 0 降下来。取舍是"主循环被拖慢"换成"扫描被高优先级中断拉长"。

**权威定义**：ABI 看[设计文档 §5 + §5.1](../specs/2026-09-19-osc-deep-acquisition-design.md)；迟滞语义、自适应电平、连续确认的完整说明看 §6.3。

---

## 全局约束

- **项目不是 git 仓库**（`git status` 报 `fatal: not a git repository`）。本计划**没有 commit 步骤**，每个任务的检查点是：编译通过 + 烧录 + 观察串口/USB 输出。
- **生成代码不要手改**：`CM4/mx-generated.cmake`、`CM7/mx-generated.cmake`、`CM7/cmake/touchgfx/CMakeLists.txt`、`TouchGFX/generated/` 全是生成物。要改配置就改 CubeMX 的 `.ioc`（用户已在做）。可以自由改的是：两个 `CMakeLists.txt`（文件头写明 "generated only once ... User is free to modify"）、`Mylibrary/`、`TouchGFX/gui/`、以及 `Core/` 里的 `/* USER CODE BEGIN */ ... /* USER CODE END */` 段。
- **构建命令**（在仓库根目录执行）：
  - CM7：`cmake --build CM7/build`
  - CM4：`cmake --build CM4/build`
  - 首次或改了 CMakeLists 后先 `cmake -S CM7 -B CM7/build -G Ninja` 重新配置。
- **调试输出**：CM7 用 `uart_printf()`（USART1，`CM7/Mylibrary/Inc/uart_printf.h`）；CM4 用 `usb_printf()`（USB CDC，`CM4/Mylibrary/Inc/usb_printf.h`）。
- **固定地址契约**（跨核，编译期常量，见 `Common/Inc/sharedconfig.h`）：
  - `osc_ch1_buf` @ `0x30030000`，16 KB，`uint32_t[4096]`
  - `osc_ch2_buf` @ `0x30034000`，8 KB，`uint16_t[4096]`
  - `osc_ctrl` @ `0x30036000`，**96 B**（24 word，2026-09-20 由 64 B 扩成，见上面的变更记录 A1）
- **两个链接脚本都不需要改**：CM4 的 `RAM (0x10000000, 192K)` + `RAM_D2 (0x30030000, 96K)` 已经把 D2 全域覆盖（`0x10000000` 和 `0x30000000` 是同一块 288 KB 的两个别名），CM7 的也不动。`.dma_buffer` 段落在 `RAM_D2` 里，由 CM4 链接脚本现有的段放置规则决定。
- **MDMA 两条必守经验**（第 0 步实测得出，见设计文档 §10.2.1 / §10.2.2）：
  - `BufferTransferLength` 必须 ≥ 数据宽度。CubeMX 里已是 `64`，**不要再改成 1**。
  - `HAL_MDMA_Start` / `HAL_MDMA_Start_IT` **之前**必须写 `h->Instance->CIFCR = 0x3F` 清残留中断标志，否则一个上电残留的 TEIF 会在使能中断的瞬间把通道掐死。
- **不要用 `memcpy` 做 CPU 拷贝**：`--specs=nano.specs` 下的 `memcpy` 是 28 字节的逐字节桩，实测比手写 word 循环慢 3.5 倍。

---

## 文件结构

| 文件 | 职责 | 动作 |
|---|---|---|
| `Common/Inc/sharedconfig.h` | 跨核 ABI：`osc_ctrl_t`、地址常量、时序常量、HSEM ID | 已建（原计划写 `osc_shared.h`，实名如此） |
| `CM4/Mylibrary/Inc/oscillograph.h` | CM4 采集对外接口 | 重写 |
| `CM4/Mylibrary/Src/oscillograph.c` | ADC 循环 DMA、半帧发布、触发扫描、HSEM 通知 | 重写 |
| `CM7/Mylibrary/Inc/oscillograph.h` | CM7 搬运/环/触发状态机对外接口 | 重写 |
| `CM7/Mylibrary/Src/oscillograph.c` | MDMA 搬运、深存储环、触发状态机、HSEM ISR、采集任务 | 重写 |
| `CM7/Mylibrary/Inc/osc_display.h` | 显示抽取对外接口（纯 C，不含 `osc_shared.h`） | 新建 |
| `CM7/Mylibrary/Src/osc_display.c` | 时基表、min/max 抽取、双缓冲发布、抽取任务 | 新建 |
| `CM7/TouchGFX/gui/include/gui/model/Model.hpp` + `src/model/Model.cpp` | 暴露显示缓冲与 dirty 标志 | 改 |
| `CM7/TouchGFX/gui/include/gui/oscillograph_screen/OscillographView.hpp` + `.cpp` | `handleTickEvent()` + `setGraphData()` | 改 |
| `CM7/TouchGFX/gui/include/gui/oscillograph_screen/OscillographPresenter.hpp` + `.cpp` | `handleDisplayTick()` 里 dirty 才刷新 | 改 |
| `CM4/CMakeLists.txt` / `CM7/CMakeLists.txt` | `Common/Inc` 与 `Mylibrary/Inc` 的 include 路径、新源文件 | 改 |
| `CM7/Core/Src/freertos.c` | `USER CODE BEGIN RTOS_THREADS` 里创建两个任务 | 改 |
| `CM7/Core/Src/main.c` | 删 `OSC_Step0_Run()`，加 `OSC_Acq_Init()` | 改 |
| `CM4/Core/Src/main.c` | 删 `OSC_Test_Init()` / `OSC_Test_Print()`；主循环换成 `OSC_CH_Poll()` + 限速诊断打印 | 已改 |

---

## 任务 1：跨核 ABI 头文件与构建接入

> ⚠️ **本任务的产出清单和下面的步骤 1 代码块已过时**（2026-09-20）：结构体是 **96 B / 24 word**，magic 是 `'OSC2'`，电平字段拆成 `cfg_trig_level_ch1` / `_ch2`，并新增了 `OSC_LEVEL_AUTO` 和一组 `diag_*`。文件实名是 `Common/Inc/sharedconfig.h`。**以[设计文档 §5 + §5.1](../specs/2026-09-19-osc-deep-acquisition-design.md) 为准**，本节以下内容留作历史记录。

**文件：**
- 新建：`Common/Inc/sharedconfig.h`（原计划名 `osc_shared.h`）
- 改：`CM4/CMakeLists.txt:55-58`、`CM7/CMakeLists.txt:38-42`（include 路径）
- 改：`CM4/Mylibrary/Src/oscillograph.c`、`CM7/Mylibrary/Src/oscillograph.c`（各加一行 include，用来验证路径通了）

**产出（后面所有任务依赖这些名字与类型）：**
- `osc_ctrl_t`（~~64~~ **96** 字节结构体，2026-09-20 起）、`OSC_CTRL`（`volatile osc_ctrl_t *` 常量指针）
- `OSC_CH1_BUF_ADDR` / `OSC_CH2_BUF_ADDR` / `OSC_CTRL_ADDR`
- `OSC_CH1_BUF_WORDS` / `OSC_CH2_BUF_WORDS` / `OSC_HALF_WORDS`
- `OSC_WORD_PERIOD_NS` / `OSC_WORDS_PER_SEC`
- `OSC_SHARED_MAGIC` / `OSC_HSEM_ID` / `OSC_TRIG_NONE`

- [ ] **步骤 1：写 `Common/Inc/osc_shared.h`**

`Common/Inc/` 目录目前不存在，需要新建。

```c
#ifndef __OSC_SHARED_H
#define __OSC_SHARED_H

#include <stdint.h>

/* ===========================================================================
 * 跨核共享 ABI —— CM4 与 CM7 必须编译同一份
 *
 * 地址契约靠 CM4 的链接脚本保证: 三个符号依次落在
 *     osc_ch1_buf @ 0x30030000   (uint32, 16 KB)
 *     osc_ch2_buf @ 0x30034000   (uint16,  8 KB)
 *     osc_ctrl    @ 0x30036000   (64 B)
 * CM4 在 OSC_CH_Init() 里自检, 对不上就 Error_Handler。
 * ========================================================================= */

#define OSC_SHARED_MAGIC      0x4F534331u     /* 'OSC1': CM4 写完控制块后置位 */

#define OSC_CH1_BUF_ADDR      0x30030000u
#define OSC_CH2_BUF_ADDR      0x30034000u
#define OSC_CTRL_ADDR         0x30036000u

#define OSC_CH1_BUF_WORDS     4096u           /* uint32 个数 = 16 KB */
#define OSC_CH2_BUF_WORDS     4096u           /* uint16 个数 =  8 KB */
#define OSC_HALF_WORDS        2048u           /* 半帧 = 环形缓冲的一半 */

#define OSC_WORD_PERIOD_NS    320u            /* 1 word = 16 个 ADC 周期 @50 MHz */
#define OSC_WORDS_PER_SEC     3125000u        /* 1e9 / 320 */

#define OSC_HSEM_ID           1u              /* ID_0 被双核 boot 同步占用 */
#define OSC_TRIG_NONE         0xFFFFFFFFu

typedef struct {
    /* ---- CM4 写, CM7 读 ---- */
    uint32_t magic;             /* 必须 == OSC_SHARED_MAGIC */
    uint32_t seq_ch1;           /* 每发布一个半帧 +1; 奇偶 = 半帧下标 */
    uint32_t seq_ch2;
    uint32_t trig_pos_ch1;      /* 半帧内触发点 word 下标; OSC_TRIG_NONE = 本帧无 */
    uint32_t trig_pos_ch2;
    uint32_t trig_flags;        /* bit0: 本半帧检测到触发 */

    /* ---- CM7 写, CM4 读 ---- */
    uint32_t cfg_trig_level;    /* 0..4095, 0 V ≈ 2047 */
    uint32_t cfg_trig_hyst;     /* 迟滞半宽, LSB, 默认 16 */
    uint32_t cfg_trig_edge;     /* 0 = 上升沿, 1 = 下降沿 */
    uint32_t cfg_trig_enable;
    uint32_t cfg_run;           /* 0 = 停止采集 */

    /* ---- 诊断 ---- */
    uint32_t diag_ovr_ch1;
    uint32_t diag_ovr_ch2;
    uint32_t diag_miss;         /* CM7 来不及搬的半帧数 */
    uint32_t diag_rate_ch1;     /* Sa/s, CM4 用 DWT 实测 */
    uint32_t diag_rate_ch2;
} osc_ctrl_t;                   /* 16 word = 64 B */

#define OSC_CTRL ((volatile osc_ctrl_t *)OSC_CTRL_ADDR)

/* 这个头文件将来可能被 C++ 的 Model.cpp 间接包含 */
#ifdef __cplusplus
#define OSC_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define OSC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

OSC_STATIC_ASSERT(sizeof(osc_ctrl_t) == 64, "osc_ctrl_t 必须正好 64 字节");

#endif /* __OSC_SHARED_H */
```

- [ ] **步骤 2：两个工程的 include 路径**

`CM4/CMakeLists.txt` 第 55-58 行的 `target_include_directories` 改成：

```cmake
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Inc
)
```

`CM7/CMakeLists.txt` 第 69-72 行（`# Add include paths` 那一块）同样加两行，但**额外**还要把 `Mylibrary/Inc` 与 `Common/Inc` 挂到 `stm32cubemx` 这个 INTERFACE 库上。

原因：TouchGFX 的 GUI 源码（`Model.cpp` 等）被编进 `CM7/cmake/touchgfx/CMakeLists.txt` 里定义的 `TouchGFX` OBJECT 库，它只继承 `stm32cubemx` 的 INTERFACE 头文件路径，**继承不到可执行目标的 PRIVATE 路径**。而 `cmake/touchgfx/CMakeLists.txt` 是 TouchGFX Designer 生成的，不能改。

在 `CM7/CMakeLists.txt` 的 `include("mx-generated.cmake")` **之后**（`stm32cubemx` 由那一行创建）插入：

```cmake
# TouchGFX 的 GUI 源码在 cmake/touchgfx 下的独立 OBJECT 库里编译,
# 它只继承 stm32cubemx 的 INTERFACE 头文件路径, 所以自定义头文件要挂在这里。
target_include_directories(stm32cubemx INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Inc
)
```

- [ ] **步骤 3：在两个 spike 文件里各加一行 include（仅为验证路径）**

`CM4/Mylibrary/Src/oscillograph.c` 顶部、`CM7/Mylibrary/Src/oscillograph.c` 顶部，各加：

```c
#include "osc_shared.h"
```

加完立刻做一次不使用它的编译，能过就说明 include 路径 + `_Static_assert` 都对。

- [ ] **步骤 4：编译两个工程**

```bash
cmake -S CM7 -B CM7/build -G Ninja && cmake --build CM7/build
cmake -S CM4 -B CM4/build -G Ninja && cmake --build CM4/build
```

预期：两个工程都 `ninja: no work to do` 之外的正常编译输出，**0 error 0 warning**。

**失败排查：**
- `fatal error: osc_shared.h: No such file or directory` → 步骤 2 的路径写错，或 `include("mx-generated.cmake")` 的位置不对（`stm32cubemx` 还没被创建）。
- `static assertion failed: osc_ctrl_t 必须正好 64 字节` → 数一遍字段，必须正好 16 个 `uint32_t`。
- 改了 CMakeLists 后必须重新 `cmake -S ... -B ...` 配置，光 `--build` 不会重新读。

---

## 任务 2：CM4 控制块落地（删 spike + 地址自检 + CDR 宏修正）

**文件：**
- 重写：`CM4/Mylibrary/Inc/oscillograph.h`
- 重写：`CM4/Mylibrary/Src/oscillograph.c`
- 改：`CM4/Core/Src/main.c`（`USER CODE BEGIN 2` 与 `USER CODE BEGIN 3` 两段）

**接口：**
- 消费：任务 1 的 `osc_shared.h` 全部常量与类型
- 产出：
  - `extern uint32_t osc_ch1_buf[OSC_CH1_BUF_WORDS];`（`.dma_buffer`，@ `0x30030000`）
  - `extern uint16_t osc_ch2_buf[OSC_CH2_BUF_WORDS];`（`.dma_buffer`，@ `0x30034000`）
  - `extern volatile osc_ctrl_t osc_ctrl;`（`.dma_buffer`，@ `0x30036000`）
  - `void OSC_CH_Init(void);` — 地址自检 + 控制块默认值 + 校准 + 置 magic + 启动 DMA
  - `void OSC_CH_DiagPrint(void);` — USB 打印诊断

> **开始前注意：** 第 0 步的 spike 已经按用户要求提前删掉了（CM4 侧删掉 `osc_test32` / `osc_test16` / `osc_test_magic` / `OSC_Test_Init` / `OSC_Test_Print` / `OSC_CH_TestPrint` / `measure_rate` / `count_ovr` / `ovr_reset_latency` / `dma_words_elapsed` / `OSC_CH_GetRateCh1/2` 以及 `CH1_ADC1`/`CH1_ADC2` 两个宏；CM7 侧 `Mylibrary/Src/oscillograph.c` 已清空）。所以**下面步骤 2 里「会一并删掉的旧东西」已经不存在了**，不要去找。`CM4/Mylibrary/Src/oscillograph.c` 现在是 44 行（两个缓冲 + `dwt_init()` + `OSC_CH_Init()`），`CM7/Mylibrary/Src/oscillograph.c` 是空的。`.dma_buffer` 段当前 24 KB，`0x30036000` 已经空出来给 `osc_ctrl`。

**背景（这是 bug，不是优化）：** `CM4/Mylibrary/Inc/oscillograph.h` 原来写的是

```c
/* 交错模式: 1 word = ADC1(高16位) + ADC2(低16位) */   ← 错的
```

CMSIS 里 `ADC_CDR_RDATA_MST_Pos = 0`、`ADC_CDR_RDATA_SLV_Pos = 16`，即**主机 ADC1 在低 16 位、从机 ADC2 在高 16 位**。ADC1 先采样，所以时间顺序是「低半字在前」。

> 注意：这个归属对当前设计**不影响画面对错**——任务 7 的 min/max 抽取与样本先后无关。修它是因为：(a) 现有定义与硬件相反，是实打实的错误；(b) 将来做「档 0 原始点连线」或导出原始数据时会直接错。

- [ ] **步骤 1：重写 `CM4/Mylibrary/Inc/oscillograph.h`**

```c
#ifndef __OSCILLOGRAPH_H
#define __OSCILLOGRAPH_H

#include "stm32h7xx_hal.h"
#include "adc.h"
#include "osc_shared.h"

/* 交错模式: 1 word = ADC1(低16位) + ADC2(高16位), ADC1 在时间上在前。
 * 依据 CMSIS: ADC_CDR_RDATA_MST_Pos = 0 (主/ADC1 在低半字),
 *             ADC_CDR_RDATA_SLV_Pos = 16 (从/ADC2 在高半字) */
#define CH1_ADC1(word)      ((uint16_t)((uint32_t)(word) & 0xFFFFU))
#define CH1_ADC2(word)      ((uint16_t)((uint32_t)(word) >> 16))

extern uint32_t          osc_ch1_buf[OSC_CH1_BUF_WORDS];
extern uint16_t          osc_ch2_buf[OSC_CH2_BUF_WORDS];
extern volatile osc_ctrl_t osc_ctrl;

void OSC_CH_Init(void);
void OSC_CH_DiagPrint(void);

#endif /* __OSCILLOGRAPH_H */
```

- [ ] **步骤 2：重写 `CM4/Mylibrary/Src/oscillograph.c`（本任务只到「启动 DMA」，回调留到任务 3）**

```c
#include "oscillograph.h"
#include "usb_printf.h"
#include "stm32h7xx_hal_adc_ex.h"
#include <stdint.h>

/* ===========================================================================
 * CM4 采集侧
 *
 * 三个共享符号必须按下面这个顺序声明: .dma_buffer 段从 RAM_D2 起点
 * (0x30030000) 开始按下标顺序摆放, 所以它们依次落在
 *     osc_ch1_buf @ 0x30030000
 *     osc_ch2_buf @ 0x30034000
 *     osc_ctrl    @ 0x30036000
 * 加/删/调序都会让地址平移 —— OSC_CH_Init() 里会自检并 Error_Handler。
 * ========================================================================= */

uint32_t            osc_ch1_buf[OSC_CH1_BUF_WORDS] __attribute__((section(".dma_buffer"), aligned(32)));
uint16_t            osc_ch2_buf[OSC_CH2_BUF_WORDS] __attribute__((section(".dma_buffer"), aligned(32)));
volatile osc_ctrl_t osc_ctrl                       __attribute__((section(".dma_buffer"), aligned(32)));

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void ctrl_defaults(void)
{
    osc_ctrl.magic           = 0u;
    osc_ctrl.seq_ch1         = 0u;
    osc_ctrl.seq_ch2         = 0u;
    osc_ctrl.trig_pos_ch1    = OSC_TRIG_NONE;
    osc_ctrl.trig_pos_ch2    = OSC_TRIG_NONE;
    osc_ctrl.trig_flags      = 0u;
    osc_ctrl.cfg_trig_level  = 2047u;   /* 0 V 附近 */
    osc_ctrl.cfg_trig_hyst   = 16u;
    osc_ctrl.cfg_trig_edge   = 0u;      /* 上升沿 */
    osc_ctrl.cfg_trig_enable = 0u;
    osc_ctrl.cfg_run         = 0u;
    osc_ctrl.diag_ovr_ch1    = 0u;
    osc_ctrl.diag_ovr_ch2    = 0u;
    osc_ctrl.diag_miss       = 0u;
    osc_ctrl.diag_rate_ch1   = 0u;
    osc_ctrl.diag_rate_ch2   = 0u;
}

void OSC_CH_Init(void)
{
    /* --- 1. 地址契约自检。对不上说明 .dma_buffer 的布局被改动了,
     *        CM7 会去读错误的地址, 必须立刻停下来而不是跑出一堆脏数据。 --- */
    if ((uintptr_t)osc_ch1_buf != (uintptr_t)OSC_CH1_BUF_ADDR) Error_Handler();
    if ((uintptr_t)osc_ch2_buf != (uintptr_t)OSC_CH2_BUF_ADDR) Error_Handler();
    if ((uintptr_t)&osc_ctrl   != (uintptr_t)OSC_CTRL_ADDR)    Error_Handler();

    /* --- 2. 控制块默认值 --- */
    ctrl_defaults();
    __DMB();

    /* --- 3. ADC 校准: 交错模式下 ADC1/ADC2 是两片独立模拟核, 都要校准 --- */
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();
    if (HAL_ADCEx_Calibration_Start(&hadc3, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();

    dwt_init();

    /* --- 4. 置 magic。CM7 看到它之后才会写 cfg_*, 所以必须在启动 DMA 之前。 --- */
    __DMB();
    osc_ctrl.magic = OSC_SHARED_MAGIC;
    __DMB();

    /* --- 5. 启动循环 DMA。此后 HT/TC 回调开始发布半帧。 --- */
    if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, osc_ch1_buf, OSC_CH1_BUF_WORDS) != HAL_OK) Error_Handler();
    if (HAL_ADC_Start_DMA(&hadc3, (uint32_t *)osc_ch2_buf, OSC_CH2_BUF_WORDS) != HAL_OK) Error_Handler();

    __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR);
    __HAL_ADC_CLEAR_FLAG(&hadc3, ADC_FLAG_OVR);
}

/* 注意: 任务 3 会在这里追加 HAL_ADC_ConvHalfCpltCallback / HAL_ADC_ConvCpltCallback /
 * HAL_ADC_ErrorCallback 三个覆写, 本任务先只跑到「能启动」。 */

void OSC_CH_DiagPrint(void)
{
    usb_printf("[OSC] addr ch1=0x%08lX ch2=0x%08lX ctrl=0x%08lX\r\n",
               (unsigned long)(uintptr_t)osc_ch1_buf,
               (unsigned long)(uintptr_t)osc_ch2_buf,
               (unsigned long)(uintptr_t)&osc_ctrl);
    usb_printf("      magic=0x%08lX seq1=%lu seq2=%lu\r\n",
               (unsigned long)osc_ctrl.magic,
               (unsigned long)osc_ctrl.seq_ch1,
               (unsigned long)osc_ctrl.seq_ch2);
    usb_printf("      rate1=%lu Sa/s rate2=%lu Sa/s\r\n",
               (unsigned long)osc_ctrl.diag_rate_ch1,
               (unsigned long)osc_ctrl.diag_rate_ch2);
    usb_printf("      ovr1=%lu ovr2=%lu miss=%lu\r\n",
               (unsigned long)osc_ctrl.diag_ovr_ch1,
               (unsigned long)osc_ctrl.diag_ovr_ch2,
               (unsigned long)osc_ctrl.diag_miss);
    usb_printf("      trig pos1=%lu pos2=%lu flags=0x%08lX\r\n",
               (unsigned long)osc_ctrl.trig_pos_ch1,
               (unsigned long)osc_ctrl.trig_pos_ch2,
               (unsigned long)osc_ctrl.trig_flags);
}
```

**这次重写会一并删掉的旧东西**（原文件里的 spike，全部不要了）：`measure_rate()`、`count_ovr()`、`ovr_reset_latency()`、`dma_words_elapsed()`、`OSC_CH_GetRateCh1/2()`、`OSC_CH_TestPrint()`、`osc_test32/osc_test16/osc_test_magic`、`OSC_Test_Init()`、`OSC_Test_Print()`。

删除 `osc_test*` 三个变量是**必须的**：它们现在正占着 `0x30036000` 起的 6 KB，`osc_ctrl` 要落在那个地址上。

- [ ] **步骤 3：改 `CM4/Core/Src/main.c`**

`USER CODE BEGIN 2` 段（现在是 `OSC_Test_Init(); OSC_Ch1_Multiple(1,1); OSC_CH_Init();`）改成：

```c
  OSC_Ch1_Multiple(1, 1);
  OSC_CH_Init();
```

`while(1)` 里的 `USER CODE BEGIN 3` 段（现在是 `OSC_Test_Print(); OSC_CH_TestPrint(); HAL_Delay(1000);`）改成：

```c
    OSC_CH_DiagPrint();
    HAL_Delay(1000);
```

- [ ] **步骤 4：编译**

```bash
cmake --build CM4/build
```

预期：0 error。若报 `OSC_Test_Init` / `osc_test32` 未定义 —— 说明 `main.c` 里的调用没删干净。

- [ ] **步骤 5：烧录并读 USB 日志**

烧录 CM4（和 CM7，因为共享内存布局变了要一起）。

预期 USB 输出：

```
[OSC] addr ch1=0x30030000 ch2=0x30034000 ctrl=0x30036000
      magic=0x4F534331 seq1=0 seq2=0
      rate1=0 Sa/s rate2=0 Sa/s
      ovr1=0 ovr2=0 miss=0
```

**三条必须同时满足，缺一条就停下来排查：**
1. 三个地址**完全等于** `0x30030000 / 0x30034000 / 0x30036000`。不等就说明砍掉 spike 后布局还是偏了 —— 检查 `.dma_buffer` 里有没有漏掉的变量（`grep -rn 'dma_buffer' CM4/`）。
2. `magic = 0x4F534331`。不是就说明 `OSC_CH_Init()` 没跑到第 4 步（可能卡在 `Error_Handler`）。
3. `seq1 = seq2 = 0`。**不是 0 就说明 DMA 已经在跑但回调还在（任务 3 才有）**，或者旧的 `OSC_CH_TestPrint` 没删干净。

---

## 任务 3：CM4 半帧发布（HT/TC 回调 + HSEM 通知 + 采样率诊断）

> ⚠️ **本任务的步骤 1 代码块已过时**（2026-09-20）：发布不再由 `osc_publish_half()` 在回调里做，改为**回调只 `+1` 半帧计数**（`s_completed_ch1/2`），发布+扫描统一在**主循环**的 `OSC_CH_Poll()` 里做。`seq` 的奇偶约定不变。原因见变更记录 A4/A5 —— 那两个 DMA 中断抢占优先级为 0，在中断里扫 2048 word 会挡住 CM4 上所有其它中断。**实际实现看 `CM4/Mylibrary/Src/oscillograph.c`**，本节以下内容留作历史记录。

**文件：**
- 改：`CM4/Mylibrary/Src/oscillograph.c`（在任务 2 留下的注释处追加）

**接口：**
- 消费：任务 2 的 `OSC_CH_Init()`、`osc_ctrl`、`CH1_ADC1/CH1_ADC2`
- 产出：
  - 覆写 `HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef*)`、`HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef*)`、`HAL_ADC_ErrorCallback(ADC_HandleTypeDef*)`
  - `osc_ctrl.seq_ch1` / `seq_ch2` 每半帧 +1（奇偶 = 半帧下标）
  - `osc_ctrl.diag_rate_ch1/2`、`diag_ovr_ch1/2` 被填充

**为什么是覆写弱回调而不是设 `DMA_Handle->XferHalfCpltCallback`：** `HAL_ADCEx_MultiModeStart_DMA` 会无条件把 `hadc->DMA_Handle->XferHalfCpltCallback` 设成 `ADC_DMAHalfConvCplt`（`stm32h7xx_hal_adc_ex.c:1085-1091`），我们在调用前设的值会被冲掉。`ADC_DMAHalfConvCplt` 内部会调 `hadc->ConvHalfCpltCallback`，而 `HAL_ADC_Init()` 把它初始化成弱符号 `HAL_ADC_ConvHalfCpltCallback`（`stm32h7xx_hal_adc.c:456-457`）——覆写这个弱符号才是稳的。

顺带确认：HT 中断**本来就使能着**。`HAL_DMA_Start_IT` 只在 `XferHalfCpltCallback != NULL` 时置 `DMA_IT_HT`（`stm32h7xx_hal_dma.c:711-715`），而 ADC 驱动刚好把它设成了非空。所以现在 HT 是「在响但没人听」，不是「没响」。

- [ ] **步骤 1：追加半帧发布代码**

在 `CM4/Mylibrary/Src/oscillograph.c` 里，`dwt_init()` 之后、`OSC_CH_Init()` 之前，插入：

```c
#define OSC_CM4_HZ        240000000u
#define RATE_AVG_HALVES   16u

/* 下一个要发布的半帧下标: HT 和 TC 交替翻转 */
static uint32_t s_half_ch1;
static uint32_t s_half_ch2;

/* 采样率诊断 */
static uint32_t s_cyc_ch1, s_last_ch1;
static uint32_t s_cyc_ch2, s_last_ch2;

/* 用 DWT 测 RATE_AVG_HALVES 个半帧的总时长, 反算采样率。
 * 比原来的 100 ms 轮询法更准(半帧边界就是硬时间戳), 而且零额外开销。 */
static void rate_update(uint32_t *last, uint32_t *cnt, uint32_t samples_per_half, volatile uint32_t *out)
{
    uint32_t now = DWT->CYCCNT;
    uint32_t d   = now - *last;
    *last = now;

    if (d == 0u || d > OSC_CM4_HZ) return;      /* 首拍, 或间隔异常 */

    if (++(*cnt) < RATE_AVG_HALVES) return;
    *cnt = 0u;

    *out = (uint32_t)(((uint64_t)samples_per_half * RATE_AVG_HALVES * OSC_CM4_HZ) / (uint64_t)d);
}

/* HT 和 TC 各调一次, 每次发布"刚刚完成"的那一半。
 * 奇偶规律: 第 0 次发布半帧 0, 第 1 次发布半帧 1, 第 2 次回到 0 ...
 * CM7 侧用 seq 的奇偶推出同一件事。 */
static void osc_publish_half(uint32_t ch)
{
    if (ch == 0u) {
        s_half_ch1 ^= 1u;
        rate_update(&s_last_ch1, &s_cyc_ch1, OSC_HALF_WORDS * 2u, &osc_ctrl.diag_rate_ch1);
    } else {
        s_half_ch2 ^= 1u;
        rate_update(&s_last_ch2, &s_cyc_ch2, OSC_HALF_WORDS,     &osc_ctrl.diag_rate_ch2);
    }

    /* 半帧已经完整落在缓冲里了, 先把数据可见性保住, 再推 seq */
    __DMB();
    if (ch == 0u) osc_ctrl.seq_ch1++;
    else          osc_ctrl.seq_ch2++;
    __DMB();

    /* 通知 CM7。
     * 必须是"先取再放": 释放一个本来就空闲的信号量, 硬件不保证产生通知;
     * 只有 取->放 这个真实的 占用->释放 跳变才会置 C1ISR 的对应位。 */
    (void)HAL_HSEM_FastTake(OSC_HSEM_ID);
    HAL_HSEM_Release(OSC_HSEM_ID, 0u);
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)      osc_publish_half(0u);   /* 前半帧 [0,2048) 完成 */
    else if (hadc->Instance == ADC3) osc_publish_half(1u);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)      osc_publish_half(0u);   /* 后半帧 [2048,4096) 完成 */
    else if (hadc->Instance == ADC3) osc_publish_half(1u);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)      osc_ctrl.diag_ovr_ch1++;
    else if (hadc->Instance == ADC3) osc_ctrl.diag_ovr_ch2++;
}
```

**注意 `hadc2`（从机 ADC2）要显式排除**：交错模式下 ADC2 不做 DMA，但它的 `ConvHalfCpltCallback` 也可能被调用。上面用 `else if`，ADC2 落进 else 分支什么都不做，是对的。

- [ ] **步骤 2：`.ioc` 里确认 ADC 中断不会掉进 `Default_Handler`**

`HAL_ADC_Start_DMA` 会 `__HAL_ADC_ENABLE_IT(hadc, ADC_IT_OVR)`。到 CM4 的 `stm32h7xx_it.c` 里确认 `ADC_IRQHandler` 存在（CubeMX 生成时会带上）：

```bash
grep -n "ADC_IRQHandler" CM4/Core/Src/stm32h7xx_it.c
```

预期：存在且调用 `HAL_ADC_IRQHandler(&hadc1)` 之类。若**不存在**，就在 `CM4/Core/Src/main.c` 的 `USER CODE BEGIN 4` 段里自己补：

```c
void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
    HAL_ADC_IRQHandler(&hadc3);
}
```

（自己写在 `USER CODE` 段里，CubeMX 重新生成不会覆盖。启动文件里它们都是 weak 符号，强定义能顶掉。）

- [ ] **步骤 3：编译并烧录**

```bash
cmake --build CM4/build
```

烧录 CM4，读 USB 日志。预期 1 秒后：

```
      magic=0x4F534331 seq1=≈3052 seq2=≈3052
      rate1=6250000 Sa/s rate2=3125000 Sa/s
      ovr1=0 ovr2=0 miss=0
```

**逐条判定：**
- `seq1` 和 `seq2` 各自每秒 +3052 左右（1 s / 655.36 µs ≈ 1526 半帧…… 实际是 2×1526 = 3052，因为每个半帧 327.68 µs）。两通道**必须几乎相等**（差值 ≤ 1）。差得多说明有一路回调没接上。
- `rate1 = 6250000 ± 1%`，`rate2 = 3125000 ± 1%`。这两个数**就是设计文档 §2 里那两个待验证的量**：`rate1/rate2` 应当正好等于 2.000。若比值不是 2，说明 ADC3 的 prescaler 不是 DIV2，整个时基表（任务 7）要重算 —— **停下来先解决这个**。
- `ovr1` / `ovr2` 必须是 0 或极小。持续增长说明 DMA 跟不上，采样率算得再准也没意义。
- `seq` 完全不动 → HT/TC 回调没被调到。检查 `hadc->Instance == ADC1` 这个判断（用调试器在 `osc_publish_half` 打断点最快）。

---

## 任务 4：CM4 触发扫描（迟滞 + 边沿）

> ⚠️ **本任务已被重做**（2026-09-20），下面的 `TRIG_TEST` 宏方案仅作历史记录。改动：
> 1. **扫描位置**：从 HT/TC 中断移到主循环 `OSC_CH_Poll()`（变更记录 A4）；
> 2. **迟滞语义**：改成非对称 —— 迟滞只压"武装"侧，触发阈值就是 `level`（A3）；
> 3. **电平**：每通道独立 `cfg_trig_level_ch1/_ch2`，并支持 `OSC_LEVEL_AUTO` 由 CM4 闭环自适应（A1/A2）；
> 4. **扫描方式**：单趟跑完整个半帧，命中只锁存位置不提前 `return`（A10）。
>
> 连续确认（`OSC_TRIG_CONSEC`）与电平平滑已在二次简化中砍掉，见 A11。
>
> **看[设计文档 §6.3](../specs/2026-09-19-osc-deep-acquisition-design.md) 和 `CM4/Mylibrary/Src/oscillograph.c`。** 接口契约不变：产出 `osc_ctrl.trig_pos_ch1` / `trig_pos_ch2`（word 下标）/ `trig_flags` bit0。

**文件：**
- 改：`CM4/Mylibrary/Src/oscillograph.c`

**接口：**
- 消费：任务 3 的半帧完成计数、任务 1 的 `osc_ctrl.cfg_trig_*`
- 产出：`osc_ctrl.trig_pos_ch1` / `trig_pos_ch2`（word 下标）/ `trig_flags` bit0 / `diag_min·max·level_ch1/2`

- [ ] **步骤 1：追加触发扫描**

在任务 3 的 `osc_publish_half()` **之前**插入：

```c
/* 迟滞触发: 先跌破 LOW 才武装, 再穿过 HIGH 才触发(上升沿)。
 * 返回命中样本的下标; 未命中返回 OSC_TRIG_NONE。
 * 每个通道各自持有 *armed, 不能共用。 */
#define TRIG_TEST(v)                                                          \
    do {                                                                      \
        uint32_t _v = (v);                                                    \
        if (!*armed) {                                                        \
            if (falling ? (_v > hi) : (_v < lo)) *armed = 1u;                 \
        } else if (falling ? (_v < lo) : (_v > hi)) {                         \
            *armed = 0u;                                                      \
            return i;                                                         \
        }                                                                     \
    } while (0)

static uint32_t trig_scan_ch2(const uint16_t *s, uint32_t n, uint32_t *armed)
{
    uint32_t level   = osc_ctrl.cfg_trig_level;
    uint32_t hyst    = osc_ctrl.cfg_trig_hyst;
    uint32_t falling = osc_ctrl.cfg_trig_edge & 1u;
    uint32_t hi = level + hyst;
    uint32_t lo = (level > hyst) ? (level - hyst) : 0u;
    if (hi > 4095u) hi = 4095u;

    for (uint32_t i = 0; i < n; i++) {
        TRIG_TEST(s[i]);                 /* CH2: 1 word = 1 个样本, 下标就是 word 下标 */
    }
    return OSC_TRIG_NONE;
}

static uint32_t trig_scan_ch1(const uint32_t *w, uint32_t n, uint32_t *armed)
{
    uint32_t level   = osc_ctrl.cfg_trig_level;
    uint32_t hyst    = osc_ctrl.cfg_trig_hyst;
    uint32_t falling = osc_ctrl.cfg_trig_edge & 1u;
    uint32_t hi = level + hyst;
    uint32_t lo = (level > hyst) ? (level - hyst) : 0u;
    if (hi > 4095u) hi = 4095u;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t word = w[i];
        /* ADC1 在低 16 位, 且时间上在前 —— 先比它 */
        TRIG_TEST(word & 0xFFFFu);
        TRIG_TEST(word >> 16);
    }
    return OSC_TRIG_NONE;                /* 返回的是 word 下标, 不是样本下标 */
}
```

- [ ] **步骤 2：把扫描接进 `osc_publish_half()`**

`osc_publish_half()` 改成（新增的部分用注释标出）：

```c
static uint32_t s_armed_ch1;             /* 迟滞状态, 每通道一个 */
static uint32_t s_armed_ch2;

static void osc_publish_half(uint32_t ch)
{
    uint32_t pos = OSC_TRIG_NONE;

    if (osc_ctrl.cfg_run && osc_ctrl.cfg_trig_enable) {
        if (ch == 0u) {
            const uint32_t *half = &osc_ch1_buf[(s_half_ch1 ^ 1u) * OSC_HALF_WORDS];
            pos = trig_scan_ch1(half, OSC_HALF_WORDS, &s_armed_ch1);
        } else {
            const uint16_t *half = &osc_ch2_buf[(s_half_ch2 ^ 1u) * OSC_HALF_WORDS];
            pos = trig_scan_ch2(half, OSC_HALF_WORDS, &s_armed_ch2);
        }
    }

    if (ch == 0u) {
        osc_ctrl.trig_pos_ch1 = pos;
        if (pos != OSC_TRIG_NONE) osc_ctrl.trig_flags |=  1u;
        else                      osc_ctrl.trig_flags &= ~1u;
    } else {
        osc_ctrl.trig_pos_ch2 = pos;
        if (pos != OSC_TRIG_NONE) osc_ctrl.trig_flags |=  2u;
        else                      osc_ctrl.trig_flags &= ~2u;
    }

    if (ch == 0u) {
        s_half_ch1 ^= 1u;
        rate_update(&s_last_ch1, &s_cyc_ch1, OSC_HALF_WORDS * 2u, &osc_ctrl.diag_rate_ch1);
    } else {
        s_half_ch2 ^= 1u;
        rate_update(&s_last_ch2, &s_cyc_ch2, OSC_HALF_WORDS,     &osc_ctrl.diag_rate_ch2);
    }

    __DMB();
    if (ch == 0u) osc_ctrl.seq_ch1++;
    else          osc_ctrl.seq_ch2++;
    __DMB();

    (void)HAL_HSEM_FastTake(OSC_HSEM_ID);
    HAL_HSEM_Release(OSC_HSEM_ID, 0u);
}
```

**关键点：`(s_half_ch1 ^ 1u)` 是「刚刚完成的那一半」。** `s_half_ch1` 存的是「下次要发布的下标」，而回调进来时刚完成的是上一个，所以取反。同时 `if (cfg_run && cfg_trig_enable)` 只在扫描时读配置，**`cfg_run = 0` 时不扫描也不发布**（见下方步骤 3 的 `if`）。

- [ ] **步骤 3：`cfg_run = 0` 时不发布**

在 `osc_publish_half()` 的最开头加：

```c
    if (!osc_ctrl.cfg_run) return;       /* CM7 要求停止: 连 seq 都不推, CM7 会看到静止 */
```

**注意顺序**：这个 `return` 必须在 `s_half_ch1 ^= 1u` **之前**，否则半帧下标会和 DMA 的实际进度错位。加完检查一遍：`s_half_*` 的翻转只在函数末尾那一处。

- [ ] **步骤 4：编译、烧录、验证**

先临时把 CM7 的 `cfg_*` 写默认值那步（任务 5 才有）跳过 —— 本任务验证时 CM4 侧 `cfg_run` 还是 0，所以**不会有触发**。为了能看到扫描在工作，在 `OSC_CH_Init()` 里临时把默认值改成：

```c
    osc_ctrl.cfg_trig_level  = 2047u;
    osc_ctrl.cfg_trig_hyst   = 16u;
    osc_ctrl.cfg_trig_edge   = 0u;
    osc_ctrl.cfg_trig_enable = 1u;
    osc_ctrl.cfg_run         = 1u;      /* ← 临时, 任务 5 完成后由 CM7 写, 这里要改回 0 */
```

```bash
cmake --build CM4/build
```

烧录，读 USB 日志。预期：

```
      rate1=6250000 Sa/s rate2=3125000 Sa/s
      ovr1=0 ovr2=0 miss=0
      trig pos1=0xFFFFFFFF pos2=0xFFFFFFFF flags=0x00000000
```

**没有输入信号时 `pos` 全是 `OSC_TRIG_NONE`、`flags = 0`，这是正确的。** 要验证扫描逻辑真的在跑，用调试器或临时打印：

- **判定 1（扫描在跑）**：把 `cfg_trig_level` 临时改成 `0`、`cfg_trig_hyst` 改成 `0`，上升沿模式下 `v > 0` 几乎立刻成立 → `flags` 应该变成 `0x3`，`pos1`/`pos2` 是小数字。改回去。
- **判定 2（迟滞在起作用）**：`cfg_trig_level = 2047`、`hyst = 16` 时，没有信号则永不触发（如上）。这是**测试通过**，不是失败。
- **判定 3（CPU 预算）**：扫描两个通道的半帧共 2048×2 + 2048 = 6144 个样本。`seq1` 必须还是 ≈3052/s，`rate1` 还是 6250000 ± 1%。若 `rate` 掉下来或 `ovr` 涨，说明扫描吃太多时间，需要把 `cfg_trig_enable` 的默认值关掉再想别的办法（先不用优化）。

**验证完把 `cfg_run` 的临时默认值改回 `0`** —— 它由 CM7 在任务 5 之后负责置 1。

---

## 任务 5：CM7 深存储搬运任务（MDMA + 环形写指针 + 丢帧统计）

**文件：**
- 重写：`CM7/Mylibrary/Inc/oscillograph.h`
- 重写：`CM7/Mylibrary/Src/oscillograph.c`（替换掉整个 Step 0 spike）
- 改：`CM7/Core/Src/main.c:180`（删 `OSC_Step0_Run()`，加 `OSC_Acq_Init()`）
- 改：`CM7/Core/Src/freertos.c`（`USER CODE BEGIN RTOS_THREADS`）

**接口：**
- 消费：任务 1 的 `osc_shared.h`；任务 3/4 的 `seq` / `trig_pos` / `trig_flags` 语义
- 产出：
  - `void OSC_Acq_Init(void);` — HSEM 中断接线（在 `main.c` 的 `USER CODE BEGIN 2` 里调用）
  - `void OSC_Acq_CreateTasks(void);` — 建两个任务（在 `freertos.c` 里调用）
  - `uint32_t OSC_Acq_GetWriteWords(void);` — 环里已写入的 word 总数（抽取任务读）
  - `uint32_t OSC_Acq_GetTrigAbs(void);` — 触发点绝对 word 序号，`OSC_TRIG_NONE` = 无
  - `int OSC_Acq_IsFrozen(void);` / `int OSC_Acq_IsWarm(void);` / `void OSC_Acq_Rearm(void);`
  - `void OSC_Acq_DiagPrint(void);`
  - 覆盖弱符号 `void HSEM1_IRQHandler(void);`

**为什么 HSEM 中断处理要自己写而不调 `HAL_HSEM_IRQHandler()`：** HAL 那版在 `USE_MULTI_CORE_SHARED_CODE` 未定义时会去读写 `HSEM_COMMON->IER`，那是**两个核共用**的使能位；而 CPU1 和 CPU2 各自有独立的 `C1IER/C2IER`。自己写 `HSEM1_IRQHandler` 直接用 C1 线，语义明确，而且不清 `C1IER`（HAL 会清，导致通知变成一次性的）。

- [ ] **步骤 1：写 `CM7/Mylibrary/Inc/oscillograph.h`**

```c
#ifndef __OSCILLOGRAPH_H
#define __OSCILLOGRAPH_H

#include "stm32h7xx_hal.h"
#include "osc_shared.h"

/* 深存储环: 两个通道各 4,194,304 word, 时间跨度同为 1.342 s */
#define OSC_REGION_WORDS   0x400000u
#define OSC_REGION_MASK    (OSC_REGION_WORDS - 1u)

#define OSC_SDRAM_CH1_BASE 0xD0200000u
#define OSC_SDRAM_CH2_BASE 0xD1200000u

void     OSC_Acq_Init(void);          /* 在 main.c 的 USER CODE BEGIN 2 里调用 */
void     OSC_Acq_CreateTasks(void);   /* 在 freertos.c 的 USER CODE BEGIN RTOS_THREADS 里调用 */

uint32_t OSC_Acq_GetWriteWords(void);
uint32_t OSC_Acq_GetTrigAbs(void);
int      OSC_Acq_IsFrozen(void);
int      OSC_Acq_IsWarm(void);
void     OSC_Acq_Rearm(void);
void     OSC_Acq_DiagPrint(void);

#endif /* __OSCILLOGRAPH_H */
```

- [ ] **步骤 2：写 `CM7/Mylibrary/Src/oscillograph.c`**

```c
#include "oscillograph.h"
#include "osc_display.h"
#include "mdma.h"
#include "uart_printf.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

/* ===========================================================================
 * CM7 搬运侧: HSEM 通知 -> 按 seq 补搬半帧 (MDMA) -> 推进深存储环
 * ========================================================================= */

#define HALF_BYTES_CH1   (OSC_HALF_WORDS * 4u)   /* 8192 */
#define HALF_BYTES_CH2   (OSC_HALF_WORDS * 2u)   /* 4096 */
#define POST_WORDS       (OSC_REGION_WORDS / 2u) /* 触发后 50% 后触发 */
#define MDMA_TIMEOUT_MS  10u
#define OSC_TASK_STACK   2048u

extern MDMA_HandleTypeDef hmdma_mdma_channel0_sw_0;
extern MDMA_HandleTypeDef hmdma_mdma_channel1_sw_0;

#define OSC_TRIG_RUN     0u
#define OSC_TRIG_ARMED   1u
#define OSC_TRIG_FROZEN  2u

static osThreadId_t s_acq_task;
static osThreadId_t s_sweep_task;

static volatile uint32_t s_wr_words;     /* 已写入环的 word 总数 */
static volatile uint32_t s_consumed;     /* 已消费的半帧数 */
static volatile uint32_t s_state;
static volatile uint32_t s_trig_abs;
static volatile uint32_t s_post_words;
static volatile uint32_t s_miss;

/* --------------------------------------------------------------------------
 * 半帧搬运
 * ------------------------------------------------------------------------ */
static void move_one_half(void)
{
    uint32_t half = s_consumed & 1u;                  /* 0 = 前半, 1 = 后半 */
    uint32_t src1 = OSC_CH1_BUF_ADDR + half * HALF_BYTES_CH1;
    uint32_t src2 = OSC_CH2_BUF_ADDR + half * HALF_BYTES_CH2;
    uint32_t off  = (s_wr_words & OSC_REGION_MASK);
    uint32_t dst1 = OSC_SDRAM_CH1_BASE + off * 4u;
    uint32_t dst2 = OSC_SDRAM_CH2_BASE + off * 2u;

    /* 残留的 TEIF 会让通道在使能的瞬间被掐死, 而且清 TEIF 会一起清掉 CESR
     * (错误原因读成 0)。开跑前必须清干净。 */
    hmdma_mdma_channel0_sw_0.Instance->CIFCR = 0x3Fu;
    hmdma_mdma_channel1_sw_0.Instance->CIFCR = 0x3Fu;

    if (HAL_MDMA_Start(&hmdma_mdma_channel0_sw_0, src1, dst1, HALF_BYTES_CH1, 1u) != HAL_OK) {
        s_miss++;
        return;
    }
    (void)HAL_MDMA_PollForTransfer(&hmdma_mdma_channel0_sw_0, HAL_MDMA_FULL_TRANSFER, MDMA_TIMEOUT_MS);

    if (HAL_MDMA_Start(&hmdma_mdma_channel1_sw_0, src2, dst2, HALF_BYTES_CH2, 1u) != HAL_OK) {
        s_miss++;
        return;
    }
    (void)HAL_MDMA_PollForTransfer(&hmdma_mdma_channel1_sw_0, HAL_MDMA_FULL_TRANSFER, MDMA_TIMEOUT_MS);

    s_wr_words += OSC_HALF_WORDS;
    OSC_CTRL->diag_miss = s_miss;
}

/* --------------------------------------------------------------------------
 * 触发状态机
 * ------------------------------------------------------------------------ */
void OSC_Acq_Rearm(void)
{
    s_trig_abs   = OSC_TRIG_NONE;
    s_post_words = 0u;
    s_state      = OSC_TRIG_RUN;
}

static void trig_on_half(uint32_t tpos)
{
    if (s_state != OSC_TRIG_RUN) return;

    /* 刚搬完的半帧起点是 s_wr_words - HALF, 触发点在半帧内偏移 tpos 个 word */
    s_trig_abs   = (s_wr_words - OSC_HALF_WORDS) + tpos;
    s_post_words = 0u;
    s_state      = OSC_TRIG_ARMED;
}

/* --------------------------------------------------------------------------
 * 通知处理: 以 seq 为唯一真相, HSEM 只是"来看一眼"的闹钟
 * ------------------------------------------------------------------------ */
static void acq_service(void)
{
    uint32_t seq = OSC_CTRL->seq_ch1;
    if (seq == s_consumed) return;

    uint32_t tpos  = OSC_CTRL->trig_pos_ch1;
    uint32_t flags = OSC_CTRL->trig_flags;

    /* CM4 停止采集后会有一段 seq 空洞; 落后超过一整帧说明中间那些半帧
     * 已经被 DMA 覆盖了, 搬了也是坏数据 —— 记账然后跳到最新。 */
    if ((uint32_t)(seq - s_consumed) > 2u) {
        s_miss += (uint32_t)(seq - s_consumed) - 2u;
        s_consumed = seq - 2u;
        OSC_CTRL->diag_miss = s_miss;
    }

    while (s_consumed != seq) {
        if (s_state != OSC_TRIG_FROZEN) {
            move_one_half();
            if (s_state == OSC_TRIG_ARMED) {
                s_post_words += OSC_HALF_WORDS;
                if (s_post_words >= POST_WORDS) s_state = OSC_TRIG_FROZEN;
            }
        }
        s_consumed++;
    }

    if (flags & 1u) trig_on_half(tpos);

    /* 冻结了, 或者自由运行中攒够了数据, 就去刷一屏 */
    if (s_state == OSC_TRIG_FROZEN || OSC_Acq_IsWarm()) {
        OSC_Disp_RequestSweep();
    }
}

/* --------------------------------------------------------------------------
 * 中断与任务
 * ------------------------------------------------------------------------ */
void HSEM1_IRQHandler(void)
{
    uint32_t mask = __HAL_HSEM_SEMID_TO_MASK(OSC_HSEM_ID);

    if ((HSEM->C1ISR & mask) != 0u) {
        HSEM->C1ICR = mask;              /* 先清, 免得丢掉紧随其后的一次 */
        BaseType_t woken = pdFALSE;
        if (s_acq_task != NULL) {
            vTaskNotifyGiveFromISR((TaskHandle_t)s_acq_task, &woken);
        }
        portYIELD_FROM_ISR(woken);
    }
}

static void acq_task(void *argument)
{
    (void)argument;

    /* 等 CM4 把控制块置成有效。CM4 是在置 magic 之后才启动 DMA 的,
     * 所以看到 magic 就等于看到一份完整的默认值。 */
    for (uint32_t i = 0; i < 1000u && OSC_CTRL->magic != OSC_SHARED_MAGIC; i++) {
        osDelay(1);
    }
    if (OSC_CTRL->magic != OSC_SHARED_MAGIC) {
        uart_printf("[OSC] CM4 控制块未就绪, magic=0x%08lX\r\n", (unsigned long)OSC_CTRL->magic);
        vTaskDelete(NULL);
    }

    /* 只有 magic 有效之后才允许写 cfg_*, 否则会被 CM4 的初始化覆盖 */
    OSC_CTRL->cfg_trig_level  = 2047u;   /* 0 V 附近 */
    OSC_CTRL->cfg_trig_hyst   = 16u;
    OSC_CTRL->cfg_trig_edge   = 0u;
    OSC_CTRL->cfg_trig_enable = 1u;
    OSC_CTRL->cfg_run         = 1u;
    __DMB();

    OSC_Acq_Rearm();

    /* 任务句柄有效之后才武装通知 */
    HSEM->C1IER |= __HAL_HSEM_SEMID_TO_MASK(OSC_HSEM_ID);

    uart_printf("[OSC] CM7 搬运任务启动\r\n");

    for (;;) {
        /* 10 ms 超时是心跳: 万一漏了一次通知, 也能靠 seq 补上 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        acq_service();
    }
}

/* --------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------ */
void OSC_Acq_Init(void)
{
    uint32_t mask = __HAL_HSEM_SEMID_TO_MASK(OSC_HSEM_ID);

    __HAL_RCC_HSEM_CLK_ENABLE();
    HSEM->C1ICR = mask;                  /* 清掉 boot 阶段可能残留的通知 */
    HSEM->C1IER &= ~mask;                /* 任务建好之前先关, 免得喂空句柄 */

    HAL_NVIC_SetPriority(HSEM1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(HSEM1_IRQn);

    OSC_Disp_Init();
}

void OSC_Acq_CreateTasks(void)
{
    const osThreadAttr_t acq_attr = {
        .name = "oscAcq",
        .stack_size = OSC_TASK_STACK,
        .priority = (osPriority_t)osPriorityAboveNormal,
    };
    s_acq_task = osThreadNew(acq_task, NULL, &acq_attr);

    /* 抽取任务必须比采集任务低: 慢时基下一次抽取要 331 ms,
     * 但采集任务每 655 µs 就要醒一次, 靠优先级抢占保证不丢帧。 */
    OSC_Disp_CreateTask();
}

uint32_t OSC_Acq_GetWriteWords(void) { return s_wr_words; }
uint32_t OSC_Acq_GetTrigAbs(void)    { return s_trig_abs; }
int      OSC_Acq_IsFrozen(void)      { return s_state == OSC_TRIG_FROZEN; }
int      OSC_Acq_IsWarm(void)        { return s_wr_words >= OSC_REGION_WORDS; }

void OSC_Acq_DiagPrint(void)
{
    uart_printf("[OSC] wr=%lu seq=%lu state=%lu trig=%lu miss=%lu\r\n",
                (unsigned long)s_wr_words, (unsigned long)s_consumed,
                (unsigned long)s_state,    (unsigned long)s_trig_abs,
                (unsigned long)s_miss);
    uart_printf("      rate1=%lu rate2=%lu ovr1=%lu ovr2=%lu\r\n",
                (unsigned long)OSC_CTRL->diag_rate_ch1,
                (unsigned long)OSC_CTRL->diag_rate_ch2,
                (unsigned long)OSC_CTRL->diag_ovr_ch1,
                (unsigned long)OSC_CTRL->diag_ovr_ch2);
}
```

**本任务会删掉的 spike 代码**：`CM7/Mylibrary/Src/oscillograph.c` 里原来那 580 行的 raw/阶梯/memcpy/写测试全部删掉，整个文件用上面的内容替换。

- [ ] **步骤 3：把 `osc_display` 的空壳先建出来**

任务 5 只是让搬运跑通，显示抽取在任务 6。为了能单独编译，先建最小可用的两个文件。

`CM7/Mylibrary/Inc/osc_display.h`：

```c
#ifndef __OSC_DISPLAY_H
#define __OSC_DISPLAY_H

#include <stdint.h>

#define OSC_DISP_POINTS   512u
#define OSC_DISP_GROUPS   (OSC_DISP_POINTS / 2u)   /* 256 组 (min,max) */

void OSC_Disp_Init(void);
void OSC_Disp_CreateTask(void);
void OSC_Disp_RequestSweep(void);

#endif /* __OSC_DISPLAY_H */
```

`CM7/Mylibrary/Src/osc_display.c`：

```c
#include "osc_display.h"
#include "oscillograph.h"
#include "uart_printf.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

static osThreadId_t s_sweep_task;

static void sweep_task(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* 任务 6 在这里填 min/max 抽取 */
    }
}

void OSC_Disp_Init(void)
{
}

void OSC_Disp_CreateTask(void)
{
    const osThreadAttr_t attr = {
        .name = "oscSweep",
        .stack_size = 2048u,
        .priority = (osPriority_t)osPriorityNormal,
    };
    s_sweep_task = osThreadNew(sweep_task, NULL, &attr);
}

void OSC_Disp_RequestSweep(void)
{
    if (s_sweep_task != NULL) {
        xTaskNotifyGive((TaskHandle_t)s_sweep_task);
    }
}
```

- [ ] **步骤 4：改 `CM7/CMakeLists.txt` 加新源文件**

第 75-81 行的 `target_sources` 加上：

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/osc_display.c
```

- [ ] **步骤 5：改 `CM7/Core/Src/main.c`**

`USER CODE BEGIN 2` 段里 `OSC_Step0_Run();` 那一行替换成：

```c
  OSC_Acq_Init();           /* HSEM 中断接线; 任务在 freertos.c 里建 */
```

并且 `#include "oscillograph.h"` 那一行保留不动（第 39 行）。

- [ ] **步骤 6：改 `CM7/Core/Src/freertos.c`**

`USER CODE BEGIN RTOS_THREADS` 段（现在只有 `xTaskCreate(TouchGFX_Task, ...)`）下面加一行：

```c
  OSC_Acq_CreateTasks();
```

和顶部 `USER CODE BEGIN Includes` 段里加：

```c
#include "oscillograph.h"
```

- [ ] **步骤 7：编译**

```bash
cmake -S CM7 -B CM7/build -G Ninja && cmake --build CM7/build
```

预期 0 error 0 warning。

- [ ] **步骤 8：烧录并验证**

同时烧录 CM4（任务 4 版本，`cfg_run` 已被 CM7 置 1）和 CM7。CM7 的 USART1 预期：

```
[OSC] CM7 搬运任务启动
```

然后在 `acq_task` 的 `for(;;)` 里临时加一句每秒打印（或者用调试器看 `s_wr_words`）：

```c
    static uint32_t last_dbg;
    if (s_wr_words - last_dbg >= OSC_REGION_WORDS) {   /* 每绕一圈打一次 */
        last_dbg = s_wr_words;
        OSC_Acq_DiagPrint();
    }
```

预期输出：

```
[OSC] wr=4194304 seq=2048 state=0 trig=4294967295 miss=0
```

**逐条判定：**
- **`wr` 的增长速度必须是 3,125,000 word/s**（每秒绕环 0.745 圈）。用两次打印的时间差算：`Δwr / Δt ≈ 3.125e6`。慢了就是丢帧。
- **`miss` 必须保持 0**。持续增长说明搬运跟不上 —— 先看是不是 `HAL_MDMA_PollForTransfer` 超时（`MDMA_TIMEOUT_MS` 调到 50 再试）。
- **`seq` 和 CM4 打印的 `seq1` 对得上**（同一时刻两边应该很接近）。差很多说明通知链路有问题。
- **`state = 0`（RUN）**，`trig = 4294967295`（无触发）—— 因为 `trig_enable=1` 但没有信号，正确。
- **数据内容比对**：这一步必须做，因为它验证的是"搬对了"而不只是"搬了"。把 `OSC_Acq_DiagPrint()` 里临时加：

```c
    uart_printf("      sdram[0]=0x%08lX [1]=0x%08lX [100]=0x%08lX\r\n",
                (unsigned long)((volatile uint32_t *)OSC_SDRAM_CH1_BASE)[0],
                (unsigned long)((volatile uint32_t *)OSC_SDRAM_CH1_BASE)[1],
                (unsigned long)((volatile uint32_t *)OSC_SDRAM_CH1_BASE)[100]);
```

ADC 悬空时输入应为 0 附近或漂移的小值，`(word & 0xFFF)` 与 `(word >> 16) & 0xFFF` 都应在 0..4095。**出现 0xFFFFFFFF 或全 0 说明 SDRAM 没写进去**（MDMA 失败被 `s_miss++` 掩盖了 —— 检查 `HAL_MDMA_PollForTransfer` 的返回值，把它也打出来）。

**失败排查顺序：**
1. `[OSC] CM4 控制块未就绪` → CM4 没跑起来，或 `OSC_CH_Init()` 卡在地址自检。先单独验证 CM4（任务 2 步骤 5）。
2. `wr` 完全不涨 → HSEM 通知没进来。检查 `HSEM->C1IER` 是否真的被置上、`HSEM1_IRQHandler` 有没有被链接进去（`arm-none-eabi-nm CM7/build/*.elf | grep HSEM1`）。
3. `wr` 在涨但 `miss` 涨 → MDMA 失败。在 `move_one_half()` 里把 `HAL_MDMA_PollForTransfer` 的返回值和 `Instance->CESR` 打出来。若 `CESR = 0x800` (BSE)，说明 `BufferTransferLength` 被改回了 1。
4. 系统卡死/`Default_Handler` → `HSEM1_IRQn` 的优先级 5 和 FreeRTOS 的 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 冲突。检查 `FreeRTOSConfig.h`：必须满足 `5 >= configMAX_SYSCALL_INTERRUPT_PRIORITY` 的数值语义（Cortex-M 里数值越大优先级越低）。若 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 是 5，把 HSEM 优先级改成 6。

---

## 任务 6：显示通路（抽取模块 + Model/Presenter/View，先固定 step=8 打通）

**文件：**
- 改：`CM7/Mylibrary/Inc/osc_display.h`、`CM7/Mylibrary/Src/osc_display.c`
- 改：`CM7/TouchGFX/gui/include/gui/model/Model.hpp`、`src/model/Model.cpp`
- 改：`CM7/TouchGFX/gui/include/gui/oscillograph_screen/OscillographView.hpp`、`src/oscillograph_screen/OscillographView.cpp`
- 改：`CM7/TouchGFX/gui/include/gui/oscillograph_screen/OscillographPresenter.hpp`、`src/oscillograph_screen/OscillographPresenter.cpp`

**接口：**
- 消费：任务 5 的 `OSC_Acq_GetWriteWords()` / `OSC_Acq_IsWarm()` / `OSC_REGION_MASK`
- 产出：
  - `int OSC_Disp_TakeDirty(void);` — 有新帧返回 1 并清标志
  - `const uint16_t *OSC_Disp_GetCh1(void);` / `const uint16_t *OSC_Disp_GetCh2(void);`
  - `uint32_t OSC_Disp_GetPoints(void);` — 恒为 512
  - `void OSC_Disp_SetTimebase(uint32_t idx);` / `uint32_t OSC_Disp_GetTimebase(void);`

**本任务用定长抽样（每 `step` 个 word 取第一个），任务 7 再换成 min/max。** 这样能把"内存路径对不对"和"抽取算法对不对"分开验证。

- [ ] **步骤 1：写 `osc_display.h` 完整版**

```c
#ifndef __OSC_DISPLAY_H
#define __OSC_DISPLAY_H

#include <stdint.h>

#define OSC_DISP_POINTS   512u
#define OSC_DISP_GROUPS   (OSC_DISP_POINTS / 2u)   /* 256 组 (min,max) */

void OSC_Disp_Init(void);
void OSC_Disp_CreateTask(void);
void OSC_Disp_RequestSweep(void);

/* 下面四个由 TouchGFX 任务(UI 线程)调用 */
int             OSC_Disp_TakeDirty(void);
const uint16_t *OSC_Disp_GetCh1(void);
const uint16_t *OSC_Disp_GetCh2(void);
uint32_t        OSC_Disp_GetPoints(void);

void     OSC_Disp_SetTimebase(uint32_t idx);
uint32_t OSC_Disp_GetTimebase(void);
uint32_t OSC_Disp_GetStep(void);

#endif /* __OSC_DISPLAY_H */
```

**这个头文件刻意不包含 `osc_shared.h`**：它会被 C++ 的 `Model.cpp` 包含，而 `osc_shared.h` 里有 `_Static_assert`，跨语言不干净。用 `extern "C"` 包一下就行。

- [ ] **步骤 2：写 `osc_display.c` 完整版（本任务先只有定长抽样）**

```c
#include "osc_display.h"
#include "oscillograph.h"
#include "osc_shared.h"
#include "uart_printf.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

#define SWEEP_TASK_STACK  2048u

/* 双缓冲: [0/1][通道 0/1][点]。
 * 抽取任务只写 back, 写完把 front 指过去; UI 拿到的 s_ui_idx 在整个
 * 一帧的绘制期间不会被改写。 */
static uint16_t s_buf[2][2][OSC_DISP_POINTS];
static volatile uint32_t s_front;
static volatile uint32_t s_back;
static volatile uint32_t s_ui_idx;
static volatile uint32_t s_dirty;
static volatile uint32_t s_timebase_idx;

static osThreadId_t s_sweep_task;

/* 任务 6 先用定长抽样, 任务 7 换成 min/max */
static void sweep_once(void)
{
    uint32_t wr    = OSC_Acq_GetWriteWords();
    uint32_t step  = 8u;                       /* 临时固定值, 任务 7 换成查表 */
    uint32_t span  = OSC_DISP_GROUPS * step;
    uint32_t start = (wr >= span) ? (wr - span) : 0u;

    const volatile uint32_t *ch1 = (const volatile uint32_t *)OSC_SDRAM_CH1_BASE;
    const volatile uint16_t *ch2 = (const volatile uint16_t *)OSC_SDRAM_CH2_BASE;

    uint32_t i = start & OSC_REGION_MASK;
    uint16_t *o1 = s_buf[s_back][0];
    uint16_t *o2 = s_buf[s_back][1];

    for (uint32_t g = 0; g < OSC_DISP_GROUPS; g++) {
        uint32_t w = ch1[i];
        *o1++ = (uint16_t)(w & 0x0FFFu);
        *o1++ = (uint16_t)((w >> 16) & 0x0FFFu);

        *o2++ = (uint16_t)(ch2[i] & 0x0FFFu);
        *o2++ = (uint16_t)(ch2[i] & 0x0FFFu);   /* 定长抽样先复制两点占位 */

        i = (i + step) & OSC_REGION_MASK;
    }
}

static void sweep_task(void *argument)
{
    (void)argument;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* 上一帧还没被 UI 取走就跳过本轮: 否则会写进 UI 正在读的那份缓冲 */
        if (s_dirty) continue;
        if (!OSC_Acq_IsWarm() && !OSC_Acq_IsFrozen()) continue;

        sweep_once();

        s_front = s_back;
        s_back ^= 1u;
        s_dirty = 1u;
    }
}

void OSC_Disp_Init(void)
{
    s_timebase_idx = 3u;      /* 档 3: step=8, 655 µs —— 最容易看出对错 */
}

void OSC_Disp_CreateTask(void)
{
    const osThreadAttr_t attr = {
        .name = "oscSweep",
        .stack_size = SWEEP_TASK_STACK,
        .priority = (osPriority_t)osPriorityNormal,
    };
    s_sweep_task = osThreadNew(sweep_task, NULL, &attr);
}

void OSC_Disp_RequestSweep(void)
{
    if (s_sweep_task != NULL) {
        xTaskNotifyGive((TaskHandle_t)s_sweep_task);
    }
}

int OSC_Disp_TakeDirty(void)
{
    if (!s_dirty) return 0;
    s_ui_idx = s_front;      /* 冻结本次要读的缓冲 */
    s_dirty  = 0u;
    return 1;
}

const uint16_t *OSC_Disp_GetCh1(void) { return s_buf[s_ui_idx][0]; }
const uint16_t *OSC_Disp_GetCh2(void) { return s_buf[s_ui_idx][1]; }
uint32_t OSC_Disp_GetPoints(void)     { return OSC_DISP_POINTS; }

void OSC_Disp_SetTimebase(uint32_t idx) { s_timebase_idx = idx; }
uint32_t OSC_Disp_GetTimebase(void)     { return s_timebase_idx; }
uint32_t OSC_Disp_GetStep(void)         { return 8u; }   /* 任务 7 换成查表 */
```

**`s_dirty` 那道闸门是必须的，不是保险。** 只有两个缓冲时，如果抽取任务在 UI 取走上一帧之前又刷一帧，它就会写进 `s_buf[s_ui_idx]` —— 也就是 UI 正在画的那份。有了这道闸门，`s_back` 永远是 UI 没在读的那份。

- [ ] **步骤 3：改 `Model.hpp` / `Model.cpp`**

`Model.hpp` 整个替换成：

```cpp
#ifndef MODEL_HPP
#define MODEL_HPP

#include <stdint.h>

class ModelListener;

class Model
{
public:
    Model();

    void bind(ModelListener* listener)
    {
        modelListener = listener;
    }

    void tick();

    /** 有新的一屏数据则返回 true 并消费掉 dirty 标志 */
    bool takeDisplayUpdate();

    /** 只在 takeDisplayUpdate() 返回 true 之后调用 */
    const uint16_t* getDisplayCh1() const;
    const uint16_t* getDisplayCh2() const;
    uint32_t getDisplayPoints() const;

    void setTimebase(uint32_t idx);
    uint32_t getTimebase() const;

protected:
    ModelListener* modelListener;
};

#endif // MODEL_HPP
```

`Model.cpp` 整个替换成：

```cpp
#include <gui/model/Model.hpp>
#include <gui/model/ModelListener.hpp>

extern "C" {
#include "osc_display.h"
}

Model::Model() : modelListener(0)
{
}

void Model::tick()
{
    /* 抽取在 oscSweep 任务里做, 这里只负责推进 TouchGFX 内部状态。
     * 千万不要在这里做抽取: 慢时基下一次要 331 ms, 会把界面整个卡住。 */
}

bool Model::takeDisplayUpdate()
{
    return OSC_Disp_TakeDirty() != 0;
}

const uint16_t* Model::getDisplayCh1() const
{
    return OSC_Disp_GetCh1();
}

const uint16_t* Model::getDisplayCh2() const
{
    return OSC_Disp_GetCh2();
}

uint32_t Model::getDisplayPoints() const
{
    return OSC_Disp_GetPoints();
}

void Model::setTimebase(uint32_t idx)
{
    OSC_Disp_SetTimebase(idx);
}

uint32_t Model::getTimebase() const
{
    return OSC_Disp_GetTimebase();
}
```

**注意 `extern "C" { }` 包住 include** —— `osc_display.h` 是 C 头文件，不包会链接报 `undefined reference to OSC_Disp_TakeDirty()`（C++ 名字修饰）。

- [ ] **步骤 4：改 `OscillographView.hpp` / `.cpp`**

`OscillographView.hpp` 的 `protected:` 上面加两行：

```cpp
    /** TouchGFX 每帧对"当前屏幕"调一次, 见 touchgfx/Screen.hpp */
    virtual void handleTickEvent();

    /** n = 512: 256 组 (min,max) 拼成的 CH1 / CH2 显示缓冲 */
    void setGraphData(const uint16_t* ch1, const uint16_t* ch2, uint32_t n);
```

`OscillographView.cpp` 加实现：

```cpp
void OscillographView::handleTickEvent()
{
    /* 屏幕每帧被调一次。本轮该刷哪一帧的判断归 Presenter, 这里只转一手。 */
    presenter->handleDisplayTick();
}

void OscillographView::setGraphData(const uint16_t* ch1, const uint16_t* ch2, uint32_t n)
{
    /* clear() 内部已经 invalidate 过一次, 但 addDataPoint() 不会,
     * 所以填完还要再 invalidate 一次。 */
    dynamicGraph1.clear();
    for (uint32_t i = 0; i < n; i++)
    {
        dynamicGraph1.addDataPoint(ch1[i]);
    }
    dynamicGraph1.invalidate();

    dynamicGraph2.clear();
    for (uint32_t i = 0; i < n; i++)
    {
        dynamicGraph2.addDataPoint(ch2[i]);
    }
    dynamicGraph2.invalidate();
}
```

**三个必须记住的点：**

1. **方法名是 `clear()`，不是 `clearGraph()`。** TouchGFX 4.26 的 `AbstractDataGraph` 里没有 `clearGraph` 这个名字（`AbstractDataGraph.hpp:690`）。写错会编译不过。
2. **`clear()` 之后要填满正好 512 个点。** `GraphWrapAndOverwrite<512>` 填满之后才开始覆盖回绕，正好 512 个就不会触发回绕。少填或多填都会让图形的点数状态不对。
3. **Y 轴范围不用管。** 生成的 `OscillographViewBase.cpp:44,57` 里已经是 `setGraphRangeY(0, 4096)`，直接喂 0..4095 的 ADC 原始值就行，不需要任何缩放。（这一条单独写出来是因为：万一范围被改过，症状会是「波形全部贴在顶端一条直线」，很容易误判成数据通路坏了。）

`presenter` 是 `touchgfx::View<T>` 的 protected 成员（`framework/include/mvp/View.hpp:56`），`OscillographView` 继承自 `OscillographViewBase : public touchgfx::View<OscillographPresenter>`，可以直接用。

- [ ] **步骤 5：改 `OscillographPresenter.hpp` / `.cpp`**

**注意不要写成 `virtual void tick()`。** `touchgfx::Presenter` 里没有 `tick()`（`framework/include/mvp/Presenter.hpp` 只有 `activate()` / `deactivate()`），在子类里新加一个也不会有人调 —— 挂点是 View 的 `handleTickEvent()`，不是 Presenter 的 `tick()`。

`OscillographPresenter.hpp` 的 `private:` 上面加：

```cpp
    /** 由 OscillographView::handleTickEvent() 每帧调用 */
    void handleDisplayTick();
```

`OscillographPresenter.cpp` 加：

```cpp
void OscillographPresenter::handleDisplayTick()
{
    /* 只在有新数据时才清图重画。无条件每帧重画在慢时基下会闪。 */
    if (!model->takeDisplayUpdate())
    {
        return;
    }
    view.setGraphData(model->getDisplayCh1(),
                      model->getDisplayCh2(),
                      model->getDisplayPoints());
}
```

`model` 是 `ModelListener` 的 protected 成员（`ModelListener.hpp:16`），`OscillographPresenter` 继承自 `ModelListener`，可以直接用。

- [ ] **步骤 6：编译并烧录**

```bash
cmake --build CM7/build
```

烧录 CM4 + CM7。

**验证：**
1. 进到示波器界面，两个折线图应该有波形在动。**没有输入信号时由于 CH1 悬空、噪声被放大，看到的应该是快速抖动的噪声带**，这是正常的。
2. 屏幕上一条平直的 0 线，或者波形全贴在顶端 → 前者说明 SDRAM 里全是 0（回任务 5 步骤 8 的第 5 条排查）；后者说明 Y 轴范围被改过，检查 `OscillographViewBase.cpp:44,57` 是不是 `setGraphRangeY(0, 4096)`。
3. 画面卡住不动 → 抽取任务或 UI 通路断了。在 `sweep_task` 的 `s_dirty = 1u;` 那行下断点。若断点不命中，说明 `handleTickEvent()` 没被调到 —— 确认它写成了 `virtual void handleTickEvent()` 并且 `OscillographView.cpp` 里真的实现了（不是只声明）。
4. 只有 graph1 有波形 → `dynamicGraph2` 的调用被 graph1 盖住了。这是设计文档 §9.4 记录的已知问题（两个图位置完全重叠在 `(0, 69, 674, 361)`），**本计划不解决**。
5. 界面卡顿/触摸无响应 → 抽取任务优先级配错了。确认 `oscSweep` 是 `osPriorityNormal`，低于 TouchGFX 任务的 `osPriorityAboveNormal1`。
6. **这是预期的**：切到别的界面（比如 Logic 界面）时波形会停住不动 —— `handleTickEvent()` 只对「当前屏幕」调用，离开后 `s_dirty` 一直挂着，抽取任务就停在那儿空转。这正是我们想要的（没人看的时候不烧 CPU），回到示波器界面会立刻恢复。若你希望离开界面也继续采集，那就是另一件事，本计划不做。

- [ ] **步骤 7：补 LTDC 欠载中断（诊断用）**

设计文档 §10.5 的前提不对 —— `HAL_LTDC_Init`（`stm32h7xx_hal_ltdc.c:304`）已经使能了 `LTDC_IT_TE | LTDC_IT_FU`，缺的是**错误中断线的 NVIC 使能和处理函数**。`LTDC_IRQn = 88`（行中断）和 `LTDC_ER_IRQn = 89`（错误中断）是两条独立的 NVIC 线，我们只要后者。`LTDC->ICR` 寄存器存在（偏移 `0x3C`），可以直接写 1 清标志。

在 `CM7/Mylibrary/Src/osc_display.c` 末尾追加：

```c
static volatile uint32_t s_ltdc_err;

/* LTDC_ER_IRQHandler 在启动文件里是 weak 符号, 这里强定义顶掉它。
 * 自己写在 Mylibrary 里, CubeMX 重新生成不会覆盖。 */
void LTDC_ER_IRQHandler(void)
{
    uint32_t isr = LTDC->ISR;
    if ((isr & (LTDC_ISR_TERRIF | LTDC_ISR_FUIF)) != 0u) {
        s_ltdc_err++;
        LTDC->ICR = (LTDC_ISR_TERRIF | LTDC_ISR_FUIF);
    }
}

uint32_t OSC_Disp_GetLtdcErrors(void)
{
    return s_ltdc_err;
}
```

在 `OSC_Disp_Init()` 里加：

```c
    HAL_NVIC_SetPriority(LTDC_ER_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(LTDC_ER_IRQn);
```

`osc_display.h` 里加声明：

```c
uint32_t OSC_Disp_GetLtdcErrors(void);
```

然后在 `OSC_Acq_DiagPrint()` 里加一行打印这个计数。

**判定**：`s_ltdc_err` 保持 0 或偶尔 +1 是健康的。持续快速增长说明 SDRAM 带宽被抽取任务挤爆了，画面会撕裂 —— 那就要考虑设计文档 §13 里说的「让深存储区可缓存 + MDMA 后 invalidate」这条路。

---

## 任务 7：时基与 min/max 抽取

**文件：**
- 改：`CM7/Mylibrary/Inc/osc_display.h`
- 改：`CM7/Mylibrary/Src/osc_display.c`

**接口：**
- 消费：任务 6 的 `sweep_once()` 结构与双缓冲
- 产出：`OSC_Disp_GetStep()` 返回查表值；`OSC_Disp_SetTimebase(idx)` 接受 0..13

**时基公式：** 一屏 512 点 = 256 组，每组覆盖 `step` 个 word，每 word 320 ns。

```
T = 256 × step × 320 ns = step × 81.92 µs
```

**抽取单帧耗时（实测推算）：** 每次访问 18.93 cyc @480 MHz，一帧访问 512×step 次（CH1 256×step 个 word + CH2 256×step 个 halfword）。

```
T_sweep ≈ 512 × step × 18.93 / 480e6 = step × 20.2 µs
```

- [ ] **步骤 1：在 `osc_display.c` 里加时基表**

```c
typedef struct {
    uint32_t step;      /* 每组的 word 数 */
    uint32_t window_us; /* 一屏 512 点覆盖的时间 */
} osc_timebase_t;

/* T = 256 × step × 320 ns = step × 81.92 µs */
static const osc_timebase_t s_timebase[] = {
    {     1u,     81920u },   /* 档  0:  81.9 µs */
    {     2u,    163840u },   /* 档  1: 163.8 µs */
    {     4u,    327680u },   /* 档  2: 327.7 µs */
    {     8u,    655360u },   /* 档  3: 655.4 µs */
    {    20u,   1638400u },   /* 档  4: 1.638 ms */
    {    40u,   3276800u },   /* 档  5: 3.277 ms */
    {    80u,   6553600u },   /* 档  6: 6.554 ms */
    {   200u,  16384000u },   /* 档  7: 16.38 ms */
    {   400u,  32768000u },   /* 档  8: 32.77 ms */
    {   800u,  65536000u },   /* 档  9: 65.54 ms */
    {  2000u, 163840000u },   /* 档 10: 163.8 ms */
    {  4000u, 327680000u },   /* 档 11: 327.7 ms */
    {  8000u, 655360000u },   /* 档 12: 655.4 ms */
    { 16384u,      0u     },  /* 档 13: 1.342 s (溢出 u32, 不填) */
};

#define OSC_TIMEBASE_COUNT (sizeof(s_timebase) / sizeof(s_timebase[0]))
```

**档 13 的 `window_us` 是 1,342,177,280 µs，超过 `uint32_t`**。这个字段只用于界面显示，改成 `uint64_t` 或者干脆档 13 存 0 由界面特判。这里存 0，界面读到 0 就显示「1.342 s」。

- [ ] **步骤 2：把 `sweep_once()` 换成 min/max 版本**

```c
/* min/max 抽取: 每组 step 个 word 压成一对 (min, max)。
 * 逐点抽样会整个漏掉窄脉冲, min/max 是示波器的标准做法。
 *
 * 两个附带性质:
 * 1. min/max 与样本先后无关 —— 就算 CH1_ADC1/CH1_ADC2 判反了, 结果照样对。
 * 2. CH1 一个 word 含 2 个样本, CH2 一个 word 含 1 个, 所以 CH2 内层循环
 *    多读一个 word 才能凑满 step*2 个样本, 两通道时间跨度才一致。 */
static void sweep_once(void)
{
    uint32_t wr    = OSC_Acq_GetWriteWords();
    uint32_t step  = s_timebase[s_timebase_idx].step;
    uint32_t span  = OSC_DISP_GROUPS * step;
    uint32_t start = (wr >= span) ? (wr - span) : 0u;

    const volatile uint32_t *ch1 = (const volatile uint32_t *)OSC_SDRAM_CH1_BASE;
    const volatile uint16_t *ch2 = (const volatile uint16_t *)OSC_SDRAM_CH2_BASE;

    uint32_t i = start & OSC_REGION_MASK;
    uint16_t *o1 = s_buf[s_back][0];
    uint16_t *o2 = s_buf[s_back][1];

    for (uint32_t g = 0; g < OSC_DISP_GROUPS; g++) {
        uint32_t lo1 = 0xFFFFu, hi1 = 0u;
        uint32_t lo2 = 0xFFFFu, hi2 = 0u;

        for (uint32_t k = 0; k < step; k++) {
            uint32_t w = ch1[i];
            uint32_t a =  w        & 0x0FFFu;
            uint32_t b = (w >> 16) & 0x0FFFu;
            if (a < lo1) lo1 = a;
            if (a > hi1) hi1 = a;
            if (b < lo1) lo1 = b;
            if (b > hi1) hi1 = b;

            uint32_t v = ch2[i] & 0x0FFFu;
            if (v < lo2) lo2 = v;
            if (v > hi2) hi2 = v;

            /* 绕回只在内层判断一次; REGION_WORDS 是 2 的幂, 用 & 代替 % */
            i = (i + 1u) & OSC_REGION_MASK;
        }

        if (lo1 > hi1) { lo1 = 0u; hi1 = 0u; }   /* step 为奇数时的空组 */
        if (lo2 > hi2) { lo2 = 0u; hi2 = 0u; }

        *o1++ = (uint16_t)lo1;  *o1++ = (uint16_t)hi1;
        *o2++ = (uint16_t)lo2;  *o2++ = (uint16_t)hi2;
    }
}
```

**注意 `ch2` 是 `uint16_t` 数组但按 word 寻址会错位。** 上面 `ch2[i]` 用的是同一个 `i`（word 下标），而 `ch2` 是 `uint16_t*`，所以 `ch2[i]` 实际读的是第 `i` 个 halfword —— 对 CH2 来说 `halfword` 下标就是 word 下标，两者一致，这样写是对的，**但必须保持 `i` 的语义是"word 下标"**。不要在 CH2 上做任何 `*2` 的换算。

- [ ] **步骤 3：加 setter/getter**

```c
void OSC_Disp_SetTimebase(uint32_t idx)
{
    if (idx >= OSC_TIMEBASE_COUNT) idx = OSC_TIMEBASE_COUNT - 1u;
    s_timebase_idx = idx;
    s_dirty = 0u;              /* 丢掉可能排队中的旧时基帧 */
    OSC_Disp_RequestSweep();
}

uint32_t OSC_Disp_GetStep(void)
{
    return s_timebase[s_timebase_idx].step;
}
```

`OSC_Disp_Init()` 里把 `s_timebase_idx = 3u;` 保留（档 3 最容易看出对错）。

- [ ] **步骤 4：编译、烧录、逐档验证**

```bash
cmake --build CM7/build
```

在 `acq_task` 的心跳打印里调 `OSC_Disp_SetTimebase(...)` 逐档切换，或用调试器直接写 `s_timebase_idx`。逐档检查：

| 档 | step | 预期时间窗 | 预期波形的样子 |
|---|---|---|---|
| 3 | 8 | 655 µs | 噪声带仍然很密 |
| 7 | 200 | 16.4 ms | 噪声带变疏，能看到慢的漂移 |
| 10 | 2000 | 164 ms | 明显能看到毫秒级的变化 |
| 13 | 16384 | 1.342 s | 刷新明显变慢（约 3 fps），波形很"横" |

**判定：**
- 每换一档，波形的**横向密度**要跟着变。不变就是 `step` 没生效。
- 档 13 刷新率约 3 fps 是**预期的**（不是 bug）。设计文档 §8 说 160 ms / 6.2 fps 是只算了 CH1 的笔误，实际 331 ms。感到太慢的话，按设计文档 §12 走「min/max 归约金字塔」，本计划不做。
- 两通道的波形横向要**严格对齐**（同一个 `step`）。错位说明 `i` 的语义被搞混了。
- 切档时如果画面撕裂或卡顿超过 1 秒 → `OSC_Disp_SetTimebase()` 里清了 `s_dirty` 但抽取任务正好在写 back 缓冲。去掉那行 `s_dirty = 0u;`（代价只是可能多刷一帧旧时基的画面）。

---

## 任务 8：触发状态机（冻结 / 重臂 / 预热提示）

**文件：**
- 改：`CM7/Mylibrary/Src/osc_display.c`（抽取任务里的重臂与预热判定）
- 可选改：`CM7/TouchGFX/gui/src/oscillograph_screen/OscillographView.cpp`（预热提示）

**接口：**
- 消费：任务 5 的 `OSC_Acq_IsFrozen()` / `OSC_Acq_IsWarm()` / `OSC_Acq_GetTrigAbs()` / `OSC_Acq_Rearm()`
- 产出：触发后画面冻在触发点居中的一屏；搬完自动重臂

**冻结的语义：** 触发后 CM7 继续搬 `POST_WORDS`（= 环的一半）个 word，然后停止推进写指针。此时环里正好是「触发点前 50% + 触发点后 50%」，抽取任务读到的就是触发点居中、时间跨度 1.342 s 的一屏。

- [ ] **步骤 1：抽取任务里加重臂**

`sweep_task` 的循环改成：

```c
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_dirty) continue;
        if (!OSC_Acq_IsWarm()) continue;

        sweep_once();

        /* 先把 back 填完再发布, UI 永远看不到半份数据 */
        s_front = s_back;
        s_back ^= 1u;
        s_dirty = 1u;

        /* 冻结帧已经搬出来了, 立刻重臂去等下一次触发。
         * 这就是"正常"触发模式: 屏幕上始终是最近一次触发的一屏。
         * 将来要做"单次"模式, 就在这里改成由 UI 按需调 OSC_Acq_Rearm()。 */
        if (OSC_Acq_IsFrozen()) {
            OSC_Acq_Rearm();
        }
    }
```

**注意 `if (!OSC_Acq_IsWarm()) continue;` 少了 `&& !OSC_Acq_IsFrozen()`** —— 开到 1.342 s 满深度之前触发了，也应该能出一屏（只是左边有一部分是空的）。这是刻意的：否则开机后第一秒内界面会一直空白。

- [ ] **步骤 2：验证**

烧录。**用一个真实信号源**（函数发生器或者干脆用手碰探头引入 50 Hz 工频）验证：

1. **触发点居中**：给一个周期方波，`cfg_trig_level = 2047`、`hyst = 16`、上升沿。屏幕上波形的上升沿应当**稳定地停在同一水平位置**，不再左右漂移。漂移说明触发没生效（`state` 一直停在 0）。
2. **冻结是真的**：在 `OSC_Acq_Rearm()` 里打断点，命中时看 `s_wr_words` —— 从冻结点到重臂之间它**不应该变**。
3. **重臂在工作**：`OSC_Acq_DiagPrint()` 里 `state` 应该大部分时间在 0（RUN），偶尔跳到 1（ARMED）再回 0。长期停在 2 说明重臂没被调到。
4. **改触发电平**：把 `cfg_trig_level` 从 2047 改成 3000，波形应该跳到一个新的相位置。电平调到超出信号范围（比如 4095）则不再触发，画面变成自由运行的样子。

- [ ] **步骤 3（可选）：预热提示**

`OSC_Acq_IsWarm()` 在开机 1.342 s 内是 0。这段时间环里还有一半是垃圾数据。

最简单的做法：在 `OscillographView::setGraphData()` 里，当 `!OSC_Acq_IsWarm()` 时把 `dynamicGraph1` / `dynamicGraph2` 的波形色设成灰（`setGraphAreaColor` / `setLineColor`），预热完再设回正常色。接口不新增，直接在 View 里 `extern "C"` 引 `oscillograph.h`。

**验证**：上电后约 1.3 秒内波形是灰的，之后变亮。

---

## 收尾核对清单

全部任务做完后逐条确认：

- [ ] `CM4/Mylibrary/Src/oscillograph.c` 里没有任何 `osc_test*` / `OSC_Test_*` / `OSC_CH_TestPrint` 残留
- [ ] `CM7/Mylibrary/Src/oscillograph.c` 里没有任何 `OSC_Step0_Run` / `mdma_raw` / `LADDER` 残留
- [ ] `CM4/Core/Src/main.c` 里没有 `OSC_Test_` 调用
- [ ] `CM7/Core/Src/main.c` 里没有 `OSC_Step0_Run` 调用
- [ ] `grep -rn "0x30036000\|0x30030000" CM7/ CM4/` 的结果只出现在 `Common/Inc/osc_shared.h` 和注释里（CM7 不再硬编码地址，全部走 `OSC_CTRL` / `OSC_CH1_BUF_ADDR` 宏）
- [ ] CubeMX 里 `SourceBurst` / `DestBurst` 两通道都是 `8BEATS`，`BufferTransferLength` 都是 `64`，`TransferTriggerMode` 都是 `MDMA_FULL_TRANSFER`
- [ ] MPU region 1（`0xD0000000`, 32 MB）是 TEX=Level1 / C=0 / B=0 / S=0
- [ ] MPU region 2（`0x30030000`, 64 KB）同上
- [ ] CM4 侧 MDMA 仍然是禁用状态（`CM4/Core/Src/mdma.c` 不存在）

---

## 设计文档需要同步更新的地方

执行完本计划后，回头改 `docs/superpowers/specs/2026-09-19-osc-deep-acquisition-design.md`：

1. §6.1 的 HT 修复方式（改弱回调，不是 `DMA_Handle->XferHalfCpltCallback`）
2. §5 的 `osc_ctrl_t` 字段表（删 `_pad[3]`）
3. §7.1 的搬运方式（轮询，不是中断回调）
4. §8 表格上方补上 `T_sweep ≈ step × 20.2 µs`，档 13 改成 331 ms / 3 fps
5. §9.3 改成「抽取在独立任务里做」，§9 的「Presenter 重写 `tick()`」改成「`OscillographView::handleTickEvent()` → `presenter->handleDisplayTick()`」（`touchgfx::Presenter` 没有 `tick()`）
6. §10.4 的文件名（`mx-generated.cmake`，不是 `CMakeLists.txt`）
7. §10.5 整段重写（LTDC `TE|FU` 本来就使能了，缺的是 `LTDC_ER_IRQn`）
8. §11 实施顺序按本计划的 8 个任务重排
9. §13 风险表：划掉「总线吞吐只有 ~110 MB/s」（已改 8BEATS，实测 242 MB/s）
