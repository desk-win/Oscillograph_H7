# OTA 完整升级计划

## 1. 当前基线

### 已完成

- CM7 Bootloader 独立工程，运行在 `0x08000000`，大小限制 128 KB。
- CM7 应用迁移到 `0x08020000`，大小 1280 KB。
- CM4 应用保持在 `0x08180000`，大小 512 KB。
- Bootloader 可以跳转 CM7 应用，已验证中断状态恢复正常。
- Bootloader 和 CM7 应用均可读写 BL24C16F EEPROM。
- Bootloader 已具备 W25Q256 QSPI 读写、4 KB/64 KB 擦除和 CRC32。
- Bootloader 已具备内部 Flash 扇区擦除、32 字节 flash word 编程和回读 CRC32。
- CM7 应用已接入 USART1 IDLE + DMA 和 Ymodem 单文件接收。
- Ymodem 数据块 CRC16、QSPI 写入、ACK/NAK/C、EOT 流程可运行。
- TouchGFX 已有 OTA 页面，但进度条和数字区尚未接入 OTA 状态。

### 当前限制

- 外部 Flash 暂存地址仍固定为 `0x00000000`。
- 只接收一个 BIN 文件，尚未区分 CM7 和 CM4。
- 尚未写外部 Flash 包头的“最后提交”信息。
- 尚未对完整 BIN 计算和回读校验 CRC32。
- 尚未实现 EEPROM 元数据双副本和 A/B 槽状态管理。
- Bootloader 尚未读取元数据、校验包、搬运固件或升级 CM4。
- 当前选项字节 `BCM4=1`，CM4 上电自动运行。

## 2. 最终目标

应用运行期间通过 USART1 + Ymodem 分别接收 CM7、CM4 BIN：

1. 选择非活动外部 Flash 槽。
2. 将 CM7、CM4 镜像写入同一个 2 MB 槽。
3. 回读并校验两个镜像的 CRC32。
4. 最后写外部 Flash 包头。
5. 写 EEPROM `VALID` 元数据并请求复位。

下次启动时 Bootloader：

1. 从 EEPROM 选择最新有效元数据。
2. 校验外部包头的槽位、长度和 CRC32。
3. 安装 CM7 和 CM4 到内部 Flash。
4. 回读内部 Flash 校验 CRC32。
5. 提交新 `ACTIVE` 元数据。
6. 释放 CM4 并跳转 CM7 应用。

## 3. 固定布局

### 内部 Flash

```text
0x08000000 - 0x0801FFFF  CM7 Bootloader，128 KB
0x08020000 - 0x0815FFFF  CM7 应用，1280 KB
0x08160000 - 0x0817FFFF  预留，128 KB
0x08180000 - 0x081FFFFF  CM4 应用，512 KB
```

### 外部 Flash

```text
Slot A: 0x000000 - 0x1FFFFF，2 MB
Slot B: 0x200000 - 0x3FFFFF，2 MB
0x400000 以后保留
```

单槽内部布局：

```text
+0x000000  包头区，使用前 4 KB
+0x010000  CM7 镜像，最大 1280 KB
+0x150000  CM4 镜像，最大 512 KB
+0x1D0000  保留到 2 MB
```

### EEPROM

```text
Metadata copy 0: 0x0000，最大 128 字节
Metadata copy 1: 0x0080，最大 128 字节
```

每次覆盖序号较旧的副本；两份都校验 magic 和 record CRC32，选择最高有效 sequence。

## 4. 实施阶段

### 阶段 1：共享 OTA 格式定义

- 在 `Common` 新建 OTA 布局和包结构头文件。
- 定义 A/B 槽基址、槽大小、CM7/CM4 偏移和最大长度。
- 定义包头 magic、版本、长度、CRC32 和 header CRC32。
- 定义 EEPROM 元数据结构和槽状态。
- 复用同一个 CRC32 算法，避免应用和 Bootloader 结果不一致。

验收：两个工程使用同一份布局定义，编译期检查槽内区域不重叠。

### 阶段 2：应用双文件接收

- 从 Ymodem Block 0 解析文件名。
- `S7.bin` 写入当前非活动槽的 CM7 区。
- `M4.bin` 写入当前非活动槽的 CM4 区。
- 两次传输属于同一个 OTA 会话，不生成两个版本。
- 每个文件分别计算接收 CRC32 和 QSPI 回读 CRC32。
- 两个文件完成前不写有效包头，不结束整个 OTA 事务。

