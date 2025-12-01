# ESP32 RF Module Library

ESP32 RF 收发模块库，支持 315MHz 和 433MHz 双频段 RF 信号收发。

本库专为 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 项目设计，集成了 MCP（Model Context Protocol）工具功能，支持通过 AI 对话控制 RF 信号收发。

## 功能特性

- ✅ 支持 315MHz 和 433MHz 双频段收发
- ✅ 信号捕获和重放
- ✅ 信号持久化存储（NVS Flash，最多10个信号）
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

## 硬件设计

🔗 **[ESP32 RF 管理模块 PCB 设计](https://u.lceda.cn/account/user/projects/index/detail?project=14f6a9072add4fdd9f9f65be7babce14)**

## 安装

在项目 `idf_component.yml` 中添加：

```yaml
dependencies:
  zhoushoujianwork/esp32-rf-module: "^0.1.9"
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

1. **self.rf.copy** - 复制/克隆RF信号
2. **self.rf.replay** - 重播最后复制的信号
3. **self.rf.send** - 发送RF信号
4. **self.rf.list_signals** - 列出所有保存的信号
5. **self.rf.send_by_index** - 按索引发送信号
6. **self.rf.clear_signals** - 清理保存的信号
7. **self.rf.get_status** - 获取模块状态
8. **self.rf.set_config** - 配置模块参数

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
