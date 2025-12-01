#include "rf_module.h"
#include "rcswitch.h"
#include "tcswitch.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include <nvs.h>  // NVS available on all ESP32 series chips

#define TAG "RFModule"

RFModule::RFModule(gpio_num_t tx433_pin, gpio_num_t rx433_pin,
                   gpio_num_t tx315_pin, gpio_num_t rx315_pin)
    : tx433_pin_(tx433_pin), rx433_pin_(rx433_pin),
      tx315_pin_(tx315_pin), rx315_pin_(rx315_pin),
      rc_switch_(nullptr), tc_switch_(nullptr),
      current_frequency_(RF_433MHZ),
      repeat_count_433_(3), repeat_count_315_(3),
      protocol_433_(1), protocol_315_(1),
      pulse_length_433_(320), pulse_length_315_(320),
      send_count_(0), receive_count_(0),
      receive_callback_(nullptr),
      replay_buffer_enabled_(false),
      replay_buffer_(nullptr),
      replay_buffer_size_(0),
      replay_buffer_index_(0),
      replay_buffer_count_(0),
      capture_mode_(false), has_captured_signal_(false),
      receive_enabled_433_(true), receive_enabled_315_(true),
      flash_storage_enabled_(false),
      nvs_handle_(0),
      flash_namespace_("rf_replay"),
      flash_signal_count_(0),
      flash_signal_index_(0),
      enabled_(false) {
}

RFModule::~RFModule() {
    End();
}

