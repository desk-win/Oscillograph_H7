# 双核固件升级（UART -> QSPI -> 内部 Flash A/B + 回滚）设计

> 状态：**待评审**（2026-10-16）
> 实施计划：[docs/superpowers/plans/2026-10-16-ota-upgrade.md](../plans/2026-10-16-ota-upgrade.md)
> 适用代码库：本仓库当前 `main`（CM7 应用 + CM4 应用 + `Common/` 共享 ABI）
> 命名约定：文档里 **CM7 = M7**、**CM4 = M4**，与代码中的 `CortexM7/CortexM4` 一致。

---

## 1. 目标与非目标

### 1.1 目标

1. 上位机通过 UART 把升级包（内含 CM7 固件 + CM4 固件）送给 CM7；CM7 先把整包落到外部 QSPI Flash 暂存区。
2. **只有整包 CRC 校验通过之后**，CM7 才把固件写入内部 Flash；写入后再做一次回读校验。
3. CM7 与 CM4 的内部 Flash 各划两个区（活动区 + 备份/候选区），任一核的新固件启动失败都能回到上一个可用版本。
4. 升级过程可断电：任一时刻断开电源，下次上电都能启动到「某个可用版本」或进入「BL 救援模式」，不出现砖机。
5. CM4 固件由 CM7 烧录，且 CM4 固件与 CM7 固件作为**一对**做兼容性校验（避免 `sharedconfig.h` 里那条「漏烧一个核 -> ABI 对不上」的坑）。

### 1.2 非目标（本设计不做，但预留位置）

- 固件加密/签名验证（预留 16 B 的签名字段与 `flags` 位，后续可接 ECDSA/AES-GCM）。
- 固件压缩（LZ4/heatshrink）——先做通链路，压缩是后续优化项。
- 通过 USB CDC / SD 卡升级。CM4 已有可用的 USB CDC（`usb_printf` 通路），CM7 有 FatFs+SD，**都可以作为后续的第二/第三传输通道**，但本设计只做 UART。
- 运行期「不停机升级」：升级期间设备停止采集，这是可以接受的（示波器/逻辑分析仪不属于不可中断业务）。

---

## 2. 现状勘察（本仓库实测事实）

这些是设计的地基，都来自当前代码/构建产物，不是推测：

| 项 | 事实 | 出处 |
|---|---|---|
| MCU | STM32H747XIH6，TFBGA240，2 MB 内部 Flash | `Oscillograph_H7.ioc`（`Mcu.CPN`） |
| 内部 Flash 结构 | 2 个 bank x 1 MB，每 bank 8 个 128 KB 扇区（`FLASH_SECTOR_0..7` 是 bank 内编号，擦除时还要给 `FLASH_BANK_x`） | `stm32h7xx_hal_flash.h:348-355`、H7 参考手册 |
| 当前 CM7 分区 | `0x0800_0000 + 1536 KB`（跨 bank1 全部 + bank2 前 4 个扇区） | `CM7/stm32h747xx_flash_CM7.ld` |
| 当前 CM4 分区 | `0x0818_0000 + 512 KB`（bank2 的 S4..S7） | `CM4/stm32h747xx_flash_CM4.ld` |
| CM4 启动地址 | 由选项字节给出，当前有效值 `0x0818_0000`；字段是**地址的高 16 位**（即 64 KB 对齐，值 `0x0818`）。「CM4 只能从 bank2 启动」这一条来自参考手册，**必须在 M0（T0.6/T0.8）实测确认**，本设计的 bank 划分依赖它 | `CM7/stm32h747xx_flash_CM7.ld:40-43`、`stm32h7xx_hal.c:707-731`、`stm32h747xx.h:23092-23098` |
| 双核启动握手 | CM7 完成时钟/MPU 初始化后，用 HSEM0 通知；CM4 在 `HAL_PWREx_EnterSTOPMode(D2)` 里等这个通知 | `CM7/Core/Src/main.c:120-166`、`CM4/Core/Src/main.c:98-118` |
| HSEM 占用 | `0`=双核启动同步，`1`=示波器，`2`=逻辑分析仪 -> `3` 及以上空闲 | `sharedconfig.h`（`OSC_HSEM_ID`、`LOG_HSEM_ID`） |
| QSPI | 引脚已配（PF6/PF7/PF8/PF9/PF10 + PG6），但 `ClockPrescaler=31`、`FlashSize=1`、**无 memory-mapped 模式、仓库里没有任何外部 Flash 读写驱动** -> 目前等于占位 | `CM7/Core/Src/quadspi.c`、`Oscillograph_H7.ioc` |
| UART | USART1 @ PA9/PA10，115200 8N1，**只有阻塞发送**（`HAL_UART_Transmit` 200 ms 超时），无 RX、无中断、无 DMA | `CM7/Core/Src/usart.c`、`CM7/Mylibrary/Src/uart_printf.c`、`CM7/Core/Src/stm32h7xx_it.c` |
| CRC | 只有 `MX_CRC_Init()`，默认多项式 `0x04C11DB7` / init `0xFFFFFFFF` / 输入输出都不反转（等价 CRC-32/MPEG-2）；**当前代码里没有任何地方真的算 CRC** | `CM7/Core/Src/crc.c`、`stm32h7xx_hal_crc.h:144-161` |
| FreeRTOS | heap 只有 **32 KB**；任务：TouchGFX 16 KB + default 1 KB + LOG_Analyze 1 KB + LOG_Save 2 KB | `CM7/Core/Inc/FreeRTOSConfig.h:72`、`CM7/Core/Src/freertos.c` |
| 镜像体积（实测 map） | CM7：Flash 用满到 `0x0809_4AB0` 约 **595 KB**，其中 TouchGFX 生成的位图/字库资源约 420 KB；CM4：约 **51 KB** | `CM7/build/*.map`、`CM4/build/*.map` |
| MPU | 已用 3 个区域：`0x3800_0000`(64 KB, 非缓存)、`0xD000_0000`(32 MB SDRAM)、`0x3003_0000`(64 KB, 非缓存)；剩 13 个可用 | `CM7/Core/Src/main.c`（`MPU_Config`） |
| 看门狗 / 保护 | IWDG/WWDG 都没启用；未配置 RDP；未启用 RTC/备份域（BKP 寄存器当前不可用） | `Oscillograph_H7.ioc`、`stm32h7xx_hal_conf.h` |
| 空闲引脚 | `PD6`、`PA0`、`PC6`、`PC7` 在 `.ioc` 里完全没被占用（PC6/PC7 正好是 USART6_TX/RX） | `Oscillograph_H7.ioc` |

