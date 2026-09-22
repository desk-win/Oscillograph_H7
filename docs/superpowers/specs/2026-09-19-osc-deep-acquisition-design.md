# 双核示波器深存储采集/显示设计

日期：2026-09-19
平台：STM32H747 (CM7 @480 MHz / CM4 @240 MHz)，800×480 LTDC + TouchGFX

---

## 1. 目标与非目标

### 目标
- CM4 负责采集 + 触发检测，CM7 负责搬运 + 深存储 + 显示。
- CM4 **不访问 SDRAM**（跨域过于麻烦），只写 D2 SRAM；SDRAM 由 CM7 独占。
- CM7 用 **MDMA** 把数据从 D2 SRAM 搬进 SDRAM 深存储环，CPU 不参与搬运。
- 深存储支持**触发电平 + 迟滞**，触发点之前的历史可回溯。
- 时基可调（14 档），两通道时间窗严格一致。
- 两个动态折线图分别显示通道 A（CH1）和通道 B（CH2）。

### 非目标（本设计明确不做）
- SD 卡 / SD NAND 存储 —— 已有结论：写速率跟不上持续流，仅适合"归档一帧"，本设计不做。
- 时基编码器的实际接线与中断 —— 接口预留，实现在后续阶段。
- 触发迟滞的自动整定 —— 给固定默认值 + 可由 CM7 配置的寄存器。
- min/max 归约金字塔 —— 作为慢时基的后续优化，本设计不用。

---

## 2. 事实基础（已从代码确认）

### 2.1 时钟与数据率

| 项 | 值 | 来源 |
|---|---|---|
| HSE | 25 MHz | PLL1: M=5, N=192, P=2 → 25×192/5/2 = 480 MHz ✓ |
| SYSCLK / HCLK | 480 / 240 MHz | `CM7/Core/Src/main.c` SystemClock_Config |
| APB1 / APB2 | 120 MHz | AHB DIV2 + APB DIV2 |
| TIM1/TIM2 时钟 | 240 MHz | APB2 预分频 ≠1 → 定时器时钟 ×2 |
| ADC 时钟 | **50 MHz** | PLL2 = 25/2×16/2 = 100 MHz，`ADC_CLOCK_ASYNC_DIV2` |
| ADC 分辨率 | 12 bit | `ADC_RESOLUTION_12B` |
| 双模式格式 | `ADC_DUALMODEDATAFORMAT_32_10_BITS` | 12/10 位对应的 MDMA 打包格式（正确） |
| 交错延迟 | `ADC_TWOSAMPLINGDELAY_8CYCLES` | |
| **1 个 CDR word 的周期** | **320 ns** | 16 个 ADC 周期 @50 MHz，与 `oscillograph.c:160` 注释一致 |

### 2.2 两通道数据率

| | 通道 A（CH1） | 通道 B（CH2） |
|---|---|---|
| 硬件 | ADC1+ADC2 交错采**同一路** | ADC3 单独 |
| 1 word 含 | 2 个样本 | 1 个样本 |
| 样本率 | 6.25 MSa/s | 3.125 MSa/s |
| **word 率** | **3.125 Mword/s** | **3.125 Mword/s** |
| 存储字节率（16 bit/样本） | 12.5 MB/s | 6.25 MB/s |

> **关键性质：两通道 word 周期完全相同（320 ns）。** 因此按 word 计数时，两通道时间窗天然逐点对齐，且 CH1 样本率恰为 CH2 的 2 倍。整个时基/抽取设计都建立在这两点上。

> **待验证**：ADC3 的 prescaler 是否也是 DIV2。若否，2:1 关系不成立，时基表需重算。用 `OSC_CH_GetRateCh2()` 实测。

### 2.3 显示与存储现状

- 图表：`GraphWrapAndOverwrite<512>` × 2，位置均为 `(0, 69, 674, 361)`，**完全重叠**（graph2 盖在 graph1 上）。Y 轴范围已改为 `setGraphRangeY(0, 4096)` ✓
- Y 轴刻度线：`dynamicGraph2MajorXAxisGrid.setInterval(85)`、`MajorYAxisGrid.setInterval(453)`
- 帧缓冲：`0xD0000000` 和 `0xD00BB800`，各 768,000 B（800×480 RGB565），共 1.5 MB
- SDRAM：FMC Bank2，32 位总线，9 列 / 12 行 / 4 bank，CAS 3，SDCLK = 120 MHz，32 MB

### 2.4 缓存与 MPU 现状（重要）

- **CM4 没有 MPU 配置，也没有开 D-Cache**（`CM4/Core/Src/main.c` 无 `MPU_Config` / `SCB_EnableDCache`）→ M4 侧不存在缓存一致性问题。
- **CM7 开了 I-Cache + D-Cache**，MPU 配置见 `CM7/Core/Src/main.c` `MPU_Config()`。
- **缺陷**：region 1（SDRAM @`0xD0000000`, 32 MB）只写了 `Number/BaseAddress/Size/SubRegionDisable`，其余字段**残留**上一个 region 的值 → TEX=0/C=0/B=0/S=1 = **Strongly-ordered**。这对 SDRAM 是最差属性（无写缓冲、无投机读、严格顺序）。
  - 目前未暴露，因为读写 SDRAM 的是 LTDC / DMA2D —— **总线主设备不经过 MPU**。
  - **一旦 CM7 的 CPU 开始从 SDRAM 读样本做抽取，此属性会直接卡住刷新。必须修。**
- `MPU_InitStruct` 在 `MPU_Config()` 开头清零一次后**不再清零**，这是"继承"假象的根源（C 结构体残留，非硬件行为）。MPU 硬件层面各 region 完全独立。

---

## 3. 数据通路

```
ADC1+ADC2 (交错, 通道A)  ──DMA1_Stream0 (WORD,   circular, 4096 word)──┐
                                                                      ├─→ D2 SRAM (RAM_D2) 环形缓冲 @0x30030000 / 0x30034000
ADC3      (单核, 通道B)  ──DMA1_Stream1 (HALFWORD,circular, 4096 word)──┘         │
                                                                                  │ HT/TC 中断 (655 µs)
                                                                                  ▼
                                                          CM4: 扫触发 → 写 osc_ctrl → HSEM_ID_1
                                                                                  │
                                                            ┌─────────────────────┘
                                                            ▼  (跨域: D1 主设备 → D2 从设备)
                                          CM7: HSEM ISR → 唤醒任务 → MDMA 搬运 (CPU 零占用)
                                                                                  │
                                                                                  ▼
                                                            SDRAM 深存储环 @0xD0200000 / 0xD1200000
                                                                                  │
                                                            CM7: min/max 抽取 512 点 (CPU 读)
                                                                                  ▼
                                                          TouchGFX dynamicGraph1 / dynamicGraph2
```

**设计决策：CM7 直接从 CM4 的活跃环形缓冲搬走，不设暂存区。**
- CM4 的 DMA 正在写后半区时，MDMA 搬前半区 → 地址不重叠，无竞争。
- 相比"CM4 memcpy 到共享暂存区"的方案：CM4 中断工作量减半、省掉一次 SRAM 带宽占用、共享区从 16 KB 缩到 64 B。
- 代价：MDMA 必须在 655 µs 内完成（实测余量约 8 倍），用一个丢帧计数器兜底。

---

## 4. 内存布局

### 4.1 D2 域 SRAM（`0x30000000`，共 288 KB）

**CM4 链接脚本不需要任何改动。**