void RFModule::Begin() {
    if (enabled_) {
        ESP_LOGW(TAG, "RF module already enabled");
        return;
    }
    
#if CONFIG_RF_MODULE_ENABLE_433MHZ
    // Initialize 433MHz TX pin
    gpio_set_direction(tx433_pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(tx433_pin_, 0);
    
    // Initialize RCSwitch (433MHz)
    if (rc_switch_ == nullptr) {
        rc_switch_ = new RCSwitch();
        rc_switch_->enableTransmit(static_cast<int>(tx433_pin_));
        rc_switch_->setProtocol(protocol_433_);
        rc_switch_->setPulseLength(pulse_length_433_);
        rc_switch_->setRepeatTransmit(repeat_count_433_);
        
        if (receive_enabled_433_) {
            rc_switch_->enableReceive(static_cast<int>(rx433_pin_));
        }
    }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    
#if CONFIG_RF_MODULE_ENABLE_315MHZ
    // Initialize 315MHz TX pin
    gpio_set_direction(tx315_pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(tx315_pin_, 0);
    
    // Initialize TCSwitch (315MHz)
    if (tc_switch_ == nullptr) {
        tc_switch_ = new TCSwitch();
        tc_switch_->enableTransmit(static_cast<int>(tx315_pin_));
        tc_switch_->setProtocol(protocol_315_);
        tc_switch_->setPulseLength(pulse_length_315_);
        tc_switch_->setRepeatTransmit(repeat_count_315_);
        
        if (receive_enabled_315_) {
            tc_switch_->enableReceive(static_cast<int>(rx315_pin_));
        }
    }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    
    enabled_ = true;
    ResetCounters();
    
#if CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE
    // Enable flash storage and load saved signal
    EnableFlashStorage("rf_replay");
    ESP_LOGI(TAG, "[闪存] Flash storage enabled: enabled=%d, handle=%lu", 
            flash_storage_enabled_, (unsigned long)nvs_handle_);
    LoadFromFlash();  // Load the last saved signal
    ESP_LOGI(TAG, "[闪存] After LoadFromFlash: count=%d, has_signal=%d", 
            flash_signal_count_, has_captured_signal_);
#endif // CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE
    
    ESP_LOGI(TAG, "RF module initialized: TX433=%d, RX433=%d, TX315=%d, RX315=%d",
             tx433_pin_, rx433_pin_, tx315_pin_, rx315_pin_);
}

void RFModule::End() {
    if (!enabled_) {
        return;
    }
    
#if CONFIG_RF_MODULE_ENABLE_433MHZ
    if (rc_switch_ != nullptr) {
        rc_switch_->disableReceive();
        delete rc_switch_;
        rc_switch_ = nullptr;
    }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    
#if CONFIG_RF_MODULE_ENABLE_315MHZ
    if (tc_switch_ != nullptr) {
        tc_switch_->disableReceive();
        delete tc_switch_;
        tc_switch_ = nullptr;
    }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    
    // Cleanup replay buffer
    DisableReplayBuffer();
    
#if CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE
    DisableFlashStorage();
#endif // CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE
    
    enabled_ = false;
    ESP_LOGI(TAG, "RF module disabled");
}

void RFModule::Send(const std::string& address, const std::string& key, RFFrequency freq) {
    if (!enabled_) {
        ESP_LOGW(TAG, "RF module not enabled");
        return;
    }
    
    send_count_++;
    
    if (freq == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        SendSignalTCSwitch(address, key);
#else
        ESP_LOGE(TAG, "315MHz frequency support is disabled");
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        SendSignalRCSwitch(address, key);
#else
        ESP_LOGE(TAG, "433MHz frequency support is disabled");
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    }
}

void RFModule::Send(const RFSignal& signal) {
    Send(signal.address, signal.key, signal.frequency);
}

bool RFModule::ReceiveAvailable() {
    if (!enabled_) {
        return false;
    }
    
#if CONFIG_RF_MODULE_ENABLE_433MHZ
    // Check 433MHz interrupt receive
    if (rc_switch_ != nullptr && receive_enabled_433_ && rc_switch_->available()) {
        ESP_LOGI(TAG, "[433MHz接收] 检测到可用信号");
        return true;
    }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    
#if CONFIG_RF_MODULE_ENABLE_315MHZ
    // Check 315MHz interrupt receive
    if (tc_switch_ != nullptr && receive_enabled_315_ && tc_switch_->available()) {
        ESP_LOGI(TAG, "[315MHz接收] 检测到可用信号");
        return true;
    }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    
    return false;
}

bool RFModule::Receive(RFSignal& signal) {
    if (!enabled_) {
        return false;
    }
    
#if CONFIG_RF_MODULE_ENABLE_433MHZ
    // Check 433MHz interrupt receive
    if (rc_switch_ != nullptr && receive_enabled_433_ && rc_switch_->available()) {
        unsigned long value = rc_switch_->getReceivedValue();
        unsigned int bitlength = rc_switch_->getReceivedBitlength();
        unsigned int protocol = rc_switch_->getReceivedProtocol();
        unsigned int delay = rc_switch_->getReceivedDelay();
        
        ESP_LOGI(TAG, "[433MHz接收] 原始值:0x%lX, 位长:%d, 协议:%d, 脉冲:%dμs", value, bitlength, protocol, delay);
        
        if (value > 0 && bitlength > 0) {
            // Convert to hex string (based on actual bit length)
            char hex_str[9];
            if (bitlength >= 24) {
                // 24位数据：只取低24位，格式化为6位十六进制
                uint32_t value24bit = value & 0xFFFFFF;
                snprintf(hex_str, sizeof(hex_str), "%06lX", value24bit);
                std::string hex_value = hex_str;
                // 24位数据：全部6位都是地址码，按键值设为00
                signal.address = hex_value;  // 完整的6位十六进制作为地址码
                signal.key = "00";  // 24位数据没有按键值，设为00
            } else {
                // 位长度不足24位
                int hex_len = (bitlength + 3) / 4;  // 转换为十六进制长度
                snprintf(hex_str, sizeof(hex_str), "%0*lX", hex_len, value);
                std::string hex_value = hex_str;
                signal.address = hex_value.substr(0, std::min(6, hex_len));
                signal.key = hex_value.substr(std::min(6, hex_len), std::min(2, hex_len + 2 - std::min(6, hex_len)));
                // 如果key为空（不足24位时可能发生），设置为默认值"00"
                if (signal.key.empty()) {
                    signal.key = "00";
                }
            }
            
            signal.frequency = RF_433MHZ;
            signal.protocol = protocol;
            signal.pulse_length = delay;
            
            receive_count_++;
            last_received_ = signal;
            
            // Check for duplicate signal
            uint8_t duplicate_index = 0;
            bool is_duplicate = CheckDuplicateSignal(signal, duplicate_index);
            
            // Print receive log
            if (is_duplicate) {
                ESP_LOGW(TAG, "[433MHz接收] ⚠️ 信号重复: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 位长:%d) - 与闪存中索引%d的信号相同",
                        signal.address.c_str(), signal.key.c_str(), value & 0xFFFFFF, protocol, delay, bitlength, duplicate_index);
            } else {
                ESP_LOGI(TAG, "[433MHz接收] ✓ 信号接收成功: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 位长:%d)",
                        signal.address.c_str(), signal.key.c_str(), value & 0xFFFFFF, protocol, delay, bitlength);
            }
            
            // Add to replay buffer
            AddToReplayBuffer(signal);
            
            // Check capture mode (will save to captured_signal_ if in capture mode)
            CheckCaptureMode(signal);
            
            // Always save to captured_signal_ for replay functionality
            // This allows self.rf.replay to work even if capture mode was not enabled
            captured_signal_ = signal;
            has_captured_signal_ = true;
            
            // Save to flash storage only in capture mode (handled by CheckCaptureMode)
            // For explicit save via MCP tools (self.rf.receive), save is handled in the tool callback
            // Removed unconditional SaveToFlash() here to avoid saving on every automatic receive
            
            // Call callback if set
            if (receive_callback_ != nullptr) {
                receive_callback_(signal);
            }
            
            rc_switch_->resetAvailable();
            return true;
        }
        rc_switch_->resetAvailable();
    }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    
#if CONFIG_RF_MODULE_ENABLE_315MHZ
    // Check 315MHz interrupt receive
    if (tc_switch_ != nullptr && receive_enabled_315_ && tc_switch_->available()) {
        unsigned long value = tc_switch_->getReceivedValue();
        unsigned int bitlength = tc_switch_->getReceivedBitlength();
        unsigned int protocol = tc_switch_->getReceivedProtocol();
        unsigned int delay = tc_switch_->getReceivedDelay();
        
        ESP_LOGI(TAG, "[315MHz接收] 原始值:0x%lX, 位长:%d, 协议:%d, 脉冲:%dμs", value, bitlength, protocol, delay);
        
        if (value > 0 && bitlength > 0) {
            // Convert to hex string (based on actual bit length)
            char hex_str[9];
            if (bitlength >= 24) {
                // 24位数据：只取低24位，格式化为6位十六进制
                uint32_t value24bit = value & 0xFFFFFF;
                snprintf(hex_str, sizeof(hex_str), "%06lX", value24bit);
                std::string hex_value = hex_str;
                // 24位数据：全部6位都是地址码，按键值设为00
                signal.address = hex_value;  // 完整的6位十六进制作为地址码
                signal.key = "00";  // 24位数据没有按键值，设为00
            } else {
                // 位长度不足24位
                int hex_len = (bitlength + 3) / 4;  // 转换为十六进制长度
                snprintf(hex_str, sizeof(hex_str), "%0*lX", hex_len, value);
                std::string hex_value = hex_str;
                signal.address = hex_value.substr(0, std::min(6, hex_len));
                signal.key = hex_value.substr(std::min(6, hex_len), std::min(2, hex_len + 2 - std::min(6, hex_len)));
                // 如果key为空（不足24位时可能发生），设置为默认值"00"
                if (signal.key.empty()) {
                    signal.key = "00";
                }
            }
            
            signal.frequency = RF_315MHZ;
            signal.protocol = protocol;
            signal.pulse_length = delay;
            
            receive_count_++;
            last_received_ = signal;
            
            // Print receive log
            ESP_LOGI(TAG, "[315MHz接收] ✓ 信号接收成功: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 位长:%d)",
                    signal.address.c_str(), signal.key.c_str(), value & 0xFFFFFF, protocol, delay, bitlength);
            
            // Add to replay buffer
            AddToReplayBuffer(signal);
            
            // Check capture mode (will save to captured_signal_ if in capture mode)
            CheckCaptureMode(signal);
            
            // Always save to captured_signal_ for replay functionality
            // This allows self.rf.replay to work even if capture mode was not enabled
            captured_signal_ = signal;
            has_captured_signal_ = true;
            
            // Save to flash storage only in capture mode (handled by CheckCaptureMode)
            // For explicit save via MCP tools (self.rf.receive), save is handled in the tool callback
            // Removed unconditional SaveToFlash() here to avoid saving on every automatic receive
            
            // Call callback if set
            if (receive_callback_ != nullptr) {
                receive_callback_(signal);
            }
            
            tc_switch_->resetAvailable();
            return true;
        }
        tc_switch_->resetAvailable();
    }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    
    return false;
}

void RFModule::SetRepeatCount(uint8_t count, RFFrequency freq) {
    // If freq is 0xFF (not specified), set both frequencies
    if (freq == (RFFrequency)0xFF) {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        repeat_count_433_ = count;
        if (rc_switch_ != nullptr) {
            rc_switch_->setRepeatTransmit(count);
        }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        repeat_count_315_ = count;
        if (tc_switch_ != nullptr) {
            tc_switch_->setRepeatTransmit(count);
        }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else if (freq == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        repeat_count_315_ = count;
        if (tc_switch_ != nullptr) {
            tc_switch_->setRepeatTransmit(count);
        }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        repeat_count_433_ = count;
        if (rc_switch_ != nullptr) {
            rc_switch_->setRepeatTransmit(count);
        }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    }
}

void RFModule::SetProtocol(uint8_t protocol, RFFrequency freq) {
    // If freq is 0xFF (not specified), set both frequencies
    if (freq == (RFFrequency)0xFF) {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        protocol_433_ = protocol;
        if (rc_switch_ != nullptr) {
            rc_switch_->setProtocol(protocol);
        }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        protocol_315_ = protocol;
        if (tc_switch_ != nullptr) {
            tc_switch_->setProtocol(protocol);
        }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else if (freq == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        protocol_315_ = protocol;
        if (tc_switch_ != nullptr) {
            tc_switch_->setProtocol(protocol);
        }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        protocol_433_ = protocol;
        if (rc_switch_ != nullptr) {
            rc_switch_->setProtocol(protocol);
        }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    }
}

void RFModule::SetPulseLength(uint16_t pulse_length, RFFrequency freq) {
    // If freq is 0xFF (not specified), set both frequencies
    if (freq == (RFFrequency)0xFF) {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        pulse_length_433_ = pulse_length;
        if (rc_switch_ != nullptr) {
            rc_switch_->setPulseLength(pulse_length);
        }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        pulse_length_315_ = pulse_length;
        if (tc_switch_ != nullptr) {
            tc_switch_->setPulseLength(pulse_length);
        }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else if (freq == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        pulse_length_315_ = pulse_length;
        if (tc_switch_ != nullptr) {
            tc_switch_->setPulseLength(pulse_length);
        }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        pulse_length_433_ = pulse_length;
        if (rc_switch_ != nullptr) {
            rc_switch_->setPulseLength(pulse_length);
        }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    }
}

void RFModule::SetFrequency(RFFrequency freq) {
    current_frequency_ = freq;
}

void RFModule::EnableCaptureMode() {
    capture_mode_ = true;
    has_captured_signal_ = false;
    captured_signal_ = RFSignal();
}

void RFModule::DisableCaptureMode() {
    capture_mode_ = false;
}

void RFModule::ClearCapturedSignal() {
    has_captured_signal_ = false;
    captured_signal_ = RFSignal();
#if CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE
    if (flash_storage_enabled_) {
        ClearFlash();
    }
#endif // CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE
}

void RFModule::SetReceiveCallback(ReceiveCallback callback) {
    receive_callback_ = callback;
}

void RFModule::EnableReplayBuffer(uint8_t size) {
    if (replay_buffer_ != nullptr) {
        delete[] replay_buffer_;
    }
    replay_buffer_size_ = size;
    replay_buffer_ = new RFSignal[size];
    replay_buffer_index_ = 0;
    replay_buffer_count_ = 0;
    replay_buffer_enabled_ = true;
}

void RFModule::DisableReplayBuffer() {
    if (replay_buffer_ != nullptr) {
        delete[] replay_buffer_;
        replay_buffer_ = nullptr;
    }
    replay_buffer_enabled_ = false;
    replay_buffer_size_ = 0;
    replay_buffer_index_ = 0;
    replay_buffer_count_ = 0;
}

uint8_t RFModule::GetReplayBufferCount() const {
    return replay_buffer_count_;
}

bool RFModule::GetReplaySignal(uint8_t index, RFSignal& signal) const {
    if (!replay_buffer_enabled_ || replay_buffer_ == nullptr || index >= replay_buffer_count_) {
        return false;
    }
    int start_idx = (replay_buffer_index_ - replay_buffer_count_ + replay_buffer_size_) % replay_buffer_size_;
    int idx = (start_idx + index) % replay_buffer_size_;
    signal = replay_buffer_[idx];
    return true;
}

void RFModule::ClearReplayBuffer() {
    replay_buffer_index_ = 0;
    replay_buffer_count_ = 0;
}

void RFModule::EnableFlashStorage(const char* namespace_name) {
    flash_storage_enabled_ = true;
    flash_namespace_ = namespace_name;
    if (nvs_handle_ == 0) {
        esp_err_t err = nvs_open(namespace_name, NVS_READWRITE, &nvs_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
            nvs_handle_ = 0;
            flash_storage_enabled_ = false;
        }
    }
}

void RFModule::DisableFlashStorage() {
    flash_storage_enabled_ = false;
    if (nvs_handle_ != 0) {
        nvs_close(nvs_handle_);
        nvs_handle_ = 0;
    }
}

bool RFModule::SaveToFlash() {
    if (!flash_storage_enabled_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "[闪存] SaveToFlash: flash_storage_enabled_=%d, nvs_handle_=%lu", 
                flash_storage_enabled_, (unsigned long)nvs_handle_);
        return false;
    }
    
    if (!has_captured_signal_ || captured_signal_.address.empty()) {
        ESP_LOGW(TAG, "[闪存] SaveToFlash: has_captured_signal_=%d, address.empty()=%d", 
                has_captured_signal_, captured_signal_.address.empty());
        return false;
    }
    
    // Check for duplicate signal BEFORE saving
    uint8_t duplicate_index = 0;
    bool is_duplicate = CheckDuplicateSignal(captured_signal_, duplicate_index);
    if (is_duplicate) {
        ESP_LOGW(TAG, "[闪存] 检测到重复信号，不保存: %s%s (%sMHz) - 与闪存中索引%d的信号相同", 
                captured_signal_.address.c_str(), captured_signal_.key.c_str(),
                captured_signal_.frequency == RF_315MHZ ? "315" : "433",
                duplicate_index);
        return false;  // 不保存重复信号
    }
    
    // Check if flash storage is full (circular buffer allows overwriting, but we warn user)
    // Note: Circular buffer will overwrite oldest signal when full, but we should warn
    if (flash_signal_count_ >= MAX_FLASH_SIGNALS) {
        ESP_LOGW(TAG, "[闪存] 信号存储已满 (%d/%d)，将覆盖最旧的信号", 
                flash_signal_count_, MAX_FLASH_SIGNALS);
        // Continue to save (overwrite oldest), but log warning
    }
    
    // Use circular buffer: save to current index, then advance
    char key_prefix[32];
    snprintf(key_prefix, sizeof(key_prefix), "sig_%d_", flash_signal_index_);
    
    esp_err_t err;
    std::string addr_key = std::string(key_prefix) + "addr";
    std::string key_key = std::string(key_prefix) + "key";
    std::string freq_key = std::string(key_prefix) + "freq";
    std::string proto_key = std::string(key_prefix) + "proto";
    std::string pulse_key = std::string(key_prefix) + "pulse";
    
    err = nvs_set_str(nvs_handle_, addr_key.c_str(), captured_signal_.address.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save address: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_set_str(nvs_handle_, key_key.c_str(), captured_signal_.key.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save key: %s", esp_err_to_name(err));
        return false;
    }
    
    nvs_set_u8(nvs_handle_, freq_key.c_str(), captured_signal_.frequency);
    nvs_set_u8(nvs_handle_, proto_key.c_str(), captured_signal_.protocol);
    nvs_set_u16(nvs_handle_, pulse_key.c_str(), captured_signal_.pulse_length);
    
    // Update circular buffer index and count
    flash_signal_index_ = (flash_signal_index_ + 1) % MAX_FLASH_SIGNALS;
    if (flash_signal_count_ < MAX_FLASH_SIGNALS) {
        flash_signal_count_++;
    }
    
    // Save metadata
    nvs_set_u8(nvs_handle_, "count", flash_signal_count_);
    nvs_set_u8(nvs_handle_, "index", flash_signal_index_);
    nvs_set_u8(nvs_handle_, "has_signal", 1);
    
    err = nvs_commit(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    // 显示用户索引：最新信号索引最大，按录入顺序递增
    uint8_t user_index = flash_signal_count_;
    ESP_LOGI(TAG, "[闪存] 信号已保存到索引 %d (共%d个信号)", 
             user_index, 
             flash_signal_count_);
    return true;
}

bool RFModule::LoadFromFlash() {
    if (!flash_storage_enabled_ || nvs_handle_ == 0) {
        return false;
    }
    
    // Load metadata
    uint8_t has_signal = 0;
    esp_err_t err = nvs_get_u8(nvs_handle_, "has_signal", &has_signal);
    if (err != ESP_OK || !has_signal) {
        flash_signal_count_ = 0;
        flash_signal_index_ = 0;
        has_captured_signal_ = false;
        return false;
    }
    
    // Load count and index
    nvs_get_u8(nvs_handle_, "count", &flash_signal_count_);
    nvs_get_u8(nvs_handle_, "index", &flash_signal_index_);
    
    if (flash_signal_count_ == 0) {
        has_captured_signal_ = false;
        return false;
    }
    
    // Load the most recent signal (index - 1, wrapping around)
    uint8_t load_index = (flash_signal_index_ - 1 + MAX_FLASH_SIGNALS) % MAX_FLASH_SIGNALS;
    char key_prefix[32];
    snprintf(key_prefix, sizeof(key_prefix), "sig_%d_", load_index);
    
    std::string addr_key = std::string(key_prefix) + "addr";
    std::string key_key = std::string(key_prefix) + "key";
    std::string freq_key = std::string(key_prefix) + "freq";
    std::string proto_key = std::string(key_prefix) + "proto";
    std::string pulse_key = std::string(key_prefix) + "pulse";
    
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle_, addr_key.c_str(), nullptr, &required_size);
    if (err == ESP_OK && required_size > 0) {
        char* addr_buf = new char[required_size];
        err = nvs_get_str(nvs_handle_, addr_key.c_str(), addr_buf, &required_size);
        if (err == ESP_OK) {
            captured_signal_.address = addr_buf;
            
            required_size = 0;
            err = nvs_get_str(nvs_handle_, key_key.c_str(), nullptr, &required_size);
            if (err == ESP_OK && required_size > 0) {
                char* key_buf = new char[required_size];
                err = nvs_get_str(nvs_handle_, key_key.c_str(), key_buf, &required_size);
                if (err == ESP_OK) {
                    captured_signal_.key = key_buf;
                    
                    uint8_t freq = 0;
                    nvs_get_u8(nvs_handle_, freq_key.c_str(), &freq);
                    captured_signal_.frequency = (RFFrequency)freq;
                    
                    nvs_get_u8(nvs_handle_, proto_key.c_str(), &captured_signal_.protocol);
                    nvs_get_u16(nvs_handle_, pulse_key.c_str(), &captured_signal_.pulse_length);
                    
                    delete[] key_buf;
                    delete[] addr_buf;
                    
                    if (!captured_signal_.address.empty() && !captured_signal_.key.empty()) {
                        has_captured_signal_ = true;
                        ESP_LOGI(TAG, "[闪存] 已加载信号: %s%s (%sMHz, 共%d个信号)", 
                                captured_signal_.address.c_str(), captured_signal_.key.c_str(),
                                captured_signal_.frequency == RF_315MHZ ? "315" : "433",
                                flash_signal_count_);
                        return true;
                    }
                } else {
                    delete[] key_buf;
                }
            }
            delete[] addr_buf;
        }
    }
    
    has_captured_signal_ = false;
    return false;
}

void RFModule::ClearFlash() {
    if (!flash_storage_enabled_ || nvs_handle_ == 0) {
        return;
    }
    
    // Erase all signal entries
    for (uint8_t i = 0; i < MAX_FLASH_SIGNALS; i++) {
        char key_prefix[32];
        snprintf(key_prefix, sizeof(key_prefix), "sig_%d_", i);
        nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "addr").c_str());
        nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "key").c_str());
        nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "freq").c_str());
        nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "proto").c_str());
        nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "pulse").c_str());
    }
    
    // Clear metadata
    nvs_erase_key(nvs_handle_, "count");
    nvs_erase_key(nvs_handle_, "index");
    nvs_set_u8(nvs_handle_, "has_signal", 0);
    nvs_commit(nvs_handle_);
    
    flash_signal_count_ = 0;
    flash_signal_index_ = 0;
    ESP_LOGI(TAG, "[闪存] 已清除所有保存的信号");
}

