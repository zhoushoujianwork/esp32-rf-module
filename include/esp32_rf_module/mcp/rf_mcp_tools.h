#ifndef RF_MCP_TOOLS_H
#define RF_MCP_TOOLS_H

#include "mcp_server.h"  // 需要项目提供 MCP Server
#include "esp32_rf_module/rf_module.h"
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
        "如需保存信号以便后续重播，请先使用 self.rf.receive 接收信号。"
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

    mcp_server.AddTool("self.rf.receive",
        "接收RF信号（自动识别315MHz或433MHz频率）。"
        "这是复制/克隆信号的第一步：调用此工具并等待用户按下遥控器。"
        "RF模块同时监听两个频率并自动识别信号频率。"
        "所有接收到的信号都会自动保存到闪存（最多10个信号，循环缓冲区）。"
        "这是一个阻塞调用，最多等待10秒接收信号。"
        "返回值说明："
        "- 成功接收信号：返回JSON对象，包含address, key, frequency, protocol, pulse_length, is_duplicate=false。"
        "- 检测到重复信号：工具会抛出异常（error响应），错误消息为'信号保存失败：检测到重复信号...'，此时信号不会被保存。这不是超时，而是重复信号错误。"
        "- 超时未接收到信号：返回null（不是error响应）。"
        "重要：如果工具返回error响应，说明检测到重复信号或存储已满，错误消息会详细说明原因。如果返回null，说明超时未接收到信号。"
        "重要：要复制/克隆信号，需要两个步骤：(1) 调用 self.rf.receive 接收信号，(2) 调用 self.rf.replay 重播/发送复制的信号。"
        "仅接收信号并不等于复制/克隆，必须同时调用 self.rf.replay 才能完成复制/克隆操作。"
        "使用 self.rf.get_status 可以非阻塞查询最新接收的信号。"
        "使用 self.rf.list_signals 可以查看所有保存的信号（最多10个）及其索引。"
        "参数：timeout_ms（可选，默认10000）",
        PropertyList({
            Property("timeout_ms", kPropertyTypeInteger, 10000)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // timeout_ms 有默认值10000，如果用户提供了值会被覆盖
            int timeout_ms = properties["timeout_ms"].value<int>();
            int64_t start_time = esp_timer_get_time();
            
            ESP_LOGI(TAG_RF_MCP, "[接收] 开始等待RF信号，超时时间: %dms", timeout_ms);
            
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
                    // Explicitly save to flash storage for self.rf.receive tool
                    // Check if storage is full before saving
                    if (rf_module->IsFlashStorageEnabled()) {
                        uint8_t current_count = rf_module->GetFlashSignalCount();
                        if (current_count >= 10) {
                            ESP_LOGW(TAG_RF_MCP, "[接收] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                            throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                        }
                        if (!rf_module->SaveToFlash()) {
                            ESP_LOGW(TAG_RF_MCP, "[接收] ⚠️ 保存信号到闪存失败");
                        }
                    }
                    
                    // Check for duplicate signal
                    uint8_t duplicate_index = 0;
                    bool is_duplicate = rf_module->CheckDuplicateSignal(signal, duplicate_index);
                    
                    if (is_duplicate) {
                        ESP_LOGW(TAG_RF_MCP, "[接收] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与闪存中索引%d的信号相同", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433", duplicate_index);
                    } else {
                        ESP_LOGI(TAG_RF_MCP, "[接收] 立即接收到信号: %s%s (%sMHz)", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433");
                    }
                    
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddStringToObject(json, "address", signal.address.c_str());
                    cJSON_AddStringToObject(json, "key", signal.key.c_str());
                    cJSON_AddStringToObject(json, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                    cJSON_AddNumberToObject(json, "protocol", signal.protocol);
                    cJSON_AddNumberToObject(json, "pulse_length", signal.pulse_length);
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
                        // Check for duplicate signal BEFORE saving
                        uint8_t duplicate_index = 0;
                        bool is_duplicate = rf_module->CheckDuplicateSignal(signal, duplicate_index);
                        
                        if (is_duplicate) {
                            ESP_LOGW(TAG_RF_MCP, "[接收] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与闪存中索引%d的信号相同", 
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
                            cJSON_AddBoolToObject(json, "is_duplicate", true);
                            cJSON_AddNumberToObject(json, "duplicate_index", duplicate_index);
                            return json;
                        }
                        
                        // Explicitly save to flash storage for self.rf.receive tool
                        // Check if storage is full before saving
                        if (rf_module->IsFlashStorageEnabled()) {
                            uint8_t current_count = rf_module->GetFlashSignalCount();
                            if (current_count >= 10) {
                                ESP_LOGW(TAG_RF_MCP, "[接收] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                                throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                            }
                            if (!rf_module->SaveToFlash()) {
                                ESP_LOGE(TAG_RF_MCP, "[接收] ✗ 保存信号到闪存失败");
                                throw std::runtime_error("Failed to save signal to flash storage.");
                            }
                        }
                        
                        int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
                        ESP_LOGI(TAG_RF_MCP, "[接收] ✓ 接收到信号: %s%s (%sMHz, 协议:%d, 脉冲:%dμs, 等待时间:%ldms)", 
                                signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433",
                                signal.protocol, signal.pulse_length, (long)elapsed_ms);
                        
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
                vTaskDelay(pdMS_TO_TICKS(100));  // 每100ms检查一次
            }
            
            // 超时
            ESP_LOGW(TAG_RF_MCP, "[接收] ✗ 等待超时，未接收到信号 (超时时间: %dms)", timeout_ms);
            return cJSON_CreateNull();
        });

    mcp_server.AddTool("self.rf.get_status",
        "获取RF模块实时状态和统计信息（非阻塞查询）。"
        "返回：enabled状态、send_count、receive_count、last_signal（最近接收的信号）和saved_signals_count。"
        "saved_signals_count字段显示闪存中实际保存的信号数量（最多10个，循环缓冲区）。"
        "使用此工具可以快速检查模块状态和最新信号，无需阻塞。"
        "注意：要列出所有保存的信号及其索引，请使用 self.rf.list_signals。"
        "last_signal字段包含最新信号（address, key, frequency, protocol, pulse_length）。"
        "此工具不会返回完整的保存信号列表，请使用 self.rf.list_signals 查看。"
        "重复信号（地址+按键+频率相同）会被检测并警告，但仍会保存到闪存。",
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
                cJSON_AddItemToObject(json, "last_signal", last);
            }
            
            // Add flash storage count only (not the full list to avoid confusion with list_signals)
            if (rf_module->IsFlashStorageEnabled()) {
                uint8_t flash_count = rf_module->GetFlashSignalCount();
                cJSON_AddNumberToObject(json, "saved_signals_count", flash_count);
                ESP_LOGI(TAG_RF_MCP, "[状态] 闪存中保存了 %d 个信号", flash_count);
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
        "捕捉到的信号会自动保存到闪存（最多10个信号，循环缓冲区）。"
        "返回值说明："
        "- 成功捕捉信号：返回JSON对象，包含address, key, frequency, protocol, pulse_length, is_duplicate=false。"
        "- 检测到重复信号：工具会抛出异常（error响应），错误消息为'信号保存失败：检测到重复信号...'，此时信号不会被保存。这不是超时，而是重复信号错误。"
        "- 超时未捕捉到信号：返回null（不是error响应）。"
        "重要：如果工具返回error响应，说明检测到重复信号或存储已满，错误消息会详细说明原因。如果返回null，说明超时未捕捉到信号。"
        "重要：此工具仅捕捉信号，不会复制/克隆信号。"
        "要复制/克隆信号，请使用：self.rf.receive（步骤1）+ self.rf.replay（步骤2）。"
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
                    ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与闪存中索引%d的信号相同", 
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
                if (rf_module->IsFlashStorageEnabled()) {
                    uint8_t current_count = rf_module->GetFlashSignalCount();
                    if (current_count >= 10) {
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 信号存储已满 (10/10)，无法保存新信号");
                        rf_module->DisableCaptureMode();
                        throw std::runtime_error("Signal storage is full (10/10). Please use self.rf.list_signals to see saved signals, or clear some signals.");
                    }
                    
                    // Verify that SaveToFlash was successful (CheckCaptureMode should have called it)
                    // If SaveToFlash failed (e.g., due to duplicate), it would have returned false
                    // But since we already checked for duplicate above, this should succeed
                    // However, we can verify by checking if the signal count increased
                    uint8_t count_before = current_count;
                    // Re-check count after a small delay to ensure SaveToFlash completed
                    vTaskDelay(pdMS_TO_TICKS(10));
                    uint8_t count_after = rf_module->GetFlashSignalCount();
                    if (count_after <= count_before) {
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 信号可能未成功保存到闪存");
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
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与闪存中索引%d的信号相同", 
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
                    if (rf_module->IsFlashStorageEnabled()) {
                        uint8_t current_count = rf_module->GetFlashSignalCount();
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
                        ESP_LOGW(TAG_RF_MCP, "[捕捉] ⚠️ 接收到重复信号: %s%s (%sMHz) - 与闪存中索引%d的信号相同", 
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
                    if (rf_module->IsFlashStorageEnabled()) {
                        uint8_t current_count = rf_module->GetFlashSignalCount();
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
        "这是完成复制/克隆信号的第二步：在调用 self.rf.receive（步骤1）接收信号后，"
        "调用此工具重播/发送该信号，完成复制/克隆操作。"
        "所有通过 self.rf.receive 接收的信号都会自动保存到闪存（最多10个信号，循环缓冲区）。"
        "此工具重播/发送最近接收的信号。"
        "重要：复制/克隆信号需要两个步骤：(1) self.rf.receive - 接收信号，(2) self.rf.replay - 发送/重播信号。"
        "只有完成这两个步骤后，信号才被复制/克隆。"
        "可以选择更改重播频率（例如，以433MHz重播315MHz信号）。"
        "信号默认发送3次（行业标准）。"
        "注意：如果要重播较旧的信号，请使用 self.rf.list_signals 查找其索引，然后使用 self.rf.send_by_index。"
        "参数：frequency（可选，如果未提供则使用原始频率，\"315\" 或 \"433\"）",
        PropertyList({
            Property("frequency", kPropertyTypeString, "")  // 使用空字符串作为默认值，表示使用原始频率
        }),
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
            
            // 检查是否提供了 frequency 参数（空字符串表示使用原始频率）
            RFFrequency freq = signal.frequency;  // 默认使用原始信号的频率
            try {
                auto freq_str = properties["frequency"].value<std::string>();
                if (!freq_str.empty()) {
                    // 如果提供了非空参数，使用用户指定的频率
                    if (freq_str == "315") {
                        freq = RF_315MHZ;
                        ESP_LOGI(TAG_RF_MCP, "[重播] 频率已改为: 315MHz");
                    } else if (freq_str == "433") {
                        freq = RF_433MHZ;
                        ESP_LOGI(TAG_RF_MCP, "[重播] 频率已改为: 433MHz");
                    }
                    // 如果提供了其他值，保持使用原始频率
                } else {
                    ESP_LOGI(TAG_RF_MCP, "[重播] 使用原始频率: %sMHz", 
                            signal.frequency == RF_315MHZ ? "315" : "433");
                }
            } catch (...) {
                // 参数不存在，使用原始频率
                ESP_LOGI(TAG_RF_MCP, "[重播] 参数不存在，使用原始频率: %sMHz", 
                        signal.frequency == RF_315MHZ ? "315" : "433");
            }
            
            signal.frequency = freq;
            ESP_LOGI(TAG_RF_MCP, "[重播] 最终发送频率: %sMHz", 
                    freq == RF_315MHZ ? "315" : "433");
            rf_module->Send(signal);
            return true;
        });

    mcp_server.AddTool("self.rf.list_signals",
        "列出闪存中所有保存的RF信号及其索引（1-based）。"
        "返回：total_count（实际保存的信号数量，最多10个）和signals数组。"
        "闪存使用循环缓冲区，最大容量为10个信号。"
        "当缓冲区满时，新信号会覆盖最旧的信号。"
        "最新信号位于索引1，较旧的信号索引更大。"
        "重复信号（地址+按键+频率相同）在接收时会被检测并警告，但仍会保存。"
        "使用此工具查看所有保存的信号，然后通过 self.rf.send_by_index 按索引发送特定信号。"
        "数组中的每个信号包括：index（1-based）、address、key、frequency、protocol和pulse_length。"
        "参数：无",
        PropertyList(),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // Check if flash storage is enabled
            if (!rf_module->IsFlashStorageEnabled()) {
                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "total_count", 0);
                cJSON* signals = cJSON_CreateArray();
                cJSON_AddItemToObject(json, "signals", signals);
                ESP_LOGW(TAG_RF_MCP, "[列表] Flash storage not enabled");
                return json;
            }
            
            uint8_t flash_count = rf_module->GetFlashSignalCount();
            
            ESP_LOGI(TAG_RF_MCP, "[列表] 闪存中保存了 %d 个信号", flash_count);
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddNumberToObject(json, "total_count", flash_count);
            
            if (flash_count > 0) {
                cJSON* signals = cJSON_CreateArray();
                for (uint8_t i = 0; i < flash_count; i++) {
                    RFSignal signal;
                    if (rf_module->GetFlashSignal(i, signal)) {
                        // 用户看到的索引是从1开始的（1-based），最新的信号索引最大
                        // GetFlashSignal(i=0) 返回最新的信号，所以 user_index = flash_count - i
                        // 例如：如果有2个信号，最新的(i=0)索引为2，旧的(i=1)索引为1
                        uint8_t user_index = flash_count - i;  // 最新的信号索引最大
                        
                        ESP_LOGI(TAG_RF_MCP, "[列表] 信号[%d]: %s%s (%sMHz, 协议:%d, 脉冲:%dμs)", 
                                user_index, signal.address.c_str(), signal.key.c_str(),
                                signal.frequency == RF_315MHZ ? "315" : "433",
                                signal.protocol, signal.pulse_length);
                        
                        cJSON* sig_obj = cJSON_CreateObject();
                        cJSON_AddNumberToObject(sig_obj, "index", user_index);  // 1-based index for user
                        cJSON_AddStringToObject(sig_obj, "address", signal.address.c_str());
                        cJSON_AddStringToObject(sig_obj, "key", signal.key.c_str());
                        cJSON_AddStringToObject(sig_obj, "frequency", signal.frequency == RF_315MHZ ? "315" : "433");
                        cJSON_AddNumberToObject(sig_obj, "protocol", signal.protocol);
                        cJSON_AddNumberToObject(sig_obj, "pulse_length", signal.pulse_length);
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
        "可以选择更改发送频率（例如，以433MHz发送315MHz信号）。"
        "注意：闪存最多可保存10个信号（循环缓冲区）。"
        "如果尝试发送不存在的索引，会抛出错误。"
        "参数：index（整数，1-based，必需，范围：1到saved_signals_count）、frequency（可选，\"315\" 或 \"433\"，默认：使用原始频率）",
        PropertyList({
            Property("index", kPropertyTypeInteger),
            Property("frequency", kPropertyTypeString)
        }),
        [rf_module](const PropertyList& properties) -> ReturnValue {
            // Check if flash storage is enabled
            if (!rf_module->IsFlashStorageEnabled()) {
                throw std::runtime_error("Flash storage not enabled. Cannot send signal by index.");
            }
            
            int user_index = properties["index"].value<int>();
            
            if (user_index < 1) {
                throw std::runtime_error("Index must be >= 1 (1-based indexing)");
            }
            
            uint8_t flash_count = rf_module->GetFlashSignalCount();
            if (user_index > flash_count) {
                throw std::runtime_error("Index " + std::to_string(user_index) + " exceeds available signals count (" + std::to_string(flash_count) + ")");
            }
            
            // Convert 1-based user index to 0-based internal index
            // Latest signal has the largest user index (flash_count), which is index 0 (internal)
            // GetFlashSignal(i=0) returns the latest signal
            // user_index = flash_count -> internal_index = 0 (latest)
            // user_index = 1 -> internal_index = flash_count - 1 (oldest)
            uint8_t internal_index = flash_count - user_index;
            
            RFSignal signal;
            if (!rf_module->GetFlashSignal(internal_index, signal)) {
                throw std::runtime_error("Failed to retrieve signal at index " + std::to_string(user_index));
            }
            
            ESP_LOGI(TAG_RF_MCP, "[按索引发送] 发送信号[%d]: %s%s (%sMHz, 协议:%d, 脉冲:%dμs)", 
                    user_index, signal.address.c_str(), signal.key.c_str(),
                    signal.frequency == RF_315MHZ ? "315" : "433",
                    signal.protocol, signal.pulse_length);
            
            // Check if user wants to change frequency
            try {
                auto freq_str = properties["frequency"].value<std::string>();
                if (freq_str == "315") {
                    signal.frequency = RF_315MHZ;
                } else if (freq_str == "433") {
                    signal.frequency = RF_433MHZ;
                }
                ESP_LOGI(TAG_RF_MCP, "[按索引发送] 频率已改为: %sMHz", freq_str.c_str());
            } catch (...) {
                // frequency parameter not provided, use original frequency
            }
            
            rf_module->Send(signal);
            return true;
        });

    mcp_server.AddTool("self.rf.clear_signals",
        "清理闪存中保存的RF信号。"
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
            if (!rf_module->IsFlashStorageEnabled()) {
                throw std::runtime_error("Flash storage not enabled. Cannot clear signals.");
            }
            
            uint8_t initial_count = rf_module->GetFlashSignalCount();
            
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
                rf_module->ClearFlash();
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
                
                // Convert 1-based user index to 0-based internal index
                uint8_t internal_index = user_index - 1;
                
                // Get signal info before clearing (for logging)
                RFSignal signal;
                std::string signal_info = "";
                if (rf_module->GetFlashSignal(internal_index, signal)) {
                    signal_info = signal.address + signal.key + " (" + 
                                 (signal.frequency == RF_315MHZ ? "315" : "433") + "MHz)";
                }
                
                if (rf_module->ClearFlashSignal(internal_index)) {
                    uint8_t remaining_count = rf_module->GetFlashSignalCount();
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
                } else {
                    throw std::runtime_error("Failed to clear signal at index " + std::to_string(user_index));
                }
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

