# 8 通道逻辑分析仪实现计划

> 基于你 2026-09-26 给出的方案（M4 采集 → M7 找跳变 → 记录进 D1 → 存满后显示），
> 以及你对问题的回答（边沿速率 100 k~2 M 边沿/秒、触发 = 任一通道与上次不同、
> 无需预触发、速率是目标值可降）。
> 本文档只做设计与拆解，不含已落地的代码。
>
> 相关：`docs/cm4-build-optimization.md`（CM4 必须 -O2）

---

## 0. 一句话

**TIM2 以 20 MHz 触发 DMA2_Stream0 把 `GPIOG->IDR` 搬进 D2 SRAM 的循环双缓冲；
每个半帧采完，M4 用 2 号硬件信号量叫一次 M7，M7 在中断里读掉那一格、找跳变点、把 8 字节
记录写进 D1 AXI SRAM；记录满了或长时间没有跳变就停止，M7 在 tick 里把记录抽成
520 列 × 8 通道画到 Logic 屏的 8 个框里。**

```
TIM2 ──触发──> DMA2_Stream0 ──写──> D2 采样缓冲（32 KB，2×16 KB 环）
                                          │
                                   每半帧 take+release 2 号 HSEM
                                          ▼
                        M7 中断: 找跳变 → 8 B 记录 → D1 AXI SRAM（512 KB）
                                          │
                              记录满 / 空闲超时 → s_build_seq++
                                          ▼
                        M7 tick: 抽成 520 列 → 画到 Logic 屏 8 个框
```

**"每个半帧通知一次"是这套设计成立的前提**（§8.10）：半帧只有 409.6 µs，挂在 60 Hz 的
tick 上就是每 41 格才看 1 格。

**分工边界**：M4 只碰 D2，M7 碰 D2 和 D1。**M4 不做跳变检测，也不碰 D1。**

---

## 1. 你已经定下、本文档不再讨论的部分

| 决定 | 说明 |
|---|---|
| M4 采集（TIM2 + DMA2） | 采纳，你已在 CubeMX 改好 |
| M7 找跳变点、记录写 D1 AXI SRAM | 采纳，这是本文档的架构 |
| 存满后（或长时间无跳变）再显示 | 采纳 |
| 不用预触发 | 采纳，理由成立（见 §2.5） |
| 滑动开关 = 总开关，不开不采 | 采纳 |
| 有数据输入时点亮 LED | 采纳（改由 M7 驱动，见 §8.5） |
| **TIM1 不动，留给示波器控制采样率** | 采纳，本文档不把 TIM1 列为可用资源 |
| **MPU 只能在 CubeMX 里配** | 采纳。按本方案**一行 MPU 配置都不需要**（见 §5.2） |
| **采集期间不重绘屏幕** | **暂不考虑**，等真出冲突再说（见 §9） |

---

## 2. 需要澄清的几点

### 2.1 采样率：已改对，确认一下

`CM4/Core/Src/tim.c:90` 现在是 `Period = 11`，`.ioc` 里也是 `TIM2.Period=11`。✓

TIM2 在 APB1 上，APB1 分频 `/2` → PCLK1 = 120 MHz，**定时器时钟在 APB 分频不为 1 时翻倍**
→ TIM2CLK = 240 MHz：

```
fs = 240 MHz / (Prescaler+1) / (Period+1) = 240 MHz / 12 = 20 MHz ✓
```

**`fs` 是目标值不是保证值。** 定时器一定以 20 MHz 发请求，但 DMA 可能漏请求（总线争用），
漏了不报错，只会让实际速率低于标称。所以 §7.4 实测 `fs` 并交给 M7 算时间轴——
**时间刻度用实测值，不用标称值。**

### 2.2 DMA 模式：已改对

`CM4/Core/Src/tim.c:145` 现在是 `DMA_CIRCULAR` ✓，`.ioc` 里也是 ✓。

其余几项**已经是对的，不要动**：
- `PDATAALIGN_HALFWORD` + `MDATAALIGN_HALFWORD` + `FIFOMode = DISABLE`
  → 宽度相同，不需要 FIFO。正好匹配"存 uint16_t"的意图。
- `PeriphInc = DISABLE`（外设侧固定读 `GPIOG->IDR`）、`MemInc = ENABLE`（内存侧递增）。
- `Priority = HIGH`。

### 2.3 记录结构体：8 位装不下"跳变方向"

**澄清：`uint8_t` 装得下 8 个通道——前提是那 8 位表示"当前电平"，不是"跳变方向"。**
你原方案写的是后者（"1 为上升沿，0 为下降沿"），那个不行。

**反例**（CH1 = bit0 = PG2，CH2 = bit1 = PG3）：

```
初始:  CH1 = 0,  CH2 = 1
t=100: CH1 上升 (0→1)，CH2 纹丝不动
t=200: CH2 下降 (1→0)，CH1 纹丝不动
```

按"1=上升、0=下降"编码：

- **t=100**：bit0 写 1 ✓。可 bit1 写什么？CH2 没动。写 0 → 解出来是"CH2 下降了"（假的）；
  写 1 → "CH2 上升了"（也是假的）。**没有正确的值可写。**
- **t=200**：bit1 写 0 ✓，bit0 同样没有正确的值。该条字节 = `0b00000000`。

换一段历史：**t=200 时 CH1 和 CH2 同时下降**，字节也是 `0b00000000`。
**两个完全不同的物理事件产生同一条记录**——不是精度损失，是数据没了。

根子：一条记录要表达的信息是"哪几个通道跳了"（8 位成员资格）+ "各自朝哪跳"。
而**只要知道"哪几个跳了"和上一次的电平，方向就自动推得出来**（变 1 的必然原本是 0）。
所以 8 位恰好够，但必须编码**变化掩码**，方向是算出来的。

**改成存完整电平**（见 §6.2）：

```c
typedef struct {
    uint32_t ts;    /* 采样序号 */
    uint16_t smp;   /* 该时刻的 GPIOG 电平(掩码后的原始位) */
    uint8_t  flags; /* bit0 = 本条之前有丢帧 */
    uint8_t  rsvd;  /* 补齐到 8 字节 */
} log_rec_t;
```

**而且"用 uint8_t 省 3 字节"并不成立**：`{uint32_t; uint8_t}` 受 4 字节对齐约束，
`sizeof` 是 **8 不是 5**。数组放哪儿都占一样大。既然那几个字节本来就是白送的填充，
就该填满——`uint16_t smp` 填两个，`flags` 填一个。

`uint16_t` 相比 `uint8_t` 电平还多两个好处：

1. **热路径零重排。** 扫描循环本来就在算 `s & LOG_PIN_MASK`（`0x3E8C`），结果直接存。
   压成 8 位就得把 bit 2,3,7,9,10,11,12,13 搬成 bit 0~7——纯属白花。
2. **`flags` 免费。** 第 8 字节本来也是填充，拿来放 GAP 标志（§6.2）。

> 别用 `__attribute__((packed))` 压到 5 字节：那会让 32 位字段非对齐，编译器改成
> 逐字节存储，H7 上更慢，得不偿失。

### 2.4 DWT 不能用来给跳变打时间戳

M7 是在跳变发生**之后**才看到它的（DMA 先写进 D2，M7 之后才去读那半帧）。
20 MSa/s、16 KB 半帧下，**从跳变发生到被读到延迟 0～409.6 µs**，且随跳变落在半帧里的
位置而变。在读到的那一刻读 `DWT->CYCCNT`，得到的不是跳变的时刻。

**改成用采样序号当时间戳**（零成本、无延迟、丢帧也照样准）：

```
ts = 半帧序号 × 8192 + 半帧内样本偏移
真实时间 = ts / fs
```

半帧序号就是 DMA 的 HT/TC 计数，帧内偏移就是扫描循环的下标。
**DWT 仍要用，但用途换成实测采样率**（§7.4）。

### 2.5 "触发"这个概念消失了

你的回答是：**"当 8 个通道里面有一个和上次存的不一样就是触发信号"**。

那就是说——**触发条件和记录条件完全是同一件事**。不需要触发状态机，不需要等触发：

```
记录 0 = 开关打开那一刻的电平（ts = 0）
记录 i = 第 i 次检测到电平变化（ts = 变化发生的采样序号）
```

**这比"等触发再采"严格更好**：连"本来会被当成触发点之前的那一段"也一起留下来了。

你对"不需要预触发"的判断是对的，但**成立的前提是记录 0 必须存下初始电平**——
否则"触发点是下降沿说明之前一直是高电平"这个推理没有起点。所以记录 0 不是可选项。

> 附带好处：因为 `smp` 存完整电平，将来想加真正的触发（比如"CH3 上升沿触发"）
> 不需要改数据结构，只需在扫描循环里加一个条件。

### 2.6 显示几何：你看的是**框的上边沿**，不是线

我读错了。`LogicViewBase.cpp` 里每个通道是一个 **520×50 的框**，线画在框的**底边**：

```c
line1.setPosition(96, 6, 520, 50);   // 框: x=96, y=6, 宽520, 高50
line1.setStart(0, 50);
line1.setEnd(520, 50);               // 线在框内局部 y=50 = 框的底边
line1.setLineWidth(3);
```

所以我报的 `6, 62, 119, 176, 240, 295, 354, 412` 是**框的上边沿 y**，真正的线在**框上沿 + 50**。
按你说的画法（框上沿以下 5~45 px，高 40 px，上下各留 5 px）：

```c
LOG_BOX_TOP[8] = {6, 62, 119, 176, 240, 295, 354, 412};   /* Designer 里的框 y */
y_hi[c] = LOG_BOX_TOP[c] + 5;    /* 高电平 */
y_lo[c] = LOG_BOX_TOP[c] + 45;   /* 低电平 */
```

| 通道 | 框 y 范围 | 波形带 y 范围 | 线 y |
|---|---|---|---|
| CH1 | 6 – 56 | **11 – 51** | 56 |
| CH2 | 62 – 112 | **67 – 107** | 112 |
| CH3 | 119 – 169 | **124 – 164** | 169 |
| CH4 | 176 – 226 | **181 – 221** | 226 |
| CH5 | 240 – 290 | **245 – 285** | 290 |
| CH6 | 295 – 345 | **300 – 340** | 345 |
| CH7 | 354 – 404 | **359 – 399** | 404 |
| CH8 | 412 – 462 | **417 – 457** | 462 |

CH1 在最上 ✓。框间距 56~64、框高 50，互不重叠 ✓。
标签 `textArea2`~`textArea9` 在 x=29（左侧栏），波形区 x ∈ [96, 616]，**横向不冲突** ✓。

> `line2_1.setPosition(96, 62, 520, 50)` 和 `line2` 完全相同 → Designer 里的多余残留，可删。

---

## 3. 已确认的硬件事实

### 3.1 引脚

