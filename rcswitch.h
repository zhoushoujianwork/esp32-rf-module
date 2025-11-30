#ifndef RCSWITCH_H
#define RCSWITCH_H

#include <driver/gpio.h>
#include <esp_attr.h>
#include <stdint.h>
#include <stdbool.h>

class RCSwitch {
public:
    RCSwitch();
    ~RCSwitch();
    
    void enableTransmit(int nTransmitterPin);
    void disableTransmit();
    void setPulseLength(int nPulseLength);
    void setRepeatTransmit(int nRepeatTransmit);
    void setProtocol(int nProtocol);
    void send(unsigned long code, unsigned int length);
    
    void enableReceive(int interrupt);
    void disableReceive();
    bool available();
    void resetAvailable();
    
    unsigned long getReceivedValue();
    unsigned int getReceivedBitlength();
    unsigned int getReceivedDelay();
    unsigned int getReceivedProtocol();

    struct HighLow {
        uint8_t high;
        uint8_t low;
    };
    
    struct Protocol {
        uint16_t pulseLength;
        HighLow syncFactor;
        HighLow zero;
        HighLow one;
        bool invertedSignal;
    };
    
private:
    void transmit(HighLow pulses);
    static void IRAM_ATTR handleInterrupt(void* arg);
    static bool receiveProtocol(const int p, unsigned int changeCount);
    
    gpio_num_t nTransmitterPin;
    int nRepeatTransmit;
    Protocol protocol;
    
    int nReceiverInterrupt;
    static volatile unsigned long nReceivedValue;
    static volatile unsigned int nReceivedBitlength;
    static volatile unsigned int nReceivedDelay;
    static volatile unsigned int nReceivedProtocol;
    static int nReceiveTolerance;
    static const unsigned int nSeparationLimit;
    static unsigned int timings[67];
    static RCSwitch* instance;
};

#endif // RCSWITCH_H