D2 SRAM 共 288 KB（SRAM1 128K + SRAM2 128K + SRAM3 32K），CM4 通过两个视图访问它，但**是同一块物理内存**：

| CM4 链接区 | 地址 | CM7 看到的地址 | 大小 |
|---|---|---|---|
| `RAM` | `0x10000000` + 192K | `0x30000000`–`0x3002FFFF` | 192 KB（`.data`/`.bss`/栈） |
| `RAM_D2` | `0x30030000` + 96K | `0x30030000`–`0x30047FFF` | 96 KB（跨核共享区） |

`192 + 96 = 288 KB` = D2 SRAM 总容量 ✓。两个视图互为别名，但 **CM7 只能用 `0x30000000` 视图**（`0x10000000` 是 CM4 的 AHBS 别名，CM7 不可访问）。

因此 **`RAM_D2` 这个专门区域必须保留** —— 它的 `ORIGIN` 就是两核约定的编译期常量。如果把缓冲区直接声明在 `RAM` 里，链接器会按 `.data`/`.bss`/栈的排布随机摆放，CM7 无从得知地址。

| 地址 | 符号 | 大小 | 说明 |
|---|---|---|---|
| `0x30030000` | `osc_ch1_buf[4096]` (uint32) | 16 KB | ADC1+ADC2 交错循环目标（**现有，不动**） |
| `0x30034000` | `osc_ch2_buf[4096]` (uint16) | 8 KB | ADC3 循环目标（**现有，不动**） |
| `0x30036000` | `osc_ctrl` | 64 B | 跨核控制块（**新增**） |

三者都在 `RAM_D2`（`0x30030000`–`0x30047FFF`）之内，且按上表顺序声明即依次落位。

> **`osc_ctrl` 必须是 `.dma_buffer` 段里的真实变量定义**，不能只是一个宏地址 —— 否则链接器会把别的变量放到 `0x30036000` 造成重叠。CM7 侧用固定常量 `0x30036000` 访问，CM4 在 `OSC_CH_Init()` 里校验：
> ```c
> if ((uintptr_t)&osc_ctrl != 0x30036000u) Error_Handler();
> ```
> 地址一旦漂移，开机立刻停在 `Error_Handler`，不会变成难查的内存踩踏。

**MPU 必须把这个区域声明为 Normal Non-cacheable**（见 §10.1），否则 CM7 读到的是 D-Cache 里的陈旧副本。CM4 没有 Cache，不受影响。

环形缓冲取 **4096 word**（半帧 2048 word = 655 µs），而非 2048 word —— 多花 8 KB SRAM 把 CM7 的响应窗口从 327 µs 翻到 655 µs。

### 4.2 D1 域 SDRAM（`0xD0000000`，32 MB，已用 1.5 MB）

| 地址 | 大小 | 说明 |
|---|---|---|
| `0xD0000000` | 768,000 B | 帧缓冲 0（现有，不动） |
| `0xD00BB800` | 768,000 B | 帧缓冲 1（现有，不动） |
| `0xD0177000` | 561 KB | 空闲（预留给未来的归约金字塔） |
| `0xD0200000` | **16 MB** (`0x01000000`) | **CH1 深存储环** → `0xD1200000` |
| `0xD1200000` | **8 MB** (`0x00800000`) | **CH2 深存储环** → `0xD1A00000` |
| `0xD1A00000` | 6 MB | 空闲 |

**为什么 CH1 是 16 MB 而 CH2 是 8 MB**：两个环按 **word 数相等**来划分，各 `4,194,304` word（`0x400000`，即 2 的幂）。因为 word 周期相同，word 数相等 = 时间跨度相等，两个环在同一时刻绕回，时间轴永远锁步。

- CH1：`4,194,304 word × 4 B = 16,777,216 B = 16 MB`
- CH2：`4,194,304 word × 2 B = 8,388,608 B = 8 MB`
- 绕回周期：`4,194,304 / 3.125 Mword/s = 1.342 s`（两通道相同）

**取 2 的幂的理由**：抽取内层循环要跑 `256 × step` 次（慢时基下多达 420 万次），`REGION_WORDS` 是 2 的幂才能把环形寻址的取模换成按位与。代价是每个环多占约 2.6% 空间，`step` 上限从 15360 涨到 16384 —— 是笔划算的买卖。

**分块对齐**（让 MDMA 目标指针永不跨环形边界）：
- CH1 半帧 = 2048 word × 4 B = 8192 B；`16,777,216 / 8192 = 2048` 整除 ✓
- CH2 半帧 = 2048 word × 2 B = 4096 B；`8,388,608 / 4096 = 2048` 整除 ✓
- 两个环基址 `0xD0200000` / `0xD1200000` 均为 8192 对齐 ✓

⇒ **每个半帧都落在单个连续块内，MDMA 永远不需要拆成两次传输。**

深存储深度：**1.342 s**（预触发 + 后触发的总预算）。
> 注意：这是总预算。要 50% 预触发，就是触发前 671 ms 的历史。开机后 1.342 s 内没有完整历史可看，界面上应有"预热中"提示。

---

## 5. 跨核 ABI

`Common/Inc/osc_shared.h`（**两个工程的 include 路径都要加 `Common/Inc`**）：

```c
#define OSC_SHARED_MAGIC   0x4F534332u   /* 'OSC2' */
#define OSC_LEVEL_AUTO     0xFFFFFFFFu   /* cfg_trig_level_* 填它 = CM4 自适应定电平 */

typedef struct {
    /* ---- CM4 写, CM7 读 ---- */
    uint32_t magic;             /* 必须 == OSC_SHARED_MAGIC */
    uint32_t seq_ch1;           /* CM4 每发布一个半帧 +1; 第 n 次发布 = 半帧 (n-1) & 1 */
    uint32_t seq_ch2;
    uint32_t trig_pos_ch1;      /* 半帧内触发点下标; OSC_TRIG_NONE = 本帧无 */
    uint32_t trig_pos_ch2;      /* CH1 是 word 下标, CH2 是样本下标 —— 量纲不同 */
    uint32_t trig_flags;        /* bit0: CH1 本帧检测到触发; bit1: CH2 */

    /* ---- CM7 写, CM4 读 ---- */
    uint32_t cfg_trig_level_ch1;/* 0..4095, 或 OSC_LEVEL_AUTO */
    uint32_t cfg_trig_level_ch2;
    uint32_t cfg_trig_hyst;     /* 迟滞半宽, LSB, 默认 16; 只压在"武装"侧 */
    uint32_t cfg_trig_edge;     /* 0 = 上升沿, 1 = 下降沿 */
    uint32_t cfg_trig_enable;
    uint32_t cfg_run;           /* 0 = 停止采集 */

    /* ---- 诊断: 每通道一份, 单写者 ---- */
    uint32_t diag_ovr_ch1, diag_ovr_ch2;
    uint32_t diag_miss;         /* CM4 主循环来不及扫的半帧数 */
    uint32_t diag_rate_ch1, diag_rate_ch2;
    uint32_t diag_min_ch1, diag_max_ch1, diag_level_ch1;
    uint32_t diag_min_ch2, diag_max_ch2, diag_level_ch2;
    uint32_t reserved[1];
} osc_ctrl_t;   /* 24 word = 96 B */

#define OSC_CTRL ((volatile osc_ctrl_t *)0x30036000u)
```

> 头文件的实际路径是 `Common/Inc/sharedconfig.h`，不是本文早期草稿里的 `osc_shared.h`。

