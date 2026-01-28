#include "rf_module.h"
#include "rcswitch.h"
#include "tcswitch.h"
#if CONFIG_RF_MODULE_ENABLE_CC1101
#include "cc1101.h"
#include "cc1101_defs.h"
#include <driver/spi_master.h>
#include <esp_rom_sys.h>
#endif
#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

#if CONFIG_RF_MODULE_ENABLE_SD_STORAGE
#include <cstdio>
#include <cstdlib>
#endif

#define TAG "RFModule"

RFModule::RFModule(gpio_num_t tx433_pin, gpio_num_t rx433_pin,
                   gpio_num_t tx315_pin, gpio_num_t rx315_pin)
    : tx433_pin_(tx433_pin), rx433_pin_(rx433_pin),
      tx315_pin_(tx315_pin), rx315_pin_(rx315_pin),
      rc_switch_(nullptr), tc_switch_(nullptr),
#if CONFIG_RF_MODULE_ENABLE_CC1101
      cc1101_(nullptr), cc1101_initialized_(false),
#endif
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
      sd_storage_enabled_(false),
      stored_signals_(nullptr),
      storage_signal_count_(0),
      storage_signal_index_(0),
      enabled_(false) {
}

RFModule::~RFModule() {
    End();
}

#if CONFIG_RF_MODULE_ENABLE_CC1101
static volatile unsigned long cc1101_last_edge_time = 0;
static volatile unsigned int cc1101_change_count = 0;
static volatile unsigned int cc1101_timings[200];
static RFModule* cc1101_instance = nullptr;

static void IRAM_ATTR cc1101_gpio_isr_handler(void* arg) {
    if (!cc1101_instance || !cc1101_instance->IsCaptureMode()) return;
    int level = gpio_get_level((gpio_num_t)(intptr_t)arg);
    unsigned long time = esp_timer_get_time();
    unsigned long duration = time - cc1101_last_edge_time;
    if (duration > 5000 && duration > 350 * 31 - 2000) {
        if (level == 1) {
            cc1101_change_count = 0;
        }
    }
    if (cc1101_change_count < 200) {
        cc1101_timings[cc1101_change_count++] = (unsigned int)duration;
    }
    cc1101_last_edge_time = time;
}
#endif

void RFModule::Begin() {
    if (enabled_) {
        ESP_LOGW(TAG, "RF module already enabled");
        return;
    }

#if CONFIG_RF_MODULE_ENABLE_CC1101
    if (!cc1101_initialized_) {
        ESP_LOGW(TAG, "CC1101 mode: call Begin(spi_host, sck, mosi, miso) to initialize");
        return;
    }
#endif

#if CONFIG_RF_MODULE_ENABLE_433MHZ && !CONFIG_RF_MODULE_ENABLE_CC1101
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
#endif // CONFIG_RF_MODULE_ENABLE_433MHZ && !CONFIG_RF_MODULE_ENABLE_CC1101

#if CONFIG_RF_MODULE_ENABLE_315MHZ && !CONFIG_RF_MODULE_ENABLE_CC1101
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
#endif // CONFIG_RF_MODULE_ENABLE_315MHZ && !CONFIG_RF_MODULE_ENABLE_CC1101

    enabled_ = true;
    ResetCounters();

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

    DisableSDStorage();

#if CONFIG_RF_MODULE_ENABLE_CC1101
    if (cc1101_initialized_ && cc1101_ != nullptr) {
        gpio_isr_handler_remove(rx433_pin_);
        cc1101_instance = nullptr;
        delete cc1101_;
        cc1101_ = nullptr;
        cc1101_initialized_ = false;
    }
#endif

    enabled_ = false;
    ESP_LOGI(TAG, "RF module disabled");
}

#if CONFIG_RF_MODULE_ENABLE_CC1101
void RFModule::Begin(int spi_host, int sck_pin, int mosi_pin, int miso_pin) {
    if (cc1101_initialized_) {
        ESP_LOGW(TAG, "CC1101 already initialized");
        return;
    }
    cc1101_instance = this;
    cc1101_ = new CC1101();
    esp_err_t ret = cc1101_->Init((spi_host_device_t)spi_host, (int)tx433_pin_,
                                  sck_pin, mosi_pin, miso_pin,
                                  (int)rx433_pin_, (int)tx315_pin_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 Init failed");
        delete cc1101_;
        cc1101_ = nullptr;
        return;
    }
    gpio_install_isr_service(0);
    SetupCC1101ForRx(433);
    gpio_isr_handler_add(rx433_pin_, cc1101_gpio_isr_handler, (void*)(intptr_t)rx433_pin_);
    cc1101_initialized_ = true;
    enabled_ = true;
    ResetCounters();
    ESP_LOGI(TAG, "RF module initialized (CC1101): CS=%d GDO0=%d GDO2=%d", tx433_pin_, rx433_pin_, tx315_pin_);
}