---

## 3. 总体架构

```
     +------------+   UART(帧协议+CRC+ACK/重传)   +--------------------------+
     | 上位机工具  | ---------------------------> | CM7 应用（slot A 或 B）   |
     | pack/发送   | <--------------------------- |  收包->写 QSPI->校验->烧录 |
     +------------+        状态/进度/错误码        +------------+-------------+
                                                             |
                                   +-------------------------+-------------------+
                                   | 外部 QSPI Flash（暂存区 + 元数据镜像 + 资源）  |
                                   +-------------------------+-------------------+
                                                             | 回读校验
     +--------------------------------------------------------+-------------------+
     | 内部 Flash：CM7 BL / CM7 slot A / CM7 slot B / CM4 slot A / CM4 slot B /  |
     |             OTA 元数据扇区（追加式日志）                                    |
     +---------------------------------------------------------------------------+
```

### 3.1 组件职责

| 组件 | 位置 | 职责 |
|---|---|---|
| **CM7 BL**（新建） | bank1 S0，128 KB | 上电第一段代码。读元数据日志 -> 处理 CM4 的「上一轮没写完」恢复 -> 校验活动槽镜像（头 + 整镜像 CRC）-> 试用计数/回滚 -> 置 VTOR/MSP 跳转到活动槽。若没有任何可用镜像或收到强制标志，则进入 **UART 救援模式**（收包到 QSPI、校验、烧 CM7/CM4 槽）。 |
| **CM7 应用**（改造现有 CM7 工程） | slot A(`0x0802_0000`) 或 slot B(`0x0810_0000`) | 正常仪器功能 + OTA 服务：UART 协议、QSPI 暂存、整包校验、写**非活动**的 CM7 槽、写 CM4 槽、写元数据、请求重启。 |
| **CM4 应用**（现有 CM4 工程，仅改链接地址） | slot A `0x081C_0000`（固定启动地址） | 采集 + USB CDC，保持现有功能；新增「已启动确认」写入共享区。 |
| **共享 Flash 驱动**（新建 `Common/`） | 编译进 BL 与 CM7 应用 | 32 B flash word 编程、扇区擦除、回读校验、`.RamFunc` 版本、容错读。 |
| **QSPI Flash 驱动**（新建） | CM7 应用 + BL | JEDEC ID 自动识别、读/页写/扇区擦/块擦、页写后回读校验、暂存区管理。 |
| **元数据日志**（新建 `Common/`） | bank1 S7 128 KB + QSPI 镜像 | 记录「活动槽/试用计数/确认/各槽镜像描述/错误码」，追加式、双半乒乓、掉电安全。 |
| **上位机工具**（新建，Python） | 仓库内 `tools/` | 打包（`pack`）、经 UART 下载（`send`）、查询状态、回滚、恢复；与固件共用同一份 CRC 规范与自检向量。 |

### 3.2 三条设计铁律（后续所有实现都必须满足）

- **R1**：任何时刻只写「非活动副本」；并且**只写与当前执行代码所在 bank 不同的 bank**。同 bank 写入会让 CPU 取指停摆、且违反读-写（RWW）约束，这种场景必须改走 RAM 常驻函数（`.RamFunc`）+ 关中断。
- **R2**：正常升级路径**永不改写选项字节**。「改 CM4 启动地址」只允许作为一次性生产/迁移动作，且必须有回读校验与 SWD 救援预案。
- **R3**：任何时刻至少保有一份「已知可用」镜像：CM7 侧是未被触碰的另一槽；CM4 侧是 slot B（备份库）。不允许出现「两个槽同时处于未验证状态」。

---

## 4. 内部 Flash 分区

### 4.1 推荐方案（Variant A2）