bool RFModule::ClearFlashSignal(uint8_t index) {
    // index is 0-based internal index (0 = latest signal)
    if (!flash_storage_enabled_ || nvs_handle_ == 0 || index >= flash_signal_count_) {
        return false;
    }
    
    // Calculate the actual index in the circular buffer
    uint8_t actual_index = (flash_signal_index_ - 1 - index + MAX_FLASH_SIGNALS) % MAX_FLASH_SIGNALS;
    
    // Erase the signal entry
    char key_prefix[32];
    snprintf(key_prefix, sizeof(key_prefix), "sig_%d_", actual_index);
    nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "addr").c_str());
    nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "key").c_str());
    nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "freq").c_str());
    nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "proto").c_str());
    nvs_erase_key(nvs_handle_, (std::string(key_prefix) + "pulse").c_str());
    
    // Update count
    if (flash_signal_count_ > 0) {
        flash_signal_count_--;
    }
    
    // Update metadata
    if (flash_signal_count_ == 0) {
        nvs_set_u8(nvs_handle_, "has_signal", 0);
        flash_signal_index_ = 0;
    } else {
        nvs_set_u8(nvs_handle_, "has_signal", 1);
    }
    nvs_set_u8(nvs_handle_, "count", flash_signal_count_);
    nvs_set_u8(nvs_handle_, "index", flash_signal_index_);
    nvs_commit(nvs_handle_);
    
    ESP_LOGI(TAG, "[闪存] 已清除信号索引 %d (剩余%d个信号)", index + 1, flash_signal_count_);
    return true;
}