void RFModule::SetupCC1101ForRx(int frequency_mhz) {
    gpio_isr_handler_remove(rx433_pin_);
    cc1101_->SetIdle();
    cc1101_->SetFrequency(frequency_mhz == 433 ? 433.92f : 315.0f);
    cc1101_->SetPktFormat(3);
    cc1101_->SpiWriteReg(CC1101_IOCFG0, 0x0D);
    cc1101_->SetModulation(2);
    cc1101_->SetRxBW(270.0f);
    cc1101_->SetDRate(2.0f);
    cc1101_->SetRx();
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = (1ULL << rx433_pin_);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    gpio_isr_handler_add(rx433_pin_, cc1101_gpio_isr_handler, (void*)(intptr_t)rx433_pin_);
}

void RFModule::SetupCC1101ForTx(int frequency_mhz) {
    gpio_isr_handler_remove(rx433_pin_);
    cc1101_->SetIdle();
    cc1101_->SetFrequency(frequency_mhz == 433 ? 433.92f : 315.0f);
    cc1101_->SetPktFormat(3);
    cc1101_->SpiWriteReg(CC1101_IOCFG0, 0x0D);
    cc1101_->SetModulation(2);
    cc1101_->SetPA(10);
    cc1101_->SetTx();
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << rx433_pin_);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
}

void RFModule::SendSignalCC1101(const std::string& address, const std::string& key, RFFrequency freq, uint16_t pulse_length, uint8_t protocol) {
    (void)protocol;
    (void)key;
    int freq_mhz = (freq == RF_315MHZ) ? 315 : 433;
    SetupCC1101ForTx(freq_mhz);
    unsigned long code = HexToUint32(address.length() >= 6 ? address.substr(0, 6) : address) & 0xFFFFFF;
    int bits = 24;
    int pulse_len = pulse_length > 0 ? pulse_length : 350;
    for (int r = 0; r < 3; r++) {
        gpio_set_level(rx433_pin_, 1);
        esp_rom_delay_us(pulse_len);
        gpio_set_level(rx433_pin_, 0);
        esp_rom_delay_us(pulse_len * 31);
        for (int i = bits - 1; i >= 0; i--) {
            if ((code >> i) & 1) {
                gpio_set_level(rx433_pin_, 1);
                esp_rom_delay_us(pulse_len * 3);
                gpio_set_level(rx433_pin_, 0);
                esp_rom_delay_us(pulse_len);
            } else {
                gpio_set_level(rx433_pin_, 1);
                esp_rom_delay_us(pulse_len);
                gpio_set_level(rx433_pin_, 0);
                esp_rom_delay_us(pulse_len * 3);
            }
        }
    }
    SetupCC1101ForRx(freq_mhz);
}
#endif

void RFModule::Send(const std::string& address, const std::string& key, RFFrequency freq) {
    if (!enabled_) {
        ESP_LOGW(TAG, "RF module not enabled");
        return;
    }
    send_count_++;
#if CONFIG_RF_MODULE_ENABLE_CC1101
    if (cc1101_initialized_ && cc1101_ != nullptr) {
        uint16_t pl = (freq == RF_315MHZ) ? pulse_length_315_ : pulse_length_433_;
        uint8_t proto = (freq == RF_315MHZ) ? protocol_315_ : protocol_433_;
        SendSignalCC1101(address, key, freq, pl, proto);
        return;
    }
#endif
    if (freq == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        SendSignalTCSwitch(address, key, pulse_length_315_, protocol_315_);
#else
        ESP_LOGE(TAG, "315MHz frequency support is disabled");
#endif
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        SendSignalRCSwitch(address, key, pulse_length_433_, protocol_433_);
#else
        ESP_LOGE(TAG, "433MHz frequency support is disabled");
#endif
    }
}

