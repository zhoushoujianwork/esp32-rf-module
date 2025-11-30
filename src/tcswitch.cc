#include "esp32_rf_module/tcswitch.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <string.h>

#define TAG "TCSwitch"

// Static member initialization
volatile unsigned long TCSwitch::nReceivedValue = 0;
volatile unsigned int TCSwitch::nReceivedBitlength = 0;
volatile unsigned int TCSwitch::nReceivedDelay = 0;
volatile unsigned int TCSwitch::nReceivedProtocol = 0;
int TCSwitch::nReceiveTolerance = 60;
const unsigned int TCSwitch::nSeparationLimit = 4300;
unsigned int TCSwitch::timings[67] = {0};
TCSwitch* TCSwitch::instance = nullptr;

// Protocol definitions (same as RCSwitch for 315MHz)
static const TCSwitch::Protocol proto[] = {
    { 350, {  1, 31 }, {  1,  3 }, {  3,  1 }, false },    // protocol 1
    { 650, {  1, 10 }, {  1,  2 }, {  2,  1 }, false },    // protocol 2
    { 100, { 30, 71 }, {  4, 11 }, {  9,  6 }, false },    // protocol 3
    { 380, {  1,  6 }, {  1,  3 }, {  3,  1 }, false },    // protocol 4
    { 500, {  6, 14 }, {  1,  2 }, {  2,  1 }, false },    // protocol 5
};

TCSwitch::TCSwitch() {
    nTransmitterPin = GPIO_NUM_NC;
    nRepeatTransmit = 10;
    nReceiverInterrupt = -1;
    setProtocol(1);
    instance = this;
}

TCSwitch::~TCSwitch() {
    disableReceive();
    disableTransmit();
    if (instance == this) {
        instance = nullptr;
    }
}

void TCSwitch::enableTransmit(int nTransmitterPin) {
    this->nTransmitterPin = static_cast<gpio_num_t>(nTransmitterPin);
    gpio_set_direction(this->nTransmitterPin, GPIO_MODE_OUTPUT);
    gpio_set_level(this->nTransmitterPin, 0);
}

void TCSwitch::disableTransmit() {
    if (nTransmitterPin != GPIO_NUM_NC) {
        gpio_set_level(nTransmitterPin, 0);
        nTransmitterPin = GPIO_NUM_NC;
    }
}

void TCSwitch::setPulseLength(int nPulseLength) {
    protocol.pulseLength = nPulseLength;
}

void TCSwitch::setRepeatTransmit(int nRepeatTransmit) {
    this->nRepeatTransmit = nRepeatTransmit;
}

void TCSwitch::setProtocol(int nProtocol) {
    if (nProtocol >= 1 && nProtocol <= 5) {
        protocol = proto[nProtocol - 1];
    } else {
        protocol = proto[0];  // Default to protocol 1
    }
}

void TCSwitch::send(unsigned long code, unsigned int length) {
    if (nTransmitterPin == GPIO_NUM_NC) {
        return;
    }
    
    for (int nRepeat = 0; nRepeat < nRepeatTransmit; nRepeat++) {
        // Send sync
        transmit(protocol.syncFactor);
        
        // Send code bits
        for (int i = length - 1; i >= 0; i--) {
            if (code & (1UL << i)) {
                transmit(protocol.one);
            } else {
                transmit(protocol.zero);
            }
        }
        
        // Send sync again
        transmit(protocol.syncFactor);
    }
}

void TCSwitch::transmit(HighLow pulses) {
    int pulse_length = protocol.pulseLength;
    
    auto delay_us = [](uint32_t us) {
        uint64_t start = esp_timer_get_time();
        while ((esp_timer_get_time() - start) < us) {
            // Busy wait
        }
    };
    
    if (protocol.invertedSignal) {
        gpio_set_level(nTransmitterPin, 0);
        delay_us(pulse_length * pulses.high);
        gpio_set_level(nTransmitterPin, 1);
        delay_us(pulse_length * pulses.low);
    } else {
        gpio_set_level(nTransmitterPin, 1);
        delay_us(pulse_length * pulses.high);
        gpio_set_level(nTransmitterPin, 0);
        delay_us(pulse_length * pulses.low);
    }
}