| 区域 | 地址范围 | 大小 | 扇区 | 说明 |
|---|---|---|---|---|
| CM7 BL | `0x0800_0000 - 0x0801_FFFF` | 128 KB | bank1 S0 | 永久区，只在产线/SWD 更新 |
| CM7 应用 slot A | `0x0802_0000 - 0x080D_FFFF` | 768 KB | bank1 S1..S6 | 与 slot B **链接地址不同**，需单独构建 |
| OTA 元数据日志 | `0x080E_0000 - 0x080F_FFFF` | 128 KB | bank1 S7 | 追加式日志，双 64 KB 乒乓 |
| CM7 应用 slot B | `0x0810_0000 - 0x081B_FFFF` | 768 KB | bank2 S0..S5 | 与 slot A 跨 bank，保证 RWW 可写 |
| CM4 应用 slot A | `0x081C_0000 - 0x081D_FFFF` | 128 KB | bank2 S6 | **固定启动槽**（`BOOT_CM4_ADD = 0x081C`） |
| CM4 应用 slot B | `0x081E_0000 - 0x081F_FFFF` | 128 KB | bank2 S7 | 备份库（永远不执行，只做字节仓库） |

合计 128 + 768 + 128 + 768 + 128 + 128 = 2048 KB，正好用满 2 MB。

**为什么这样切**

1. **CM7 两个槽必须跨 bank**：应用在运行中要写「另一个槽」；只有跨 bank 才能靠 RWW 边跑边写（写同 bank 会阻塞取指，等于整个系统冻结，并且要把所有热路径函数搬进 RAM，代价远大于收益）。
2. **CM4 两个槽只能在 bank2**：CM4 的启动地址字段是 bank2 内的偏移（64 KB 粒度），bank2 归 CM4 的启动区，所以 CM7 的 slot B 和 CM4 的两个槽只能挤在 bank2 里，这就是 CM7 槽只能给到 768 KB 的原因。
3. **CM4 槽 A 固定、槽 B 只当仓库**：CM4 固件的链接地址永远是 `0x081C_0000`，槽 B 里放的是「同样是给 `0x081C_0000` 用的字节」，**从不执行**。这样 CM4 工程只需要一份链接脚本、一次构建，也不需要改选项字节/写跳转桩。
4. **元数据独占一个扇区**：128 KB 擦除粒度决定了它不可能和 BL 共享扇区（一擦就把 BL 擦了）。它放在 bank1 S7，代价是 BL 写元数据时要走 RAM 常驻代码（同 bank 写入）。
5. **768 KB vs 实测 595 KB**：留 29% 余量。发布构建（-O2 + 去掉调试）还会更小一些，但 **595 KB 里有约 420 KB 是 TouchGFX 位图资源**，这块后续应搬到 QSPI（见 4.3）。

### 4.2 一次性迁移（从当前布局到这个布局）

切分方案与现状不同，需要一次**产线/调试器**操作（不是 OTA 路径）：

1. 先把 CM4 的固件准备好并烧到新地址 `0x081C_0000`（CM4 链接脚本 `FLASH ORIGIN` 改 `0x081C0000`，`LENGTH` 改 `128K`）。
2. 改写选项字节 `BOOT_CM4_ADD0`：`0x0818` -> `0x081C`（即有效地址 `0x0818_0000` -> `0x081C_0000`）。写前关 CM4、写后回读校验 `SYSCFG->UR3[31:16]`。
3. SWD 烧 BL 到 `0x0800_0000`，烧 CM7 应用 slot A 到 `0x0802_0000`，烧 CM7 应用 slot B 到 `0x0810_0000`。
4. 写元数据种子记录：`active = A`、`trial = 0`、两个槽的描述（版本/长度/CRC）与 CM4 槽 A 的描述。
5. 回滚预案：CM4 起不来时，用 CubeProgrammer 把 `BOOT_CM4_ADD0` 写回 `0x0818` 并烧回旧固件（**这一条必须在动手前演练一遍**）。

### 4.3 备选方案与取舍

| 方案 | 差异 | 何时选它 |
|---|---|---|
| **A2（推荐）** | CM4 启动地址迁到 `0x081C_0000` | 想马上拿到 768 KB x 2 的 CM7 槽。代价：一次性改选项字节。 |
| **A3（长期最优）** | 把 TouchGFX 位图资源搬到 QSPI（memory-mapped XIP），CM7 镜像降到约 200 KB | 只要愿意先做「资源外置」这件事：内部 Flash 立刻宽松（CM7 槽可退到 384 KB，还能留出 M4 跳转桩/工厂镜像），升级包与传输时间也砍掉 2/3。**建议作为第二期**。 |
| **A1（保留 CM4 现有地址）** | CM4 仍在 `0x0818_0000`，则 bank2 只剩 512 KB 给 CM7 slot B -> CM7 每槽 <= 512 KB | 必须先做资源外置（即先做 A3），因为当前 595 KB 塞不进 512 KB。 |
| **B（M4 加自校验桩）** | CM4 启动地址指向一个 128 KB「桩」，由桩自己校验 A/B 并跳转 | 需要 M4 侧也能自主回滚、且不依赖 CM7 时。代价：CM4 需要 3 个扇区（桩+A+B=384 KB），CM7 槽要缩到 640 KB（也就是**必须先做资源外置**）。 |

> 决策建议：**先做 A2 把链路跑通，再做 A3 把资源搬走**。A2 的选项字节改动是一次性的，风险可控；A3 是纯收益，但依赖 TouchGFX 外置资源的适配，工作量大，不适合和 OTA 链路一起首次落地。

---

## 5. 镜像与升级包格式

### 5.1 通用约定