void RFModule::Send(const RFSignal& signal) {
    if (!enabled_) {
        ESP_LOGW(TAG, "RF module not enabled");
        return;
    }
    send_count_++;
#if CONFIG_RF_MODULE_ENABLE_CC1101
    if (cc1101_initialized_ && cc1101_ != nullptr) {
        SendSignalCC1101(signal.address, signal.key, signal.frequency, signal.pulse_length, signal.protocol);
        return;
    }
#endif
    if (signal.frequency == RF_315MHZ) {
#if CONFIG_RF_MODULE_ENABLE_315MHZ
        SendSignalTCSwitch(signal.address, signal.key, signal.pulse_length, signal.protocol);
#else
        ESP_LOGE(TAG, "315MHz frequency support is disabled");
#endif
    } else {
#if CONFIG_RF_MODULE_ENABLE_433MHZ
        SendSignalRCSwitch(signal.address, signal.key, signal.pulse_length, signal.protocol);
#else
        ESP_LOGE(TAG, "433MHz frequency support is disabled");
#endif
    }
}

bool RFModule::ReceiveAvailable() {
    if (!enabled_) {
        return false;
    }
#if CONFIG_RF_MODULE_ENABLE_CC1101
    if (cc1101_initialized_ && cc1101_ != nullptr && capture_mode_) {
        if (cc1101_change_count > 24 * 2) {
            unsigned long code = 0;
            int bits = 0;
            for (unsigned int i = 1; i + 1 < cc1101_change_count; i += 2) {
                unsigned int t0 = cc1101_timings[i];
                unsigned int t1 = cc1101_timings[i + 1];
                if (t0 > 1000 || t1 > 1000) break;
                if (t0 > t1 * 2 || t1 > t0 * 2) {
                    code = (code << 1) | (t0 > t1 ? 1u : 0u);
                    bits++;
                } else if (t1 > t0) {
                    code = (code << 1) | 0;
                    bits++;
                } else {
                    break;
                }
            }
            if (bits >= 24) {
                char hex_buf[9];
                snprintf(hex_buf, sizeof(hex_buf), "%06lX", code & 0xFFFFFF);
                captured_signal_.address = hex_buf;
                captured_signal_.key = "00";
                captured_signal_.frequency = RF_433MHZ;
                captured_signal_.protocol = 1;
                captured_signal_.pulse_length = 350;
                receive_count_++;
                last_received_ = captured_signal_;
                AddToReplayBuffer(captured_signal_);
                CheckCaptureMode(captured_signal_);
                has_captured_signal_ = true;
                if (receive_callback_) receive_callback_(captured_signal_);
                cc1101_change_count = 0;
                ESP_LOGI(TAG, "[CC1101接收] ✓ 信号: %s", captured_signal_.address.c_str());
                return true;
            }
        }
    }
#endif
#if CONFIG_RF_MODULE_ENABLE_433MHZ
    if (rc_switch_ != nullptr && receive_enabled_433_ && rc_switch_->available()) {
        ESP_LOGI(TAG, "[433MHz接收] 检测到可用信号");
        return true;
    }
#endif
#if CONFIG_RF_MODULE_ENABLE_315MHZ
    if (tc_switch_ != nullptr && receive_enabled_315_ && tc_switch_->available()) {
        ESP_LOGI(TAG, "[315MHz接收] 检测到可用信号");
        return true;
    }
#endif
    return false;
}