验收：分两次发送 BIN 后，两个镜像都落在同一个槽的正确偏移。

### 阶段 3：包头和外部镜像校验

- 两个镜像完成后，最后写外部 Flash 包头。
- 包头记录 CM7、CM4 偏移、长度和 CRC32。
- 包头自身再计算 header CRC32。
- 写完包头后重新读取包头和两个镜像，验证全部 CRC32。

验收：任一镜像或包头被破坏时，校验明确失败，槽不可用。

### 阶段 4：EEPROM 元数据模块

- 新建应用和 Bootloader 共用的 `ota_metadata.c/h`。
- 实现双副本加载、CRC 校验、序号选择和原子提交。
- 应用在接收开始时写 `PENDING`。
- 两个镜像和包头全部通过后写 `VALID`。
- Bootloader 只有安装并校验内部 Flash 成功后，才写 `ACTIVE`。

验收：任意时刻掉电后，EEPROM 至少保留一份可解析的旧记录。

### 阶段 5：Bootloader 安装逻辑

- 启动时加载 EEPROM 元数据。
- 无升级任务且内部应用向量有效时继续启动原应用。
- 存在 `VALID` 且 `pending_slot` 时，校验外部槽包头和镜像 CRC32。
- 擦除并写入 CM7 内部应用槽。
- 回读 CM7 CRC32。
- 处理 CM4 前确保 CM4 已停止。
- 擦除、写入并回读校验 CM4 应用槽。
- 全部成功后才提交 `ACTIVE`。

验收：外部包损坏时绝不擦除当前可运行的内部应用；安装失败停留在 Bootloader。

### 阶段 6：CM4 启动控制

- 完成 CM4 烧录和校验后再释放 CM4。
- 将 `BCM4` 改为 `Disable`，由 CM7 调用 `HAL_RCCEx_EnableBootCore(RCC_BOOT_C2)`。
- 保持 `BOOT_CM4_ADD0=0x08180000`。

验收：复位后 CM4 不执行旧/半写固件，只在 Bootloader 或应用确认后启动。

### 阶段 7：OTA 页面进度和结果

- OTA 任务维护总长度、已接收长度、当前文件、阶段和错误码。
- TouchGFX Presenter/View 在 UI 线程读取状态。
- 更新 `slider1` 进度和 `textArea1` 百分比。
- 显示等待 CM7、等待 CM4、校验中、写入中、成功和失败。
- 失败时恢复普通串口日志。

验收：传输期间 UI 不阻塞，进度单调增长，错误状态可读。

### 阶段 8：确认与回滚

- Bootloader 启动新应用前递增 `boot_attempts`。
- CM7 应用正常启动后写回 `confirmed=1`。
- 连续启动失败达到阈值时切回上一 `VALID` 槽。
- 保持旧槽内容不变，直到新应用确认成功。

验收：新应用启动失败可自动回到旧版本。

### 阶段 9：完整测试和收尾

- 正常升级 CM7。
- 正常升级 CM4。
- 分两次升级 CM7+CM4。
- 传输中断电。
- 包头写入前掉电。
- EEPROM 记录写入中掉电。
- 内部 Flash 复制中掉电。
- 外部镜像或包头 CRC32 损坏。
- A/B 交替升级和回滚。
- OTA 期间其他任务和 UI 持续运行。

## 5. 完成标准

- 应用可以分两次接收 CM7、CM4 BIN 并写入同一外部槽。
- Bootloader 只安装完整且 CRC32 正确的包。
- 任意阶段掉电后仍能启动旧版本或重新升级。
- 内部 Flash 写入前不会破坏当前可运行应用。
- CM4 不会在烧录完成前执行半写镜像。
- OTA 页面显示真实进度和最终结果。
- 不依赖外部 Flash 内存映射模式。

## 6. 主要风险

- 当前内部 CM7 应用只有 1280 KB，不能安装 2 MB 的单个 CM7 镜像。
- 当前 EEPROM 和 QSPI 驱动由应用、Bootloader 各自维护，需尽快统一，避免格式漂移。
- 修改 `BCM4` 前必须先实现并验证 CM4 释放逻辑。
- CRC32 只能保证完整性，不能证明固件来源；后续如需安全升级再增加签名验证。