| 约定 | 值 |
|---|---|
| 字节序 | 小端 |
| CRC | **CRC-32/ISO-HDLC（zlib 兼容）**：多项式 `0xEDB88320`（反射）、init `0xFFFFFFFF`、refin/refout = true、xorout `0xFFFFFFFF`。自检向量：`"123456789"` -> `0xCBF43926` |
| 对齐 | 所有长度/偏移按 32 B（一个 flash word）对齐 |
| 版本号 | `major.minor.patch.build` 打包成 `uint32`：`(major<<24)|(minor<<16)|(patch<<8)|build` |
| 板型 ID | `board_id`（`uint16`）：仪器硬件版本；不匹配直接拒绝 |
| ABI 版本 | `abi_ver`（`uint16`）：与 `sharedconfig.h` 的结构体布局绑定，CM7/CM4 必须一致 |

> CRC 为什么不用片上硬件 CRC：片上外设默认是 CRC-32/MPEG-2（`0x04C11DB7`、不反射），和 PC 工具常用的 `zlib.crc32` 不一致，双方对不上时会浪费大量排查时间。软件表驱动 CRC-32 在 480 MHz 上算 600 KB 只要几毫秒（每次上电都算一遍也划算）。硬件 CRC 保留为后续加速选项，但**规范只认一个**（zlib 兼容）。

### 5.2 镜像头（每个可执行固件镜像都有，位于镜像起始处 32 B）

| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | `magic` | u32 | `0x4F544149`（`'OTAI'`） |
| 4 | `hdr_ver` | u16 | 镜像头版本，从 1 开始 |
| 6 | `core_id` | u8 | 0=CM7，1=CM4 |
| 7 | `slot_id` | u8 | CM7：0=A，1=B；CM4：固定 0 |
| 8 | `load_addr` | u32 | 镜像期望的加载/执行地址（校验用，必须与槽地址一致） |
| 12 | `total_len` | u32 | 含头的总长度（32 B 对齐） |
| 16 | `version` | u32 | 见 5.1 |
| 20 | `board_id` | u16 | 板型 |
| 22 | `abi_ver` | u16 | 跨核 ABI 版本 |
| 24 | `payload_crc` | u32 | 对 `[load_addr+32, +total_len)` 的 zlib CRC-32 |
| 28 | `hdr_crc` | u32 | 对本头前 28 B 的 zlib CRC-32 |

BL 校验顺序：`magic` -> `hdr_crc` -> `load_addr == 槽地址` -> `core_id/slot_id` 匹配 -> `payload_crc`（覆盖整镜像，唯一的「可信」判据）。

### 5.3 升级包（`.ospkg`）

```
+-------------------- 包总头 64 B --------------------+
| magic 'OSPK' | ver | board_id | abi_ver | pkg_flags |
| pkg_version | total_len | entry_count | hdr_crc     |
| pkg_crc（除本字段外整包）  | 保留 16 B（签名预留）      |
+-------------------- 条目表（每条 32 B） -------------+
| entry[0..n]：{core_id, slot_id, load_addr, len, crc, flags, 保留} |
+-------------------- 负载（32 B 对齐） ----------------+
| CM7 镜像(A 链接版) | CM7 镜像(B 链接版) | CM4 镜像     |
+------------------------------------------------------+
```

- CM7 两个槽**链接地址不同**，所以包里带两份 CM7 镜像；运行时应用只挑「不是自己那份」烧进另一个槽。
- 包里 CM4 镜像**只带一份**（链接地址固定 `0x081C_0000`）。
- 升级包不压缩（第一期）。`pkg_flags` 里预留给压缩/签名/差分。
- 建议单包上限 2 MB（对应 QSPI 暂存区一区的大小）。

### 5.4 构建产物

每个 CM7 构建产出 `Oscillograph_H7_CM7_A.bin`、`Oscillograph_H7_CM7_B.bin`（同一份源码、两个链接脚本 + `-DOTA_SLOT_ID`）；CM4 产出 `Oscillograph_H7_CM4.bin`；工具 `tools/otapack.py` 负责加头、算 CRC、拼包、生成 manifest（含各镜像版本与 CRC 的文本清单，便于人工核对）。

---

## 6. 外部 QSPI Flash 布局

按 16 MB（W25Q128/IS25LP128 级别）规划；8 MB 器件砍掉「资源区」即可。

| 区域 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| Metadata 镜像 | `0x000000` | 64 KB | 内部元数据日志的最新记录副本（内部日志不可读时的兜底） |
| Stage A | `0x010000` | 2 MB | 一次完整升级包的暂存区（含 stage 头） |
| Stage B | `0x210000` | 2 MB | 与 A 交替使用（磨损均衡 + 保留上一包） |
| 上一版归档 | `0x410000` | 2 MB | 上一个包（手工回滚用，可裁剪） |
| 资源区（A3 用） | `0x610000` | 余量 | TouchGFX 位图/字库，memory-mapped XIP |

**Stage 头（每区起始 256 B，含自身 CRC）**：`state`（EMPTY/RECEIVING/COMPLETE/VERIFIED/INSTALLING）、`bytes_written`（断点续传用）、`pkg_len`、`pkg_crc`、`pkg_version`、`last_frame_seq`、`host_session_id`、`hdr_crc`。

**写作规则**

- 只按页写（256 B），每次页写后回读校验；擦除以 4 KB 扇区为单位，只擦要写的区域。
- `bytes_written` 每写满 64 KB 更新一次（避免每帧都写头）。
- Stage 区不参与启动决策——**QSPI 只是通道，内部 Flash 的元数据才是权威**（QSPI 坏了设备仍能正常启动）。