bool RFModule::Receive(RFSignal& signal) {
    if (!enabled_) {
        return false;
    }
#if CONFIG_RF_MODULE_ENABLE_CC1101
    if (cc1101_initialized_ && has_captured_signal_) {
        signal = captured_signal_;
        has_captured_signal_ = false;
        return true;
    }
#endif
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
                ESP_LOGW(TAG, "[433MHz接收] ⚠️ 信号重复: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 位长:%d) - 与存储中索引%d的信号相同",
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

void RFModule::SetCapturedSignalName(const std::string& name) {
    if (has_captured_signal_) {
        captured_signal_.name = name;
    }
}

void RFModule::ClearCapturedSignal() {
    has_captured_signal_ = false;
    captured_signal_ = RFSignal();
    if (sd_storage_enabled_) {
        ClearStorage();
    }
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

void RFModule::EnableSDStorage(const char* path) {
    if (path == nullptr || path[0] == '\0') return;
    sd_storage_path_ = path;
    if (stored_signals_ == nullptr) {
        stored_signals_ = new RFSignal[MAX_STORED_SIGNALS];
    }
    storage_signal_count_ = 0;
    storage_signal_index_ = 0;
    sd_storage_enabled_ = true;
    LoadFromStorage();
}

void RFModule::DisableSDStorage() {
    sd_storage_enabled_ = false;
    if (stored_signals_ != nullptr) {
        delete[] stored_signals_;
        stored_signals_ = nullptr;
    }
    storage_signal_count_ = 0;
    storage_signal_index_ = 0;
}

uint8_t RFModule::GetStorageSignalCount() const {
    return storage_signal_count_;
}

bool RFModule::IsSDStorageEnabled() const {
    return sd_storage_enabled_;
}

bool RFModule::SaveToStorage() {
#if !CONFIG_RF_MODULE_ENABLE_SD_STORAGE
    (void)this;
    return false;
#else
    if (!sd_storage_enabled_ || stored_signals_ == nullptr) return false;
    if (!has_captured_signal_ || captured_signal_.address.empty()) return false;
    uint8_t duplicate_index = 0;
    if (CheckDuplicateSignal(captured_signal_, duplicate_index)) {
        ESP_LOGW(TAG, "[SD存储] 重复信号，不保存: %s%s - 与索引%d相同",
                captured_signal_.address.c_str(), captured_signal_.key.c_str(), duplicate_index);
        return false;
    }
    if (storage_signal_count_ >= MAX_STORED_SIGNALS) {
        ESP_LOGW(TAG, "[SD存储] 已满 (%d/%d)，覆盖最旧", storage_signal_count_, MAX_STORED_SIGNALS);
    }
    uint8_t write_idx = storage_signal_index_ % MAX_STORED_SIGNALS;
    stored_signals_[write_idx] = captured_signal_;
    storage_signal_index_ = (storage_signal_index_ + 1) % MAX_STORED_SIGNALS;
    if (storage_signal_count_ < MAX_STORED_SIGNALS) storage_signal_count_++;
    std::string filepath = sd_storage_path_ + "/rf_signals.txt";
    FILE* f = fopen(filepath.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for write", filepath.c_str());
        return false;
    }
    fprintf(f, "%u\n", (unsigned)storage_signal_count_);
    for (uint8_t i = 0; i < storage_signal_count_; i++) {
        uint8_t idx = (storage_signal_index_ - 1 - i + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
        const RFSignal& s = stored_signals_[idx];
        fprintf(f, "%s\t%s\t%u\t%u\t%u\t%s\n",
                s.address.c_str(), s.key.c_str(), (unsigned)s.frequency, (unsigned)s.protocol,
                (unsigned)s.pulse_length, s.name.empty() ? "" : s.name.c_str());
    }
    fclose(f);
    ESP_LOGI(TAG, "[SD存储] 已保存到索引 %u (共%u个)", (unsigned)storage_signal_count_, (unsigned)storage_signal_count_);
    return true;
#endif
}

bool RFModule::LoadFromStorage() {
#if !CONFIG_RF_MODULE_ENABLE_SD_STORAGE
    return false;
#else
    if (!sd_storage_enabled_ || stored_signals_ == nullptr || sd_storage_path_.empty()) return false;
    std::string filepath = sd_storage_path_ + "/rf_signals.txt";
    FILE* f = fopen(filepath.c_str(), "r");
    if (!f) return false;
    unsigned int count = 0;
    if (fscanf(f, "%u", &count) != 1 || count > MAX_STORED_SIGNALS) {
        fclose(f);
        return false;
    }
    storage_signal_count_ = (uint8_t)count;
    storage_signal_index_ = storage_signal_count_ % MAX_STORED_SIGNALS;
    char line[512];
    if (fgets(line, sizeof(line), f) == nullptr && count > 0) { fclose(f); return false; }
    for (uint8_t i = 0; i < storage_signal_count_; i++) {
        if (fgets(line, sizeof(line), f) == nullptr) break;
        uint8_t slot = (storage_signal_count_ - 1 - i + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
        RFSignal& s = stored_signals_[slot];
        char* p = line;
        char* tabs[6];
        int nt = 0;
        for (nt = 0; nt < 6 && p; nt++) {
            tabs[nt] = p;
            p = strchr(p, '\t');
            if (p) { *p = '\0'; p++; }
        }
        if (nt >= 5) {
            s.address = tabs[0];
            s.key = tabs[1];
            s.frequency = (RFFrequency)(atoi(tabs[2]) & 1);
            s.protocol = (uint8_t)atoi(tabs[3]);
            s.pulse_length = (uint16_t)atoi(tabs[4]);
            s.name = (nt >= 6) ? tabs[5] : "";
        }
    }
    if (storage_signal_count_ > 0) {
        uint8_t newest = (storage_signal_index_ - 1 + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
        captured_signal_ = stored_signals_[newest];
        has_captured_signal_ = true;
        ESP_LOGI(TAG, "[SD存储] 已加载 %u 个信号", (unsigned)storage_signal_count_);
    }
    fclose(f);
    return storage_signal_count_ > 0;
#endif
}

void RFModule::ClearStorage() {
    if (!sd_storage_enabled_ || stored_signals_ == nullptr) return;
    storage_signal_count_ = 0;
    storage_signal_index_ = 0;
#if CONFIG_RF_MODULE_ENABLE_SD_STORAGE
    std::string filepath = sd_storage_path_ + "/rf_signals.txt";
    FILE* f = fopen(filepath.c_str(), "w");
    if (f) {
        fprintf(f, "0\n");
        fclose(f);
    }
#endif
    ESP_LOGI(TAG, "[SD存储] 已清除所有信号");
}

bool RFModule::ClearStorageSignal(uint8_t index) {
#if !CONFIG_RF_MODULE_ENABLE_SD_STORAGE
    (void)index;
    return false;
#else
    if (!sd_storage_enabled_ || stored_signals_ == nullptr || index >= storage_signal_count_) return false;
    uint8_t actual = (storage_signal_index_ - 1 - index + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
    for (uint8_t i = actual; i + 1 < storage_signal_count_; i++) {
        uint8_t next = (i + 1) % MAX_STORED_SIGNALS;
        stored_signals_[i] = stored_signals_[next];
    }
    storage_signal_count_--;
    storage_signal_index_ = (storage_signal_index_ - 1 + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
    std::string filepath = sd_storage_path_ + "/rf_signals.txt";
    FILE* f = fopen(filepath.c_str(), "w");
    if (!f) return false;
    fprintf(f, "%u\n", (unsigned)storage_signal_count_);
    for (uint8_t i = 0; i < storage_signal_count_; i++) {
        uint8_t idx = (storage_signal_index_ - 1 - i + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
        const RFSignal& s = stored_signals_[idx];
        fprintf(f, "%s\t%s\t%u\t%u\t%u\t%s\n",
                s.address.c_str(), s.key.c_str(), (unsigned)s.frequency, (unsigned)s.protocol,
                (unsigned)s.pulse_length, s.name.empty() ? "" : s.name.c_str());
    }
    fclose(f);
    ESP_LOGI(TAG, "[SD存储] 已清除索引 %u (剩余%u个)", (unsigned)(index + 1), (unsigned)storage_signal_count_);
    return true;
#endif
}

bool RFModule::GetStorageSignal(uint8_t index, RFSignal& signal) const {
#if !CONFIG_RF_MODULE_ENABLE_SD_STORAGE
    (void)index;
    (void)signal;
    return false;
#else
    if (!sd_storage_enabled_ || stored_signals_ == nullptr || index >= storage_signal_count_) return false;
    uint8_t actual_index = (storage_signal_index_ - 1 - index + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
    signal = stored_signals_[actual_index];
    return true;
#endif
}

bool RFModule::UpdateStorageSignalName(uint8_t index, const std::string& name) {
#if !CONFIG_RF_MODULE_ENABLE_SD_STORAGE
    (void)index;
    (void)name;
    return false;
#else
    if (!sd_storage_enabled_ || stored_signals_ == nullptr || index >= storage_signal_count_) return false;
    uint8_t actual_index = (storage_signal_index_ - 1 - index + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
    stored_signals_[actual_index].name = name;
    std::string filepath = sd_storage_path_ + "/rf_signals.txt";
    FILE* f = fopen(filepath.c_str(), "w");
    if (!f) return false;
    fprintf(f, "%u\n", (unsigned)storage_signal_count_);
    for (uint8_t i = 0; i < storage_signal_count_; i++) {
        uint8_t idx = (storage_signal_index_ - 1 - i + MAX_STORED_SIGNALS) % MAX_STORED_SIGNALS;
        const RFSignal& s = stored_signals_[idx];
        fprintf(f, "%s\t%s\t%u\t%u\t%u\t%s\n",
                s.address.c_str(), s.key.c_str(), (unsigned)s.frequency, (unsigned)s.protocol,
                (unsigned)s.pulse_length, s.name.empty() ? "" : s.name.c_str());
    }
    fclose(f);
    ESP_LOGI(TAG, "[SD存储] 已更新索引 %u 名称: %s", (unsigned)(index + 1), name.c_str());
    return true;
#endif
}

bool RFModule::CheckDuplicateSignal(const RFSignal& signal, uint8_t& duplicate_index) const {
#if !CONFIG_RF_MODULE_ENABLE_SD_STORAGE
    (void)signal;
    (void)duplicate_index;
    return false;
#else
    if (!sd_storage_enabled_ || storage_signal_count_ == 0) return false;
    for (uint8_t i = 0; i < storage_signal_count_; i++) {
        RFSignal stored_signal;
        if (GetStorageSignal(i, stored_signal) &&
            stored_signal.address == signal.address &&
            stored_signal.key == signal.key &&
            stored_signal.frequency == signal.frequency) {
            duplicate_index = storage_signal_count_ - i;
            return true;
        }
    }
    return false;
#endif
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

void RFModule::SendSignalRCSwitch(const std::string& address, const std::string& key, uint16_t pulse_length, uint8_t protocol) {
    if (rc_switch_ == nullptr || !enabled_) {
        return;
    }
    
    uint32_t code24bit = 0;
    
    // 直接解析address（6位十六进制 = 24位）
    for (size_t i = 0; i < 6 && i < address.length(); i++) {
        uint8_t val = HexToNum(address[i]);
        code24bit = (code24bit << 4) | val;
    }
    
    // Use provided pulse_length and protocol instead of global variables
    // This ensures signals are sent with their captured pulse length
    rc_switch_->setProtocol(protocol);
    rc_switch_->setPulseLength(pulse_length);
    rc_switch_->setRepeatTransmit(repeat_count_433_);
    
    ESP_LOGI(TAG, "[433MHz发送] 开始发送信号: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次)",
             address.c_str(), key.c_str(), code24bit, protocol, pulse_length, repeat_count_433_);
    
    // Send 24-bit data (standard industry practice: repeat 3 times)
    int64_t send_start_time = esp_timer_get_time();
    rc_switch_->send(code24bit, 24);
    int64_t send_duration = (esp_timer_get_time() - send_start_time) / 1000;  // Convert to milliseconds
    
    ESP_LOGI(TAG, "[433MHz发送] ✓ 发送完成: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次, 耗时:%ldms)",
             address.c_str(), key.c_str(), (unsigned long)code24bit, protocol, pulse_length, repeat_count_433_, (long)send_duration);
}

void RFModule::SendSignalTCSwitch(const std::string& address, const std::string& key, uint16_t pulse_length, uint8_t protocol) {
    if (tc_switch_ == nullptr || !enabled_) {
        return;
    }
    
    uint32_t code24bit = 0;
    
    // 直接解析address（6位十六进制 = 24位）
    for (size_t i = 0; i < 6 && i < address.length(); i++) {
        uint8_t val = HexToNum(address[i]);
        code24bit = (code24bit << 4) | val;
    }
    
    // Use provided pulse_length and protocol instead of global variables
    // This ensures signals are sent with their captured pulse length
    tc_switch_->setProtocol(protocol);
    tc_switch_->setPulseLength(pulse_length);
    tc_switch_->setRepeatTransmit(repeat_count_315_);
    
    ESP_LOGI(TAG, "[315MHz发送] 开始发送信号: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次)",
             address.c_str(), key.c_str(), code24bit, protocol, pulse_length, repeat_count_315_);
    
    // Send 24-bit data (standard industry practice: repeat 3 times)
    int64_t send_start_time = esp_timer_get_time();
    tc_switch_->send(code24bit, 24);
    int64_t send_duration = (esp_timer_get_time() - send_start_time) / 1000;  // Convert to milliseconds
    
    ESP_LOGI(TAG, "[315MHz发送] ✓ 发送完成: %s%s (24位:0x%06lX, 协议:%d, 脉冲:%dμs, 重复:%d次, 耗时:%ldms)",
             address.c_str(), key.c_str(), (unsigned long)code24bit, protocol, pulse_length, repeat_count_315_, (long)send_duration);
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
        
        // Save to storage (SD) when in capture mode
        if (sd_storage_enabled_) {
            if (SaveToStorage()) {
                has_captured_signal_ = true;
            } else {
                has_captured_signal_ = true;  // Still set so tool can detect duplicate error
            }
        } else {
            has_captured_signal_ = true;
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