| 通道 | 引脚 | IDR 位 |
|---|---|---|
| LOG1 | PG2 | bit 2 |
| LOG2 | PG3 | bit 3 |
| LOG3 | PG7 | bit 7 |
| LOG4 | PG9 | bit 9 |
| LOG5 | PG10 | bit 10 |
| LOG6 | PG11 | bit 11 |
| LOG7 | PG12 | bit 12 |
| LOG8 | PG13 | bit 13 |
| LOG_EN（输出） | PG14 | bit 14 |
| EC_SW（开关） | PC4 | — |
| **M7_LED** | **PA3** | — |
| M4_LED | PB1 | — |

全部是 `GPIO_MODE_INPUT` / `GPIO_NOPULL`（`CM4/Core/Src/gpio.c:61-67`）。
**8 个通道都在 GPIOG，一次 16 位读就同时抓下所有通道——8 通道的采集成本和 1 通道一样。**

**`LOG_EN` 是低有效**：它驱动的是 8 路开关芯片的使能端，**拉低通道才通，拉高断开**。
`Logic_EN(level)` 这个已有函数是"把 level 原样写到引脚上"、不替谁翻极性，所以调用点上的
参数和函数名的直觉是反的 —— `logic.c` 里为此定义了 `LOG_EN_ON = 0` / `LOG_EN_OFF = 1`，
**必须用这两个名字写调用点，不要直接写 `Logic_EN(1)`**。写反了不报任何错，只会让采回来的
8 路电平全是乱的（开关从没打开过）。

**必须屏蔽掉的位**：PG14 是 `LOG_EN` 输出，会一起被读进来。掩码：

```
LOG_PIN_MASK = (1<<2)|(1<<3)|(1<<7)|(1<<9)|(1<<10)|(1<<11)|(1<<12)|(1<<13)
             = 0x3E8C
```

PG0/1/4/5/6/8/14/15 没配置（复位后浮空输入），**可能随机翻转**，所以这个掩码不是可选的——
不屏蔽会没事就触发。

**通道编号不重新打包。** 记录里 `smp` 就是掩码后的原始 GPIOG 位，"第几通道"只是显示层的
一张表 `LOG_CH_BIT[8] = {2,3,7,9,10,11,12,13}`。热路径零重排，将来改引脚也只改这张表和掩码。

### 3.2 你已经改好的部分

- `TIM2.Period = 11` → 20 MSa/s ✓（`tim.c` 和 `.ioc` 一致）
- `DMA2_Stream0 Mode = DMA_CIRCULAR` ✓（`tim.c` 和 `.ioc` 一致）
- `MX_DMA_Init()` 已使能 DMA2 时钟，已配好并打开 `DMA2_Stream0_IRQn`（NVIC 优先级 0,0）
- `CM4/Core/Src/stm32h7xx_it.c:253` 的 `DMA2_Stream0_IRQHandler` 已存在，调用
  `HAL_DMA_IRQHandler(&hdma_tim2_up)`
- `HAL_TIM_Base_MspInit()` 已完整初始化 `hdma_tim2_up` 并 `__HAL_LINKDMA` 到
  `hdma[TIM_DMA_ID_UPDATE]`
- 时钟源 `TIM_CLOCKSOURCE_INTERNAL`，`MX_TIM2_Init()` 已在 `main.c:126` 被调用

**硬件层不用再动了。**

### 3.3 空闲资源

| 资源 | 状态 |
|---|---|
| TIM2 | 已被本功能占用 |
| **TIM1** | **保留给示波器控制采样率，不要碰** |
| DMA2 全部 8 条流 | 空闲（CM4 只占 DMA1_Stream0/1 给 ADC1/ADC3） |
| D2 SRAM `0x30036060`~`0x3003FFFF` | 空闲 40,864 B，**且已经是非缓存**（MPU 区域 2） |
| D1 AXI SRAM `0x24000000` 512 KB | 完全空闲，**M7 独占**（见 §5.2） |
| D3 SRAM4 `0x38000000` 64 KB | 空闲，**本方案不用**（见 §5.3） |
| SDRAM `0xD1A00000` 6 MB / `0xD0177000` 561 KB | M7 可用，**本方案不用** |
| TCM（ITCM/DTCM） | 不可用——任何 DMA 引擎都访问不到 |

### 3.4 TouchGFX 的 Logic 屏已经有骨架

- 8 个 **520×50 的框**，位置见 §2.6，线画在框底
- 每通道一种颜色，已在 `LogicViewBase.cpp` 里定义好
- 标签 `textArea2`~`textArea9` = CH1~CH8，x=29
- `toggleButton1` 在 (657, 0)，**没有 `setAction`，是个装饰**——正好留给"开始采集"开关
- `Setting` 按钮 → Logic_Setting 屏；`button1` → 回 start_screen
- `line2_1` 与 `line2` 完全重叠，是残留

**没有任何数据通路**：没有 `Analyzer` 符号，`LogicView.cpp` 是空直通，`LogicPresenter` 是空的，
没有实现任何 `ModelListener` 回调。另外 `Logic_EN()`（`CM4/Mylibrary/Src/logic.c`）已经写好了
但**全工程从未被调用**——正好用来驱动 PG14。

---

## 4. 分工

| | M4 | M7 |
|---|---|---|
| 配置/启停 TIM2 + DMA2 | ✓ | |
| 收 HT/TC，报"哪个半帧好了" | ✓ | |
| 使能 LOG_EN | ✓ | |
| 测量实测采样率 | ✓ | |
| **读 D2 采样缓冲** | | ✓ |
| **找跳变点** | | ✓ |
| **写 8 B 记录到 D1** | | ✓ |
| 记录满 / 空闲超时判定 | | ✓ |
| **驱动 LED** | | ✓ |
| 抽点 + 画到屏幕 | | ✓ |

**M4 不碰 D1，M7 不做 DMA 配置。** 两边各自只干自己内存域里的事。

> M4 看起来"清闲"，这是对的——它的任务就是**保证采样时序**，而这件事有硬期限；
> 跳变检测没有硬期限（缓冲给了 409.6 µs 的余量机制，见 §9），所以放在 M7。

---

## 5. 内存布局

### 5.1 D2 SRAM（采样缓冲 + 控制块）—— 在已有 MPU 区域 2 内，零改动

```
0x30030000  osc_ch1_buf       16 KB    ← 示波器, 不动
0x30034000  osc_ch2_buf        8 KB    ← 示波器, 不动
0x30036000  osc_ctrl            96 B   ← 示波器, 不动
0x30036080  log_ctrl           128 B   ← 新增: 跨核控制块
0x30038000  采样缓冲 前半帧    16 KB    ← 新增: DMA 目标 (0x30038000–0x3003BFFF)
0x3003C000  采样缓冲 后半帧    16 KB    ← 新增          (0x3003C000–0x3003FFFF)
0x30040000  (区域 2 结束)
```

- **采样缓冲 32 KB，两个 16 KB 半帧 = 各 8192 个样本 = 各 409.6 µs @ 20 MSa/s**
- 缓冲正好铺满区域 2 剩下的空间，`0x30038000` 是 32 KB 对齐的，**不需要调整任何东西**
- 空闲残余：`0x30036060`–`0x3003607F`（32 B）、`0x30036100`–`0x30037FFF`（7,936 B），忽略

> **想再大一点**：把采样缓冲挪到 `0x30036080`（紧接 `log_ctrl` 之后，总 40,704 B，
> 半帧 20,352 B ≈ 509 µs），仍然完全在区域 2 内、仍然不需要改 MPU。
> **但 32 KB → 40 KB 只多 22%，按你"等真出问题再说"的原则，先用 32 KB 这个整齐的值。**

### 5.2 D1 AXI SRAM（记录区）—— M7 独占，零 MPU 改动

```
0x24000000  记录区  512 KB   ← 65536 条 × 8 B
```

**为什么不需要配 MPU**：`0x24000000` 不在 CM7 的任何 MPU 区域里，会落到默认的
**可写回可缓存**映射。**而这里只有 M7 一个访问者**——M7 写、M7 读，缓存就是唯一真相，
不会自相矛盾。**MPU 非缓存区域只在"这块内存有 CM7 之外的写者"时才需要**，本方案不满足
这个条件。

（顺带：`MPU_Config()` 在 `CM7/Core/Src/main.c` 里位于 `USER CODE BEGIN 4` / `END 4` 之外，
属于 CubeMX 管理区，代码里改了会被覆盖。本方案不需要碰它。）

> 你原计划说 256 KB。D1 AXI SRAM 整块 512 KB 都是空的、M7 独占，**没有理由只开一半**——
> 记录区容量直接等于采集时长上限，白给的一倍不要白不要。当然它只是一个 `#define`，
> 想保守就写 `LOG_REC_MAX = 32768`。

### 5.3 D3 SRAM4：本方案不用

`0x38000000` 那块 64 KB 完全空闲（全工程扫过，只有 MPU 区域 0 的地址提到它、无代码使用），
但**本方案用不上**：它只能当采样缓冲的备选，而 64 KB 相比 32 KB 只把 M7 的余量从 819 µs
提到 1.64 ms——同一量级，换不来质变，却要引入"DMA2 从 D2 跨域写 D3"这个本工程没有先例的
路径。**留着不动。**

---

## 6. 跨核 ABI

放在 `Common/Inc/sharedconfig.h`，两个核都编译（沿用现有 `osc_ctrl_t` 的做法）。

### 6.1 地址常量

```c
#define LOG_CTRL_ADDR         0x30036080u   /* D2, 区域 2 内, 已非缓存 */
#define LOG_SAMPLE_BUF_ADDR   0x30038000u   /* D2, 32 KB */
#define LOG_REC_BUF_ADDR      0x24000000u   /* D1 AXI SRAM, 512 KB */

#define LOG_SAMPLE_HALF_WORDS 4096u         /* 16 KB / 4 = 每半帧 4096 个 word */
#define LOG_SAMPLE_HALF_SAMPS 8192u         /* 每半帧 8192 个样本 */
#define LOG_PIN_MASK          0x3E8Cu
#define LOG_REC_MAX           65536u        /* 512 KB / 8 B */
#define LOG_CH_BIT {2,3,7,9,10,11,12,13}    /* 逻辑通道 1..8 → GPIOG 位 */
```

**用 `#define` 绝对地址，不用链接脚本**——和 `OSC_CTRL_ADDR` 一样。链接脚本那套是为了钉死
CM4 自己的数组，这里两块内存都不在 CM4 的链接区里，`#define` 就是绝对地址，保证强度一样，
而且 CM4 的 `.ld` 一个字都不用改。

### 6.2 记录结构体（M7 写，M7 读）

