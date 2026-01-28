#ifndef CC1101_H
#define CC1101_H

#include "cc1101_defs.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_err.h>

class CC1101 {
public:
    CC1101();
    ~CC1101();

    esp_err_t Init(spi_host_device_t host, int cs_pin, int sck_pin, int mosi_pin, int miso_pin, int gdo0_pin, int gdo2_pin);
    void Reset();

    void SetModulation(uint8_t m);
    void SetPA(int p);
    void SetFrequency(float mhz);
    void SetChannel(uint8_t chnl);
    void SetChsp(float f);
    void SetRxBW(float f);
    void SetDRate(float d);
    void SetDeviation(float d);
    void SetSyncWord(uint8_t sh, uint8_t sl);
    void SetAddr(uint8_t v);
    void SetWhiteData(bool v);
    void SetPktFormat(uint8_t v);
    void SetCrc(bool v);
    void SetLengthConfig(uint8_t v);
    void SetPacketLength(uint8_t v);
    void SetDcFilterOff(bool v);
    void SetManchester(bool v);
    void SetSyncMode(uint8_t v);
    void SetFEC(bool v);
    void SetPRE(uint8_t v);
    void SetPQT(uint8_t v);
    void SetAppendStatus(bool v);

    void SetTx();
    void SetRx();
    void SetIdle();
    void SetPowerDown();

    void SendData(uint8_t *txBuffer, uint8_t size);
    bool CheckReceiveFlag();
    uint8_t ReceiveData(uint8_t *rxBuffer);

    int GetRssi();
    uint8_t GetLqi();
    uint8_t GetMode();
    bool CheckRxFifo(int t);

    void SpiWriteReg(uint8_t addr, uint8_t value);
    void SpiWriteBurstReg(uint8_t addr, uint8_t *buffer, uint8_t num);
    void SpiStrobe(uint8_t strobe);
    uint8_t SpiReadReg(uint8_t addr);
    void SpiReadBurstReg(uint8_t addr, uint8_t *buffer, uint8_t num);
    uint8_t SpiReadStatus(uint8_t addr);

    bool CheckChip();

private:
    spi_device_handle_t spi_;

    void Split_MDMCFG1();
    void Split_MDMCFG2();
    void Split_MDMCFG4();

    void RegConfigSettings();

    int cs_pin_;
    int gdo0_pin_;
    int gdo2_pin_;
    float last_freq_mhz_;
};

#endif /* CC1101_H */