---

## 7. 流程与状态机

### 7.1 CM7 自升级（谁在哪个 bank 执行都标出来）

1. 应用（运行于 slot X ∈ {A,B}）收到「进入升级」命令（UI 入口或 UART 命令）；停采集、隔离 CM4（见 §9），UI 切到全屏「正在升级，请勿断电」。
2. 握手：上报 `board_id / abi_ver / 当前版本 / 各槽状态 / QSPI 容量 / 最大帧长 / 可续传偏移`。
3. 收包 -> 逐帧 CRC + 序号校验 -> 写 QSPI Stage 区（另一区），按偏移续传。
4. 收包结束：CM7 从 **QSPI 回读**整包算 CRC，与包总头比对；再做条目/板型/ABI/长度/槽地址一致性检查。
5. 按条目选择「不是自己」的那份 CM7 镜像，从 QSPI 读出并写入**另一个槽 Y**（跨 bank，RWW，无需 RAM 常驻）。每 32 B 一个 flash word，写完后回读整槽算 CRC 校验。
6. CM4 镜像此时**只做校验与登记**（连同它自己的 CRC 一起算过），不落内部 Flash：烧 CM4 是 §7.2 的事，而且它需要先隔离 CM4、刷新备份槽 B，顺序不能和 CM7 槽的烧录混在一起。
7. 写元数据记录：`m7_state = PENDING(slot Y, version V, trial = 3)`、各槽描述（版本/长度/CRC）、以及 `m4_pending = (version W, 已校验)`。CM4 的实际写入由 §7.2 在同一个会话里完成（或由工具显式发起第二次 INSTALL 命令）。
8. 应用请求复位（或提示用户断电重启）。
9. BL 启动：读元数据 -> 校验 slot Y 头+CRC -> 通过则 `m7_state = TRIAL(slot Y, trial = 3)` 并跳转 slot Y；不通过则直接回滚到 slot X（**不写任何镜像，只改元数据**）。
10. 新应用启动：完成外设初始化 -> 启动 CM4 -> 检查 CM4「已启动确认」-> 全部通过后写 `m7_state = CONFIRMED`（`m4_state` 按 §7.2 的结果）。
11. 若新应用在 3 次启动内都没写 CONFIRMED（崩溃、看门狗、根本没跑起来），BL 把活动槽切回 X（元数据层面），继续正常工作。

### 7.2 CM4 固件升级与回滚

CM4 的槽 A 是固定启动槽，槽 B 是仓库。**槽 A 里永远是「能被 CM4 直接执行的镜像」**，槽 B 里的字节只用于回写 A。

1. 进入升级时先做「备份刷新」：若元数据表明 B 里不是「当前 A」的副本，则把当前 A 整镜像读出、擦 B、写入 B、回读校验（此时 B 就是「上一个可用版本」）。
2. 隔离 CM4：CM7 通过共享控制块 + HSEM 让 CM4 停止采集并进入 D2 STOP（复用现有 `HAL_PWREx_EnterSTOPMode` 路径），再把 `RCC_GCR.BOOT_C2` 置 1 把 CPU2 按在复位里（上电时序未知项见 §9）。
3. CM7 擦 slot A、把包里的 CM4 镜像（来自 QSPI）写入 A、回读整槽 CRC 校验。
4. 写元数据：`m4_state = PENDING(version W, source = pkg)`。
5. 释放 CPU2（`BOOT_C2 = 0`）-> CM4 从 `0x081C_0000` 重新开始执行（硬件按选项字节重取向量表）。
6. CM4 启动成功后把 `{magic, version, uptime}` 写进共享区（非缓存 D2 区）。
7. CM7 若在超时（建议 5 s，含最多 2 次重启重试）内没看到确认：
   - 擦 A、把 **B** 的字节写回 A、校验；
   - 释放 CPU2 -> 回到旧版本；
   - 写 `m4_state = ROLLED_BACK(reason)`。
8. 确认成功 -> `m4_state = CONFIRMED`。

### 7.3 状态机（元数据里的字段）

```
CM7: BOOT(A, trial=0) --升级--> PENDING(B,v2) --BL 校验通过--> TRIAL(B,v2,trial=3)
                                        |                            |
                                        | BL 校验失败                 +- 应用确认 -> CONFIRMED(B,v2)
                                        v                            +- 试用次数耗尽 -> ROLLED_BACK(A)
                                   ROLLED_BACK(A)                       （只改元数据，不擦写镜像）

CM4: CONFIRMED(A,v1) --写入 A(v2)--> PENDING(A,v2)
        ^                                    +- 确认 -> CONFIRMED(A,v2)（B 保存 v1）
        +---- 恢复：从 B 写回 A(v1) <----------+- 未确认 -> BROKEN -> 应用或 BL 从 B 恢复
```

### 7.4 BL 救援模式（防砖）

满足任一条件时 BL 停在救援模式，用 UART 提供「收包 + 烧录」的最小子集（HELLO/ERASE/DATA/VERIFY/INSTALL/REBOOT）：

- 两个 CM7 槽的头+CRC 都无效；
- 元数据记录全部不可读 / CRC 全错；
- 元数据里 `force_recovery` 位置位（可由「上电时按住某个按键」或上一条运行中的应用写 QSPI 请求块触发）；
- BL 自己在恢复 CM4 时失败。