```c
typedef struct {
    uint32_t ts;    /* 采样序号: 从采集开始按采样周期计数 */
    uint16_t smp;   /* 该采样点的 GPIOG 电平, 位映射见 LOG_CH_BIT */
    uint8_t  flags; /* bit0 = LOG_REC_GAP: 本条之前有丢帧 */
    uint8_t  rsvd;
} log_rec_t;
#define LOG_REC_GAP  0x01u
OSC_STATIC_ASSERT(sizeof(log_rec_t) == 8, "log_rec_t 必须正好 8 字节");
```

**`ts` 是单调递增的采样序号**（不是数组下标）。记录 0 的 `ts` 恒为 0，
所以 `last_ts / fs` 就是这次采集的真实时长。

**方向不需要存**：`上升沿 = ~smp[i-1] & smp[i]`、`下降沿 = smp[i-1] & ~smp[i]`、
`变化掩码 = smp[i] ^ smp[i-1]`。信息完备。

**`LOG_REC_GAP` 必须存在**：M7 偶尔来不及读某个半帧时（§9），那 8192 个样本根本没被扫过，
中间可能藏着跳变。没有这个标志，"漏了一帧"和"这段时间没变化"在数据上完全一样，
**波形会画出一段根本不存在的平线**。

### 6.3 控制块（精简版）

因为 M7 自己就是记录的写者，记录相关的状态（条数、最后 ts、丢帧数）**全部留在 M7 本地即可**，
`log_ctrl` 只需要跨核的那几项：

```c
typedef struct {
    /* ---- CM7 写, CM4 读 ---- */
    volatile uint32_t cfg_run;         /* 0=停, 1=采 (滑动开关) */
    volatile uint32_t cfg_sample_arr;  /* TIM2 的 ARR: fs = 240e6/(arr+1) */
    volatile uint32_t cmd_seq;         /* 配置序号, CM4 看到变化才重新应用 */

    /* ---- CM4 写, CM7 读 ---- */
    volatile uint32_t frame_seq;       /* 单调递增: 已完成的半帧数 (HT/TC 各 +1) */
    volatile uint32_t state;           /* 0=idle 1=running */
    volatile uint32_t fs_hz;           /* 实测采样率 */
    volatile uint32_t diag_halves;     /* 完成的半帧数 (= frame_seq) */
    volatile uint32_t diag_cycles;     /* 最近一个半帧的 DWT 周期数 */
    volatile uint32_t diag_scan_max;   /* DMA 侧最大中断延迟(周期) */

    volatile uint32_t _rsvd[23];       /* 补到 128 字节 */
} log_ctrl_t;
OSC_STATIC_ASSERT(sizeof(log_ctrl_t) == 128, "log_ctrl_t 必须正好 128 字节");
```

`LOG_CTRL_ADDR + 128 = 0x30036100`，低于采样缓冲的 `0x30038000`，**不重叠**。
建议在 `LOG_Init()` 里加一条写死的自检（照抄 `OSC_CH_Init()` 的地址自检风格）。

**`cmd_seq` 的作用**：`cfg_*` 是 CM7 写的多个字段，CM4 不能逐字段轮询（会读到半更新状态）。
CM7 把 `cfg_*` 写完、`__DMB()` 之后再 `cmd_seq++`；CM4 发现 `cmd_seq` 变了才整体读一遍。

**`frame_seq` 是 M7 的消费游标**：M7 记住自己处理到第几个半帧，`frame_seq - consumed > 1`
就意味着**追丢了**（DMA 已经绕回来把那个半帧覆盖了）→ 丢一帧，下一条记录打 `LOG_REC_GAP`。

用 **`(frame_seq - 1) & 1`** 判断当前该读哪个半帧，**不是 `frame_seq & 1`**。`frame_seq` 是
"已完成的半帧数"，读它的这一刻第 `frame_seq` 格正在被 DMA 写、第 `frame_seq - 1` 格才刚写完，
所以刚完成的那一格编号是 `frame_seq - 1`。写成 `frame_seq & 1` 会读到正在被写的那一格 ——
半帧里会混进一截上一轮的数据，表现是波形上偶发的毛刺，而且只在 DMA 正好写了一半时才出现。

（`volatile` 写在结构体成员上还是写在指针上都可以，本实现用的是
`#define LOG_CTRL ((volatile log_ctrl_t *)LOG_CTRL_ADDR)`，成员不带 `volatile` ——
两种写法生成的代码一样，别混着用就行。）

---

## 7. CM4 侧

新增 `CM4/Mylibrary/Src/logic.c`（扩展现有文件）+ `Inc/logic.h`。**这一侧很薄。**

### 7.1 对外接口

```c
void     LOG_Init(void);   /* MX_TIM2_Init() 之后调一次 */
void     LOG_Poll(void);   /* 主循环里调, 和 OSC_CH_Poll() 并列 */
```

`LOG_Init()` 加在 `main.c:131` 附近（`USER CODE BEGIN 2` 里，紧跟 `OSC_CH_Init()`）。
`LOG_Poll()` 加在 `main.c:144` `OSC_CH_Poll()` 旁边。

> **主循环的纪律不能破**：`main.c:141-143` 说得很清楚，这个循环里不许有阻塞调用。
> `LOG_Poll()` 也必须是非阻塞的。

> **诊断打印要挪走**：`main.c:148-152` 的 `OSC_CH_DiagPrint()` 用了阻塞的 `usb_printf`，
> 每秒一次、每次几毫秒。逻辑采集期间它不影响 DMA（M4 不是在扫数据），但**跑完整个采集后
> 会拖慢 M7 的 M4 侧发布节奏**，建议采集期间关掉。

### 7.2 启停采样

启动（`cfg_run` 0→1）：

```c
HAL_TIM_Base_Stop(&htim2);                       /* 先停定时器 */
HAL_DMA_Abort(&hdma_tim2_up);                    /* 再停 DMA, 清残留状态 */
__HAL_TIM_SET_AUTORELOAD(&htim2, ctrl->cfg_sample_arr);   /* 20 MSa/s → 11 */
__HAL_TIM_SET_COUNTER(&htim2, 0);
s_frame_seq = 0;
Logic_EN(LOG_EN_ON);                             /* 已有函数; 低有效, 见 §3.1 */
hdma_tim2_up.XferHalfCpltCallback = log_dma_half;   /* 必须赶在 Start_IT 之前 */
hdma_tim2_up.XferCpltCallback     = log_dma_full;
HAL_DMA_Start_IT(&hdma_tim2_up, (uint32_t)&GPIOG->IDR, LOG_SAMPLE_BUF_ADDR,
                 2u * LOG_SAMPLE_HALF_SAMPS);     /* 16384 个**半字** = 32 KB */
__HAL_TIM_ENABLE_DMA(&htim2, TIM_DMA_UPDATE);     /* 少了这行 = 一个半帧都收不到 */
HAL_TIM_Base_Start(&htim2);                       /* 最后才放定时器 */
__DMB();
ctrl->state = LOG_ST_RUNNING;
```

> **`HAL_TIM_Base_Start()` 不会打开 DMA 请求。** 它只置 `CR1.CEN`；`DIER.UDE`
> （TIM_DMA_UPDATE）得自己 `__HAL_TIM_ENABLE_DMA()` 置。会替你做这件事的只有
> `HAL_TIM_Base_Start_DMA()`，而它顺带会把 `hdma->XferCpltCallback` 覆盖成
> `TIM_DMAPeriodElapsedCplt`（把我们的回调顶掉），所以也不能用它。
> 忘了这一行的症状：定时器在跑、DMA 在等、`frame_seq` 永远是 0，**一条错误都不报**。
> 停机时对应的 `__HAL_TIM_DISABLE_DMA()` 见下面"停止"。

> **最后一个参数是按"半字"算的，不是按 word。** DMA 的 `PDATAALIGN`/`MDATAALIGN` 都是
> `HALFWORD`（源是 `GPIOG->IDR`，一次搬 16 位），所以整块 32 KB = **16384 个半字**。
> 写成 `2 * LOG_SAMPLE_HALF_WORDS` 也能算出 8192，但那个 8192 是 word 数，实际只搬了
> 16 KB —— 后半块缓冲**永远不被写**，表现是波形每隔一格出现一段重复/空白，而且完全不报错。
> `sharedconfig.h` 里为这个专门定义了 `LOG_SAMPLE_DMA_LEN = 2u * LOG_SAMPLE_HALF_SAMPS`
> = 16384，两处注释都写明了是半字。

`Mode = DMA_CIRCULAR` 已在 CubeMX 里配好，**不要再重配 `hdma_tim2_up.Init`**。

**顺序有讲究**：
- DMA 的源地址必须是 `&GPIOG->IDR`（不是 `GPIOG->IDR`），`PeriphInc = DISABLE` 会让它
  每次都读同一个地址。
- 定时器**最后**启动，否则会有一小段"DMA 还没就绪但已经在发请求"的时间。
- `Logic_EN(LOG_EN_ON)`（= 拉低）要在 DMA 之前——让 8 路开关先稳定。

停止：`HAL_TIM_Base_Stop()` → `__HAL_TIM_DISABLE_DMA(&htim2, TIM_DMA_UPDATE)`
→ `HAL_DMA_Abort()` → `Logic_EN(LOG_EN_OFF)`（= 拉高）→ `ctrl->state = LOG_ST_IDLE`。

> **`MX_GPIO_Init()` 里 `LOG_EN` 的初值是 `GPIO_PIN_RESET`（低 = 开）**，所以开机到
> `LOG_Init()` 跑完这一小段里 8 路开关是开着的。无害（那会儿没人采），但记一笔，
> 免得哪天看到"没开采集电流却偏高"往别处找。

### 7.3 HT/TC：只做记账

```c
static void log_dma_half(DMA_HandleTypeDef *h)   /* HT: 前半帧 [0, 8192) 满 */
static void log_dma_full(DMA_HandleTypeDef *h)   /* TC: 后半帧 [8192, 16384) 满 */
/* 两个函数体都只有一句: if (h->Instance != DMA2_Stream0) return; log_half_done(); */
```

> **不要用 `HAL_DMA_XferHalfCpltCallback` / `HAL_DMA_ConvCpltCallback`。** 这两个
> 弱符号在本项目这份 HAL 里**没有任何地方引用** —— `HAL_DMA_IRQHandler()` 调的是
> `hdma->XferHalfCpltCallback(hdma)`（句柄里的函数指针），不是全局弱符号。
> 你把逻辑写在全局弱符号里，编译链接全过，中断来了却进不去，一声不响。
> 正确做法是注册到句柄上（见 §7.2 的 `hdma_tim2_up.XferHalfCpltCallback = ...`）。
>
> 写 `static` 而不是全局，也就顺手避免了和 ADC 的 DMA 撞车 —— 只有 `Instance`
> 对得上的那个句柄会调到它，其余根本进不来。`h->Instance` 那道判断仍然保留，
> 是给"以后有人往同一个句柄上挂第二个流"留的。
>
> **注册必须紧挨着 `HAL_DMA_Start_IT` 且在其之前。** `HAL_DMA_Start_IT()` 里是
> `if (hdma->XferHalfCpltCallback != NULL) CR |= DMA_IT_HT;` —— **HT 中断的使能位
> 是它看回调是否非空来决定的**。注册晚一步，这一次就只剩 TC：半帧全部静默跳过，
> 消费侧按半帧计数，看到的是每隔一帧才来一次。

