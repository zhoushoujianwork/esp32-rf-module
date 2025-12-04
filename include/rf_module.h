#ifndef RF_MODULE_H
#define RF_MODULE_H

#include <driver/gpio.h>
#include <string>
#include <cstdint>
#include "rf_module_config.h"

#if CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE
#include <nvs.h>  // NVS available on all ESP32 series chips
#endif

// Forward declarations
class RCSwitch;
class TCSwitch;

enum RFFrequency {
    RF_433MHZ = 0,
    RF_315MHZ = 1
};

struct RFSignal {
    std::string address;      // 6位十六进制地址码
    std::string key;          // 2位十六进制按键值
    RFFrequency frequency;    // 频率类型
    uint8_t protocol;        // 协议编号
    uint16_t pulse_length;    // 脉冲长度（微秒）
    std::string name;         // 信号主题/名称（如"卧室灯开关"、"空调开关"）
    
    RFSignal() : frequency(RF_433MHZ), protocol(1), pulse_length(320) {}
};

class RFModule {
public:
    RFModule(gpio_num_t tx433_pin, gpio_num_t rx433_pin,
             gpio_num_t tx315_pin, gpio_num_t rx315_pin);
    ~RFModule();
    
    // Initialization
    void Begin();
    void End();
    
    // Send functions
    void Send(const std::string& address, const std::string& key, RFFrequency freq = RF_433MHZ);
    void Send(const RFSignal& signal);
    
    // Receive functions
    bool ReceiveAvailable();
    bool Receive(RFSignal& signal);
    
    // Configuration
    // Note: When freq is not specified (0xFF), sets both frequencies (433MHz and 315MHz)
    void SetRepeatCount(uint8_t count, RFFrequency freq = RF_433MHZ);
    void SetProtocol(uint8_t protocol, RFFrequency freq = RF_433MHZ);
    void SetPulseLength(uint16_t pulse_length, RFFrequency freq = RF_433MHZ);
    
    // Frequency selection
    void SetFrequency(RFFrequency freq);
    RFFrequency GetFrequency() const { return current_frequency_; }
    
    // Capture mode
    void EnableCaptureMode();
    void DisableCaptureMode();
    bool IsCaptureMode() const { return capture_mode_; }
    bool HasCapturedSignal() const { return has_captured_signal_; }
    RFSignal GetCapturedSignal() const { return captured_signal_; }
    void SetCapturedSignalName(const std::string& name);  // Set name for captured signal
    void ClearCapturedSignal();
    
    // Statistics
    uint32_t GetSendCount() const { return send_count_; }
    uint32_t GetReceiveCount() const { return receive_count_; }
    void ResetCounters();
    
    // Receive control
    void EnableReceive(RFFrequency freq = RF_433MHZ);
    void DisableReceive(RFFrequency freq = RF_433MHZ);
    bool IsReceiving(RFFrequency freq = RF_433MHZ) const;
    
    // Callback support
    typedef void (*ReceiveCallback)(const RFSignal& signal);
    void SetReceiveCallback(ReceiveCallback callback);
    
    // Replay buffer functions (signal history)
    void EnableReplayBuffer(uint8_t size = 10);
    void DisableReplayBuffer();
    uint8_t GetReplayBufferCount() const;
    bool GetReplaySignal(uint8_t index, RFSignal& signal) const;
    RFSignal GetLastReceived() const { return last_received_; }
    void ClearReplayBuffer();
    
    // Flash persistence functions (NVS available on all ESP32 series chips)
    void EnableFlashStorage(const char* namespace_name = "rf_replay");
    void DisableFlashStorage();
    bool SaveToFlash();
    bool LoadFromFlash();
    void ClearFlash();
    bool ClearFlashSignal(uint8_t index);  // Clear a single signal by index (0-based, internal index)
    uint8_t GetFlashSignalCount() const { return flash_signal_count_; }
    bool GetFlashSignal(uint8_t index, RFSignal& signal) const;
    bool UpdateFlashSignalName(uint8_t index, const std::string& name);  // Update name for a signal by index (0-based, internal index)
    bool IsFlashStorageEnabled() const { return flash_storage_enabled_; }
    bool CheckDuplicateSignal(const RFSignal& signal, uint8_t& duplicate_index) const;  // Check if signal already exists in flash storage
    
    // Status
    bool IsEnabled() const { return enabled_; }

private:
    // Hardware pins
    gpio_num_t tx433_pin_;
    gpio_num_t rx433_pin_;
    gpio_num_t tx315_pin_;
    gpio_num_t rx315_pin_;
    
    // Switch instances
    RCSwitch* rc_switch_;  // 433MHz
    TCSwitch* tc_switch_;  // 315MHz
    
    // Current frequency
    RFFrequency current_frequency_;
    
    // Configuration
    uint8_t repeat_count_433_;
    uint8_t repeat_count_315_;
    uint8_t protocol_433_;
    uint8_t protocol_315_;
    uint16_t pulse_length_433_;
    uint16_t pulse_length_315_;
    
    // Statistics
    uint32_t send_count_;
    uint32_t receive_count_;
    
    // Callback
    ReceiveCallback receive_callback_;
    
    // Replay buffer
    bool replay_buffer_enabled_;
    RFSignal* replay_buffer_;
    uint8_t replay_buffer_size_;
    uint8_t replay_buffer_index_;
    uint8_t replay_buffer_count_;
    
    // Capture mode
    bool capture_mode_;
    RFSignal captured_signal_;
    bool has_captured_signal_;
    
    // Receive control
    bool receive_enabled_433_;
    bool receive_enabled_315_;
    
    // Flash storage (NVS available on all ESP32 series chips)
    static constexpr uint8_t MAX_FLASH_SIGNALS = CONFIG_RF_MODULE_MAX_FLASH_SIGNALS;  // Maximum number of signals to store in flash (configurable via CMake/Kconfig)
    bool flash_storage_enabled_;
    nvs_handle_t nvs_handle_;
    std::string flash_namespace_;
    uint8_t flash_signal_count_;  // Number of signals currently stored in flash
    uint8_t flash_signal_index_;  // Current write index (circular buffer)
    
    // Status
    bool enabled_;
    RFSignal last_received_;
    
    // Internal functions
    uint8_t HexToNum(char c);
    void SendSignalRCSwitch(const std::string& address, const std::string& key);
    void SendSignalTCSwitch(const std::string& address, const std::string& key);
    void AddToReplayBuffer(const RFSignal& signal);
    void CheckCaptureMode(const RFSignal& signal);
    std::string Uint32ToHex(uint32_t value, int length);
    uint32_t HexToUint32(const std::string& hex);
};

#endif // RF_MODULE_H