LED 用固定闪烁模式提示「在救援模式等我」（CM7 的 `M7_LED`，PA3）。

### 7.5 断电点与期望结果

完整矩阵放实施计划里（见 [plans/2026-10-16-ota-upgrade.md](../plans/2026-10-16-ota-upgrade.md) 的「断电注入测试矩阵」）。核心期望：**任何一个断电点之后，设备要么启动旧版本（自动回滚），要么进救援模式；不允许变砖、不允许两槽同时失效。**

---

## 8. 元数据日志

### 8.1 记录格式（32 B = 一个 flash word，可原子写入）

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `magic` u32 | `'OTAM'` |
| 4 | `seq` u32 | 单调递增；`seq` 最大且 CRC 正确的那条就是最新状态 |
| 8 | `kind` u8 | 1=SELECT（活动槽/试用/确认），2=IMAGE（某槽镜像描述），3=ERROR |
| 9 | `gen` u8 | 第几「代」（对应哪个 64 KB 半区），用于乒乓 |
| 10 | `payload[18]` | 与 `kind` 相关的紧凑字段 |
| 28 | `rec_crc` u32 | 对前 28 B 的 zlib CRC-32 |

`SELECT` 的 payload：`m7_active`、`m7_trial_left`、`m7_confirmed`、`m4_state`、`m4_trial_left`、`m4_ver`、`flags`。
`IMAGE` 的 payload：`core_id`、`slot_id`、`version`、`total_len`、`payload_crc`。

### 8.2 写入 / 扫描规则

- 扇区切成两个 64 KB 半区；**同一时间只有一个是「当前代」**，新记录追加到当前代末尾。
- 当前代写满时：擦另一个半区 -> 把当前代扫出的最新状态作为新代第 0 条写进去 -> 之后继续追加。这样**任何时刻两个半区里至少有一个含完整状态**，不存在「为了腾空间而丢掉状态」的窗口。
- 扫描：两个半区各扫一遍，取「CRC 正确 + `seq` 最大」的记录。只读 `[0, 已用长度)`，已用长度由记录连续性推出。
- 每写一条记录同步把这个状态写到 QSPI 的 Metadata 镜像区（可选但建议），内部日志不可读时用镜像兜底。

### 8.3 读损坏记录（必须正面处理）

H7 内部 Flash 带 ECC：**读一个「写了一半」的 flash word 会触发 ECC 错误**（CM7 上是 BusFault 级）。所以扫描器不能天真地线性读：

1. 把扫描 + 读取函数放进 `.RamFunc`（同时避免同 bank 写入时取指停摆）；
2. 用一个 RAM 常驻的 `flash_read_word_safe()`，内部只做一次 word 读取，由专门的 BusFault 处理路径捕获后返回「本 word 不可读」；
3. 不可读的 word 直接跳过（记录本身有 CRC，跳过不会误判成「有效状态」）；
4. 若内部日志整体不可读 -> 用 QSPI 里的 Metadata 镜像；再不行 -> 进救援模式。

这一条必须在 M0 阶段用「人为在写记录时断电」造出真实坏 word 来验证，不能只靠推理。

---

## 9. 与 CM4 的协同

### 9.1 隔离（quiesce）

烧 CM4 槽（bank2）之前必须让 CM4 停止一切 Flash 取指与共享区写入：

1. CM7 在 `osc_ctrl` / `log_ctrl` 里置「升级中」标志（两路采集停），HSEM3 通知 CM4（**新号**，避开 0/1/2）。
2. CM4 在 `OSC_CH_Poll()` / `LOG_Poll()` 里看到标志后：停 ADC/TIM2 DMA、把 `state` 记为 IDLE、写「已停」应答，然后进入 D2 STOP（沿用现成的进入 STOP 的代码路径）。
3. CM7 等到应答 + 超时兜底（500 ms）后，把 `RCC_GCR.BOOT_C2` 置 1（CPU2 进复位）。
4. 全部烧写/校验做完，`BOOT_C2` 清零 -> CM4 从选项字节地址重新启动 -> CM4 走一遍它自己的启动流程（含它现有的 HSEM0 与 CM7 同步等待）-> CM7 侧必须**重新执行一遍现有的启动握手函数**（把 `main.c` 里 `Boot_Mode_Sequence_2` 那段抽成 `OTA_StartCM4()` 复用）。

### 9.2 确认（CM4 活着）

在共享区（非缓存 D2，建议紧邻 `LOG_CTRL` 之后）新增 32 B 状态块，ABI 里定义：

```
m4_status_t { u32 magic; u32 fw_version; u32 abi_ver; u32 boot_count;
              u32 uptime_ms; u32 fault_code; u32 reserved[2]; }
```

CM4 在自己的启动流程里先写 `magic/version/abi_ver/boot_count`，之后周期性更新 `uptime_ms`（主循环里顺带，不加阻塞调用）。CM7 用 `uptime_ms` 递增作为「活着」的最终判据，避免只看到一次写入就误判。

### 9.3 ABI 配对

`sharedconfig.h` 当前没有版本号字段（magic 只是打印用）。本次新增 `OSC_SHARED_ABI_VERSION`，并写进：

- CM7/CM4 镜像头（`abi_ver`）；
- 升级包总头；
- `m4_status_t`。