// Helper function for receiveProtocol
static inline unsigned int diff(int A, int B) {
    return (A > B) ? (A - B) : (B - A);
}

void IRAM_ATTR TCSwitch::handleInterrupt(void* arg) {
    TCSwitch* self = static_cast<TCSwitch*>(arg);
    if (!self) return;
    
    static unsigned long lastTime = 0;
    static unsigned int changeCount = 0;
    static unsigned int repeatCount = 0;
    
    unsigned long now = esp_timer_get_time();  // Already in microseconds
    unsigned int duration = (now > lastTime) ? (now - lastTime) : 0;
    
    if (duration > nSeparationLimit) {
        if ((repeatCount == 0) || (diff(duration, timings[0]) < 200)) {
            repeatCount++;
            if (repeatCount == 2) {
                // Try to decode with all protocols
                for (unsigned int i = 1; i <= 5; i++) {
                    if (receiveProtocol(i, changeCount)) {
                        // Signal decoded successfully
                        break;
                    }
                }
                repeatCount = 0;
            }
        }
        changeCount = 0;
    }
    
    // Detect overflow
    if (changeCount >= 67) {
        changeCount = 0;
        repeatCount = 0;
    }
    
    if (changeCount < 67) {
        timings[changeCount++] = duration;
    }
    lastTime = now;
}

bool TCSwitch::receiveProtocol(const int p, unsigned int changeCount) {
    if (p < 1 || p > 5) return false;  // Only support protocols 1-5 for now
    
    const Protocol& pro = proto[p - 1];
    
    unsigned long code = 0;
    // Assuming the longer pulse length is the pulse captured in timings[0]
    const unsigned int syncLengthInPulses = ((pro.syncFactor.low) > (pro.syncFactor.high)) ? (pro.syncFactor.low) : (pro.syncFactor.high);
    const unsigned int delay = timings[0] / syncLengthInPulses;
    const unsigned int delayTolerance = delay * nReceiveTolerance / 100;
    
    const unsigned int firstDataTiming = (pro.invertedSignal) ? (2) : (1);
    
    for (unsigned int i = firstDataTiming; i < changeCount - 1; i += 2) {
        code <<= 1;
        if (diff(timings[i], delay * pro.zero.high) < delayTolerance &&
            diff(timings[i + 1], delay * pro.zero.low) < delayTolerance) {
            // zero bit
        } else if (diff(timings[i], delay * pro.one.high) < delayTolerance &&
                   diff(timings[i + 1], delay * pro.one.low) < delayTolerance) {
            // one bit
            code |= 1;
        } else {
            // Failed to decode
            return false;
        }
    }
    
    if (changeCount > 7) {  // ignore very short transmissions: no device sends them, so this must be noise
        nReceivedValue = code;
        nReceivedBitlength = (changeCount - 1) / 2;
        nReceivedDelay = delay;
        nReceivedProtocol = p;
        return true;
    }
    
    return false;
}

void TCSwitch::enableReceive(int interrupt) {
    nReceiverInterrupt = interrupt;
    gpio_num_t pin = static_cast<gpio_num_t>(interrupt);
    
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);
    
    gpio_install_isr_service(0);
    gpio_isr_handler_add(pin, handleInterrupt, this);
    
    nReceivedValue = 0;
    nReceivedBitlength = 0;
    nReceivedDelay = 0;
    nReceivedProtocol = 0;
    memset(timings, 0, sizeof(timings));
}

void TCSwitch::disableReceive() {
    if (nReceiverInterrupt >= 0) {
        gpio_isr_handler_remove(static_cast<gpio_num_t>(nReceiverInterrupt));
        nReceiverInterrupt = -1;
    }
}

bool TCSwitch::available() {
    return nReceivedValue != 0;
}

void TCSwitch::resetAvailable() {
    nReceivedValue = 0;
    nReceivedBitlength = 0;
    nReceivedDelay = 0;
    nReceivedProtocol = 0;
}

unsigned long TCSwitch::getReceivedValue() {
    return nReceivedValue;
}

unsigned int TCSwitch::getReceivedBitlength() {
    return nReceivedBitlength;
}

unsigned int TCSwitch::getReceivedDelay() {
    return nReceivedDelay;
}

unsigned int TCSwitch::getReceivedProtocol() {
    return nReceivedProtocol;
}