回调体极短——只有三行：

```c
if (h->Instance != DMA2_Stream0) return;
uint32_t now = DWT->CYCCNT;                  /* DWT 由 LOG_Init() 自己使能, 见下 */
s_diag_cycles = now - s_last_tc;
s_last_tc = now;

ctrl->frame_seq++;                           /* 发布: 哪个半帧好了 */
__DMB();
ctrl->diag_halves = ctrl->frame_seq;
```

**M4 不做扫描、不做检测、不碰 D1。** 这个 ISR 的耗时是常数级的，不会因为信号密集而变长。

> **DWT 由 `LOG_Init()` 自己使能，不要依赖 `oscillograph.c` 里那份。** 那边有个
> 功能一样的 `dwt_init()`，但它是 `static`、只在 `OSC_CH_Init()` 里调，而
> `main.c` 里 `OSC_CH_Init()` 是注释掉的（示波器这会儿不跑）。不自己开一份的话
> `DWT->CYCCNT` 恒为 0，`diag_cycles` 恒 0、`fs_hz` 被除零保护挡成 0 —— 不崩不报错，
> 只是判据 2 和判据 3 永远量不出数来。两个核各有自己的 DWT，这里动的是 CM4 那个。

### 7.4 实测采样率

**期望的半帧周期**由**配置值**算，不能由实测的 `fs` 算 —— 那就成循环论证了：

```
期望周期 (DWT 周期) = SystemCoreClock × 8192 × (arr + 1) / LOG_TIM2_CLK_HZ
```

两个时钟都是 240 MHz 时它化简成 `8192 × (arr + 1)`，ARR = 11 时 = **98,304 周期**
（这就是判据 2 那个数）。保留两个时钟项是为了 CPU 分频一旦变了，这个换算是跟着变的而不是
悄悄错掉。

- 实测 ≈ 98,304 → DMA 一个请求都没漏，`fs` = 标称值
- 实测明显更大 → DMA 在漏请求，实际 `fs` 低于标称值

发布 `fs_hz = SystemCoreClock × 8192 / (diag_cycles 平滑值)`，**M7 用它算时间轴**。
这里用的是实测周期，所以 DMA 漏请求时算出来的 `fs` 会低于标称值 —— 正是想要的。
这样即使最后决定降速、或者 DMA 漏请求，屏幕上的时间刻度仍然是对的。

**这就是"实测达不到可降"的落地方式**：不是猜，是量出来再决定。

### 7.5 空闲超时判定在 M7，不在 M4

M4 不知道有没有跳变（它不检测）。所以"一段时间没有触发信息就停"由 M7 判定，
M7 写 `cfg_run = 0`。**M4 只认 `cfg_run`。**

### 7.6 与示波器互斥

你说两者不同时开。**不要依赖"用户不会同时开"**——两个屏幕各自有个开关，切屏不改变
对方的状态。所以在 CM4 里强制互斥：

- `LOG_Poll()` 看到 `cfg_run` 0→1 时：先 `HAL_ADC_Stop_DMA(&hadc1)` 停掉示波器采集，
  再启动逻辑采样
- 反过来，示波器那边（`OSC_CH_Init()` 重新启动时）先检查 `cfg_run`，为 1 则拒绝或先停

M7 侧也加一层：`LOG_Trace_Start()` 调 `OSC_Trace_Stop()`，反之亦然
（`oscillograph_trace.c` 里已有 `OSC_Trace_Stop()` 可复用）。

---

## 8. CM7 侧

新增 `CM7/Mylibrary/Inc/logic_trace.h` + `Src/logic_trace.c`，**结构照抄
`CM7/Mylibrary/Src/oscillograph_trace.c`**（那份 344 行实现就是"读环 → 归约 → 输出点"的
完整范例）。

**消费拆成两个入口**，分界线是"有没有硬期限"（理由见 **§8.10**，那是本节最初的写法改掉的
地方）：

| 入口 | 上下文 | 干什么 | 期限 |
|---|---|---|---|
| `LOG_Trace_OnHalfFrame()` | HSEM1_IRQn 中断 | 读半帧 → 找跳变 → 写记录 → 判满/超时 | **有**：409.6 µs |
| `LOG_Trace_Poll()` | `Model::tick()`（60 Hz） | 记录 → 520 列 → LED → 启动门 | 无 |

### 8.1 对外接口

```c
void LOG_Trace_Init(void);
void LOG_Trace_Start(void);        /* 写 cfg_* + cmd_seq++, 打开开关, 记启动时刻 */
void LOG_Trace_Stop(void);         /* cfg_run = 0。可以从中断里调 */
int  LOG_Trace_IsRunning(void);
void LOG_Trace_OnHalfFrame(void);  /* 中断上下文, 由 HAL_HSEM_FreeCallback 调 */
void LOG_Trace_Poll(void);         /* 在 Model::tick() 里调, 每个 tick 都要调 */
uint8_t LOG_Trace_GetCols(const uint16_t **level, const uint16_t **change, uint32_t *ncols);
```

`LOG_Trace_Poll()` **必须每个 tick 都调，不能加 `if (isRunning())` 之类的门**：采集是在中断里
停的（记录满 / 空闲超时），停完那一屏的归约得靠 tick 做；而那一刻 `IsRunning()` 已经是 0 了。
函数自己会先做归约和 LED，再判要不要等 CM4 起来。

### 8.2 消费半帧 + 找跳变（中断里）

```c
void LOG_Trace_OnHalfFrame(void)
{
    if (s_running == 0u) return;              /* 采集已停, 通知在路上 */
    if (LOG_CTRL->state != LOG_ST_RUNNING) return;   /* CM4 还没清完 frame_seq */
    if (s_full != 0u) return;

    uint32_t fs = LOG_CTRL->frame_seq;
    if (fs == s_consumed) return;             /* 没有新数据 */

    if (fs - s_consumed > 1u) {               /* 追丢了: 中间有半帧被覆盖 */
        s_gap_pending = 1u;
        log_diag_gap_recs++;
    }
    s_consumed = fs;                          /* 立即推进游标 (只处理最新那个) */

    const uint32_t half = (fs - 1u) & 1u;     /* 刚完成的是哪个半帧 */
    const uint32_t *w  = (const uint32_t *)(LOG_SAMPLE_BUF_ADDR + half * 16384u);
    log_scan_half(w, LOG_SAMPLE_HALF_WORDS, (uint32_t)((fs - 1u) * LOG_SAMPLE_HALF_SAMPS));

    /* 满 / 空闲超时 → 收工, 只记一个号, 归约留给 tick */
    const uint32_t now_ts = fs * LOG_SAMPLE_HALF_SAMPS;
    if (s_full != 0u || (now_ts - s_last_ts) > LOG_IDLE_TIMEOUT_SAMPS) {
        LOG_Trace_Stop();
        s_build_seq++;
    }
}
```

**`LOG_CTRL->state` 那一道不能省。** `LOG_Trace_Start()` 只是把 `cfg_run` 置 1，CM4 要等它下一圈
主循环才看到、才会去清 `frame_seq`。在那之前 `frame_seq` 里是**上一次采集留下的值**，拿去当
"新数据"会去扫一块上一次的残留缓冲，写出几条时间戳离谱的假记录 —— 而且它们的时间戳比后面
所有真记录都大，出图时会挤在最后几列上。`state` 是 CM4 在 `log_start()` 最后一步才置
`RUNNING` 的，那之前 `frame_seq` 已经清干净了，拿它当"可以开始消费了"的信号正好。

### 8.3 扫描循环

一次 32 位读拿两个样本，用 SWAR 把"这两个样本里有没有任何通道变过"压成一次判断，
**绝大多数迭代（没有跳变）走最快路径**：

```c
#define LOG_MASK_REP (LOG_PIN_MASK | (LOG_PIN_MASK << 16))

/* s_prev32: **上一个样本**复制到两个半字 —— 不是上一个 word, 见下面的说明 */
static void log_scan_half(const uint32_t *w, uint32_t nwords, uint32_t base_index)
{
    uint32_t prev = s_prev32;
    uint32_t idx  = base_index;          /* = 半帧序号 × 8192 */

    for (uint32_t i = 0; i < nwords; i++) {
        uint32_t v = w[i];                              /* s0 = 低半字, s1 = 高半字 */
        uint32_t inner = (v ^ (v >> 16)) & 0xFFFFu;     /* s0 vs s1 */
        inner |= inner << 16;                           /* 复制到两个半字 */
        uint32_t any = (inner | (v ^ prev)) & LOG_MASK_REP;
        if (any) {
            log_slow(v, idx, any);                      /* 慢路径, 罕见 */
        }
        prev = v >> 16;                                 /* 上一个样本 = 本 word 的 s1 */
        prev |= prev << 16;                             /* 复制到两个半字 */
        idx += 2;
    }
    s_prev32 = prev;
}
```

快路径 8 条指令 / 2 个样本 = **4 条/样本**（`shr` `eor` `and` `orr lsl` `eor` `orr` `and` `cbz`）。

> **`prev` 必须是"上一个样本复制到两个半字"，不是"上一个 word"** —— 这一条容易写错成
> `prev = v;`，写成那样低半字算出来是 `s0[i] ^ s0[i-1]`，中间隔着一个 `s1[i-1]`，不是相邻
> 两拍，也就不是要判的边界变化。反例：`s1[i-1]=0, s0[i]=1, s0[i-1]=1, s1[i]=1` —— 一拍的
> 窄脉冲，快路径整条放过去，跳变连记录都不会有。
>
> 改成"上一个样本"之后：低半字 = `s0[i] ^ s1[i-1]`（正是要判的边界变化），高半字 =
> `s1[i] ^ s1[i-1]`（有假阳性，但不会漏）。高半字那一项严格说是多余的，`inner` 已经覆盖了
> `s0[i] ^ s1[i]`；留着只是为了让快路径的推导简单，代价是偶尔多走一次慢路径，慢路径自己会用
> 精确条件把它筛掉。

慢路径只处理"这一对样本里有跳变"的迭代，用 `(s ^ prev16) & LOG_PIN_MASK` 判断每个样本
是否真的是变化点，是就写记录：

```c
static void log_slow(uint32_t v, uint32_t idx, uint32_t any)
{
    uint32_t s0 = v & 0xFFFFu, s1 = v >> 16;

    if ((any & LOG_PIN_MASK) && (((s0 ^ s_prev16) & LOG_PIN_MASK))) {
        log_rec_write(idx, s0 & LOG_PIN_MASK);
    }
    if ((any & (LOG_PIN_MASK << 16)) && (((s1 ^ s0) & LOG_PIN_MASK))) {
        log_rec_write(idx + 1u, s1 & LOG_PIN_MASK);
    }
    s_prev16 = s1;
}
```