安装时三者不一致就拒绝升级；运行时 CM7 发现 CM4 的 `abi_ver` 不匹配则在界面上明确报「配对固件不一致」，而不是让数值乱跳。

### 9.4 上电时序未知项（必须实测）

- 上电后 CM4 是否**立即**开始执行它自己的 Flash 代码（当前 CM4 代码在 `HAL_Init()` 之前就进 STOP，说明它至少执行了若干条指令）；`RCC_GCR.BOOT_C2` 的复位默认语义也要实测。
- 若结论是「CM4 上电即跑」，那么「CM7 正在写 CM4 槽 A 时断电」这一种情况下，下次上电 CM4 可能短暂执行半写镜像。对策（本设计已包含）：**先写有效载荷、把向量表所在的首个 flash word 留到最后写**；BL 在第一时间（时钟配置之前）就把 `BOOT_C2` 置 1 抢住 CM4；BL 依据元数据 `m4_state != CLEAN` 无条件从 B 恢复 A。残余风险窗口约几百毫秒，且只在「恰好写 A 时断电」出现，需要专门覆盖测试。

---

## 10. 关键实现硬约束（清单）

1. **编程粒度 32 B**：H7 内部 Flash 按 256-bit flash word 编程，`HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, ...)` 的地址与源缓冲都必须 32 B 对齐，长度必须是 32 B 的整数倍。缓冲放 DTCM 或 D2。
2. **PSIZE 与 VDD**：flash word 的并行编程宽度受供电电压限制（低压只能用小并行度）。设备固定 3.3 V 时按最大并行度写；若将来有低压工况，编程前按电压设置 PSIZE。需在 M0 对照参考手册的 Flash 特性表核对一次。
3. **RWW 规则**：写 bank P 时，从 bank Q（Q 不等于 P）读可以继续；从 bank P 读会被阻塞。因此「谁在写、谁在跑」必须分清（见 R1）。
4. **RAM 常驻代码**：擦/写**自己所在 bank** 时必须走 `.RamFunc`（BL 写元数据扇区、BL 的救援烧录），执行期间关中断。
5. **Cache / ART 失效**：写完之后、读回校验或跳转之前，必须让相关缓存失效：
   - CM7：`SCB_CleanInvalidateDCache()` + `SCB_InvalidateICache()`（或按地址的版本）；
   - CM4 侧由「进复位再重启」天然获得干净的取指环境（这也是升级完成后要复位 CPU2、而不是让它继续跑的原因）。
6. **DMA 缓冲必须在非缓存区**：UART RX 环形缓冲、QSPI 搬运缓冲都要放在 MPU 区域 2（`0x3003_0000` + 64 KB，非缓存）或新开的非缓存区域；否则 D-Cache 会读到陈旧数据。
7. **调度与中断**：擦/写期间不要让调度器切到会访问 Flash 的其他任务；同 bank 写入期间关中断；跨 bank 写入可以开着中断，但要持互斥量，保证任一时刻只有一个 flash 操作。
8. **看门狗**：当前没有启用；实现时预留喂狗钩子（`OTA_WDG_Kick()`），将来加 IWDG 时擦/写循环里必须调用。
9. **任务与内存预算**：FreeRTOS heap 只有 32 KB，已用约 20 KB。OTA 任务栈要小（先给 1 KB，实测后再定），**协议缓冲/帧缓冲不要放栈上**，用静态缓冲放 D2 非缓存区。
10. **跳转前收尾**：BL 跳转前关中断、清 NVIC 使能/挂起、`HAL_UART_DeInit`/`HAL_QSPI_DeInit`、置 VTOR/MSP，然后再跳。不要留下已使能的外设和半配置的中断。
11. **绝不擦写正在运行的槽**；**绝不在现场改选项字节**（R2）。
12. **每次写入后必须回读校验**：QSPI 页写后校验、内部槽写完整槽 CRC 校验；校验用直接算 CRC，不要依赖 `memcmp` 与缓存语义。

---

## 11. 需要新增/改动的文件清单

