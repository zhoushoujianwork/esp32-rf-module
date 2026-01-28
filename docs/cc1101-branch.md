# CC1101 分支说明

本分支（如 `feature/cc1101` / `cc1101`）为**独立实现**，不合并到 main。所有代码与配置仅在本仓库内完成，无工作区外依赖。

## 用途与区别

- **main 分支**：315/433MHz 双频段，GPIO + RCSwitch/TCSwitch，4 引脚（TX433/RX433/TX315/RX315），信号持久化可选 NVS Flash。
- **CC1101 分支**：单芯片 CC1101 经 SPI 收发（CS、SCK、MOSI、MISO、GDO0/GDO2），与现有 RFSignal、MCP、存储等上层接口保持一致；**信号持久化改为 SD 卡**，ReaderRole 下**不再使用 NVS**。

## 配置选项

在 `CMakeLists.txt` / Kconfig 中：

- `CONFIG_RF_MODULE_ENABLE_CC1101`：启用 CC1101 驱动与 RFModule 的 CC1101 路径。
- `CONFIG_RF_MODULE_ENABLE_SD_STORAGE`：启用 SD 卡信号存储（可选；无 SD 时仅内存 Replay，不持久化）。
- `CONFIG_RF_MODULE_MAX_STORED_SIGNALS`：存储中最多保存的信号数量（默认 10）。

与 main 的 `CONFIG_RF_MODULE_ENABLE_433MHZ/315MHZ` 可互斥或按需共存（二选一或同时编入）。

## 引脚定义

CC1101 使用：

| 功能   | 说明                    |
|--------|-------------------------|
| CS     | 片选（构造函数中 tx433） |
| SCK    | SPI 时钟                |
| MOSI   | SPI 主机出从机入        |
| MISO   | SPI 主机入从机出        |
| GDO0   | 接收/时序（构造函数中 rx433） |
| GDO2   | 备用/315MHz（构造函数中 tx315） |

初始化：主工程先准备好 SPI 总线，再调用：

```c
rf_module.Begin(SPI2_HOST, SCK_PIN, MOSI_PIN, MISO_PIN);
```

若同板使用 SD 卡（SPI 模式），可与 CC1101 **共用同一 SPI host**，使用**不同 CS**；主工程先挂载 SD，再初始化 CC1101。

## 信号持久化（SD 卡）

- **挂载**：SD 卡由**主工程**挂载（如挂载到 `/sdcard`），RFModule 不负责挂载。
- **启用存储**：在 `Begin()` 之后，由主工程显式调用 `EnableSDStorage("/sdcard")`（或实际挂载路径）；RFModule 在该路径下读写信号文件（如 `path/rf_signals.txt`）。
- **无 SD 时**：不调用 `EnableSDStorage` 时，仅使用内存 Replay 缓冲，无持久化；`RF_MODULE_ENABLE_SD_STORAGE=OFF` 时编译不链接 SD/fatfs，存储相关 API 返回“未启用”或 0。

## 实现说明

- CC1101 驱动与 RFModule 集成均在**本仓库内**实现。
- 参考 TI CC1101 数据手册及 arduino_cc1101 的**行为与协议**，无外部代码引用或拷贝。
- MCP 工具语义与 main 一致，仅底层由 CC1101 + SD 存储实现。
