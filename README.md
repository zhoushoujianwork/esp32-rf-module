# ESP32 RF Module Library

ESP32 RF 收发模块库，支持 315MHz 和 433MHz 双频段 RF 信号收发。

本库专为 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 项目设计，集成了 MCP（Model Context Protocol）工具功能，支持通过 AI 对话控制 RF 信号收发。

## 功能特性

- ✅ 支持 315MHz 和 433MHz 双频段收发
- ✅ 信号捕获和重放
- ✅ 信号持久化存储（main 分支：NVS Flash；CC1101 分支：SD 卡，见下文）
- ✅ **信号名称/主题管理**：支持为信号设置设备名称（如"卧室灯开关"、"大门开"等）
- ✅ **按名称发送**：支持通过设备名称发送信号，无需记忆索引
- ✅ **自然语言支持**：AI 可从自然语言中提取设备名称（如"录制大门信号"→"大门"）
- ✅ MCP 工具集成，支持 AI 对话控制

## 硬件连接

![RF模块连接示意图](docs/rf-module-setup.jpg)

外部拓展模块采用 6P 引脚接口：

| 引脚编号 | 名称 | 说明 |
|---------|------|------|
| 1 | G | 地线 |
| 2 | 3T | 315MHz 发送 |
| 3 | 3R | 315MHz 接收 |
| 4 | 4R | 433MHz 接收 |
| 5 | 4T | 433MHz 发送 |
| 6 | V | 电源 (3.3V) |

需要配置 4 个 GPIO 引脚：`RF_TX_315_PIN`, `RF_RX_315_PIN`, `RF_TX_433_PIN`, `RF_RX_433_PIN`

### CC1101 分支（独立分支，不合并 main）

在 `feature/cc1101` 或 `cc1101` 分支中，支持 TI CC1101 单芯片 SPI 收发，与「仅 GPIO 双频段」模式二选一或按需共存。

- **用途**：使用 CC1101 做 315/433MHz OOK 收发，与现有 RFSignal/MCP/存储等上层接口一致。
- **配置**：`CONFIG_RF_MODULE_ENABLE_CC1101=1`；信号存储为 **SD 卡**（`CONFIG_RF_MODULE_ENABLE_SD_STORAGE=1`）。**ReaderRole 下不再使用 NVS**，持久化仅通过 SD 卡；主工程负责挂载 SD（如 `/sdcard`），RFModule 在 `Begin()` 后由主工程显式调用 `EnableSDStorage("/sdcard")`，再读写例如 `path/rf_signals.txt`。
- **引脚**：CC1101 使用 SPI：CS、SCK、MOSI、MISO、GDO0、GDO2。构造函数中 `tx433`=CS、`rx433`=GDO0、`tx315`=GDO2；调用 `Begin(spi_host, sck, mosi, miso)` 传入 SPI 引脚。若与 SD 卡同板，可共用同一 SPI host、不同 CS，主工程先挂载 SD 再初始化 CC1101。
- **实现说明**：驱动与集成均在仓库内实现，参考 CC1101 数据手册及 arduino_cc1101 行为，无外部代码依赖。

## 硬件设计