> **`inner` 那一项为什么要复制到两个半字**：s0 和 s1 之间的变化同时属于这两个样本，
> 只标在低半字会漏掉 s1。

> **中断里的循环要能提前退出**（`if (s_full) break;`）：记录写满之后 `log_rec_write` 每次都会
> 立刻返回，但它每次都要判一次满、而且第一下就调了 `LOG_Trace_Stop()`。密集信号下半个半帧能
> 有几万次调用，不在这里拦一道的话那 409.6 µs 全花在空转上。

> 这段是**骨架**，不是最终代码。`docs/cm4-build-optimization.md` 第 9 节记过一个教训：
> 编译器会不会把状态提进寄存器、会不会走数据相关的分支，只有反汇编能看到。
> **写完必须反汇编确认快路径里没有访存、没有意外的分支。**

### 8.4 记录写入

```c
static void log_rec_write(uint32_t ts, uint32_t smp)
{
    if (s_rec_count >= LOG_REC_MAX) {            /* 满 → 停止 */
        LOG_Trace_Stop();
        s_full = 1u;
        return;
    }
    log_rec_t *r = &((log_rec_t *)LOG_REC_BUF_ADDR)[s_rec_count];
    r->ts    = ts;
    r->smp   = (uint16_t)smp;                    /* uint16_t, 不是 uint8_t! */
    r->flags = (s_gap_pending && s_rec_count) ? LOG_REC_GAP : 0u;
    r->rsvd  = 0u;
    s_rec_count++;
    s_last_ts = ts;
    s_gap_pending = 0;
}
```

因为记录区是 **M7 自己写、自己读**的（§5.2），**不需要 `__DMB()`／发布序号那一套**——
单一访问者，没有任何一致性窗口。

> **`smp` 从头到尾都必须是 `uint16_t`**（§2.3 已经说过，这里再钉一次，因为它是一路传下去的）：
> `LOG_PIN_MASK = 0x3E8C` 有 bit 13，装进 `uint8_t` 会截成 `0x8C`——丢掉 3～8 通道，而且
> 第 8 通道（bit 13）**永远读到 0**，表现是"8 个框里只有前两个有波形"。同一条约束适用于
> 扫描里的 `lvl`/`prev`、记录里的 `smp`、列数组 `level[]`/`change[]`、以及 `GetCols()` 的出参。

### 8.5 LED

**用 M7 的 `M7_LED`（PA3），不用 M4_LED。** 因为"有没有数据输入"只有 M7 知道
（它做检测），跨核传给 M4 只会多一层延迟。PA3 目前只在 `CM7/Core/Src/gpio.c:64`
初始化时置位过一次，之后无人引用。

- 采集中且 `s_rec_count > 1`（至少见过一个跳变）→ **点亮**
- 停止 / 记录满 / 一个跳变都没有 → **熄灭**

**LED 只由 `LOG_Trace_Poll()`（tick）驱动，中断里绝不碰它。** 两边都写的话会出现"中断在
tick 的判断和写引脚之间插进来"——那次翻转被 tick 随后的写入吃掉，表现是灯偶尔不灭。所以
`LOG_Trace_Stop()` 里没有 `led_off()`，中断路径上也没有。tick 那边用本地变量记住实际状态，
只在**状态真的变了**时才写引脚。

### 8.6 采集生命周期

```
idle ──(开关 on)──> 写 cfg_run=1 ──> [中断] 消费半帧、写记录 ──┬─(记录满)──> 写 cfg_run=0
                                            ▲                 └─(空闲超时)─> 写 cfg_run=0
                                            │                                   │
                                            │                         s_build_seq++ (中断)
                                            │                                   ▼
                                            └──────────────────── [tick] 归约成列 → invalidate
```

- **记录满**：`s_rec_count >= LOG_REC_MAX`
- **空闲超时**（你说的"一段时间没有触发信息"）：`s_last_ts` 与当前样本号 `frame_seq × 8192`
  的差超过阈值（建议 1 秒 = 20e6 个样本）→ 停止
- 两种情况都在**中断**里写 `cfg_run = 0`，然后只做 `s_build_seq++`；**归约和 `invalidate()`
  在 tick 里**（中断不能碰屏幕要读的那两片列数组，见 §8.10）
- **启动门**：`LOG_Trace_Start()` 记下 `HAL_GetTick()`，tick 里超过
  `LOG_START_TIMEOUT_MS`（100 ms）还没见 `state == LOG_ST_RUNNING` 就自己 `LOG_Trace_Stop()`。
  没有这一道的话，CM4 那边 `HAL_DMA_Start_IT` 一旦失败，开关看着是 ON 的却永远没有波形，
  界面上没有任何提示

### 8.7 记录 → 列

屏幕波形区 x 从 96 到 616，共 **520 列**。

```c
#define LOG_TRACE_COLS  520
```

一条游标沿记录数组前进，把每条记录归到它落在的那一列：

```c
uint32_t n  = s_rec_count;
uint32_t ts1 = s_last_ts;                /* 记录 0 的 ts 恒为 0 */
if (n <= 1) { /* 只有记录 0, 画 8 条平线 */ }

uint16_t lvl  = rec[0].smp;              /* 初始电平 */
uint16_t prev = lvl, chg = 0;
uint32_t ri  = 1;                        /* 从记录 1 开始 */

for (uint32_t c = 0; c < LOG_TRACE_COLS; c++) {
    uint32_t ts_c = (uint32_t)((uint64_t)ts1 * c / (LOG_TRACE_COLS - 1));
    while (ri < n && rec[ri].ts <= ts_c) {
        chg |= (uint16_t)(rec[ri].smp ^ prev);  /* 这一列发生跳变的通道 */
        prev = rec[ri].smp;
        lvl  = prev;
        ri++;
    }
    level[c]  = lvl;     /* 列末电平 */
    change[c] = chg;     /* 列内跳变掩码 */
    chg = 0;
}
```

`level[c]` 和 `change[c]` 都是 **16 位**、位映射同 `LOG_CH_BIT`。**两个数组各 1040 字节。**

> `ts_c` 用 `uint64_t` 中间量，避免 `ts1 * c` 在长采集下溢出（`ts1` 可以到 20e6 量级）。
>
> 一条记录都没有（`n == 0`，开关刚合上就停了）时直接返回、`s_ncols = 0`，让控件什么都不画 ——
> 这和"只画平线"不是一回事：记录 0 都还没写下去，连电平都无从谈起。

### 8.8 绘制

新增 `LogicTraceWidget`（`touchgfx::Widget` 子类），**`draw()` 照抄
`OscillographView.cpp:64-154` 的 `OscTraceWidget::draw()`**——直接锁帧缓冲写 RGB565
（那里有 `USE_BPP != 16` 的保护，一并照抄）：

```c
static const uint16_t LOG_BOX_TOP[8] = {6, 62, 119, 176, 240, 295, 354, 412};
/* y_hi = LOG_BOX_TOP[c] + 5;  y_lo = LOG_BOX_TOP[c] + 45;  (见 §2.6) */

for (uint32_t c = 0; c < LOG_TRACE_COLS; c++) {
    uint32_t x = 96 + c;
    for (uint32_t ch = 0; ch < 8; ch++) {
        uint16_t bit = LOG_CH_BIT[ch];
        uint16_t y   = (level[c] >> bit) & 1u ? (LOG_BOX_TOP[ch] + 5)
                                              : (LOG_BOX_TOP[ch] + 45);
        put_pixel(x, y, COLOR[ch]);                     /* 水平段 */
        if ((change[c] >> bit) & 1u) {                  /* 竖条表示跳变 */
            for (uint16_t yy = LOG_BOX_TOP[ch] + 5; yy <= LOG_BOX_TOP[ch] + 45; yy++)
                put_pixel(x, yy, COLOR[ch]);
        }
    }
}
```

- `COLOR[ch]` 直接用 `LogicViewBase.cpp` 里已定义好的 8 个通道颜色
  （L1 (20,151,197) / L2 (35,204,22) / L3 (196,22,22) / L4 (118,22,196) /
   L5 (196,170,22) / L6 (0,158,126) / L7 (196,109,22) / L8 (196,14,181)）
- 每列最多 8 段水平 + 8 条竖条，520 列 ≈ 4160 次写 → 一次性重绘毫无压力

**没有跳变的通道**：`change` 为 0，只画水平段，自然是一条平线 ✓。
**一个跳变都没等到**（`n <= 1`）：画 8 条平线，电平取 `rec[0].smp` ✓。

### 8.9 接线

| 要做的 | 位置 |
|---|---|
| `ModelListener` 加 `virtual void logicUpdated(const uint16_t *level, const uint16_t *change, uint32_t ncols)` | `gui/include/gui/model/ModelListener.hpp` |
| `Model::tick()` 里调 `LOG_Trace_Poll()`（在"两路都停了就 return"的守卫**之前**） | `gui/src/model/Model.cpp` |
| `HAL_HSEM_FreeCallback` 里按掩码分派到 `LOG_Trace_OnHalfFrame()` | `CM7/Mylibrary/Src/oscillograph.c` |
| `LOG_Trace_Init()` 里 `HAL_HSEM_ActivateNotification(LOG_HSEM_MASK)` | `CM7/Mylibrary/Src/logic_trace.c` |
| `M7_OSC_Init()` 旁边调 `LOG_Trace_Init()`（在 `osKernelStart()` 之前） | `CM7/Core/Src/main.c` |
| `LogicView` 里 `toggleButton1.setAction(...)`，回调按 Model 状态调 `LOG_Trace_Start/Stop` | `gui/src/logic_screen/LogicView.cpp` |
| `LogicPresenter::logicUpdated` → 转给 View → View 转给 `LogicTraceWidget` | `gui/src/logic_screen/LogicPresenter.cpp` |
| 采集完成时 `invalidate()` 触发重绘 | 同上 |
| Cancel 键：`cancel.setAction(...)`，回调里先 `cancelLogicData()` 再跳转 | `gui/src/logic_data_screen/Logic_DataView.cpp`（见 §8.11） |
| `LogicView::setupScreen()` 里按 `ncols == 0` 调 `traceWidget.clear()` | `gui/src/logic_screen/LogicView.cpp`（见 §8.11） |

`LOG_Trace_Poll()` 必须在守卫**之前**：它自己停下的那一拍（记录满 / 空闲超时）恰恰常常是
示波器两路都停着的情况 —— 放在守卫后面的话，那一帧算好的列就永远发不出去，屏幕上一直是空的。

> **`toggleButton1.setAction()` 必须写在 `LogicView.cpp`，不能写 `LogicViewBase.cpp`。**
> 后者是 TouchGFX Designer 生成的，下次生成会被整个覆盖。
> `toggleButton1` 是 `LogicViewBase` 的 protected 成员，派生类能直接访问。

