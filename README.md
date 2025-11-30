# ESP32 RF Module Library

ESP32 RF 收发模块库，支持 315MHz 和 433MHz 双频段 RF 信号收发。

## 功能特性

- ✅ 支持 315MHz 和 433MHz 双频段收发
- ✅ 信号发送和接收（中断方式）
- ✅ 信号捕获和重放
- ✅ 信号持久化存储（NVS Flash，最多10个信号）
- ✅ 信号列表和按索引发送
- ✅ 信号清理功能
- ✅ 协议配置（协议编号、脉冲长度、重复次数）
- ✅ MCP 工具集成（可选，需要 MCP Server）

## 硬件要求

### 外部拓展模块接口

外部拓展模块采用 6P 引脚接口：

| 引脚编号 | 名称 | 说明 | 对应功能 |
|---------|------|------|---------|
| 1 | G | 地线 | 公共地 |
| 2 | 3T | 315MHz 发送 | 315MHz 发射引脚 |
| 3 | 3R | 315MHz 接收 | 315MHz 接收引脚 |
| 4 | 4R | 433MHz 接收 | 433MHz 接收引脚 |
| 5 | 4T | 433MHz 发送 | 433MHz 发射引脚 |
| 6 | V | 电源 | 3.3V 供电 |

### 引脚配置

需要配置以下 4 个 GPIO 引脚：

- `RF_TX_315_PIN`: 315MHz 发送引脚
- `RF_RX_315_PIN`: 315MHz 接收引脚
- `RF_TX_433_PIN`: 433MHz 发送引脚
- `RF_RX_433_PIN`: 433MHz 接收引脚

## 安装

### 作为 ESP-IDF 组件

1. 在你的项目 `idf_component.yml` 中添加：

```yaml
dependencies:
  mikas/esp32-rf-module:
    git: https://github.com/mikas/esp32-rf-module.git
    version: ^1.0.0
```

2. 或者使用 Component Manager（需要先发布到组件注册表）：

```yaml
dependencies:
  mikas/esp32-rf-module: "^1.0.0"
```

## 使用方法

### 基本使用

```cpp
#include "rf_module.h"

// 初始化 RF 模块
RFModule rf_module(
    RF_TX_433_PIN, RF_RX_433_PIN,  // 433MHz 引脚
    RF_TX_315_PIN, RF_RX_315_PIN   // 315MHz 引脚
);

rf_module.Begin();

// 发送信号
rf_module.Send("A1B2C3", "01", RF_433MHZ);

// 接收信号
if (rf_module.ReceiveAvailable()) {
    RFSignal signal;
    if (rf_module.Receive(signal)) {
        // 处理接收到的信号
        ESP_LOGI("RF", "Received: %s%s (%sMHz)", 
                 signal.address.c_str(), signal.key.c_str(),
                 signal.frequency == RF_315MHZ ? "315" : "433");
    }
}

rf_module.End();
```

### MCP 工具集成（可选）

如果你的项目支持 MCP 协议，可以使用 `rf_mcp_tools.h` 注册 MCP 工具：

```cpp
#include "rf_mcp_tools.h"
#include "mcp_server.h"  // 需要你的项目提供 MCP Server

// 在板子初始化时注册 MCP 工具
void RegisterMcpTools() {
    if (rf_module_) {
        RegisterRFMcpTools(rf_module_);
    }
}
```

**注意**：`rf_mcp_tools.h` 依赖你的项目中的 MCP Server 实现。如果你的项目不使用 MCP 协议，可以忽略此文件。

## API 文档

### RFModule 类

#### 初始化

```cpp
RFModule(gpio_num_t tx433_pin, gpio_num_t rx433_pin,
         gpio_num_t tx315_pin, gpio_num_t rx315_pin);
void Begin();
void End();
```

#### 发送信号

```cpp
void Send(const std::string& address, const std::string& key, RFFrequency freq = RF_433MHZ);
void Send(const RFSignal& signal);
```

#### 接收信号

```cpp
bool ReceiveAvailable();
bool Receive(RFSignal& signal);
```

#### 配置

```cpp
void SetRepeatCount(uint8_t count, RFFrequency freq = RF_433MHZ);
void SetProtocol(uint8_t protocol, RFFrequency freq = RF_433MHZ);
void SetPulseLength(uint16_t pulse_length, RFFrequency freq = RF_433MHZ);
```

#### 信号存储

```cpp
void EnableFlashStorage(const char* namespace_name = "rf_replay");
bool SaveToFlash();
bool LoadFromFlash();
void ClearFlash();
uint8_t GetFlashSignalCount() const;
bool GetFlashSignal(uint8_t index, RFSignal& signal) const;
```

### RFSignal 结构体

```cpp
struct RFSignal {
    std::string address;      // 6位十六进制地址码
    std::string key;          // 2位十六进制按键值
    RFFrequency frequency;    // 频率类型（315MHz 或 433MHz）
    uint8_t protocol;        // 协议编号
    uint16_t pulse_length;    // 脉冲长度（微秒）
};
```

## 信号格式

RF 信号采用 24 位编码格式：
- 前 6 位十六进制：地址码（address）
- 后 2 位十六进制：按键值（key）

例如：`address="A1B2C3"`, `key="01"` 表示地址码为 A1B2C3，按键值为 01。

## 协议支持

- **RCSwitch**：433MHz 协议处理（移植自 Arduino RCSwitch 库）
- **TCSwitch**：315MHz 协议处理（移植自 Arduino TCSwitch 库）

## 依赖

- ESP-IDF >= 5.4.0
- nvs_flash（用于信号持久化存储）

## 文件说明

- `rf_module.h/cc`: RF 模块核心类
- `rcswitch.h/cc`: 433MHz 协议处理
- `tcswitch.h/cc`: 315MHz 协议处理
- `rf_mcp_tools.h`: MCP 工具集成（可选，需要 MCP Server）

## 许可证

MIT License

Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
Copyright (c) 2025 Project Contributors

## 贡献

欢迎提交 Issue 和 Pull Request！

## 更新日志

### v1.0.0
- 初始版本
- 支持 315MHz 和 433MHz 双频段收发
- 支持信号捕获和重放
- 支持信号持久化存储
- 支持 MCP 工具集成（可选）