bool RFModule::GetFlashSignal(uint8_t index, RFSignal& signal) const {
    if (!flash_storage_enabled_ || nvs_handle_ == 0 || index >= flash_signal_count_) {
        return false;
    }
    
    // Calculate the actual index in the circular buffer
    // Most recent signal is at (flash_signal_index_ - 1 + MAX_FLASH_SIGNALS) % MAX_FLASH_SIGNALS
    // Older signals go backwards from there
    uint8_t actual_index = (flash_signal_index_ - 1 - index + MAX_FLASH_SIGNALS) % MAX_FLASH_SIGNALS;
    
    char key_prefix[32];
    snprintf(key_prefix, sizeof(key_prefix), "sig_%d_", actual_index);
    
    std::string addr_key = std::string(key_prefix) + "addr";
    std::string key_key = std::string(key_prefix) + "key";
    std::string freq_key = std::string(key_prefix) + "freq";
    std::string proto_key = std::string(key_prefix) + "proto";
    std::string pulse_key = std::string(key_prefix) + "pulse";
    
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvs_handle_, addr_key.c_str(), nullptr, &required_size);
    if (err == ESP_OK && required_size > 0) {
        char* addr_buf = new char[required_size];
        err = nvs_get_str(nvs_handle_, addr_key.c_str(), addr_buf, &required_size);
        if (err == ESP_OK) {
            signal.address = addr_buf;
            
            required_size = 0;
            err = nvs_get_str(nvs_handle_, key_key.c_str(), nullptr, &required_size);
            if (err == ESP_OK && required_size > 0) {
                char* key_buf = new char[required_size];
                err = nvs_get_str(nvs_handle_, key_key.c_str(), key_buf, &required_size);
                if (err == ESP_OK) {
                    signal.key = key_buf;
                    
                    uint8_t freq = 0;
                    nvs_get_u8(nvs_handle_, freq_key.c_str(), &freq);
                    signal.frequency = (RFFrequency)freq;
                    
                    nvs_get_u8(nvs_handle_, proto_key.c_str(), &signal.protocol);
                    nvs_get_u16(nvs_handle_, pulse_key.c_str(), &signal.pulse_length);
                    
                    delete[] key_buf;
                    delete[] addr_buf;
                    return true;
                }
                delete[] key_buf;
            }
            delete[] addr_buf;
        }
    }
    
    return false;
}

