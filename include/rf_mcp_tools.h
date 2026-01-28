#ifndef RF_MCP_TOOLS_H
#define RF_MCP_TOOLS_H

#include "rf_module_config.h"

#if !CONFIG_RF_MODULE_ENABLE_MCP_TOOLS
#error "MCP Tools are disabled. Set RF_MODULE_ENABLE_MCP_TOOLS=ON in CMake or enable CONFIG_RF_MODULE_ENABLE_MCP_TOOLS in the main project's Kconfig."
#endif

#include "mcp_server.h"
#include "rf_module.h"
#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#define TAG_RF_MCP "RF_MCP"

/**
 * Register RF MCP tools for boards that have RF module configured.
 * This function should be called in board's RegisterMcpTools() method
 * when RF pins are configured.
 * 
 * @param rf_module Pointer to RFModule instance (from board's GetRFModule())
 */
inline void RegisterRFMcpTools(RFModule* rf_module) {
    if (!rf_module) {
        return;  // No RF module, skip registration
    }
    
    auto& mcp_server = McpServer::GetInstance();
    
    mcp_server.AddTool("self.rf.send",
        "发送RF信号到指定频率（315MHz或433MHz）。"
        "信号默认发送3次（行业标准）。"
        "注意：此工具直接发送信号，不会保存信号。"
        "如需保存信号以便后续重播，请先使用 self.rf.copy 复制信号。"
        "参数：address（6位十六进制，例如 \"1A2B3C\"）、key（2位十六进制，例如 \"01\"）、frequency（\"315\" 或 \"433\"）",
        PropertyList({
            Property("address", kPropertyTypeString),
            Property("key", kPropertyTypeString),
            Property("frequency", kPropertyTypeString)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            auto address = properties["address"].value<std::string>();
            auto key = properties["key"].value<std::string>();
            auto freq_str = properties["frequency"].value<std::string>();
            
            RFFrequency freq = RF_433MHZ;
            if (freq_str == "315") {
                freq = RF_315MHZ;
            } else if (freq_str != "433") {
                throw std::runtime_error("Frequency must be \"315\" or \"433\"");
            }
            
            rf_module->Send(address, key, freq);
            return true;
        });

    mcp_server.AddTool("self.rf.copy",
        "复制/克隆RF信号（自动识别315MHz或433MHz频率）。"
        "调用此工具并等待用户按下遥控器，系统会自动接收并保存信号。"
        "RF模块同时监听两个频率并自动识别信号频率。"
        "所有接收到的信号都会自动保存到存储（最多10个信号，循环缓冲区）。"
        "这是一个阻塞调用，最多等待10秒接收信号。"
        "返回值说明："
        "- 成功接收信号：返回JSON对象，包含address, key, frequency, protocol, pulse_length, name, is_duplicate=false。"
        "- 检测到重复信号：工具会抛出异常（error响应），错误消息为'信号保存失败：检测到重复信号...'，此时信号不会被保存。这不是超时，而是重复信号错误。"
        "- 超时未接收到信号：返回null（不是error响应）。"
        "重要：如果工具返回error响应，说明检测到重复信号或存储已满，错误消息会详细说明原因。如果返回null，说明超时未接收到信号。"
        "重要：要完成复制/克隆信号，需要两个步骤：(1) 调用 self.rf.copy 复制信号，(2) 调用 self.rf.replay 重播/发送复制的信号。"
        "仅复制信号并不等于完成克隆，必须同时调用 self.rf.replay 才能完成克隆操作。"
        "使用 self.rf.get_status 可以非阻塞查询最新接收的信号。"
        "使用 self.rf.list_signals 可以查看所有保存的信号（最多10个）及其索引。"
        "设备名称提取："
        "- 当用户说\"录制大门信号\"、\"复制大门信号\"、\"录制大门\"时，应提取\"大门\"作为name参数。"
        "- 当用户说\"复制卧室灯开关\"、\"录制卧室灯开关\"时，应提取\"卧室灯开关\"作为name参数。"
        "- 当用户说\"录制空调开关\"、\"复制空调\"时，应提取\"空调\"或\"空调开关\"作为name参数。"
        "- 从用户的自然语言中提取设备名称，去除\"录制\"、\"复制\"、\"信号\"等动词和通用词汇，保留具体的设备名称。"
        "参数：timeout_ms（可选，默认10000）、name（可选，字符串）- 信号主题/设备名称，从用户自然语言中提取，如\"大门\"、\"卧室灯开关\"、\"空调开关\"等。"
        "示例：用户说\"录制大门信号\"时，name应为\"大门\"；用户说\"复制卧室灯开关\"时，name应为\"卧室灯开关\"",
        PropertyList({
            Property("timeout_ms", kPropertyTypeInteger, 10000),
            Property("name", kPropertyTypeString, "")
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // timeout_ms 有默认值10000，如果用户提供了值会被覆盖
            int timeout_ms = properties["timeout_ms"].value<int>();
            // name 有默认值空字符串，如果用户提供了值会被覆盖
            std::string signal_name = "";
            try {
                signal_name = properties["name"].value<std::string>();
            } catch (...) {
                // name not provided, use empty string
            }
            
            int64_t start_time = esp_timer_get_time();
            
            ESP_LOGI(TAG_RF_MCP, "[复制] 开始等待RF信号，超时时间: %dms%s", 
                    timeout_ms, signal_name.empty() ? "" : (", 信号名称: " + signal_name).c_str());
            
            // 先清空已处理的信号，避免主循环重复处理
            // 如果已有信号，先处理掉（避免重复保存）
            if (rf_module->ReceiveAvailable()) {
                RFSignal temp_signal;
                rf_module->Receive(temp_signal);  // 处理并清空信号标志，避免主循环重复处理
            }
            
            // 先检查是否已有可用信号（避免不必要的等待）
            if (rf_module->ReceiveAvailable()) {
                RFSignal signal;
                if (rf_module->Receive(signal)) {
                    // Set signal name if provided
                    if (!signal_name.empty()) {
                        rf_module->SetCapturedSignalName(signal_name);
                        signal.name = signal_name;
                    }
                    
                    // Explicitly save to storage for self.rf.copy tool
                    // Check if storage is full before saving
                    if (rf_module->IsSDStorageEnabled()) {
                        uint8_t current_count = rf_module->GetStorageSignalCount();
                        if (current_count >= 10) {
                            ESP_LOGW(TAG_RF_MCP, "[复制] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                            throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                        }
                        if (!rf_module->SaveToStorage()) {
                            ESP_LOGW(TAG_RF_MCP, "[复制] ⚠️ 保存信号到存储失败");
                        }
                    }
                    
                    // Check for duplicate signal
                    uint8_t duplicate_index = 0;
                    bool is_duplicate = rf_module->CheckDuplicateSignal(signal, duplicate_index);
                    
                    if (is_duplicate) {
                        ESP_LOGW(TAG_RF_MCP, "[复制] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与存储中索引%d的信号相同", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433", duplicate_index);
                    } else {
                        ESP_LOGI(TAG_RF_MCP, "[复制] 立即接收到信号: %s%s (%sMHz)%s", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433",
                                signal.name.empty() ? "" : (", 名称: " + signal.name).c_str());
                    }
                    
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddStringToObject(json, "address", signal.address.c_str());
                    cJSON_AddStringToObject(json, "key", signal.key.c_str());
                    cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                    cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                    cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                    cJSON_AddStringToObject(json, "name", signal.name.empty() ? "" : signal.name.c_str());
                    if (is_duplicate) {
                        cJSON_AddBoolToObject(json, "is_duplicate", true);
                        cJSON_AddNumberToObject(json, "duplicate_index", duplicate_index);
                    } else {
                        cJSON_AddBoolToObject(json, "is_duplicate", false);
                    }
                return json;
                }
            }
            
            // 阻塞等待信号，每100ms检查一次
            while ((esp_timer_get_time() - start_time) / 1000 < timeout_ms) {
                if (rf_module->ReceiveAvailable()) {
            RFSignal signal;
            if (rf_module->Receive(signal)) {
                        // Set signal name if provided
                        if (!signal_name.empty()) {
                            rf_module->SetCapturedSignalName(signal_name);
                            signal.name = signal_name;
                        }
                        
                        // Check for duplicate signal BEFORE saving
                        uint8_t duplicate_index = 0;
                        bool is_duplicate = rf_module->CheckDuplicateSignal(signal, duplicate_index);
                        
                        if (is_duplicate) {
                            ESP_LOGW(TAG_RF_MCP, "[复制] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与存储中索引%d的信号相同", 
                                    signal.address.c_str(), signal.key.c_str(),
                                    signal.frequency == RF_315MHZ ? "315" : "433", duplicate_index);
                            
                            // 返回信号信息，标记为重复（而不是抛出异常）
                            int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
                            cJSON* json = cJSON_CreateObject();
                            cJSON_AddStringToObject(json, "address", signal.address.c_str());
                            cJSON_AddStringToObject(json, "key", signal.key.c_str());
                            cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                            cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                            cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                            cJSON_AddStringToObject(json, "name", signal.name.empty() ? "" : signal.name.c_str());
                            cJSON_AddBoolToObject(json, "is_duplicate", true);
                            cJSON_AddNumberToObject(json, "duplicate_index", duplicate_index);
                            return json;
                        }
                        
                        // Explicitly save to storage for self.rf.copy tool
                        // Check if storage is full before saving
                        if (rf_module->IsSDStorageEnabled()) {
                            uint8_t current_count = rf_module->GetStorageSignalCount();
                            if (current_count >= 10) {
                                ESP_LOGW(TAG_RF_MCP, "[复制] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                                throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                            }
                            if (!rf_module->SaveToStorage()) {
                                ESP_LOGE(TAG_RF_MCP, "[复制] ✗ 保存信号到存储失败");
                                throw std::runtime_error("Failed to save signal to storage.");
                            }
                        }
                        
                        int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
                        ESP_LOGI(TAG_RF_MCP, "[复制] ✓ 复制信号成功: %s%s (%sMHz, 协议:%d, 脉冲:%dμs, 等待时间:%ldms)%s", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433",
                                signal.protocol, signal.pulse_length, (long)elapsed_ms,
                                signal.name.empty() ? "" : (", 名称: " + signal.name).c_str());
                        
                cJSON* json = cJSON_CreateObject();
                cJSON_AddStringToObject(json, "address", signal.address.c_str());
                cJSON_AddStringToObject(json, "key", signal.key.c_str());
                cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                cJSON_AddStringToObject(json, "name", signal.name.empty() ? "" : signal.name.c_str());
                cJSON_AddBoolToObject(json, "is_duplicate", false);  // 保存成功，不是重复
                return json;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(100));  // 每100ms检查一次
            }
            
            // 超时
            ESP_LOGW(TAG_RF_MCP, "[复制] ✗ 等待超时，未接收到信号 (超时时间: %dms)", timeout_ms);
            return cJSON_CreateNull();
        });

    mcp_server.AddTool("self.rf.get_status",
        "获取RF模块实时状态和统计信息（非阻塞查询）。"
        "返回：enabled状态、send_count、receive_count、last_signal（最近接收的信号）和saved_signals_count。"
        "saved_signals_count字段显示存储中实际保存的信号数量（最多10个，循环缓冲区）。"
        "使用此工具可以快速检查模块状态和最新信号，无需阻塞。"
        "注意：要列出所有保存的信号及其索引，请使用 self.rf.list_signals。"
        "last_signal字段包含最新信号（address, key, frequency, protocol, pulse_length, name）。"
        "此工具不会返回完整的保存信号列表，请使用 self.rf.list_signals 查看。"
        "重复信号（地址+按键+频率相同）会被检测并警告，但仍会保存到存储。",
        PropertyList(),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "enabled", rf_module->IsEnabled());
            cJSON_AddNumberToObject(json, "send_count", rf_module->GetSendCount());
            cJSON_AddNumberToObject(json, "receive_count", rf_module->GetReceiveCount());
            
            auto last_signal = rf_module->GetLastReceived();
            if (!last_signal.address.empty()) {
                cJSON* last = cJSON_CreateObject();
                cJSON_AddStringToObject(last, "address", last_signal.address.c_str());
                cJSON_AddStringToObject(last, "key", last_signal.key.c_str());
                cJSON_AddStringToObject(last, "frequency", last_signal.frequency == RF_315MHZ ? "315" : "433");
                cJSON_AddNumberToObject(last, "protocol", last_signal.protocol);
                cJSON_AddNumberToObject(last, "pulse_length", last_signal.pulse_length);
                cJSON_AddStringToObject(last, "name", last_signal.name.empty() ? "" : last_signal.name.c_str());
                cJSON_AddItemToObject(json, "last_signal", last);
            }
            
            // Add storage count only (not the full list to avoid confusion with list_signals)
            if (rf_module->IsSDStorageEnabled()) {
                uint8_t flash_count = rf_module->GetStorageSignalCount();
                cJSON_AddNumberToObject(json, "saved_signals_count", flash_count);
                ESP_LOGI(TAG_RF_MCP, "[状态] 存储中保存了 %d 个信号", flash_count);
            } else {
                cJSON_AddNumberToObject(json, "saved_signals_count", 0);
            }
            
            return json;
        });

    mcp_server.AddTool("self.rf.capture",
        "启用捕捉模式并等待信号（阻塞，超时10秒）。"
        "这是捕捉信号的替代方式（不用于复制/克隆）。"
        "调用此工具并等待用户按下遥控器。"
        "RF模块自动检测315MHz和433MHz频率的信号。"
        "捕捉到的信号会自动保存到存储（最多10个信号，循环缓冲区）。"
        "返回值说明："
        "- 成功捕捉信号：返回JSON对象，包含address, key, frequency, protocol, pulse_length, is_duplicate=false。"
        "- 检测到重复信号：工具会抛出异常（error响应），错误消息为'信号保存失败：检测到重复信号...'，此时信号不会被保存。这不是超时，而是重复信号错误。"
        "- 超时未捕捉到信号：返回null（不是error响应）。"
        "重要：如果工具返回error响应，说明检测到重复信号或存储已满，错误消息会详细说明原因。如果返回null，说明超时未捕捉到信号。"
        "重要：此工具仅捕捉信号，不会复制/克隆信号。"
        "要复制/克隆信号，请使用：self.rf.copy（步骤1）+ self.rf.replay（步骤2）。"
        "此捕捉工具主要用于显式捕捉工作流，不用于复制/克隆。"
        "使用 self.rf.list_signals 可以查看所有保存的信号（最多10个）及其索引。"
        "参数：timeout_ms（可选，默认10000）",
        PropertyList({
            Property("timeout_ms", kPropertyTypeInteger, 10000)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            rf_module->EnableCaptureMode();
            // timeout_ms 有默认值10000，如果用户提供了值会被覆盖
            int timeout_ms = properties["timeout_ms"].value<int>();
            int64_t start_time = esp_timer_get_time();
            
            ESP_LOGI(TAG_RF_MCP, "[捕捉] 进入捕捉模式，等待信号，超时时间: %dms", timeout_ms);
            
            // 先清空已处理的信号，避免主循环重复处理
            // 如果已有信号，先处理掉（避免重复保存）
            if (rf_module->ReceiveAvailable()) {
                RFSignal temp_signal;
                rf_module->Receive(temp_signal);  // 处理并清空信号标志，避免主循环重复处理
            }
            
            // 先检查是否已有捕捉到的信号
            if (rf_module->HasCapturedSignal()) {
                auto signal = rf_module->GetCapturedSignal();
                
                // Check for duplicate signal BEFORE saving
                uint8_t duplicate_index = 0;
                bool is_duplicate = rf_module->CheckDuplicateSignal(signal, duplicate_index);
                
                if (is_duplicate) {
                    rf_module->DisableCaptureMode();
                    ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与存储中索引%d的信号相同", 
                            signal.address.c_str(), signal.key.c_str(),
                            signal.frequency == RF_315MHZ ? "315" : "433", duplicate_index);
                    
                    // 返回信号信息，标记为重复（而不是抛出异常）
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddStringToObject(json, "address", signal.address.c_str());
                    cJSON_AddStringToObject(json, "key", signal.key.c_str());
                    cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                    cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                    cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                    cJSON_AddStringToObject(json, "name", signal.name.empty() ? "" : signal.name.c_str());
                    cJSON_AddBoolToObject(json, "is_duplicate", true);
                    cJSON_AddNumberToObject(json, "duplicate_index", duplicate_index);
                    return json;
                }
                
                // Check if storage is full (capture mode saves via CheckCaptureMode, but we should verify)
                if (rf_module->IsSDStorageEnabled()) {
                    uint8_t current_count = rf_module->GetStorageSignalCount();
                    if (current_count >= 10) {
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                        rf_module->DisableCaptureMode();
                        throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                    }
                    
                    // Verify that SaveToStorage was successful (CheckCaptureMode should have called it)
                    // If SaveToStorage failed (e.g., due to duplicate), it would have returned false
                    // But since we already checked for duplicate above, this should succeed
                    // However, we can verify by checking if the signal count increased
                    uint8_t count_before = current_count;
                    // Re-check count after a small delay to ensure SaveToStorage completed
                    vTaskDelay(pdMS_TO_TICKS(10));
                    uint8_t count_after = rf_module->GetStorageSignalCount();
                    if (count_after <= count_before) {
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 信号可能未成功保存到存储");
                    }
                }
                
                ESP_LOGI(TAG_RF_MCP, "[捕捉] ✓ 立即捕捉到信号: %s%s (%sMHz)", 
                        signal.address.c_str(), signal.key.c_str(),
                        signal.frequency == RF_315MHZ ? "315" : "433");
                
                rf_module->DisableCaptureMode();
                
                cJSON* json = cJSON_CreateObject();
                cJSON_AddStringToObject(json, "address", signal.address.c_str());
                cJSON_AddStringToObject(json, "key", signal.key.c_str());
                cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                cJSON_AddStringToObject(json, "name", signal.name.empty() ? "" : signal.name.c_str());
                cJSON_AddBoolToObject(json, "is_duplicate", false);  // 保存成功，不是重复
                return json;
            }
            
            // 阻塞等待捕捉信号，每100ms检查一次
            while ((esp_timer_get_time() - start_time) / 1000 < timeout_ms) {
                // 主动检查并处理新到达的信号（这会触发 CheckCaptureMode）
                if (rf_module->ReceiveAvailable()) {
                    RFSignal temp_signal;
                    rf_module->Receive(temp_signal);  // 这会触发 CheckCaptureMode，设置 has_captured_signal_
                }
                
                // 检查是否已经捕捉到信号
                if (rf_module->HasCapturedSignal()) {
                    auto signal = rf_module->GetCapturedSignal();
                    int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
                    
                    // Check for duplicate signal BEFORE saving
                    uint8_t duplicate_index = 0;
                    bool is_duplicate = rf_module->CheckDuplicateSignal(signal, duplicate_index);
                    
                    if (is_duplicate) {
                        rf_module->DisableCaptureMode();
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与存储中索引%d的信号相同", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433", duplicate_index);
                        
                        // 返回信号信息，标记为重复（而不是抛出异常）
                        cJSON* json = cJSON_CreateObject();
                        cJSON_AddStringToObject(json, "address", signal.address.c_str());
                        cJSON_AddStringToObject(json, "key", signal.key.c_str());
                        cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                        cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                        cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                        cJSON_AddBoolToObject(json, "is_duplicate", true);
                        cJSON_AddNumberToObject(json, "duplicate_index", duplicate_index);
                        return json;
                    }
                    
                    // Check if storage is full (capture mode saves via CheckCaptureMode, but we should verify)
                    if (rf_module->IsSDStorageEnabled()) {
                        uint8_t current_count = rf_module->GetStorageSignalCount();
                        if (current_count >= 10) {
                            ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                            rf_module->DisableCaptureMode();
                            throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                        }
                    }
                    
                    ESP_LOGI(TAG_RF_MCP, "[捕捉] ✓ 捕捉到信号: %s%s (%sMHz, 协议:%d, 脉冲:%dμs, 等待时间:%ldms)", 
                            signal.address.c_str(), signal.key.c_str(),
                            signal.frequency == RF_315MHZ ? "315" : "433",
                            signal.protocol, signal.pulse_length, (long)elapsed_ms);
                    
                    rf_module->DisableCaptureMode();
                    
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddStringToObject(json, "address", signal.address.c_str());
                    cJSON_AddStringToObject(json, "key", signal.key.c_str());
                    cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                    cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                    cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                    cJSON_AddBoolToObject(json, "is_duplicate", false);  // 保存成功，不是重复
                    return json;
                }
                vTaskDelay(pdMS_TO_TICKS(100));  // 每100ms检查一次
            }
            
            // 超时前最后检查一次，避免信号在超时检查后立即到达
            if (rf_module->ReceiveAvailable()) {
                RFSignal temp_signal;
                rf_module->Receive(temp_signal);  // 处理可能刚到达的信号
                if (rf_module->HasCapturedSignal()) {
                    auto signal = rf_module->GetCapturedSignal();
                    int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
                    
                    // Check for duplicate signal BEFORE saving
                    uint8_t duplicate_index = 0;
                    bool is_duplicate = rf_module->CheckDuplicateSignal(signal, duplicate_index);
                    
                    if (is_duplicate) {
                        rf_module->DisableCaptureMode();
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与存储中索引%d的信号相同", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433", duplicate_index);
                        
                        // 返回信号信息，标记为重复（而不是抛出异常）
                        cJSON* json = cJSON_CreateObject();
                        cJSON_AddStringToObject(json, "address", signal.address.c_str());
                        cJSON_AddStringToObject(json, "key", signal.key.c_str());
                        cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                        cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                        cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                        cJSON_AddBoolToObject(json, "is_duplicate", true);
                        cJSON_AddNumberToObject(json, "duplicate_index", duplicate_index);
                        return json;
                    }
                    
                    // Check if storage is full
                    if (rf_module->IsSDStorageEnabled()) {
                        uint8_t current_count = rf_module->GetStorageSignalCount();
                        if (current_count >= 10) {
                            ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                            rf_module->DisableCaptureMode();
                            throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                        }
                    }
                    
                    ESP_LOGI(TAG_RF_MCP, "[捕捉] ✓ 捕捉到信号: %s%s (%sMHz, 协议:%d, 脉冲:%dμs, 等待时间:%ldms)", 
                            signal.address.c_str(), signal.key.c_str(),
                            signal.frequency == RF_315MHZ ? "315" : "433",
                            signal.protocol, signal.pulse_length, (long)elapsed_ms);
                    
                    rf_module->DisableCaptureMode();
                    
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddStringToObject(json, "address", signal.address.c_str());
                    cJSON_AddStringToObject(json, "key", signal.key.c_str());
                    cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                    cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                    cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
                    cJSON_AddBoolToObject(json, "is_duplicate", false);  // 保存成功，不是重复
                    return json;
                }
            }
            
            // 超时
            ESP_LOGW(TAG_RF_MCP, "[捕捉] ✗ 等待超时，未捕捉到信号 (超时时间: %dms)", timeout_ms);
            rf_module->DisableCaptureMode();
            return cJSON_CreateNull();
        });

    mcp_server.AddTool("self.rf.replay",
        "重播/发送最后接收的信号（复制/克隆的第二步）。"
        "这是完成复制/克隆信号的第二步：在调用 self.rf.copy（步骤1）复制信号后，"
        "调用此工具重播/发送该信号，完成复制/克隆操作。"
        "所有通过 self.rf.copy 复制的信号都会自动保存到存储（最多10个信号，循环缓冲区）。"
        "此工具重播/发送最近复制的信号。"
        "重要：复制/克隆信号需要两个步骤：(1) self.rf.copy - 复制信号，(2) self.rf.replay - 发送/重播信号。"
        "只有完成这两个步骤后，信号才被复制/克隆。"
 "信号按原始频率重播，不支持修改频率。"
        "信号默认发送3次（行业标准）。"
        "注意：如果要重播较旧的信号，请使用 self.rf.list_signals 查找其索引，然后使用 self.rf.send_by_index。"
        "参数：无",
        PropertyList(),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            RFSignal signal;
            bool has_signal = false;
            
            // 优先使用捕捉的信号
            if (rf_module->HasCapturedSignal()) {
                signal = rf_module->GetCapturedSignal();
                has_signal = true;
                ESP_LOGI(TAG_RF_MCP, "[重播] 使用捕捉的信号: %s%s (%sMHz)", 
                        signal.address.c_str(), signal.key.c_str(),
                        signal.frequency == RF_315MHZ ? "315" : "433");
            } else {
                // 如果没有捕捉信号，使用最后接收的信号
                auto last_signal = rf_module->GetLastReceived();
                if (!last_signal.address.empty()) {
                    signal = last_signal;
                    has_signal = true;
                    ESP_LOGI(TAG_RF_MCP, "[重播] 使用最后接收的信号: %s%s (%sMHz)", 
                            signal.address.c_str(), signal.key.c_str(),
                            signal.frequency == RF_315MHZ ? "315" : "433");
                }
            }
            
            if (!has_signal) {
                throw std::runtime_error("No captured or received signal available");
            }
            
            // 按原始频率发送，不支持修改频率
            ESP_LOGI(TAG_RF_MCP, "[重播] 使用原始频率: %sMHz", 
                    signal.frequency == RF_315MHZ ? "315" : "433");
            rf_module->Send(signal);
            return true;
        });

    mcp_server.AddTool("self.rf.list_signals",
        "列出存储中所有保存的RF信号及其索引（1-based）。"
        "返回：total_count（实际保存的信号数量，最多10个）和signals数组。"
        "存储使用循环缓冲区，最大容量为10个信号。"
        "当缓冲区满时，新信号会覆盖最旧的信号。"
        "信号索引按录入顺序递增：第一个录入的信号索引为1，最新录入的信号索引最大。"
        "重复信号（地址+按键+频率相同）在接收时会被检测并警告，但仍会保存。"
        "使用此工具查看所有保存的信号，然后通过 self.rf.send_by_index 按索引发送特定信号。"
        "数组中的每个信号包括：index（1-based）、address、key、frequency、protocol、pulse_length和name（设备名称，如果未设置则为空字符串）。"
        "参数：无",
        PropertyList(),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // Check if signal storage is enabled
            if (!rf_module->IsSDStorageEnabled()) {
                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "total_count", 0);
                cJSON* signals = cJSON_CreateArray();
                cJSON_AddItemToObject(json, "signals", signals);
                ESP_LOGW(TAG_RF_MCP, "[列表] Signal storage not enabled");
                return json;
            }
            
            uint8_t flash_count = rf_module->GetStorageSignalCount();
            
            ESP_LOGI(TAG_RF_MCP, "[列表] 存储中保存了 %d 个信号", flash_count);
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddNumberToObject(json, "total_count", flash_count);
            
            if (flash_count > 0) {
                cJSON* signals = cJSON_CreateArray();
                for (uint8_t i = 0; i < flash_count; i++) {
                    RFSignal signal;
                    if (rf_module->GetStorageSignal(i, signal)) {
                        // 用户看到的索引是从1开始的（1-based），按录入顺序递增
                        // GetStorageSignal(i=0) 返回最新的信号，所以 user_index = flash_count - i
                        // 例如：如果有7个信号，最新的(i=0)索引为7，最旧的(i=6)索引为1
                        uint8_t user_index = flash_count - i;  // 最新信号索引最大，按录入顺序递增
                        
                        ESP_LOGI(TAG_RF_MCP, "[列表] 信号[%d]: %s%s (%sMHz, 协议:%d, 脉冲:%dμs%s)", 
                                user_index, signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433",
                                signal.protocol, signal.pulse_length,
                                signal.name.empty() ? " (未命名)" : (", 名称: " + signal.name).c_str());
                        
                        cJSON* sig_obj = cJSON_CreateObject();
                        cJSON_AddNumberToObject(sig_obj, "index", user_index);  // 1-based index for user
                        cJSON_AddStringToObject(sig_obj, "address", signal.address.c_str());
                        cJSON_AddStringToObject(sig_obj, "key", signal.key.c_str());
                        cJSON_AddStringToObject(sig_obj, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                        cJSON_AddNumberToObject(sig_obj, "protocol", signal.protocol);
                        cJSON_AddNumberToObject(sig_obj, "pulse_length", signal.pulse_length);
                        cJSON_AddStringToObject(sig_obj, "name", signal.name.empty() ? "" : signal.name.c_str());
                        cJSON_AddItemToArray(signals, sig_obj);
                    }
                }
                cJSON_AddItemToObject(json, "signals", signals);
            } else {
                cJSON* signals = cJSON_CreateArray();
                cJSON_AddItemToObject(json, "signals", signals);
            }
            
            return json;
        });

    mcp_server.AddTool("self.rf.send_by_index",
        "按索引发送已保存的RF信号（1-based）。"
        "最新信号位于索引1，较旧的信号索引更大。"
        "使用 self.rf.list_signals 查看所有可用信号（最多10个）及其索引。"
        "信号默认发送3次（行业标准）。"
        "信号按原始频率发送，不支持修改频率。"
        "注意：存储最多可保存10个信号（循环缓冲区）。"
        "如果尝试发送不存在的索引，会抛出错误。"
        "参数：index（整数，1-based，必需，范围：1到saved_signals_count）",
        PropertyList({
            Property("index", kPropertyTypeInteger)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // Check if signal storage is enabled
            if (!rf_module->IsSDStorageEnabled()) {
                throw std::runtime_error("Signal storage not enabled. Cannot send signal by index.");
            }
            
            int user_index = properties["index"].value<int>();
            
            if (user_index < 1) {
                throw std::runtime_error("Index must be >= 1 (1-based indexing)");
            }
            
            uint8_t flash_count = rf_module->GetStorageSignalCount();
            if (user_index > flash_count) {
                throw std::runtime_error("Index " + std::to_string(user_index) + " exceeds available signals count (" + std::to_string(flash_count) + ")");
            }
            
            // Convert 1-based user index to 0-based internal index
            // 用户索引按录入顺序递增：第一个录入的信号索引为1，最新录入的信号索引最大
            // GetStorageSignal(i=0) 返回最新的信号，对应用户索引 flash_count
            // GetStorageSignal(i=flash_count-1) 返回最旧的信号，对应用户索引 1
            // user_index = flash_count -> internal_index = 0 (latest)
            // user_index = 1 -> internal_index = flash_count - 1 (oldest)
            uint8_t internal_index = flash_count - user_index;
            
            RFSignal signal;
            if (!rf_module->GetStorageSignal(internal_index, signal)) {
                throw std::runtime_error("Failed to retrieve signal at index " + std::to_string(user_index));
            }
            
            ESP_LOGI(TAG_RF_MCP, "[按索引发送] 发送信号[%d]: %s%s (%sMHz, 协议:%d, 脉冲:%dμs%s)", 
                    user_index, signal.address.c_str(), signal.key.c_str(),
                    signal.frequency == RF_315MHZ ? "315" : "433",
                    signal.protocol, signal.pulse_length,
                    signal.name.empty() ? "" : (", 名称: " + signal.name).c_str());
            
            // 按原始频率发送，不支持修改频率
            rf_module->Send(signal);
            
            // 返回信号详细信息，而不是只返回 true
            cJSON* json = cJSON_CreateObject();
            cJSON_AddNumberToObject(json, "index", user_index);
            cJSON_AddStringToObject(json, "address", signal.address.c_str());
            cJSON_AddStringToObject(json, "key", signal.key.c_str());
            cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
            cJSON_AddNumberToObject(json, "protocol", signal.protocol);
            cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
            cJSON_AddStringToObject(json, "name", signal.name.empty() ? "" : signal.name.c_str());
            cJSON_AddBoolToObject(json, "sent", true);
            return json;
        });

    mcp_server.AddTool("self.rf.set_signal_name",
        "按索引设置已保存信号的名称/主题（1-based）。"
        "使用 self.rf.list_signals 查看所有可用信号（最多10个）及其索引。"
        "设置名称后，可以通过 self.rf.send_by_name 按名称发送信号。"
        "如果 name 为空字符串，将清除信号名称。"
        "如果尝试设置不存在的索引，会抛出错误。"
        "设备名称提取："
        "- 当用户说\"把信号1命名为大门\"、\"设置信号1名称为大门\"时，应提取\"大门\"作为name参数。"
        "- 当用户说\"把索引2设置为卧室灯开关\"时，应提取\"卧室灯开关\"作为name参数。"
        "- 从用户的自然语言中提取设备名称，去除\"命名为\"、\"设置为\"、\"名称\"等动词和通用词汇，保留具体的设备名称。"
        "参数：index（整数，1-based，必需，范围：1到saved_signals_count）、name（字符串，必需）- 信号名称/设备名称，从用户自然语言中提取，如\"大门\"、\"卧室灯开关\"、\"空调开关\"等（空字符串可清除名称）",
        PropertyList({
            Property("index", kPropertyTypeInteger),
            Property("name", kPropertyTypeString)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // Check if signal storage is enabled
            if (!rf_module->IsSDStorageEnabled()) {
                throw std::runtime_error("Signal storage not enabled. Cannot set signal name.");
            }
            
            int user_index = properties["index"].value<int>();
            std::string name = properties["name"].value<std::string>();
            
            if (user_index < 1) {
                throw std::runtime_error("Index must be >= 1 (1-based indexing)");
            }
            
            // Allow empty string to clear name
            
            uint8_t flash_count = rf_module->GetStorageSignalCount();
            if (user_index > flash_count) {
                throw std::runtime_error("Index " + std::to_string(user_index) + " exceeds available signals count (" + std::to_string(flash_count) + ")");
            }
            
            // Convert 1-based user index to 0-based internal index
            // 用户索引按录入顺序递增：第一个录入的信号索引为1，最新录入的信号索引最大
            // GetStorageSignal(i=0) 返回最新的信号，对应用户索引 flash_count
            // GetStorageSignal(i=flash_count-1) 返回最旧的信号，对应用户索引 1
            // user_index = flash_count -> internal_index = 0 (latest)
            // user_index = 1 -> internal_index = flash_count - 1 (oldest)
            uint8_t internal_index = flash_count - user_index;
            
            // Get signal info before updating (for logging)
            RFSignal signal;
            if (!rf_module->GetStorageSignal(internal_index, signal)) {
                throw std::runtime_error("Failed to retrieve signal at index " + std::to_string(user_index));
            }
            
            if (!rf_module->UpdateStorageSignalName(internal_index, name)) {
                throw std::runtime_error("Failed to update signal name at index " + std::to_string(user_index));
            }
            
            ESP_LOGI(TAG_RF_MCP, "[设置名称] 信号[%d]: %s%s (%sMHz) -> 名称: %s", 
                    user_index, signal.address.c_str(), signal.key.c_str(),
                    signal.frequency == RF_315MHZ ? "315" : "433", 
                    name.empty() ? "(已清除)" : name.c_str());
            
            // Return updated signal info
            cJSON* json = cJSON_CreateObject();
            cJSON_AddNumberToObject(json, "index", user_index);
            cJSON_AddStringToObject(json, "address", signal.address.c_str());
            cJSON_AddStringToObject(json, "key", signal.key.c_str());
            cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
            cJSON_AddNumberToObject(json, "protocol", signal.protocol);
            cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
            cJSON_AddStringToObject(json, "name", name.c_str());
            cJSON_AddBoolToObject(json, "updated", true);
            return json;
        });

    mcp_server.AddTool("self.rf.send_by_name",
        "按名称发送已保存的RF信号。"
        "使用 self.rf.list_signals 查看所有可用信号及其名称。"
        "如果多个信号具有相同的名称，将发送第一个匹配的信号。"
        "信号默认发送3次（行业标准）。"
        "信号按原始频率发送，不支持修改频率。"
        "如果找不到匹配的名称，会抛出错误。"
        "设备名称提取："
        "- 当用户说\"发送大门信号\"、\"打开大门\"、\"控制大门\"时，应提取\"大门\"作为name参数。"
        "- 当用户说\"发送卧室灯开关\"、\"打开卧室灯\"时，应提取\"卧室灯开关\"或\"卧室灯\"作为name参数。"
        "- 当用户说\"发送空调开关\"、\"打开空调\"时，应提取\"空调开关\"或\"空调\"作为name参数。"
        "- 从用户的自然语言中提取设备名称，去除\"发送\"、\"打开\"、\"控制\"、\"信号\"等动词和通用词汇，保留具体的设备名称。"
        "参数：name（字符串，必需）- 信号名称/设备名称，从用户自然语言中提取，如\"大门\"、\"卧室灯开关\"、\"空调开关\"等",
        PropertyList({
            Property("name", kPropertyTypeString)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // Check if signal storage is enabled
            if (!rf_module->IsSDStorageEnabled()) {
                throw std::runtime_error("Signal storage not enabled. Cannot send signal by name.");
            }
            
            std::string name = properties["name"].value<std::string>();
            
            if (name.empty()) {
                throw std::runtime_error("Name cannot be empty.");
            }
            
            uint8_t flash_count = rf_module->GetStorageSignalCount();
            if (flash_count == 0) {
                throw std::runtime_error("No signals saved. Use self.rf.copy to save signals first.");
            }
            
            // Search for signal with matching name
            RFSignal found_signal;
            uint8_t found_index = 0;
            bool found = false;
            
            // Search from oldest to newest (index 1 to flash_count)
            for (uint8_t i = 0; i < flash_count; i++) {
                RFSignal signal;
                if (rf_module->GetStorageSignal(i, signal)) {
                    if (signal.name == name) {
                        found_signal = signal;
                        found_index = flash_count - i;  // Convert to 1-based user index
                        found = true;
                        break;  // Found first match
                    }
                }
            }
            
            if (!found) {
                throw std::runtime_error("No signal found with name: \"" + name + "\". Use self.rf.list_signals to see available signals.");
            }
            
            ESP_LOGI(TAG_RF_MCP, "[按名称发送] 发送信号[%d]: %s%s (%sMHz, 协议:%d, 脉冲:%dμs, 名称: %s)", 
                    found_index, found_signal.address.c_str(), found_signal.key.c_str(),
                    found_signal.frequency == RF_315MHZ ? "315" : "433",
                    found_signal.protocol, found_signal.pulse_length, name.c_str());
            
            // 按原始频率发送，不支持修改频率
            rf_module->Send(found_signal);
            
            // 返回信号详细信息
            cJSON* json = cJSON_CreateObject();
            cJSON_AddNumberToObject(json, "index", found_index);
            cJSON_AddStringToObject(json, "address", found_signal.address.c_str());
            cJSON_AddStringToObject(json, "key", found_signal.key.c_str());
            cJSON_AddStringToObject(json, "frequency", found_signal.frequency == RF_315MHZ ? "315" : "433");
            cJSON_AddNumberToObject(json, "protocol", found_signal.protocol);
            cJSON_AddNumberToObject(json, "pulse_length", found_signal.pulse_length);
            cJSON_AddStringToObject(json, "name", found_signal.name.c_str());
            cJSON_AddBoolToObject(json, "sent", true);
            return json;
        });

    mcp_server.AddTool("self.rf.clear_signals",
        "清理存储中保存的RF信号。"
        "可以清理所有信号，或按索引清理特定信号。"
        "清理后，使用 self.rf.list_signals 验证剩余信号。"
        "参数：clear_all（布尔值，可选，默认false）- 如果为true，清理所有信号；"
        "index（整数，可选，1-based）- 如果提供，清理此索引的信号（需要clear_all=false或省略）。"
        "如果同时提供clear_all和index，clear_all优先。"
        "返回：cleared_count（清理的信号数量）、remaining_count（剩余的信号数量）",
        PropertyList({
            Property("clear_all", kPropertyTypeBoolean, false),
            Property("index", kPropertyTypeInteger, -1)  // Default -1 means not provided
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            if (!rf_module->IsSDStorageEnabled()) {
                throw std::runtime_error("Signal storage not enabled. Cannot clear signals.");
            }
            
            uint8_t initial_count = rf_module->GetStorageSignalCount();
            
            // Check if clear_all is requested
            bool clear_all = false;
            try {
                clear_all = properties["clear_all"].value<bool>();
            } catch (...) {
                // clear_all not provided or invalid, default to false
            }
            
            // Get index (default -1 means not provided)
            int user_index = -1;
            try {
                user_index = properties["index"].value<int>();
            } catch (...) {
                // index not provided
            }
            
            if (clear_all) {
                // Clear all signals
                rf_module->ClearStorage();
                ESP_LOGI(TAG_RF_MCP, "[清理] 已清除所有信号 (共%d个)", initial_count);
                
                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "cleared_count", initial_count);
                cJSON_AddNumberToObject(json, "remaining_count", 0);
                cJSON_AddStringToObject(json, "action", "clear_all");
                return json;
            } else {
                // Clear a specific signal by index
                if (user_index < 0) {
                    throw std::runtime_error("Either 'clear_all=true' or 'index' parameter must be provided");
                }
                
                if (user_index < 1) {
                    throw std::runtime_error("Index must be >= 1 (1-based indexing)");
                }
                
                if (user_index > initial_count) {
                    throw std::runtime_error("Index " + std::to_string(user_index) + " exceeds available signals count (" + std::to_string(initial_count) + ")");
                }
                
                // Convert 1-based user index to 0-based internal index (same as list_signals/send_by_index)
                // user_index 1 = oldest = internal (initial_count-1), user_index initial_count = newest = internal 0
                uint8_t internal_index = initial_count - user_index;
                
                // Get signal info before clearing (for logging)
                RFSignal signal;
                std::string signal_info = "";
                if (rf_module->GetStorageSignal(internal_index, signal)) {
                    signal_info = signal.address + signal.key + " (" + 
                                 (signal.frequency == RF_315MHZ ? "315" : "433") + "MHz)";
                }
                
                if (!rf_module->ClearStorageSignal(internal_index)) {
                    throw std::runtime_error("Failed to clear signal at index " + std::to_string(user_index));
                }
                
                uint8_t remaining_count = rf_module->GetStorageSignalCount();
                ESP_LOGI(TAG_RF_MCP, "[清理] 已清除信号索引 %d%s (剩余%d个信号)", 
                        user_index, 
                        signal_info.empty() ? "" : (" " + signal_info).c_str(),
                        remaining_count);
                
                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "cleared_count", 1);
                cJSON_AddNumberToObject(json, "remaining_count", remaining_count);
                cJSON_AddNumberToObject(json, "cleared_index", user_index);
                cJSON_AddStringToObject(json, "action", "clear_by_index");
                return json;
            }
        });

    mcp_server.AddTool("self.rf.set_config",
        "配置RF模块的发送参数。"
        "这会影响发送信号时使用的协议、脉冲长度和重复次数。"
        "注意：RF模块在接收时会自动检测频率，因此配置主要用于发送。",
        PropertyList({
            Property("frequency", kPropertyTypeString),
            Property("protocol", kPropertyTypeInteger, 1),
            Property("pulse_length", kPropertyTypeInteger, 320),
            Property("repeat_count", kPropertyTypeInteger, 3)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            auto freq_str = properties["frequency"].value<std::string>();
            RFFrequency freq = RF_433MHZ;
            if (freq_str == "315") {
                freq = RF_315MHZ;
            } else if (freq_str != "433") {
                throw std::runtime_error("Frequency must be \"315\" or \"433\"");
            }
            
            rf_module->SetProtocol(properties["protocol"].value<int>(), freq);
            rf_module->SetPulseLength(properties["pulse_length"].value<int>(), freq);
            rf_module->SetRepeatCount(properties["repeat_count"].value<int>(), freq);
            
            return true;
        });
}

#endif // RF_MCP_TOOLS_H

