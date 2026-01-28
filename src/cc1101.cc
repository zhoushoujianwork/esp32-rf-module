#include "cc1101.h"
#include <cstring>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#define TAG "CC1101"

CC1101::CC1101() : spi_(nullptr), cs_pin_(-1), gdo0_pin_(-1), gdo2_pin_(-1), last_freq_mhz_(433.92f) {}

CC1101::~CC1101() {
    if (spi_) {
        spi_bus_remove_device(spi_);
    }
}

esp_err_t CC1101::Init(spi_host_device_t host, int cs_pin, int sck_pin, int mosi_pin, int miso_pin, int gdo0_pin, int gdo2_pin) {
    cs_pin_ = cs_pin;
    gdo0_pin_ = gdo0_pin;
    gdo2_pin_ = gdo2_pin;

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << cs_pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)cs_pin_, 1);

    if (gdo0_pin_ >= 0) {
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << gdo0_pin_);
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
    }
    if (gdo2_pin_ >= 0) {
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << gdo2_pin_);
        gpio_config(&io_conf);
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 5 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 7;

    esp_err_t ret = spi_bus_add_device(host, &devcfg, &spi_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device");
        return ret;
    }

    Reset();
    RegConfigSettings();
    return ESP_OK;
}

void CC1101::SpiWriteReg(uint8_t addr, uint8_t value) {
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &addr;
    spi_device_polling_transmit(spi_, &t);
    t.tx_buffer = &value;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level((gpio_num_t)cs_pin_, 1);
}

void CC1101::SpiWriteBurstReg(uint8_t addr, uint8_t *buffer, uint8_t num) {
    uint8_t addr_byte = addr | WRITE_BURST;
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &addr_byte;
    spi_device_polling_transmit(spi_, &t);
    t.length = 8 * num;
    t.tx_buffer = buffer;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level((gpio_num_t)cs_pin_, 1);
}

void CC1101::SpiStrobe(uint8_t strobe) {
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &strobe;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level((gpio_num_t)cs_pin_, 1);
}

uint8_t CC1101::SpiReadReg(uint8_t addr) {
    uint8_t addr_byte = addr | READ_SINGLE;
    uint8_t value = 0;
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &addr_byte;
    spi_device_polling_transmit(spi_, &t);
    t.rx_buffer = &value;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level((gpio_num_t)cs_pin_, 1);
    return value;
}

void CC1101::SpiReadBurstReg(uint8_t addr, uint8_t *buffer, uint8_t num) {
    uint8_t addr_byte = addr | READ_BURST;
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &addr_byte;
    spi_device_polling_transmit(spi_, &t);
    t.length = 8 * num;
    t.tx_buffer = NULL;
    t.rx_buffer = buffer;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level((gpio_num_t)cs_pin_, 1);
}

uint8_t CC1101::SpiReadStatus(uint8_t addr) {
    uint8_t addr_byte = addr | READ_BURST;
    uint8_t value = 0;
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &addr_byte;
    spi_device_polling_transmit(spi_, &t);
    t.rx_buffer = &value;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level((gpio_num_t)cs_pin_, 1);
    return value;
}