### 5.1 ABI 变更记录：'OSC1' → 'OSC2'（2026-09-20）

| 变更 | 原因 |
|---|---|
| `cfg_trig_level` → `cfg_trig_level_ch1` / `_ch2` | CH1(ADC1+ADC2) 与 CH2(ADC3) 的幅度可能差很远，共用一个电平必然有一路定歪 |
| 新增 `OSC_LEVEL_AUTO` 哨兵 | 自适应电平由 CM4 闭环算，CM7 只需写哨兵值开关，不用跨核往返 |
| 新增 `diag_min/max/level_ch1`、`_ch2` | 自适应模式下 CM7 不知道实际电平，画不出触发电平标记线；min/max 顺带供 CM7 做垂直量程 |
| `diag_miss` 语义变了 | 从"CM7 来不及搬"变成"CM4 主循环来不及扫"（扫描已移出中断，见 6.3） |
| `sizeof` 64 B → 96 B | 上面这些字段。RAM_D2 有 96 KB 只用到 24 KB，空间无压力 |
| magic 换 `'OSC2'` | 结构体布局变了。CM7 若编的是旧头文件，读到 magic 不匹配会直接报错，而不是按错偏移量解析出一堆脏数据 |

**写者约定**：一个字段只有一个核在写。诊断字段按通道拆成独立的字，不要写成"两路共用一个字、各改各的位"——两个 ADC 走 `DMA1_Stream0/1` 且同为抢占优先级 0（`CM4/Core/Src/dma.c`），不会互相打断所以读改写侥幸安全，但这条约束一旦被改动就会变成静默丢位。

### 同步语义

**发布方（CM4）**：
```c
__DMB();
OSC_CTRL->trig_pos_ch1 = pos;
OSC_CTRL->seq_ch1 = ++s;
__DMB();
HAL_HSEM_Release(HSEM_ID_1, 0);   /* 纯通知 */
```

**消费方（CM7）**：以 `seq` 为唯一真相，HSEM 只是"来看一眼"的闹钟。

> **HSEM 丢通知**：若 CM4 在 CM7 清旗前又 Release 一次，对已 free 的信号量是空操作 → 通知丢失。因此 CM7 的 ISR 清旗后必须**比对本地进度与 `seq`**，落后就连续补搬。丢通知只会变成延迟，不会变成丢数据。

- HSEM 用 **ID_1**（ID_0 被双核 boot 同步占用）。
- CM4 无 D-Cache、CM7 侧该区域标记非缓存 → **无缓存一致性问题**，只需 `volatile` 阻止编译器重排。

---

## 6. CM4 侧设计

### 6.1 前置修复（这是 bug，不是优化）

**HT 中断目前根本不触发。** `Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c:711` 中 HAL 无条件清掉 `DMA_IT_HT`，仅当 `XferHalfCpltCallback != NULL` 时才重新置上。CM4 从未设置该回调，所以现在只有 TC 中断。

```c
hadc1.DMA_Handle->XferHalfCpltCallback = OSC_Ch1_HalfCplt;
hadc3.DMA_Handle->XferHalfCpltCallback = OSC_Ch2_HalfCplt;
```

**删除 `OSC_CH_TestPrint()`**（`CM4/Mylibrary/Src/oscillograph.c:150`）—— 它每 1 秒 `Stop_DMA`/`Start_DMA` 一次，接入深存储后是定时炸弹。

### 6.2 DMA 配置

- `CH1_BUF_WORDS` / `CH2_BUF_WORDS`：`4*1024` → **4096**（保持）
- 缓冲地址保持在 `0x30030000` / `0x30034000`，通过链接脚本的 `RAM_D2` 区域起点 + `.dma_buffer` 段顺序保证

### 6.3 触发扫描（已移出中断，在主循环里做）

> 2026-09-20 变更：原设计把扫描放在 HT/TC 中断内。那两个中断走 `DMA1_Stream0/1`，抢占优先级都是 0，在里面扫 2048 个 word 会把 CM4 上所有别的中断（TIM6 tick、USB）一次挡住 200 µs 量级。现在中断只 +1 计数，扫描放到主循环 `OSC_CH_Poll()`。

```c
/* 中断里: 只记数, 几个周期就返回 */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)      s_completed_ch1++;
    else if (hadc->Instance == ADC3) s_completed_ch2++;
}

/* 主循环里: 每次调用处理"最新那个刚完成"的半帧 */
void OSC_CH_Poll(void);
```

- **硬约束**：主循环必须在半个帧周期（2048 word × 320 ns = **655 µs**）内调到 `OSC_CH_Poll()`。超过就会被 DMA 绕回来覆盖那块半帧。
- 落后 ≥2 个半帧时只处理最新那半、其余计入 `diag_miss`——**不静默丢数据**。
- **主循环里不许放阻塞调用**。原 `main()` 的 `HAL_Delay(1000)` 已换成 DWT/tick 限速的每秒一次 `OSC_CH_DiagPrint()`；`usb_printf` 本身是阻塞的，打印期间必然丢半帧，这个循环的位置就是给调试用的，正式跑要摘掉。
- 备选方案（改动更小）：保留扫描在中断内，只把 `DMA1_Stream0/1` 的抢占优先级从 0 降下来。取舍是"主循环被拖慢"换"扫描被高优先级中断拉长"。

#### 迟滞语义：迟滞只压在"武装"侧

```c
/* 上升沿 */
if (!armed) { if (v <= level - hyst) armed = 1; }   /* 确认起点在低位 */
else        { if (v >= level)       { armed = 0; return i; } }   /* 触发 */

/* 下降沿: 两边比较反过来 */
```

与最初的对称写法（`v < level-hyst` 武装、`v > level+hyst` 触发）的区别：

- **触发阈值就是 `level` 本身**，报出的触发点正好落在用户设定的电平上。对称写法会整体偏后 `hyst` 个采样，慢信号上肉眼可见。
- 抗噪完全来自"必须先站到反侧"这一步，触发阈值上再加 ±hyst 只贡献系统性误差。
- 与 libtiepie-hw / R&S `CHANnel<m>:THReshold:HYSTeresis` 的定义一致。
- `hyst` 有下限钳位（`OSC_HYST_MIN = 2`），否则退化成"相邻两拍比较"。
- **默认 `cfg_trig_hyst = 16` LSB**（约满量程 0.8%）。太小噪声反复误触发，太大触发不了小信号。

#### 自适应电平（`cfg_trig_level_* == OSC_LEVEL_AUTO` 时由 CM4 算）

- **目标值 = 幅度中点** `min + (max-min)/2`。不用算术平均：中点与占空比无关，且正弦在中点最陡，触发时刻抖动 ≈ 噪声/斜率 最小。
- **统计窗口 = 16 个半帧 ≈ 10.5 ms**（下限约 95 Hz）。单半帧只看得到 ≥1.5 kHz 的信号。
- **触发正常时把电平冻住**，只在"第一个窗口"或"连续 2 个窗口都触发不上"时才更新——否则电平一直动、触发点跟着动、画面左右游走。
- **直接落位，不做一阶平滑**（2026-09-20 砍掉）：既然只在"连续两个窗口都触发不上"时才动，说明电平本来就定歪了，这时候跳到正确值比慢慢挪更好。平滑只在"每窗口都更新"的方案里才有意义。
- `no_trig` 初值 = `OSC_NO_TRIG_WINS`，让第一个窗口无条件先定一次电平，省掉一个 `seeded` 标志。
- **更新电平必须同时清 `armed`**，否则新阈值配旧状态会在下一拍报一个 `pos = 0` 的假触发。