> **`line2_1` 是残留**（和 `line2` 坐标完全相同）。要删就在 Designer 里删。

### 8.10 消费为什么改到中断里 —— 2 号 HSEM 通道

本节是**改过的**。本节之前，§8.2 写的是"在 `Model::tick()` 里取一个刚完成的半帧"，算下来
行不通：

```
Model::tick()  周期 = 16.7 ms  (60 Hz)
半帧周期              = 409.6 µs
一个 tick 之间 DMA 绕回来   16.7 ms / 409.6 µs ≈ 41 个半帧
```

也就是**每 41 格才看 1 格，97.5% 的时间轴从来没被扫过**。`s_gap_recs` 会稳定在 ~60/s，
判据 6（"不追丢：`s_gap_recs` 全程为 0"）**从设计上就不可能通过**。而且这不是"偶尔漏"——
它是恒定漏，波形画出来的平线大部分是假的。

**修法：照抄示波器那一套，每完成一个半帧通知一次。**

| | 示波器（已有） | 逻辑分析仪（新增） |
|---|---|---|
| 信号量号 | 1 | **2** |
| 发送 | CM4 `osc_scan_half` 末尾 take+release | CM4 `log_half_done()` 末尾 take+release |
| 落点 | `HAL_HSEM_FreeCallback` → `M7_OSC_Poll()` | `HAL_HSEM_FreeCallback` → `LOG_Trace_OnHalfFrame()` |
| 搬运 | MDMA 把半帧搬去 SDRAM | 就地扫描 D2 里那一格 |

**take+release 就是"发通知"这个动作本身，不是抢锁** —— 那个信号量没有谁在等，只是借它把
CM7 的 `HSEM1_IRQn` 拉起来一次。写法和 `osc_scan_half` 末尾完全一样。

#### 中断线是**每核一条**，不是每个信号量一条 —— 这是最容易踩的地方

1 号和 2 号都从 CM7 的 `HSEM1_IRQn`（Interrupt 0）进来，使能位写在**同一个 `C1IER`** 里。

`HAL_HSEM_IRQHandler()` 的流程（`stm32h7xx_hal_hsem.c`）是：读整个 `C1MISR` →
照着它清 `C1IER` 的使能位 → 清标志 → **拿这个完整掩码调一次 `HAL_HSEM_FreeCallback`**。

所以：

- **NVIC 那边一个字都不用改**，`M7_OSC_Init()` 里已经把 `HSEM1_IRQn` 使能过了
- 但回调**必须能处理"一次进来两个位"**。写成 `if ((SemMask & OSC_HSEM_MASK) == 0) return;`
  的话，被一起带进来的 `LOG_HSEM_MASK` 就永远武装不回去了 —— 表现是逻辑采集**只工作一次**
  然后彻底没反应（而且没有任何报错）

回调长这样：

```c
void HAL_HSEM_FreeCallback(uint32_t SemMask)
{
    if ((SemMask & OSC_HSEM_MASK) != 0u) {       /* 示波器 —— 先武装再搬运 */
        HAL_HSEM_ActivateNotification(OSC_HSEM_MASK);
        m7_diag_irq++;
        M7_OSC_Poll();
    }
    if ((SemMask & LOG_HSEM_MASK) != 0u) {       /* 逻辑分析仪 */
        HAL_HSEM_ActivateNotification(LOG_HSEM_MASK);
        LOG_Trace_OnHalfFrame();
    }
}
```

两段各自独立，顺序无关。**"先武装再搬运"两边都不能省**：HAL 在进回调之前就把触发的那一位从
`C1IER` 清掉了，不重新武装只会进这一次中断。

#### 中断和 tick 之间只传一个单调记号

中断里**不做归约**（记录 → 520 列）。归约要读 `s_rec_count` / `s_last_ts` / 整个记录区，是
几百微秒的活，而 CM4 每 409.6 µs 就来一次 —— 放中断里等于把中断关住那么久。而且它写的那两片
列数组是给屏幕任务读的，放中断里就得考虑"读到一半被下一次采集覆盖"。

所以中断收工时只做 `s_build_seq++`（一条指令），tick 里发现 `s_build_seq != s_built_seq` 就
去归约。两边各管一段，不用互斥。

### 8.11 Logic_Data 屏：Cancel 清数据

Logic 屏右上角的 **Data** 按键（`LogicViewBase` 里的 `Setting`）跳到 `Logic_Data` 屏，
那一屏上是 **Save** 和 **Cancel** 两个键。Save 等 TF 卡文件系统，先不动；Cancel 的语义是
"这次采的不要了，回到还没采过的状态，下次从零开始"。

一条链，四段：

| 段 | 做什么 |
|---|---|
| `Logic_DataView::cancelCallbackHandler` | 先 `presenter->cancelLogicData()`，再 `application().gotoLogicScreenSlideTransitionEast()` |
| `Logic_DataPresenter::cancelLogicData` | `model->clearLogicData()` |
| `Model::clearLogicData` | `LOG_Trace_Clear()`；清 `logicLevel/logicChange`；`logicNcols = 0`；发 `logicUpdated(..., 0)` |
| `LOG_Trace_Clear` | `LOG_Trace_Stop()`，再清 `s_consumed / s_rec_count / s_last_ts / s_gap_pending / s_full / s_prev16 / s_prev32 / s_ncols / s_result_pending` |

三个地方是**故意的**，不是漏了：

1. **`LOG_Trace_Clear()` 里 `s_build_seq / s_built_seq` 不清。** tick 那边把 `s_built_seq`
   存了一份在函数里（`static`），把它清成 0 有可能和 tick 本地那份撞上，于是**漏掉一次归约**。
   这两个计数器是"版本号"不是"数据"，留着不动最安全。

2. **Cancel 的跳转在 View 里补。** `cancel` 这个 `ButtonWithLabel` 在 Designer 里配了
   interaction，生成的 `Logic_DataViewBase::buttonCallbackHandler` 里**写死了一句
   `gotoLogicScreenSlideTransitionEast()`**——所以不能只写 `setAction` 就完事：那是**换掉**
   它，跳转得自己补回来。`save` 没动，仍是 base 里那条空跳转。

3. **清完之后 Logic 屏上那屏旧波形靠 `setupScreen()` 擦。** `clearLogicData()` 发出的
   `logicUpdated` 走的是 `modelListener`，也就是**当前活动屏幕**的 Presenter——按 Cancel 时
   停在 Logic_Data 上，收到的是那一屏的 `Logic_DataPresenter`，它的 `logicUpdated` 是
   `ModelListener` 的默认空实现。所以 `LogicTraceWidget` 不会因此被擦。它靠的是回到 Logic 屏
   时 `LogicView::setupScreen()` 看见 `ncols == 0` 自己调一次 `traceWidget.clear()`。

**为什么 `Model` 不自己清、要让界面来调**：Save 以后要在这个界面里做，而它读的是清理**之前**
的数据。把"清"做成 Cancel 的隐含副作用的话，Save 那条路就得反过来先抢数据。现在
`clearLogicData()` 是个普通的显式调用，谁来调、什么时候调由界面决定。

> `LOG_Trace_Clear()` 从**屏幕任务**（按键回调）调，不在中断里调。它开头就是
> `LOG_Trace_Stop()`——清一个正被中断写的结构没有意义。理论上仍有一个微秒级窗口：停止
> 那一刻可能正好有个半帧在写记录，那一条会留在 `s_rec_count` 上；下一次 `LOG_Trace_Start`
> 从 0 重来，不影响。

---

## 9. 余量：M7 的实时性（先做，再测，别提前优化）

**这是本方案唯一的余量风险，说清楚它的形状。**

M7 必须在 DMA 绕回来覆盖某个半帧之前把它读完。32 KB 双缓冲下：

```
每个半帧的期限  = 16 KB / 40 MB/s = 409.6 µs
整块缓冲的期限  = 32 KB / 40 MB/s = 819.2 µs
```

**要分开两件事：**

**(a) 平均吞吐够不够。** 20 MSa/s 下 M7 每秒要做 1000 万次 32 位读。按本项目实测的
D2 访问延迟（约 19 周期/word）估：190 M 周期/s ÷ 480 MHz ≈ **40%**，加上跳变判断约一半。
**但那个 19 周期是 CM4 的数字，M7 走的总线路径不同，必须自己实测。**

**平均不够的话，加缓冲完全没用**——M7 长期必须以 40 MB/s 消费，缓冲只影响"能停多久"，
不影响"平均要干多少"。这种情况只能降采样率。

**(b) 偶发停顿。** 缓冲买到的是**容忍停顿的时长**（819 µs，或单个半帧的 409.6 µs）。
这是它唯一的用处。

**所以判据很明确：看 `s_gap_recs` 涨不涨。它是 0，就说明这套配置够了。**

> 这一条**只在消费改成中断驱动（§8.10）之后才有意义**。原来的写法挂在 60 Hz 的 tick 上，
> 恒定漏 97.5%，`s_gap_recs` 必然在涨 —— 那时候这个判据量的不是 (a)/(b)，量的是"tick 太慢"。

**"采集期间不重绘"这个优化先不做**（按你的要求）。真出冲突时的顺序是：
1. 先看 `s_gap_recs` 和各种诊断，确认是 (a) 还是 (b)
2. (b) 的话：把采样缓冲从 32 KB 加到 40 KB（§5.1，一行 + 改个地址）
3. 还不够：采集期间不重绘屏幕
4. (a) 的话：降采样率（CubeMX 改 `TIM2.Period`）

**注意**：`docs/cm4-build-optimization.md` 记过一条重要修正——**`-O2` 下这类循环的开销
是数据相关的**（编译器把它拆成"最快路径 + 越界分支块"）。空载测出来的只是下界的下界，
**必须带真实信号测**。跳变密集的信号才是最坏情况。

**中断优先级**：`HSEM1_IRQn` 现在是 5（`M7_OSC_Init` 里设的，理由是让 MDMA 完成回调
（优先级 0）能插进来及时清忙标志）。逻辑扫描挂进同一个中断之后，那个优先级也一起管着它 ——
如果有别的中断能压住 5 超过 409.6 µs，扫描就会追丢。真出现丢帧时，**先看是不是被谁挡了**，
再考虑加缓冲。

---

## 10. 参数与调优速查

