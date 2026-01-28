#ifndef RF_MODULE_H
#define RF_MODULE_H

#include <driver/gpio.h>
#include <string>
#include <cstdint>
#include "rf_module_config.h"

// Forward declarations
class RCSwitch;
class TCSwitch;
#if CONFIG_RF_MODULE_ENABLE_CC1101
class CC1101;
#endif

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
#if CONFIG_RF_MODULE_ENABLE_CC1101
    /** CC1101 mode: call after SPI bus is initialized by main. Uses tx433=CS, rx433=GDO0, tx315=GDO2. */
    void Begin(int spi_host, int sck_pin, int mosi_pin, int miso_pin);
#endif
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
    
    // Signal storage (SD card when CONFIG_RF_MODULE_ENABLE_SD_STORAGE; main project mounts SD)
    void EnableSDStorage(const char* path);
    void DisableSDStorage();
    bool SaveToStorage();
    bool LoadFromStorage();
    void ClearStorage();
    bool ClearStorageSignal(uint8_t index);
    uint8_t GetStorageSignalCount() const;
    bool GetStorageSignal(uint8_t index, RFSignal& signal) const;
    bool UpdateStorageSignalName(uint8_t index, const std::string& name);
    bool IsSDStorageEnabled() const;
    bool CheckDuplicateSignal(const RFSignal& signal, uint8_t& duplicate_index) const;
    
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

#if CONFIG_RF_MODULE_ENABLE_CC1101
    CC1101* cc1101_;
    bool cc1101_initialized_;
#endif

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
    
    // Signal storage (SD file when CONFIG_RF_MODULE_ENABLE_SD_STORAGE)
    static constexpr uint8_t MAX_STORED_SIGNALS = CONFIG_RF_MODULE_MAX_STORED_SIGNALS;
    bool sd_storage_enabled_;
    std::string sd_storage_path_;
    RFSignal* stored_signals_;
    uint8_t storage_signal_count_;
    uint8_t storage_signal_index_;
    
    // Status
    bool enabled_;
    RFSignal last_received_;
    
    // Internal functions
    uint8_t HexToNum(char c);
    void SendSignalRCSwitch(const std::string& address, const std::string& key, uint16_t pulse_length, uint8_t protocol);
    void SendSignalTCSwitch(const std::string& address, const std::string& key, uint16_t pulse_length, uint8_t protocol);
#if CONFIG_RF_MODULE_ENABLE_CC1101
    void SetupCC1101ForRx(int frequency_mhz);
    void SetupCC1101ForTx(int frequency_mhz);
    void SendSignalCC1101(const std::string& address, const std::string& key, RFFrequency freq, uint16_t pulse_length, uint8_t protocol);
#endif
    void AddToReplayBuffer(const RFSignal& signal);
    void CheckCaptureMode(const RFSignal& signal);
    std::string Uint32ToHex(uint32_t value, int length);
    uint32_t HexToUint32(const std::string& hex);
};

#endif // RF_MODULE_H