bool RFModule::CheckDuplicateSignal(const RFSignal& signal, uint8_t& duplicate_index) const {
    if (!flash_storage_enabled_ || flash_signal_count_ == 0) {
        return false;
    }
    
    // Check all signals in flash storage
    for (uint8_t i = 0; i < flash_signal_count_; i++) {
        RFSignal stored_signal;
        if (GetFlashSignal(i, stored_signal)) {
            // Compare address, key, and frequency (protocol and pulse_length may vary slightly)
            if (stored_signal.address == signal.address &&
                stored_signal.key == signal.key &&
                stored_signal.frequency == signal.frequency) {
                // 与 list_signals 保持一致：索引按录入顺序递增，最新信号索引最大
                // GetFlashSignal(i=0) 返回最新的信号，对应用户索引 flash_signal_count_
                // GetFlashSignal(i=1) 返回第二新的信号，对应用户索引 flash_signal_count_ - 1
                duplicate_index = flash_signal_count_ - i;  // 1-based index for user
                return true;
            }
        }
    }
    
    return false;
}

void RFModule::ResetCounters() {
    send_count_ = 0;
    receive_count_ = 0;
}

void RFModule::EnableReceive(RFFrequency freq) {
    if (freq == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        receive_enabled_315_ = true;
        if (tc_switch_ != nullptr && enabled_) {
            tc_switch_->enableReceive(static_cast<int>(rx315_pin_));
        }
#else
        ESP_LOGW(TAG, "315MHz frequency support is disabled");
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        receive_enabled_433_ = true;
        if (rc_switch_ != nullptr && enabled_) {
            rc_switch_->enableReceive(static_cast<int>(rx433_pin_));
        }
#else
        ESP_LOGW(TAG, "433MHz frequency support is disabled");
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    }
}