| 参数 | 位置 | 默认 | 说明 |
|---|---|---|---|
| `fs` | CubeMX → `TIM2.Period` | `11` → 20 MSa/s | `fs = 240 MHz/(Period+1)` |
| 半帧大小 | `LOG_SAMPLE_HALF_WORDS` | 4096 word = 8192 样本 | 32 KB 里的两个 16 KB 半区 |
| 记录上限 | `LOG_REC_MAX` | 65536 | D1 的 512 KB / 8 B |
| 空闲超时 | `LOG_IDLE_TIMEOUT_SAMPS` | 20e6（1 s） | 采样点数 |
| 启动门超时 | `LOG_START_TIMEOUT_MS` | 100 ms | 等 CM4 置 `state = RUNNING` 的上限 |
| 通知通道 | `LOG_HSEM_ID` | 2 | 1 号是示波器，0 号是 boot 同步 |
| 抽点列数 | `LOG_TRACE_COLS` | 520 | 等于波形区宽度（96→616） |
| 波形带 | `LOG_BOX_TOP[c] + 5 / +45` | — | 见 §2.6 |

---

## 11. 验收判据

按顺序验，每一步都能独立判断对错：

| # | 判据 | 怎么测 |
|---|---|---|
| 1 | DMA 以标称速率采样 | 开机采 100 ms，采到约 200 万样本 |
| 2 | 半帧节拍正确 | `diag_halves` 以 **2441/s** 上涨（20 MSa/s ÷ 8192）；`diag_cycles` ≈ 98,304 |
| 3 | 采样率准确 | `fs_hz` 稳定在 20 MHz 附近 |
| 4 | 记录内容正确 | 输入已知方波，`rec[]` 里相邻 `ts` 的差 × `1/fs` = 半个周期 |
| 5 | 8 通道独立 | LOG1 和 LOG5 各接不同频率，`smp` 的对应位应各自翻转 |
| 6 | 不追丢 | **`s_gap_recs` 全程为 0**（最关键的一条） |
| 7 | 屏幕波形正确 | 8 个框里各出现一条方波，周期与输入一致，CH1 在最上 |
| 8 | 开关有效 | 开关关着时输入信号，`frame_seq` 不动、LED 不亮 |
| 9 | LED | 有跳变时亮，停止后灭（记录满的那一次也必须灭） |
| 10 | 互斥 | 从 Logic 屏开采集，回示波器屏时 ADC 不再采（或反向） |
| 11 | **两路通知互不干扰** | 示波器和逻辑分析仪分别单独开、都正常；**再两个都开过一轮之后分别重开，仍然正常**（验的是回调里两段各自重新武装） |

判据 6 和 11 一起看：6 说明单路没追丢，11 说明两路共用一条中断线时没有被对方吃掉武装位。
**11 失败的表现是"只用一次就死"** —— 第一次采集完全正常，之后那一路再也不响应，且没有任何
报错。真遇到了就往 "`HAL_HSEM_FreeCallback` 是不是提前 return 了" 上想。

> **判据 4/5 之前先确认一件事：8 路开关确实打开了。** 判据 4 若看到"相邻 ts 的差算出来
> 不是半个周期、或者干脆一条记录都没有"，第一个要看的是 `Logic_EN(LOG_EN_ON)` 有没有
> 真的把 PG14 拉低（§3.1）。这一处写反的症状和"信号没接上"完全一样 —— 8 路全静默 ——
> 但原因完全不同，别往探头上找。

**不要用的判据**：`docs/cm4-build-optimization.md` 第 7 节记过 `rate1`/`rate2` 公式曾经有
16 倍错误。新写的诊断打印先自己算一遍期望值再信。

---

## 12. 风险与降级路径

| 风险 | 概率 | 影响 | 降级 |
|---|---|---|---|
| **M7 偶发追丢半帧** | **中** | 波形有洞（已标 GAP，不会骗人） | 先加缓冲到 40 KB；再不行则采集期间不重绘 |
| **M7 平均吞吐不够** | 低 | 持续丢帧 | 降采样率（CubeMX 改一个值） |
| **DMA 漏请求** | 中 | 时间轴偏、有效速率下降 | 已设计：实测 `fs_hz` 给 M7 |
| **跳变极密集时 M7 处理不过来** | 中 | 丢帧 | 同第 1 条 |
| **诊断打印导致节奏被打乱** | 中 | 偶发丢帧 | §7.1：采集期间关掉 `OSC_CH_DiagPrint()` |
| **`HAL_DMA_*Callback` 与 ADC 撞车** | 低 | 偶发状态错乱 | §7.3：先判 `Instance` |
| **D1 AXI SRAM 被别的东西占用** | 低 | 地址冲突 | 全工程扫过它是空的；加自检 |
| **回调漏了重新武装信号量** | 中 | 那一路"只用一次就死"，无报错 | §8.10：两段各自 `ActivateNotification`，别提前 return；判据 11 专门验它 |
| **中断里做了归约** | 低 | 中断关住几百 µs → 成片追丢 | §8.10：归约只在 tick 里，中断里只 `s_build_seq++` |

---

## 13. 实施顺序

每个阶段结束都能独立验证，**不要跳阶段**。

| 阶段 | 内容 | 验证 |
|---|---|---|
| **0** | CubeMX 已改完（你做的）。写一个最小测试：启动 DMA，`HAL_Delay(100)`，把 D2 缓冲打出来 | 采到约 200 万样本；肉眼看出引脚翻转 |
| **1** | `sharedconfig.h` 加 ABI；CM4 加 `LOG_Init()` + 地址自检；CM4 的启停 + HT/TC 记账 | 判据 1、2、3 |
| **2** | M7: 读半帧 + 扫描循环（先只用 LOG1，掩码只留 bit2）+ 写记录 | 判据 4；**同时测扫描循环每样本的周期数** |
| **3** | M7: 2 号 HSEM 通道 + `LOG_Trace_OnHalfFrame` 中断入口（**先别接屏幕**，只在中断里记诊断）；CM4 互斥 | 判据 5、6、8、11 |
| **4** | M7: `LogicTraceWidget` 绘制（先画死数据） | 8 个框里出现 8 条平线 |
| **5** | M7: 抽点归约搬到 tick + 接 `ModelListener::logicUpdated` + `toggleButton1` + LED + 启动门 | 判据 7、9、10，端到端 |
| **6** | 调优：按 §9 的诊断决定要不要加缓冲 / 降速率 | 全部 10 条判据 |

**阶段 1 是最关键的里程碑**：它把 DMA 通路和采样率钉死。阶段 1 的数字不好看，
后面所有工作都要重做。

---

## 14. 已识别的坑清单

写完代码对一遍，这些都是本次调查中具体发现、**而不是泛泛而谈**的坑：

1. **`LOG_PIN_MASK = 0x3E8C` 不是可选的**——不屏蔽的话 PG14（LOG_EN，输出）和
   PG0/1/4/5/6/8/15（浮空输入）会造成随机触发
2. **`uint8_t` 只能编码"当前电平"，不能编码"跳变方向"**——0 位无法区分"下降"和"没动"（§2.3）
3. **`{uint32_t; uint8_t}` 的 `sizeof` 是 8 不是 5**——对齐会把"省下"的字节还回来
4. **`inner |= inner << 16`**——s0↔s1 之间的变化同时属于两个样本，只标低半字会漏
5. **记录区只有 M7 一个访问者，所以不需要 `__DMB()`/发布序号那一套**；但
   `frame_seq` 是 CM4 写、M7 读，**那一处需要 `__DMB()`**
6. **`frame_seq - consumed > 1` 就是追丢了**——必须打 GAP 标志，否则波形会画出一条
   根本不存在的平线
7. **本方案一行 MPU 配置都不需要**——记录区是 M7 独占（可缓存是一致的），
   采样缓冲在已有的非缓存区域 2 里。不要去 CubeMX 里加区域
8. **`MPU_Config()` 在 CubeMX 管理区**（`USER CODE BEGIN/END 4` 之外），代码里改了会被覆盖
9. **`TIM1` 是留给示波器控制采样率的**，不要碰
10. **DMA 源地址写 `&GPIOG->IDR`**，不是 `GPIOG->IDR`
11. **别把回调写在 `HAL_DMA_ConvCpltCallback` / `HAL_DMA_XferHalfCpltCallback` 里**——
    这两个弱符号在本项目这份 HAL 里没人调用，函数永远进不去。注册到句柄上
    （`hdma_tim2_up.XferHalfCpltCallback = log_dma_half`），并且必须赶在
    `HAL_DMA_Start_IT()` **之前**：HT 中断的使能位是它看这个指针非空才置的（§7.3）
12. **先配 DMA、再 `Logic_EN(LOG_EN_ON)`、最后启动定时器**
13. **`.ioc` 是唯一该改定时器/DMA 的地方**——`tim.c`/`dma.c` 是 CubeMX 管理区
14. **`toggleButton1.setAction()` 写在 `LogicView.cpp`**，`LogicViewBase.cpp` 是生成的
15. **`OSC_CH_DiagPrint()` 是阻塞的**，采集期间建议关掉
16. **空载基准不能替代带信号实测**——`-O2` 下扫描开销是数据相关的
17. **CM4 的 `-O2` 覆盖已经在 `CM4/CMakeLists.txt` 里**——不要去改 `CMakeCache.txt`
18. **8 路开关开着（`LOG_EN` 拉低）期间`M4`主循环里不能有阻塞调用**（`HAL_Delay` 之类）
19. **HSEM 是每核一条中断线，不是每个信号量一条**——1 号和 2 号共用 CM7 的
    `HSEM1_IRQn`，使能位在同一个 `C1IER` 里（§8.10）
20. **`HAL_HSEM_FreeCallback` 收到的是整个 `C1MISR`**——写成"不是我的就 return"会让另一位
    永远武装不回去，表现是那一路只用一次就死。两段要各自判断、各自 `ActivateNotification`
21. **`HAL_HSEM_FastTake` + `Release` 是"发通知"，不是抢锁**——那个信号量没有谁在等
22. **扫描读的是 `(frame_seq - 1) & 1` 那一格**，不是 `frame_seq & 1`——后者读到的是
    DMA 正在写的那一格（§6.3）
23. **中断里绝不能做归约、绝不能碰 LED**——归约几百 µs 会成片追丢，LED 两边都写会漏掉翻转
24. **`LOG_Trace_Poll()` 每个 tick 都要调，不能加 `isRunning()` 门**——采集是在中断里停的，
    停完那一屏的归约得靠 tick 做，而那一刻 `isRunning()` 已经是 0 了
25. **`LOG_EN` 低有效**——`Logic_EN(level)` 是直接写引脚的，`Logic_EN(1)` 是**关掉**开关。
    调用点一律用 `LOG_EN_ON` / `LOG_EN_OFF`，不要写裸的 0/1（§3.1）
26. **`HAL_TIM_Base_Start()` 不打开 DMA 请求**——只置 `CR1.CEN`，`DIER.UDE` 要自己
    `__HAL_TIM_ENABLE_DMA(&htim2, TIM_DMA_UPDATE)`。会替你做的只有
    `HAL_TIM_Base_Start_DMA()`，而它会顺手覆盖掉 `hdma->XferCpltCallback`，用不得（§7.2）