#### 单趟扫描，不提前返回

CH1 每个 word 含两个样本（ADC1 低 16 位在前、ADC2 高 16 位在后），两个都要喂进状态机。命中第一拍只**锁存位置**，循环继续跑完——提前 `return` 会让窗口统计只覆盖到触发点，`min/max` 不完整，自适应电平就失准。

#### 连续确认：已砍掉（2026-09-20）

原设计有 `OSC_TRIG_CONSEC = 2`（要求连续 2 拍越过 `level`，位置补回 1 个采样），用来对付**慢速含噪信号**：正弦在阈值附近斜率小，噪声会让它反复穿越而误触发（[BTTLab/stm32osc1ch](https://github.com/BTTLab/stm32osc1ch/) 的文档记载了这个坑：正弦 <12 kHz 不稳）。

**当前实现里删掉了**，连带 `TRIG_BACK` 位置补偿一起。理由是先把采集→显示整条链路跑通再谈抗噪；而且它会带来"触发点位置有半个 word 取整误差"的副作用（CH1 一个 word 含两个样本，补偿只能按 word 计）。

> 如果实测发现慢正弦触发不稳，第一手段是先加大 `cfg_trig_hyst`（16 → 32/64），不够再加回连续确认。

#### 明确不做的：直流判定

纯直流、或峰峰值小于 `2*hyst` 的信号会退化成噪声触发。**示波器限定只测周期变化信号**，这是该前提的代价。直流输入下 `trig_flags` 恒为 0，CM7 据此走自由运行显示即可，不需要额外的直流判定字段。

#### CPU 占用

每半帧扫 4096 个样本（CH1 2048 word × 2 + CH2 2048 word）。按实测的 D2 SRAM 标量读 18.9 周期/word 估计，CH1 约 161 µs + CH2 约 161 µs，仍在 655 µs 预算内，但**主循环里还有 USB 等其它工作**——所以要实测，`diag_miss` 是最好的观测量。
> 若压测吃紧：第一优化是把扫描改到 DMA1_Stream2/3 之外的分时调度，第二优化是改用 ADC 模拟看门狗（AWD）做硬件比较。**先不要优化。**

---

## 7. CM7 侧设计

### 7.1 MDMA 搬运

```c
/* 注意: 5 个参数, 不是 4 个 */
HAL_MDMA_Start_IT(&hmdma_mdma_channel0_sw_0, (uint32_t)&osc_ch1_buf[off],
                   (uint32_t)sdram_ch1_wr, 8192, 1);
HAL_MDMA_Start_IT(&hmdma_mdma_channel1_sw_0, (uint32_t)&osc_ch2_buf[off],
                   (uint32_t)sdram_ch2_wr, 4096, 1);
```

签名（`Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mdma.c`）：
```c
HAL_StatusTypeDef HAL_MDMA_Start_IT(MDMA_HandleTypeDef *hmdma, uint32_t SrcAddress,
                                    uint32_t DstAddress, uint32_t BlockDataLength,
                                    uint32_t BlockCount);
```
- `BlockDataLength`：**1..65536 字节**（`IS_MDMA_TRANSFER_LENGTH`）
- `BlockCount`：**1..4096**（`IS_MDMA_BLOCK_COUNT`）

**回调注册方式与普通 DMA 不同**，不能直接赋值：
```c
HAL_MDMA_RegisterCallback(&hmdma_mdma_channel0_sw_0, HAL_MDMA_XFER_CPLT_CB_ID, Osc_Ch1_MdmaCplt);
HAL_MDMA_RegisterCallback(&hmdma_mdma_channel0_sw_0, HAL_MDMA_XFER_ERROR_CB_ID, Osc_Ch1_MdmaError);
```

- **MDMA 没有循环模式** —— 环形的绕回由 CM7 每次半帧重算目标地址实现。
- 时间预算：655 µs 窗口内，MDMA 搬 12 KB（8 KB + 4 KB）@ ~150 MB/s ≈ 82 µs，加中断延迟 < 5 µs → **余量约 8 倍**。

#### 总线选择是 HAL 自动做的（无需配置）

`MDMA_SetConfig()` 按地址高字节决定走 AXI 还是 AHB：只有高字节为 `0x20` 或 `0x00` 时才走 AHBS/AHB。源 `0x30030000` 和目标 `0xD0000000` 都不满足 → **两边都走 AXI**，这是正确的路径。

### 7.2 触发状态机

```
RUNNING  ── 持续喂 SDRAM 环形; 检查 trig_flags
   │ CM4 报来触发
   ▼
ARMED    ── 继续搬 post_depth; 记下触发点的绝对 word 序号
   ▼
FROZEN   ── 停止推进写指针, 置 dirty, 等重臂
```

触发点的绝对 word 序号 `trig_abs = write_seq - half_frame_words + trig_pos`。
环形寻址：`offset_words = trig_abs & (REGION_WORDS - 1)`。

### 7.3 显示窗口抽取（min/max）

一屏 512 点 = **256 组 (min, max)**，每组覆盖 `step` 个 word。

**窗口跨环形边界要拆成两段线性扫描**（`start_word + 256*step` 可能超过 `REGION_WORDS`）。环形区共 `REGION_WORDS = 4,194,304` word（`0x400000`，2 的幂）：

```c
#define REGION_WORDS  0x400000u          /* 4,194,304 word, 2 的幂 */
#define MASK          (REGION_WORDS - 1) /* → 用 & 代替 % */

uint32_t i = start_word & MASK;
for (uint32_t g = 0; g < 256; g++) {
    uint16_t lo = 0xFFFF, hi = 0x0000;
    for (uint32_t k = 0; k < step; k++) {
        uint32_t w = ch1_sdram[i];       /* i 已经在区间内, 无越界 */
        uint32_t a = w >> 16, b = w & 0xFFFF;   /* CH1: 1 word = 2 个样本 */
        if (a < lo) lo = a;  if (a > hi) hi = a;
        if (b < lo) lo = b;  if (b > hi) hi = b;

        if (++i == REGION_WORDS) i = 0;  /* 绕回, 只在外层判断一次 */
    }
    *out++ = lo;  *out++ = hi;
}
```

`REGION_WORDS = 0x400000` = 4,194,304 word 是 2 的幂，故取模可换成按位与。内存布局与分块整除性见 §4.2。

**为什么必须 min/max**：慢时基下若"每 step 个样本取一个"，窄脉冲会被整个漏掉。min/max 是示波器的标准做法。

**附带好处**：min/max 与样本先后顺序无关，因此**对 `CH1_ADC1`/`CH1_ADC2` 高低 16 位的归属判断免疫** —— 即使最初判反了，min/max 抽取结果依然正确。该归属问题只影响最快时基下放大波形的时间方向。

---

## 8. 时基

时基 = **一屏 512 点覆盖的时间 T**。两通道共用同一个 `step`（单位：word），保证横轴严格一致：

```
T = 256 × step × 320 ns = step × 81.92 µs
```

| 档 | step (word) | 时间窗 T | CH1 每组样本 | CH2 每组样本 |
|---|---|---|---|---|
| 0 | 1 | 81.92 µs | 2 | 1 |
| 1 | 2 | 163.8 µs | 4 | 2 |
| 2 | 4 | 327.7 µs | 8 | 4 |
| 3 | 8 | 655.4 µs | 16 | 8 |
| 4 | 20 | 1.638 ms | 40 | 20 |
| 5 | 40 | 3.277 ms | 80 | 40 |
| 6 | 80 | 6.554 ms | 160 | 80 |
| 7 | 200 | 16.38 ms | 400 | 200 |
| 8 | 400 | 32.77 ms | 800 | 400 |
| 9 | 800 | 65.54 ms | 1600 | 800 |
| 10 | 2000 | 163.8 ms | 4000 | 2000 |
| 11 | 4000 | 327.7 ms | 8000 | 4000 |
| 12 | 8000 | 655.4 ms | 16000 | 8000 |
| 13 | 16384 | **1.342 s** | 32768 | 16384 |

- 档 0 是 CH1 满速率极限（6.25 MSa/s 全用上，每组 min≠max，画出 1 px 竖线）。CH2 在档 0 每组仅 1 个样本，min = max，退化为普通折线 —— 这是 ADC3 确实只有一半速率的诚实反映，不是缺陷。
- 档 13 一帧读满 16 MB + 8 MB = 24 MB，按 ~150 MB/s 算约 160 ms/帧 ≈ **6.2 fps**。慢时基下刷新率本就该降，可接受。
- **时基编码器**：预留 `timebase_idx`，中断内 ±1，查表取 `step`。本设计不实现。

> 若后续认为慢时基刷新率不可接受，优化方向是建 min/max 归约金字塔（`0xD0177000` 起 561 KB 空闲正是为此预留），把任意时基的读取量压回 O(512)。本设计不做。

---

## 9. TouchGFX 集成

三处改动，全在**手写文件**里，不碰 `generated/`：

### 9.1 `CM7/TouchGFX/gui/include/gui/oscillograph_screen/OscillographView.hpp` + `.cpp`
新增：
```cpp
void setGraphData(const uint16_t *ch1, const uint16_t *ch2, uint32_t n);
```
实现：
```cpp
dynamicGraph1.clearGraph();
for (uint32_t i = 0; i < n; i++) dynamicGraph1.addDataPoint(ch1[i]);
dynamicGraph2.clearGraph();
for (uint32_t i = 0; i < n; i++) dynamicGraph2.addDataPoint(ch2[i]);
```
`n = 512`。`GraphWrapAndOverwrite<512>` 在 `clearGraph()` 后填入正好 512 点，不会触发覆盖回绕。

### 9.2 `OscillographPresenter.hpp` / `.cpp`
override `tick()`，比对 Model 的 dirty 标志，**dirty 才调** `view.setGraphData()`。
**不要每帧无条件刷新** —— 慢时基下一帧抽取要 150 ms，每帧 `clearGraph()` 会闪。

### 9.3 Model
提供 `getDisplayBuffer()` 与 `isDirty()`。min/max 抽取在 Model 里做（数据本来就在 CM7 手上）。

### 9.4 已知显示问题
两个图表位置**完全重叠**（`(0, 69, 674, 361)`），graph2 绘制在 graph1 之上。两条曲线同时可见需要：挪位置、加偏移、或用不同透明度。**本设计不解决，仅记录。**

---

## 10. 必需的配置改动清单

### 10.1 CubeMX — MPU

在 CubeMX 的 MPU 配置界面按此表设置（**不是改生成代码，改代码会被重新生成覆盖**）：

| Region | Base | Size | TEX | Cacheable | Bufferable | Shareable | AP / XN | 说明 |
|---|---|---|---|---|---|---|---|---|
| 0 | `0x38000000` | 64 KB | Level 0 | Disabled | Disabled | **Enabled** | Full / En | D3 备份 SRAM，保持不动 |
| 1 | `0xD0000000` | 32 MB | **Level 1** | **Disabled** | **Disabled** | **Disabled** | Full / En | SDRAM ← **已改** |
| 2 | `0x30030000` | 64 KB | Level 1 | Disabled | Disabled | Disabled | Full / En | D2 共享区 ← **已改** ✓ |

**已完成的核对（2026-09-19，`CM7/Core/Src/main.c:326-330`）**：region 2 只写 `Number` / `BaseAddress` / `Size` 三行，其余字段沿用结构体里 region 1 留下的值。

追踪 `MPU_InitStruct` 的实际取值：

| 字段 | 来源 | 值 | 结果 |
|---|---|---|---|
| `Enable` | region 0 | `MPU_REGION_ENABLE` | 使能 ✓ |
| `BaseAddress` | region 2 显式 | `0x30030000` | 覆盖 `0x30030000`–`0x3003FFFF` ✓ |
| `Size` | region 2 显式 | `MPU_REGION_SIZE_64KB` | ✓ |
| `SubRegionDisable` | region 1 | `0x0` | 无子区禁用 ✓ |
| `TypeExtField` | region 1 | `MPU_TEX_LEVEL1` | TEX=001 ✓ |
| `AccessPermission` | region 0 | `FULL_ACCESS` | ✓ |
| `DisableExec` | region 0 | `INSTRUCTION_ACCESS_ENABLE` | ✓ |
| `IsShareable` | region 1 | `NOT_SHAREABLE` | S=0 ✓ |
| `IsCacheable` | region 0 | `NOT_CACHEABLE` | C=0 ✓ |
| `IsBufferable` | region 0 | `NOT_BUFFERABLE` | B=0 ✓ |

TEX=001 / C=0 / B=0 → **Normal, Non-cacheable**（ARMv7-M 内存属性表）✓

覆盖范围 `0x30030000` + 64 KB = `0x30030000`–`0x3003FFFF`，装得下全部三个符号（最远 `0x3003603F`）✓，且 map 已确认 `osc_ch1_buf @ 0x30030000` / `osc_ch2_buf @ 0x30034000` / `.dma_buffer` 节尾 `_edma_buffer @ 0x30036000`（`_edma_buffer` 是链接脚本里的节尾标记，不是缓冲区，`0x30036000` 确实是空的）。

**为什么 Base 不能是 `0x30000000`**：`0x30000000`–`0x3002FFFF` 是 CM4 的 `RAM`（`.data`/`.bss`/栈），`0x30030000` 才是 `RAM_D2`，也就是本设计的共享区所在（见 §4.1）。配成前者会保护 CM4 的栈，而真正需要保护的三个符号**完全没被覆盖**，退回默认属性（Normal, Write-Back, Write-Allocate, **cacheable**）→ CM7 读到 D-Cache 里的陈旧数据。

> **残留提示**：region 2 只到 `0x3003FFFF`，而 CM4 的 `RAM_D2` 到 `0x30047FFF`，即 `0x30040000`–`0x30047FFF` 未被任何 region 覆盖（默认可缓存）。本设计不使用那片，当前无害。`osc_ctrl` 的运行时地址校验（§4.1）会在符号越界时报错。
>
> **注意**：128 KB @ `0x30030000` 不合法 —— `0x30030000` 不是 128 KB 对齐（余 `0x10000`），MPU 要求按 size 对齐。

**为什么 region 1 必须改**：TEX=0/C=0/B=0 = Strongly-ordered，对 SDRAM 是最差属性。现在没暴露是因为访问它的是 LTDC/DMA2D（总线主设备，不过 MPU）。一旦 CM7 的 CPU 读 SDRAM 做抽取就会卡死刷新。

**验证方法**：`MPU_Config()` 中 region 1 的 `Number/BaseAddress` 与 `HAL_MPU_ConfigRegion()` 之间**出现了 `TypeExtField` 和 `IsShareable`** —— 已确认出现 ✓。CubeMX 会省略与上一个 region 相同的行，行没出现就是值没变。

### 10.2 CubeMX — MDMA

**Multimedia → MDMA**，Add 两个 Channel：

| 配置项 | CH0（CH1 数据） | CH1（CH2 数据） |
|---|---|---|
| Direction | Memory → Memory | Memory → Memory |
| Source / Dest data size | **Word (32-bit)** | **Half Word (16-bit)** |
| Source / Dest address increment | Increment | Increment |
| **TransferTriggerMode** | **`MDMA_FULL_TRANSFER`** | **`MDMA_FULL_TRANSFER`** |
| Source / Dest burst size | 8 或 16 beats | 8 或 16 beats |
| Request | **Software request** | **Software request** |
| Priority | High | High |
| Interrupts | **Transfer Complete + Transfer Error** | 同左 |

NVIC 使能 **`MDMA global interrupt`**（H7 的 MDMA 是**单根全局中断线**，不是 per-channel —— 已在 `CM7/Core/Startup/startup_stm32h747xx_CM7.s:272` 确认）。

#### ⚠️ `TransferTriggerMode` 必须是 `MDMA_FULL_TRANSFER`

**不能是 `MDMA_BUFFER_TRANSFER`。** 两者区别（HAL 源码头注释）：

| 模式 | 语义 |
|---|---|
| `MDMA_BUFFER_TRANSFER` | 每次请求只搬 `BufferTransferLength` 字节 |
| `MDMA_BLOCK_TRANSFER` | 每次请求搬一个块 |
| `MDMA_FULL_TRANSFER` | **每次请求搬完全部** ← 要的就是这个 |

而 `BufferTransferLength` 一旦设为 1，`MDMA_BUFFER_TRANSFER` 就变成"**每次软件请求搬 1 字节**"，搬完 1 字节后等下一个请求，而下一个请求永远不会来 → **传输卡死**。

关键证据（`stm32h7xx_hal_mdma.c`）：
- `MDMA_Init()` 把 TLEN 写成 `(BufferTransferLength - 1)`；`BufferTransferLength = 1` → TLEN = 0。
- `MDMA_SetConfig()` **只写** `CBNDTR`(BNDT+BRC) / `CDAR` / `CSAR` / `CTBR`，**从不重写 CTCR 的 TLEN**。所以 TLEN 永远是 `HAL_MDMA_Init` 那时设的值，`HAL_MDMA_Start_IT` 不会修正它。
- `HAL_MDMA_Start_IT()` 在 `MDMA_REQUEST_SW` 下只置一次 `CCR |= MDMA_CCR_SWRQ`。

### 10.2.1 更正：`BufferTransferLength` 必须 ≥ 数据宽度（2026-09-19 第 0 步实测推翻）

**本节先前写过「改用 `MDMA_FULL_TRANSFER` 后 TLEN 不再参与，`BufferTransferLength` 留 1 也无所谓」——这句是错的。** 第 0 步实测：两个通道都在 `HAL_MDMA_Start_IT` 后约 2 µs 失败，`ErrorCode != NONE` 而 `CESR = 0`。硬件**仍然校验 TLEN**：`BufferTransferLength = 1` 让 `CTCR.TLEN = 0`（编码为 1 字节），配 WORD(4 B) / HALFWORD(2 B) 的 `SSIZE`/`DSIZE` 是非法组合。

**已改 ✓**：`BufferTransferLength` 由 1 改为 **64**（两个通道），已核对 `CM7/Core/Src/mdma.c:63` 与 `:85`。64 同时是 4 和 2 的整数倍，两通道共用。

**实测证据（第 0 步 raw 对照实验，只动 TLEN 一个变量）**：

| TLEN | PKE | CISR | CESR | 结果 |
|---|---|---|---|---|
| 1 | 1 | 0x0001 | **0x800 = BSE** | 无标志，103 cyc 就失败 |
| 64 | 1 | 0x001E | 0x000 | CTCIF，17775 cyc，坏 0 |
| 1 | 0 | 0x0001 | **0x800 = BSE** | 无标志 |
| 64 | 0 | 0x001E | 0x000 | CTCIF，17775 cyc，坏 0 |

`CESR = 0x800` 正是 `MDMA_CESR_BSE`（Block Size Error），**TLEN=1 是唯一原因，与 PKE 无关**。假设证实。

> 注意 raw 实验里 16 位通道必须配 `MDMA_SRC_INC_HALFWORD`（`SINC_1|SINCOS_0`）而非 `INC_WORD`。增量宏里的 `SINCOS`/`DINCOS` 位决定访问步长，写错会只覆盖偶数下标，剩下的填充值看起来像"坏一半"，容易误判成硬件故障。

### 10.2.2 陷阱：TE 中断清标志会把 `CESR` 一起抹掉，并顺手中止传输

`HAL_MDMA_IRQHandler` 的 TE 分支（`stm32h7xx_hal_mdma.c:1519-1566`）读 `CESR` 写 `ErrorCode` 后清 TEIF，**但不清 `State`**；随后中断尾部（`:1657-1694`）看到 `ErrorCode != NONE` 就：

```c
hmdma->State = HAL_MDMA_STATE_ABORT;
__HAL_MDMA_DISABLE(hmdma);          /* ★ 直接把通道关了 */
...
if (hmdma->XferErrorCallback != NULL) hmdma->XferErrorCallback(hmdma);
```

两个后果：

1. **任何一次 TEIF 都会立刻中止传输**，哪怕原因是配置非法、一个字节都没搬。此时 `ErrorCode` 非 0、`CESR` 因为已清 TEIF 而读成 0 ——「有错但查不出错」。
2. `MDMA_Init()` **从不碰 `CISR`**。若上电后残留一个 TEIF，`HAL_MDMA_Start_IT` 一使能 `MDMA_IT_TE` 就立即进中断并被第 1 条掐死。

**对策（生产代码必须做）**：`HAL_MDMA_Start_IT` 之前先 `h->Instance->CIFCR = 0x3F`（清全部通道中断标志）。诊断时若要看 `CESR`，必须**绕开 HAL 直写寄存器并在清标志之前读**，否则读到的永远是 0。

HAL 的基线 CTCR 组装（`stm32h7xx_hal_mdma.c:1819-1839`）——绕开 HAL 时照抄这个，才能保证只有目标变量在变：

```c
CCR  = Priority | Endianness;
CTCR = SourceInc|DestInc|SourceDataSize|DestDataSize|DataAlignment
     | SourceBurst|DestBurst | ((BufferTransferLength-1)<<18) | TransferTriggerMode;
CTCR |= (SWRM | BWM);     /* Request == MDMA_REQUEST_SW 时 */
```

#### 生成结果的核对（2026-09-19）

`CM7/Core/Src/mdma.c` 已确认正确项：`MDMA_REQUEST_SW` ✓、`MDMA_PRIORITY_HIGH` ✓、CH0 = WORD/WORD ✓、CH1 = HALFWORD/HALFWORD ✓、双向 INC ✓、`MDMA_IRQHandler` 已在 `CM7/Core/Src/stm32h7xx_it.c:211` 生成并同时分发两个通道 ✓、`MX_MDMA_Init()` 已在 `CM7/Core/Src/main.c:166` 调用 ✓、`mdma.c` 与 `stm32h7xx_hal_mdma.c` 均已列入 `CM7/mx-generated.cmake` ✓。

**已改 ✓**：`Init.TransferTriggerMode` → `MDMA_FULL_TRANSFER`（两个通道），已核对 `.ioc:344` 与 `.ioc:369`。

UI 位置：MDMA 面板里 **Channel0 → Request0(SW) 展开后的配置块**，和 `BufferTransferLength`、`SourceBurst`、`BlockCount` 在一起（`.ioc` 的 `RequestParameters` 已确认它被 UI 暴露）。若在 UI 里找不到，**直接改 `.ioc` 更确定**（关掉 CubeMX → 改 → 重开 → 重新生成）：

```
Oscillograph_H7.ioc:344   Mdma.MDMA_Channel0.SW.0.TransferTriggerMode=MDMA_BUFFER_TRANSFER
                          → MDMA_FULL_TRANSFER
Oscillograph_H7.ioc:369   Mdma.MDMA_Channel1.SW.0.TransferTriggerMode=MDMA_BUFFER_TRANSFER
                          → MDMA_FULL_TRANSFER
```

**可选优化（非正确性）**：`SourceBurst` / `DestBurst` 由 `SINGLE` 改为 `MDMA_SOURCE_BURST_8BEATS` / `MDMA_DEST_BURST_8BEATS`（`.ioc` 第 340/328 行与 365/353 行），否则 8 KB 会拆成 2048 次 4 字节单拍。**先能搬通再上 burst**。

### 10.3 链接脚本

**CM4** — `CM4/stm32h747xx_flash_CM4.ld`：**不需要任何改动。**

现有配置已经正确，`RAM_D2` 区保持原样：
```
RAM    (xrw) : ORIGIN = 0x10000000, LENGTH = 192K     ← 不动
RAM_D2 (xrw) : ORIGIN = 0x30030000, LENGTH = 96K      ← 不动
```
`osc_ch1_buf` / `osc_ch2_buf` 已在 `RAM_D2` 内（`0x30030000` / `0x30034000`），只需**追加** `osc_ctrl` 到 `.dma_buffer` 段，三者**按此顺序声明**即依次落在 `0x30030000` / `0x30034000` / `0x30036000`。CM7 侧用固定常量 `0x30036000` 访问 `osc_ctrl`，CM4 在 `OSC_CH_Init()` 里校验其地址（见 §4.1）。

> **不要**把 `RAM_D2` 的 ORIGIN 改成 `0x30000000`。那是 CM4 的 `RAM` 区（同一个物理存储体的别名视图），改了会与 `.data`/`.bss`/栈重叠。详见 §4.1。

**CM7** — `CM7/stm32h747xx_flash_CM7.ld`：本设计不需要 CM7 链接 D2 区域（CM7 只通过宏地址访问，硬件上也无法访问 `0x10000000` 别名）。

### 10.4 构建系统

`Common/Inc` 加入两个工程的 `target_include_directories`（当前两个 `CMakeLists.txt` 只把 `Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c` 列为源文件，未加 include 路径）。

### 10.5 CM7 — LTDC

`CM7/Core/Src/ltdc.c` 当前只使能了行中断（`LTDC_IER_LIE`），**没有 FIFO 欠载中断**。加上 `LTDC_IER_TERRIE`。深存储接入后 SDRAM 压力上升，欠载表现为画面偶发撕裂，有中断才能定位。

### 10.6 CubeMX — 关闭 Cortex-M4 侧的 MDMA（**已完成** ✓）

**状态（2026-09-19 已验证）**：Cortex-M4 上下文中 MDMA 已禁用。`CM4/Core/Src/mdma.c` 与 `mdma.h` 已被删除，`CM4/Core/Src/main.c` 不再 `#include "mdma.h"` 也不再调用 `MX_MDMA_Init()`，`ProjectManager.functionlistsort` 中该项带 `false-` 前缀（禁用标记）。**两个工程均编译通过。**

残留（无害，`USER CODE` 之外，不要手改）：
- `CM4/Core/Src/stm32h7xx_it.c:58-59` 仍有 `extern MDMA_HandleTypeDef hmdma_mdma_channel0_sw_0;` —— 只是声明，无引用，不产生链接错误。
- `CM4/Core/Inc/stm32h7xx_hal_conf.h:93` 仍有 `HAL_MDMA_MODULE_ENABLED`，`CM4/mx-generated.cmake:56` 仍编译 `stm32h7xx_hal_mdma.c`。只是多编一个没人调用的 HAL 文件，无运行时影响。

**背景（为什么当初要关）**：

**现状（2026-09-19 重新生成后）**：CubeMX 在 **CM4 工程里也生成了 `CM4/Core/Src/mdma.c`**，并在 `CM4/Core/Src/main.c:125` 调用了 `MX_MDMA_Init()`。**它配置的是和 CM7 同一对 `MDMA_Channel0` / `MDMA_Channel1`** —— MDMA 是 D1 域外设，两个核看到的是同一个寄存器块。

**为什么危险**：`HAL_MDMA_Init()` 第一步是 `__HAL_MDMA_DISABLE()` 然后**忙等 EN 位清零**。如果这一步发生在 CM7 正在传输期间，会**直接掐断在飞的传输**，`MDMA_XFER_CPLT` 回调永远不会触发 → CM7 的搬运状态机永久挂起。

**当前为什么没炸**：CM4 的 `MX_MDMA_Init()` 在 main.c:125，属于极早期启动；CM7 要等 `RCC_FLAG_D2CKRDY` 之后才走自己的初始化（`CM7/Core/Src/main.c:166`），而第一个 MDMA 传输更晚（TouchGFX 起来之后）。两次 init 写的是完全相同的值，所以靠时序侥幸成立。以后 CM4 单独复位就会打死 CM7 的传输。

**另外一个更危险的点已经排除**：CM4 生成的 `mdma.c` **没有** `HAL_NVIC_EnableIRQ(MDMA_IRQn)`，`.ioc` 里也没有 `NVIC2.MDMA_IRQn`，而 `CM4/Core/Src/stm32h7xx_it.c` **没有实现 `MDMA_IRQHandler`**。所以 CM4 不会收到 MDMA 中断，**不会掉进 `Default_Handler` 死循环** ✓。

**（此段为历史记录，问题已按上面的方式解决。）** CubeMX 的 M4/M7 上下文可以分别启用或禁用同一外设 —— 在 Cortex-M4 上下文里禁用 MDMA 即可，CM4 的 `mdma.c` 会被自动删除。

---

## 11. 实施顺序

每一步都可独立验证，不通过不进入下一步。

### 第 0 步 — 硬件验证（约 20 分钟，**不做完不要往下走**）
1. CM7 以合理速度读写 `0x30030000`（读 16 KB 计时，多次取均值）
2. **MDMA 能否从 D2 SRAM 搬 8 KB 到 SDRAM** —— 整个方案的命门。AN5557 声明 D1 主设备可访问 D2 从设备，但 MDMA 走这条路径未经实测。
   **退路**：(a) CM7 用 CPU memcpy（18.75 MB/s 约吃 15% CPU，能跑但难看）；(b) 让 CM4 直接写 SDRAM（本设计已排除）。
3. ADC3 prescaler 确认为 DIV2
4. `CH1_ADC1`/`CH1_ADC2` 的高低 16 位归属（**在删除 `OSC_CH_TestPrint()` 之前**先跑一次）

#### 第 0 步结果（2026-09-19 实测）

| 项 | 结果 |
|---|---|
| 1. 跨核可见性 | **PASS** — CM4 写 D2、CM7 读，图案与地址约定两边一致 |
| 2. CM7 CPU 读 D2（朴素循环） | **101 MB/s = 18.93 cyc/word**（全是 load stall，不是循环开销） |
| 3. **MDMA D2 → SDRAM** | **PASS** — **两个通道**都 `rc=0`、`ErrorCode=0`、落点比对坏 0 个。**命门通过，方案成立** |
| 4. MDMA 吞吐（burst 阶梯，ch0） | SINGLE 129 MB/s → **8BEAT 242 MB/s** → 16BEAT 260 MB/s；ch1 SINGLE 56 → **8BEAT 128 MB/s**。全部坏 0 |
| 5. CPU 写 D2 | 478.94 MB/s = 4.00 cyc/word，**但这是 posted store 发射率，不是总线带宽** |

**设计含义**：
- **`SourceBurst/DestBurst` 必改 `8BEATS`** —— ch0 提速 1.87×、ch1 提速 2.31×，且数据全对。16BEAT 只再多 7%，不值得多担对齐风险。
- MDMA 提速后 ch0 达 242 MB/s，对 12.5 MB/s 的入货有 ~19× 余量。
- **但 [2] 的 CPU 扫描成本不变（全量 4,194,304 word ≈ 165 ms / 1.342 s ≈ 12% CPU）**：那是标量 load 的延迟瓶颈，CPU 发不出 burst，burst 帮不到它。
- **抖动约 16%**（同配置跨轮 15195 vs 17775 cyc），阶梯内还单调下降像有热身效应。**单次绝对值不可当真，只看倍数。**

**两个测量陷阱（别重复踩）**：
- **`memcpy` 在 `--specs=nano.specs` 下是逐字节桩**（`memcpy` 仅 28 字节，来自 `libg_nano.a(libc_a-memcpy-stub.o)`），实测比手写 word 循环**慢 3.5 倍**（66.78 vs 18.93 cyc/word）。**退路的 CPU 拷贝方案绝不能直接用 `memcpy`。**
- raw 实验里 16 位通道的增量必须是 `MDMA_SRC_INC_HALFWORD`，写死 `INC_WORD` 会只覆盖偶数下标，剩下填充值看起来像"坏一半"。

**未解释**：ch1 每字节比 ch0 差 3.8×（8BEAT 下 7.47 vs 1.99 cyc/byte），burst 长度只能解释 2×。CH2 只有 6.25 MB/s，128 MB/s 余量 20×，不阻塞设计。

**必须保留的两条经验**：
- **`BufferTransferLength` 必须 ≥ 数据宽度**，见 §10.2.1。
- **`HAL_MDMA_Start_IT` 前必须清 `CIFCR`**，见 §10.2.2。

### 第 1 步 — CM4 侧
改 DMA 缓冲地址与大小、加 HT 回调、实现触发扫描、写 `osc_ctrl`、发 HSEM、删 `OSC_CH_TestPrint()`。
**验证**：CM7 只打印 `seq` 和 `trig_pos`，确认连续递增、触发位置合理。

### 第 2 步 — CM7 搬运
MDMA 初始化 + 写指针推进 + 丢帧计数。
**验证**：CM4 喂已知斜坡信号，CM7 读 SDRAM 反算，核对数据与地址都对。

### 第 3 步 — MPU + 显示通路
改 MPU（CubeMX）、扩展 Model / Presenter / View。**先只通档 3（655 µs, step=8）** —— 一帧只需 2048 word，最省事，最容易看出对错。

### 第 4 步 — 时基与抽取
加 min/max 抽取、加 `timebase_idx` 接口、14 个档逐个走通。

### 第 5 步 — 触发状态机
冻结 / 重臂、预触发标记、"预热中"提示。**可以最后做** —— 前四步做完已是一个能用的示波器。

---

## 12. 未决与推迟项

| 项 | 状态 |
|---|---|
| 时基编码器接线与中断 | 推迟，接口预留（`timebase_idx`） |
| 触发迟滞自动整定 | 推迟，默认 `cfg_trig_hyst = 16`，可由 CM7 调 |
| min/max 归约金字塔 | 推迟，`0xD0177000` 起 561 KB 预留 |
| SD 卡 / SD NAND 归档 | 明确不做（写速率跟不上持续流） |
| 两个图表位置重叠 | 记录未解决，需挪位置或调透明度 |
| ADC3 prescaler 确认 | 第 0 步验证 |
| `CH1_ADC1`/`CH1_ADC2` 归属 | 第 0 步验证（**不影响 min/max 抽取的正确性**） |
| MDMA 跨域传输实测 | 第 0 步验证，方案最大风险点 |
| `TransferTriggerMode` 在 CubeMX UI 中的位置 | 找不到就直接改 `.ioc` 第 344/369 行（§10.2） |
| CM4 侧 MDMA 能否在 CubeMX 中按核禁用 | 待试（§10.6） |

---

## 13. 风险汇总

| 风险 | 影响 | 缓解 |
|---|---|---|
| ~~**MDMA 无法从 D2 SRAM 读数据**~~ | 方案不成立 | **已排除** ✓ 第 0 步实测两个通道都搬通、落点比对坏 0 个 |
| ~~`BufferTransferLength = 1` 触发 Block Size Error~~ | 任何 MDMA 传输 2 µs 内失败 | **已改** ✓ 64（§10.2.1） |
| ~~残留 TEIF 一使能就掐断通道~~ | 同上，且 `CESR` 读成 0 无法定位 | **已解决** ✓ 开跑前清 `CIFCR`（§10.2.2） |
| **MPU region 1 的 Strongly-ordered 属性** | CM7 CPU 读 SDRAM 极慢，刷新卡死 | **已改** ✓（§10.1） |
| ~~MPU region 2 Base 配在 `0x30000000`~~ | 共享区无缓存保护，CM7 读到陈旧数据 | **已改** ✓ `0x30030000`（§10.1） |
| ~~CM4 也调了 `MX_MDMA_Init()`~~ | CM4 复位会掐断 CM7 在飞的 MDMA 传输 | **已解决** ✓ 关闭 CM4 侧 MDMA（§10.6） |
| HT 中断不触发 | 半帧粒度失效 | 设置 `XferHalfCpltCallback` |
| `OSC_CH_TestPrint` 停/启 DMA | 采集被周期性打断 | 删除该函数 |
| HSEM 通知丢失 | 数据延迟（非丢失） | `seq` 为唯一真相 + 补搬逻辑 |
| MDMA 超时（>655 µs） | 半帧被覆盖 | 丢帧计数器；余量约 8 倍 |
| 总线吞吐只有 ~110 MB/s（单拍） | CPU 抽取扫描吃 ~12% CPU | **待改**：`SourceBurst/DestBurst = 8BEATS`（实测 ch0 129→242 MB/s，§第 0 步结果） |
| **CPU 抽取扫描的 165 ms/sweep 降不下来** | 吃 ~12% CPU | burst 帮不到（标量 load 发不出 burst）。真要降只能考虑让深存储区可缓存 + MDMA 后 invalidate |
| SDRAM 带宽争用 | LTDC 欠载撕裂 | 实测约 40% 利用率；使能 LTDC 欠载中断 |
| 深存储预热期 | 开机 1.342 s 内无完整历史 | 界面"预热中"提示 |