void CC1101::Reset() {
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level((gpio_num_t)cs_pin_, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level((gpio_num_t)cs_pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    SpiStrobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level((gpio_num_t)cs_pin_, 1);
}

void CC1101::RegConfigSettings() {
    SpiWriteReg(CC1101_FSCTRL1,  0x06);
    SpiWriteReg(CC1101_FSCTRL0,  0x00);
    SpiWriteReg(CC1101_MDMCFG4,  0xF5);
    SpiWriteReg(CC1101_MDMCFG3,  0x83);
    SpiWriteReg(CC1101_MDMCFG2,  0x13);
    SpiWriteReg(CC1101_MDMCFG1,  0x22);
    SpiWriteReg(CC1101_MDMCFG0,  0xF8);
    SpiWriteReg(CC1101_CHANNR,   0x00);
    SpiWriteReg(CC1101_DEVIATN,  0x15);
    SpiWriteReg(CC1101_FREND1,   0x56);
    SpiWriteReg(CC1101_FREND0,   0x10);
    SpiWriteReg(CC1101_MCSM0,    0x18);
    SpiWriteReg(CC1101_FOCCFG,   0x16);
    SpiWriteReg(CC1101_BSCFG,    0x6C);
    SpiWriteReg(CC1101_AGCCTRL2, 0x03);
    SpiWriteReg(CC1101_AGCCTRL1, 0x40);
    SpiWriteReg(CC1101_AGCCTRL0, 0x91);
    SpiWriteReg(CC1101_FSCAL3,   0xE9);
    SpiWriteReg(CC1101_FSCAL2,   0x2A);
    SpiWriteReg(CC1101_FSCAL1,   0x00);
    SpiWriteReg(CC1101_FSCAL0,   0x1F);
    SpiWriteReg(CC1101_FSTEST,   0x59);
    SpiWriteReg(CC1101_TEST2,    0x81);
    SpiWriteReg(CC1101_TEST1,    0x35);
    SpiWriteReg(CC1101_TEST0,    0x09);
    SpiWriteReg(CC1101_IOCFG2,   0x0B);
    SpiWriteReg(CC1101_IOCFG0,   0x06);
    SpiWriteReg(CC1101_PKTCTRL1, 0x04);
    SpiWriteReg(CC1101_PKTCTRL0, 0x05);
    SpiWriteReg(CC1101_ADDR,     0x00);
    SpiWriteReg(CC1101_PKTLEN,   0x00);
}

void CC1101::SetFrequency(float mhz) {
    uint32_t freq = (uint32_t)(mhz * 65536.0f / 26.0f);
    SpiWriteReg(CC1101_FREQ2, (freq >> 16) & 0xFF);
    SpiWriteReg(CC1101_FREQ1, (freq >> 8) & 0xFF);
    SpiWriteReg(CC1101_FREQ0, freq & 0xFF);
    last_freq_mhz_ = mhz;
}

void CC1101::SetTx() {
    SpiStrobe(CC1101_SIDLE);
    SpiStrobe(CC1101_STX);
}

void CC1101::SetRx() {
    SpiStrobe(CC1101_SIDLE);
    SpiStrobe(CC1101_SRX);
}

void CC1101::SetIdle() {
    SpiStrobe(CC1101_SIDLE);
}

void CC1101::SendData(uint8_t *txBuffer, uint8_t size) {
    SpiWriteReg(CC1101_TXFIFO, size);
    SpiWriteBurstReg(CC1101_TXFIFO, txBuffer, size);
    SpiStrobe(CC1101_STX);
}

bool CC1101::CheckReceiveFlag() {
    if (gdo0_pin_ >= 0) {
        return gpio_get_level((gpio_num_t)gdo0_pin_);
    }
    return false;
}

uint8_t CC1101::ReceiveData(uint8_t *rxBuffer) {
    uint8_t size;
    uint8_t status[2];

    if (SpiReadStatus(CC1101_RXBYTES) & BYTES_IN_RXFIFO) {
        size = SpiReadReg(CC1101_RXFIFO);
        SpiReadBurstReg(CC1101_RXFIFO, rxBuffer, size);
        SpiReadBurstReg(CC1101_RXFIFO, status, 2);
        SpiStrobe(CC1101_SFRX);
        return size;
    }
    SpiStrobe(CC1101_SFRX);
    return 0;
}

int CC1101::GetRssi() {
    uint8_t rssi_dec = SpiReadStatus(CC1101_RSSI);
    if (rssi_dec >= 128) {
        return (int)((int)(rssi_dec - 256) / 2) - 74;
    }
    return (int)(rssi_dec / 2) - 74;
}

static const uint8_t PA_TABLE_315[8] = {0x12, 0x0D, 0x1C, 0x34, 0x51, 0x85, 0xCB, 0xC2};
static const uint8_t PA_TABLE_433[8] = {0x12, 0x0E, 0x1D, 0x34, 0x60, 0x84, 0xC8, 0xC0};

void CC1101::SetModulation(uint8_t m) {
    if (m > 4) m = 4;
    uint8_t m2modfm, frend0;
    switch (m) {
        case 0: m2modfm = 0x00; frend0 = 0x10; break;
        case 1: m2modfm = 0x10; frend0 = 0x10; break;
        case 2: m2modfm = 0x30; frend0 = 0x11; break;
        case 3: m2modfm = 0x40; frend0 = 0x10; break;
        default: m2modfm = 0x70; frend0 = 0x10; break;
    }
    uint8_t mdmcfg2 = SpiReadReg(CC1101_MDMCFG2);
    mdmcfg2 = (mdmcfg2 & 0x0F) | (m2modfm & 0xF0);
    SpiWriteReg(CC1101_MDMCFG2, mdmcfg2);
    SpiWriteReg(CC1101_FREND0, frend0);
}

void CC1101::SetPA(int p) {
    const uint8_t* table = nullptr;
    if (last_freq_mhz_ >= 300 && last_freq_mhz_ <= 348) {
        table = PA_TABLE_315;
    } else if (last_freq_mhz_ >= 378 && last_freq_mhz_ <= 464) {
        table = PA_TABLE_433;
    } else {
        return;
    }
    int idx = 4;
    if (p <= -30) idx = 0;
    else if (p <= -20) idx = 1;
    else if (p <= -15) idx = 2;
    else if (p <= -10) idx = 3;
    else if (p <= 0) idx = 4;
    else if (p <= 5) idx = 5;
    else if (p <= 7) idx = 6;
    else idx = 7;
    uint8_t pa[8] = {0};
    pa[0] = 0;
    pa[1] = table[idx];
    SpiWriteBurstReg(CC1101_PATABLE, pa, 8);
}

void CC1101::SetChannel(uint8_t chnl) { SpiWriteReg(CC1101_CHANNR, chnl); }
void CC1101::SetChsp(float f) { (void)f; }

void CC1101::SetRxBW(float f) {
    if (f <= 0) return;
    int s1 = 3, s2 = 3;
    float bw = f;
    for (int i = 0; i < 3; i++) {
        if (bw > 101.5625f) { bw /= 2.0f; s1--; }
        else break;
    }
    for (int i = 0; i < 3; i++) {
        if (bw > 58.1f) { bw /= 1.25f; s2--; }
        else break;
    }
    uint8_t m4RxBw = (uint8_t)((s1 * 64) + (s2 * 16));
    uint8_t mdmcfg4 = SpiReadReg(CC1101_MDMCFG4);
    mdmcfg4 = (mdmcfg4 & 0x0F) | m4RxBw;
    SpiWriteReg(CC1101_MDMCFG4, mdmcfg4);
}

void CC1101::SetDRate(float d) {
    float c = d;
    if (c > 1621.83f) c = 1621.83f;
    if (c < 0.0247955f) c = 0.0247955f;
    uint8_t m4DaRa = 0;
    uint8_t MDMCFG3 = 0;
    for (int i = 0; i < 20; i++) {
        if (c <= 0.0494942f) {
            c = (c - 0.0247955f) / 0.00009685f;
            MDMCFG3 = (uint8_t)c;
            if ((c - MDMCFG3) * 10.0f >= 5.0f) MDMCFG3++;
            break;
        }
        m4DaRa++;
        c /= 2.0f;
    }
    uint8_t mdmcfg4 = SpiReadReg(CC1101_MDMCFG4);
    mdmcfg4 = (mdmcfg4 & 0xF0) | (m4DaRa & 0x0F);
    SpiWriteReg(CC1101_MDMCFG4, mdmcfg4);
    SpiWriteReg(CC1101_MDMCFG3, MDMCFG3);
}

void CC1101::SetDeviation(float d) { (void)d; }
void CC1101::SetSyncWord(uint8_t sh, uint8_t sl) {
    SpiWriteReg(CC1101_SYNC1, sh);
    SpiWriteReg(CC1101_SYNC0, sl);
}
void CC1101::SetAddr(uint8_t v) { SpiWriteReg(CC1101_ADDR, v); }
void CC1101::SetWhiteData(bool v) { (void)v; }
void CC1101::SetPktFormat(uint8_t v) {
    if (v == 3) {
        SpiWriteReg(CC1101_PKTCTRL0, 0x32);
    }
}
void CC1101::SetCrc(bool v) { (void)v; }
void CC1101::SetLengthConfig(uint8_t v) { (void)v; }
void CC1101::SetPacketLength(uint8_t v) { SpiWriteReg(CC1101_PKTLEN, v); }
void CC1101::SetDcFilterOff(bool v) { (void)v; }
void CC1101::SetManchester(bool v) { (void)v; }
void CC1101::SetSyncMode(uint8_t v) { (void)v; }
void CC1101::SetFEC(bool v) { (void)v; }
void CC1101::SetPRE(uint8_t v) { (void)v; }
void CC1101::SetPQT(uint8_t v) { (void)v; }
void CC1101::SetAppendStatus(bool v) { (void)v; }
void CC1101::SetPowerDown() { SpiStrobe(CC1101_SPWD); }
uint8_t CC1101::GetLqi() { return SpiReadStatus(CC1101_LQI); }
uint8_t CC1101::GetMode() { return SpiReadStatus(CC1101_MARCSTATE); }
bool CC1101::CheckRxFifo(int t) {
    (void)t;
    return (SpiReadStatus(CC1101_RXBYTES) & BYTES_IN_RXFIFO) != 0;
}

bool CC1101::CheckChip() {
    return SpiReadStatus(CC1101_VERSION) > 0;
}

void CC1101::Split_MDMCFG1() {}
void CC1101::Split_MDMCFG2() {}
void CC1101::Split_MDMCFG4() {}