27. **本 HAL 的 `HAL_DMA_Start_IT()` 只置 TC/TE/DME 三个中断位**，HT 是看
    `XferHalfCpltCallback` 非空才补上的——所以"回调没注册"和"HT 中断没开"是同一件事，
    症状是半帧被整片跳过而不是报错（§7.3）
28. **`HAL_DMA_UnRegisterCallback()` 是唯一会清句柄回调的函数**，本项目不调它 ——
    注册一次就一直在，不需要每次启动重注册（每次重注册也无害）
29. **DWT 要自己使能**——`oscillograph.c` 里的 `dwt_init()` 是 `static` 且只从
    `OSC_CH_Init()` 调；那份 init 在 `main.c` 里是注释掉的，所以 `CYCCNT` 恒为 0，
    判据 2/3 会安静地永远读 0（§7.3）

---

## 15. 本文档没有覆盖、但值得记一笔的两件事

1. **`osc_ctrl_t` 里 `cfg_trig_hyst = 16` 是绝对计数而非比例**，且运行时无人修改
   （`docs/cm4-build-optimization.md` 第 10 节）。逻辑分析仪不碰这块，
   但换前端时会再遇到同一个问题。

2. **`LPUART`/TIM1 那条"示波器控制采样率"的线还没做**。TIM1 已在 CubeMX 里初始化
   （`Prescaler = 0`、`Period = 65535`），本文档特意不占用它。

---

## 16. 调试记录：两次"拨了开关不采集"

跑通之后连出两个症状，**表现都是"开关拨上去了却没反应"，但原因一个在 DMA 句柄的
状态机里、一个在 USB 打印上**。记在这里是因为两个**都不报任何错**，只能靠推理和
临时打印找出来。

### 16.1 症状一：采完一次之后就再也采不起来，Cancel 也没用

**现场证据**（在 `log_start()` 里把 Abort 前后的句柄状态打出来之后，一次就拿到了）：

```
[LOG] start: pre st=3 lock=0 err=0x0020 | abort=1 -> st=3 lock=0
[LOG] start FAILED ret=1 st=3 lock=0 err=0x0800
```

`st=3` 是 `HAL_DMA_STATE_ERROR`，`0x0020` 是 `HAL_DMA_ERROR_TIMEOUT`，
`0x0800` 是 `HAL_DMA_ERROR_BUSY`。

**成因链**（全在 `stm32h7xx_hal_dma.c` 里，环环相扣）：

1. `HAL_DMA_Start_IT()` 的**成功路径不释放 `Lock`** —— 只有失败分支里有
   `__HAL_UNLOCK()`。
2. `HAL_DMA_Abort()` 只在 `State == HAL_DMA_STATE_BUSY` 时才干活；`State` 是别的
   值一律**早退**，而且**一个字都不改 `State`**（只置 `NO_XFER` + unlock + 返回）。
3. `HAL_DMA_Abort()` 里有一段"写 `CR.EN = 0` 然后等它读回 0"的循环，超时是
   `HAL_TIMEOUT_DMA_ABORT = 5 ms`。超时就 `ErrorCode = TIMEOUT`、
   **`State = HAL_DMA_STATE_ERROR`**、返回。
4. `HAL_DMA_STATE_ERROR` 是**死路**：`HAL_DMA_Abort()` 从此永远走第 2 条那个早退
   分支，而 `HAL_DMA_Start_IT()` 要求 `State == READY` —— 于是**再也起不来，
   除了重新上电没救**。

**为什么一直没人发现**：`log_start()` 失败时是**全静默**的（唯一的后果是 `state`
停在 IDLE，M7 那边 100 ms 启动门超时后把开关拨回 OFF，界面上和"没信号"没有任何
区别）；而 `log_stop()` 又把 `HAL_DMA_Abort()` 的返回值**丢掉了**。两头都不吭声。

**修法**（`CM4/Mylibrary/Src/logic.c` 的 `log_start()`，压在
`HAL_DMA_Start_IT()` 之前）：

```c
if (hdma_tim2_up.State != HAL_DMA_STATE_READY) {
    hdma_tim2_up.Lock = HAL_UNLOCKED;   /* HAL_DMA_Init 不碰 Lock, 得自己来 */
    HAL_DMA_Init(&hdma_tim2_up);        /* 无条件 State=READY, ErrorCode=NONE, 重写 CR */
}
```

**但这条自救本身也会失败**：`HAL_DMA_Init()` 里有**同一个 5 ms 的 EN 等待、同一个
`HAL_TIMEOUT_DMA_ABORT`**（`stm32h7xx_hal_dma.c:261-275`），而且超时是**在重写
`CR` 之前就 return** 的。也就是说 `EN` 真卡住的时候，`HAL_DMA_Init()` 和
`HAL_DMA_Abort()` 一样救不回来，只是白等 5 ms 并把 `ErrorCode` 也涂成 `0x20`。
**每一次点击只买一次机会，而这次机会本身也会输。**

**`EN` 为什么会卡住 5 ms —— 没有定论。** 已排除的：

- **不是 burst 没凑齐**：CubeMX 配的是 `FIFOMode = DMA_FIFOMODE_DISABLE`（`tim.c`），
  也就是 direct mode，每笔就是 1 个半字，没有 burst 概念。
- **不是 F4/F7 那个 `SUSP` 状态**：H7 的 `DMA_SxCR` 压根没有这一位（只有 `EN` 和
  `TRBUFF`）。
- **不是 `HAL_GetTick()` 失真**：`pin idr=` 是每 500 ms 一条，日志里的节奏是对的，
  说明 `uwTick` 就是 1 ms —— **那 5 ms 是真的空转了 5 ms，`EN` 是真的没清掉**。
- **H7 上"`EN` 清不掉"这一类行为有先例**：HAL 里 Errata 2.22 的注释原文就是
  "UART/USART DMA transfer lock: DMA stream could be **lock**"，ST 的解药是初始化
  时置 `DMA_SxCR_TRBUFF`。但本项目的请求是 `DMA_REQUEST_TIM2_UP`，不属于
  UART/USART，那一位不会置 —— **这个"锁住"状态在本项目的配置下没有官方解药。**

要定案，缺的是一行探针（**还没加**）：

```c
usb_printf("[LOG] dma stuck: CR=0x%08lX NDTR=%lu TIM2_CR1=0x%04X TIM2_DIER=0x%04X\r\n",
           (unsigned long)DMA2_Stream0->CR, (unsigned long)DMA2_Stream0->NDTR,
           (unsigned)TIM2->CR1, (unsigned)TIM2->DIER);
```

读法：`CR` 的 bit0 还是 1 = 硬件真不放行；`TIM2_CR1` 的 `CEN` 或 `TIM2_DIER` 的
`UDE` 还没清 = 问题在停止顺序，不在 DMA 那边。

**兜底方案**（**还没实施**，但查过了是安全的）：`DMA2_Stream0` 是**全芯片唯一的
DMA2 用户** —— CM4 侧只有 `tim.c` 里这一条，CM7 一条 DMA 都没用（只有
`MDMA_Channel0/1`）。所以可以整块控制器复位来强行解锁，这是**唯一 100% 能把 `EN`
清零**的手段（`HAL_DMA_Abort()` / `HAL_DMA_Init()` 都只是"写 `EN=0` 然后祈祷"）：

```c
if (HAL_DMA_Abort(&hdma_tim2_up) != HAL_OK) {
    __HAL_RCC_DMA2_FORCE_RESET();
    __HAL_RCC_DMA2_RELEASE_RESET();
    hdma_tim2_up.Lock = HAL_UNLOCKED;
    HAL_DMA_Init(&hdma_tim2_up);
}
```

### 16.2 症状二：拨了开关没反应，要反复点几次才起得来

**成因**：`usb_printf()` 底下是 USB CDC，而 `cdc_write()`（`usb_printf.c`）在 CDC
端点没腾空时会**自旋**：

```c
#define CDC_TX_TIMEOUT_MS   100
while (CDC_Transmit_FS(...) == USBD_BUSY) {
    if (HAL_GetTick() - t0 >= CDC_TX_TIMEOUT_MS) return;   /* 自旋最长 100 ms */
}
```

**最长 100 ms。而 M7 那边逻辑分析仪的启动门 `LOG_START_TIMEOUT_MS` 也是 100 ms**
（`logic_trace.h`；`LOG_Trace_Poll()` 第 3 步：`state != LOG_ST_RUNNING` 且超时 →
`LOG_Trace_Stop()`，把开关收回去）。两个 100 撞在一起就是：

打印期间 CM4 走不到 `LOG_Poll()` → 看不到 `cfg_run` 的 0→1 边沿 → M7 的门却已经在
计时、并且到点 → **这一次点击白费**。而失败那一次会打两行（`start` +
`start FAILED`），**自己给下一次点击制造了同样的条件**。

**判据**：**插着 USB 线时必现，拔了线完全正常** —— `cdc_write()` 第一句
`if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return;` 就早退了。所以
"电池供电正常 / USB 供电不正常"这个说法**只在"线也拔了"的前提下成立**：差别不在
供电质量，在**线插没插**。这一点坑过一次 —— 先怀疑了电池/USB 的电源质量，不是。

**修法**：把打印关掉，而且是**一道总闸**而不是只关各个调用点 —— `usb_printf.c` 里
`#define USB_PRINTF_ENABLE 0`。关掉之后 `cdc_write()` 整个函数不参与编译、
`usb_printf()` 连 `vsnprintf()` 都不跑、`__io_putchar()` 变成一个空的 `bx lr`。
这样以后谁新加一句 `usb_printf()`、或者直接用 `printf()`（走 `__io_putchar()`），
都一并被关掉，不会有漏网的。配套把 `logic.c` 的 `LOG_DBG_HALF_PRINT` 置 0。

**遗留**：`CDC_TX_TIMEOUT_MS` 和 `LOG_START_TIMEOUT_MS` **这两个 100 还是相等的**，
只是现在没有打印去触发它。**要么关打印（现状），要么把 `CDC_TX_TIMEOUT_MS` 压到
0**（端点忙就丢这一行，绝不阻塞）—— 后者更好，因为打印能力可以一直留着。两条至
少做一条，否则下次谁把打印打开，这个 bug 原样回来。

### 16.3 还没做的

1. **"按键需要多按几次才有反应"这个现象本身不再追** —— 症状二解决之后它已经不复
   现，按决定记录在案、不再深挖。
2. 症状一里"`EN` 为什么卡住"的探针还没加，兜底的 DMA2 复位也还没实施。现在打印是
   关的，真要再查得**先把总闸打开、并且把 `CDC_TX_TIMEOUT_MS` 压到 0**。
3. `log_stop()` 至今仍把 `HAL_DMA_Abort()` 的返回值丢掉，而且它是**没有任何修复
   动作**的那一半（修复只在 `log_start()` 里）。真修的话应该把"修 DMA"收成一个
   函数，`log_start()` 和 `log_stop()` 都调它。