🔗 **[ESP32 RF 管理模块 PCB 设计](https://u.lceda.cn/account/user/projects/index/detail?project=14f6a9072add4fdd9f9f65be7babce14)**

## 安装

在项目 `idf_component.yml` 中添加：

```yaml
dependencies:
  zhoushoujianwork/esp32-rf-module: "^0.1.12"
```

## 配置

在主项目的 `Kconfig.projbuild` 中添加：

```kconfig
config BOARD_HAS_RF_PINS
    bool "Board has RF module pins configured"
    default n
```

在板子的 `config.h` 中定义引脚：

```c
#define RF_TX_315_PIN  GPIO_NUM_19
#define RF_RX_315_PIN  GPIO_NUM_20
#define RF_TX_433_PIN  GPIO_NUM_17
#define RF_RX_433_PIN  GPIO_NUM_18
```

## MCP 工具

本库提供以下 MCP 工具，支持通过 AI 对话控制：

1. **self.rf.copy** - 复制/克隆RF信号（支持设置设备名称）
2. **self.rf.replay** - 重播最后复制的信号
3. **self.rf.send** - 发送RF信号
4. **self.rf.list_signals** - 列出所有保存的信号（包含设备名称）
5. **self.rf.send_by_index** - 按索引发送信号
6. **self.rf.send_by_name** - 按设备名称发送信号（新增）
7. **self.rf.set_signal_name** - 设置信号设备名称（新增）
8. **self.rf.clear_signals** - 清理保存的信号
9. **self.rf.get_status** - 获取模块状态
10. **self.rf.set_config** - 配置模块参数

## 运行示例

以下是实际运行时的关键日志，展示了 RF 模块的完整功能：

### 初始化

```
I (493) Board: Initializing RF module...
I (533) RFModule: RF module initialized: TX433=17, RX433=18, TX315=19, RX315=20
I (553) MCP: Add tool: self.rf.copy
I (553) MCP: Add tool: self.rf.replay
I (563) MCP: Add tool: self.rf.send
I (563) MCP: Add tool: self.rf.list_signals
```

### 复制信号

```
I (55562) RF_MCP: [复制] 开始等待RF信号，超时时间: 10000ms
I (58372) RFModule: [315MHz接收] ✓ 信号接收成功: 79FB9C00 (24位:0x79FB9C, 协议:1, 脉冲:327μs)
I (58382) RFModule: [闪存] 信号已保存到索引 0 (共1个信号)
I (58382) RF_MCP: [复制] ✓ 复制信号成功: 79FB9C00 (315MHz, 协议:1, 脉冲:327μs, 等待时间:2822ms)
```

### 重播信号

```
I (71542) RF_MCP: [重播] 使用捕捉的信号: 79FB9C00 (315MHz)
I (71562) RFModule: [315MHz发送] 开始发送信号: 79FB9C00 (24位:0x79FB9C, 协议:1, 脉冲:320μs, 重复:3次)
I (71722) RFModule: [315MHz发送] ✓ 发送完成: 79FB9C00 (24位:0x79FB9C, 协议:1, 脉冲:320μs, 重复:3次, 耗时:153ms)
```

### 清理信号

```
I (43902) RFModule: [闪存] 已清除所有保存的信号
I (43902) RF_MCP: [清理] 已清除所有信号 (共7个)
```

## API 示例

```cpp
#include "rf_module.h"

RFModule rf_module(RF_TX_433_PIN, RF_RX_433_PIN, RF_TX_315_PIN, RF_RX_315_PIN);
rf_module.Begin();

// 发送信号
rf_module.Send("A1B2C3", "01", RF_433MHZ);

// 接收信号
if (rf_module.ReceiveAvailable()) {
    RFSignal signal;
    if (rf_module.Receive(signal)) {
        // 处理信号
    }
}
```

## 相关项目

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) - 基于 MCP 协议的 AI 聊天机器人项目

## 许可证

MIT License

## 更新日志

### v0.1.12
- **改进**：`Send(RFSignal)` 方法现在会使用信号中存储的协议和脉冲长度参数
  - 发送信号时，优先使用信号中保存的 `protocol` 和 `pulse_length` 值，而不是全局默认配置
  - 确保每个信号按照接收时的原始参数发送，提高信号复制的准确性
  - 解决了之前使用全局默认配置可能导致信号发送失败的问题

### v0.1.11
- **新增**：信号名称/主题管理功能
  - 为 `RFSignal` 结构体添加 `name` 字段，支持设备名称（如"卧室灯开关"、"大门开"等）
  - `self.rf.copy` 工具支持可选的 `name` 参数，复制时可直接设置设备名称
  - 新增 `self.rf.set_signal_name` 工具，支持按索引设置信号名称
  - 新增 `self.rf.send_by_name` 工具，支持按设备名称发送信号
  - 所有返回信号信息的工具（`list_signals`、`send_by_index`、`get_status` 等）都包含 `name` 字段
  - 支持自然语言提取设备名称（如"录制大门信号"→"大门"、"复制卧室灯开关"→"卧室灯开关"）
  - 向后兼容：旧数据自动处理，未设置名称的信号显示为空字符串

### v0.1.10
- **修复**：修复保存索引显示问题，显示用户索引（最新信号索引最大）而不是内部存储索引
- **修复**：移除频率修改功能，信号按原始频率发送，不支持修改频率
- 修复 `send_by_index` 和 `replay` 工具，确保信号按原始频率发送

### v0.1.9
- **重构**：将 MCP 工具 `self.rf.receive` 重命名为 `self.rf.copy`，更符合复制/克隆信号的语义

### v0.1.8
- **修复**：修复信号索引编号逻辑，按录入顺序递增编号

### v0.1.7
- **修复**：修复 `self.rf.send_by_index` 工具返回值问题