| 文件 | 责任 | 动作 |
|---|---|---|
| `CM7B/`（新目录，CM7 BL 工程） | BL 主流程、救援模式 | 新建（独立 CMake target + 自己的 `.ld`） |
| `Common/Src/ota_flash.c`、`Common/Inc/ota_flash.h` | 32 B 编程/擦除/回读/`.RamFunc`/容错读 | 新建 |
| `Common/Src/ota_meta.c`、`Common/Inc/ota_meta.h` | 元数据日志读写与扫描 | 新建 |
| `Common/Src/ota_image.c`、`Common/Inc/ota_image.h` | 镜像头解析/打包/CRC 校验 | 新建 |
| `Common/Inc/ota_layout.h` | **唯一**的分区表/槽地址/暂存区偏移事实源 | 新建（`.ld` 与它必须对齐） |
| `CM7/Mylibrary/Src/ota_service.c` / `Inc/ota_service.h` | OTA 状态机、烧录编排、CM4 协同 | 新建 |
| `CM7/Mylibrary/Src/ota_uart.c` / `Inc/ota_uart.h` | UART 帧协议（DMA+IDLE 收、ACK/重传/续传） | 新建 |
| `CM7/Mylibrary/Src/qspi_flash.c` / `Inc/qspi_flash.h` | QSPI 外部 Flash 驱动 + JEDEC 识别 + 暂存区管理 | 新建 |
| `CM7/Core/Src/main.c` | 抽 `OTA_StartCM4()`；OTA 入口钩子 | 改（USER CODE 段） |
| `CM7/Core/Src/stm32h7xx_it.c` | `USARTx_IRQHandler`（IDLE）+ DMA 中断 | 改（USER CODE 段） |
| `CM7/Core/Src/freertos.c` | 新增 OTA 任务 | 改（`USER CODE BEGIN RTOS_THREADS`） |
| `CM7/Core/Src/main.c`（`MPU_Config`） | 若开 QSPI memory-mapped，加 `0x9000_0000` 非缓存只读区域 | 改 |
| `CM7/Core/Src/quadspi.c` + `.ioc` | 正确的 ClockPrescaler/FlashSize/DMA | 改（**优先改 .ioc 再重生成**） |
| `CM7/stm32h747xx_flash_CM7.ld`、`CM4/stm32h747xx_flash_CM4.ld` | 新分区；CM7 再加一份 slot B 版本 | 改 |
| `CM7/CMakeLists.txt`、`CM4/CMakeLists.txt` | 双槽构建、BL target、`post-build` 生成 bin + 尺寸检查 | 改 |
| `CM7/TouchGFX/gui/...`（设置屏 + 新升级屏） | OTA 入口、进度、错误显示 | 改 |
| `tools/otapack.py`、`tools/otasend.py`、`tools/otaprotocol.py` | 打包/下载/协议与 CRC 库（含自检向量） | 新建 |

`.ioc` 里还要同步的：CM7/CM4 的 MemoryMap 区域（把 CM4 的 FLASH 区域改成 `0x081C_0000/128K`，CM7 的区域按新 BL/槽描述改）；选项字节 `BOOT_CM4_ADD0`；若用 USART6 则新增 USART6 异步模式 + DMA 通道。

---

## 12. 必须先做的前置确认（M0 门槛实验）

| # | 实验 | 为什么必须 |
|---|---|---|
| 1 | 读 QSPI flash JEDEC ID（`0x9F`），确认型号/容量/是否 4 字节地址 | 决定驱动分支与暂存区大小；也确认这颗片子真的焊在板上 |
| 2 | 从 bank1 执行时擦写 bank2（同时跑 LTDC + 定时器中断），确认无异常、无长时间停摆 | 验证 RWW 与「边跑边写」的前提 |
| 3 | 从 bank1 执行时写 bank1（走 `.RamFunc`），确认系统行为符合预期 | BL 写元数据扇区依赖它 |
| 4 | 实测 128 KB 扇区擦除时间、32 B flash word 编程时间（典型值/最坏值） | 决定升级耗时预算与超时参数 |
| 5 | 造一个「写一半」的 flash word 并读取，确认 BusFault 行为并跑通容错读 | 元数据日志的掉电安全全靠它 |
| 6 | 用 `RCC_GCR.BOOT_C2` 复位 CPU2 并让它从选项字节地址重启，确认可行；同时确认上电时 CM4 是否自动执行 | 决定 CM4 切换/确认流程与残余风险窗口 |
| 7 | 确认 PC6/PC7（USART6）是否引到对外接口 | 决定 OTA 用独立串口还是复用 USART1 |
| 8 | 改 `BOOT_CM4_ADD0` 到 `0x081C` 并回读校验，验证 CM4 能从新地址启动 | 一次性迁移的前提，也是唯一有砖机风险的步骤 |

---

## 13. 时间与容量预算

| 项 | 估算 | 备注 |
|---|---|---|
| CRC-32（软件表驱动） | 约 3-6 ms / 600 KB | 上电校验整镜像可接受 |
| 128 KB 扇区擦除 | 数百 ms 到数秒（待实测） | 单次升级要擦：CM7 槽 6 个扇区 + CM4 槽 1 个 + 元数据追加若干 |
| 32 B flash word 编程 | 数十微秒/word（待实测） | 600 KB 约 19200 word |
| UART 传输 | 115200：约 55 s / 600 KB；921600：约 7 s / 600 KB；1.2 MB（双份 CM7 镜像）在 921600 下约 14 s | 建议握手后升到 921600 |
| 单次完整升级（估） | 60-120 s（含双份 CM7 镜像 + CM4 + 校验 + 两次重启） | 主要取决于擦除时间与波特率；实测后回填 |

---

## 14. 待决问题（需要你确认）

1. **外部 QSPI Flash 的具体型号/容量**是什么？（驱动按 JEDEC 自动识别写，但暂存区大小与是否需要 4 字节地址依赖容量）
2. OTA 走 **USART1（复用调试口，PA9/PA10）** 还是 **USART6（PC6/PC7，独立口）**？后者要求这两个脚引到了对外接口。
3. 现场是否**已有部署设备**需要从当前布局迁移？如果有，M0 的第 8 项（改选项字节）就得走一遍正式的现场升级流程，而不是产线一次性动作。
4. CM4 回滚**自动**（检测不到确认就自己恢复）还是要**人工确认**？本设计默认自动。
5. 是否接受「CM7 包内带两份镜像（A 链接版 + B 链接版）」带来的传输时间翻倍？若不接受，可改用「单链接地址 + BL 从 QSPI 复制」的方案（代价：BL 要写 768 KB，回滚耗时更长）。
6. 是否把「TouchGFX 资源外置到 QSPI」排进第二期（方案 A3）？