void RFModule::DisableReceive(RFFrequency freq) {
    if (freq == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        receive_enabled_315_ = false;
        if (tc_switch_ != nullptr) {
            tc_switch_->disableReceive();
        }
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        receive_enabled_433_ = false;
        if (rc_switch_ != nullptr) {
            rc_switch_->disableReceive();
        }
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ
    }
}

bool RFModule::IsReceiving(RFFrequency freq) const {
    if (freq == RF_315MHZ) {
        return receive_enabled_315_;
    } else {
        return receive_enabled_433_;
    }
}

uint8_t RFModule::HexToNum(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

void RFModule::SendSignalRCSwitch(const std::string& address, const std::string& key) {
    if (rc_switch_ == nullptr || !enabled_) {
        return;
    }
    
    uint32_t code24bit = 0;
    
    // 直接解析address（6位十六进制 = 24位）
    for (size_t i = 0; i < 6 && i < address.length(); i++) {
        uint8_t val = HexToNum(address[i]);
        code24bit = (code24bit << 4) | val;
    }
    
    // Ensure RCSwitch configuration is correct
    rc_switch_->setProtocol(protocol_433_);
    rc_switch_->setPulseLength(pulse_length_433_);
    rc_switch_->setRepeatTransmit(repeat_count_433_);
    
    ESP_LOGI(TAG, "[433MHz发送] 开始发送信号: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次)",
             address.c_str(), key.c_str(), code24bit, protocol_433_, pulse_length_433_, repeat_count_433_);
    
    // Send 24-bit data (standard industry practice: repeat 3 times)
    int64_t send_start_time = esp_timer_get_time();
    rc_switch_->send(code24bit, 24);
    int64_t send_duration = (esp_timer_get_time() - send_start_time) / 1000;  // Convert to milliseconds
    
    ESP_LOGI(TAG, "[433MHz发送] ✓ 发送完成: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次, 耗时:%ldms)",
             address.c_str(), key.c_str(), (unsigned long)code24bit, protocol_433_, pulse_length_433_, repeat_count_433_, (long)send_duration);
}

void RFModule::SendSignalTCSwitch(const std::string& address, const std::string& key) {
    if (tc_switch_ == nullptr || !enabled_) {
        return;
    }
    
    uint32_t code24bit = 0;
    
    // 直接解析address（6位十六进制 = 24位）
    for (size_t i = 0; i < 6 && i < address.length(); i++) {
        uint8_t val = HexToNum(address[i]);
        code24bit = (code24bit << 4) | val;
    }
    
    // Ensure TCSwitch configuration is correct
    tc_switch_->setProtocol(protocol_315_);
    tc_switch_->setPulseLength(pulse_length_315_);
    tc_switch_->setRepeatTransmit(repeat_count_315_);
    
    ESP_LOGI(TAG, "[315MHz发送] 开始发送信号: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次)",
             address.c_str(), key.c_str(), code24bit, protocol_315_, pulse_length_315_, repeat_count_315_);
    
    // Send 24-bit data (standard industry practice: repeat 3 times)
    int64_t send_start_time = esp_timer_get_time();
    tc_switch_->send(code24bit, 24);
    int64_t send_duration = (esp_timer_get_time() - send_start_time) / 1000;  // Convert to milliseconds
    
    ESP_LOGI(TAG, "[315MHz发送] ✓ 发送完成: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次, 耗时:%ldms)",
             address.c_str(), key.c_str(), (unsigned long)code24bit, protocol_315_, pulse_length_315_, repeat_count_315_, (long)send_duration);
}

void RFModule::AddToReplayBuffer(const RFSignal& signal) {
    if (!replay_buffer_enabled_ || replay_buffer_ == nullptr) {
        return;
    }
    last_received_ = signal;
    replay_buffer_[replay_buffer_index_] = signal;
    replay_buffer_index_ = (replay_buffer_index_ + 1) % replay_buffer_size_;
    if (replay_buffer_count_ < replay_buffer_size_) {
        replay_buffer_count_++;
    }
}

void RFModule::CheckCaptureMode(const RFSignal& signal) {
    // This function is called from Receive() to check if we're in capture mode
    // If in capture mode, save the signal and exit capture mode
    // Note: This should only be called when explicitly capturing (via self.rf.capture tool)
    // Regular receives should not trigger this save
    if (capture_mode_) {
        captured_signal_ = signal;
        capture_mode_ = false;  // Auto-exit capture mode after capture
        
        // Save to flash storage when in capture mode
        // SaveToFlash() will check for duplicates and return false if duplicate
        // Only set has_captured_signal_ if save was successful
        if (flash_storage_enabled_) {
            if (SaveToFlash()) {
                has_captured_signal_ = true;  // Only set if save succeeded
            } else {
                // Save failed (likely due to duplicate), but keep captured_signal_ for tool to check
                has_captured_signal_ = true;  // Still set so tool can detect and throw error
            }
        } else {
            has_captured_signal_ = true;  // If flash storage disabled, still set the flag
        }
    }
}

std::string RFModule::Uint32ToHex(uint32_t value, int length) {
    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(length) << value;
    return ss.str();
}

uint32_t RFModule::HexToUint32(const std::string& hex) {
    uint32_t result = 0;
    for (size_t i = 0; i < hex.length() && i < 8; i++) {
        result = (result << 4) | HexToNum(hex[i]);
    }
    return result;
}

