#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/spi_master.h"
#include "esp_err.h"

namespace rfbridge {

struct Cc1101Config {
    int sclk_gpio;
    int miso_gpio;
    int mosi_gpio;
    int cs_gpio;
    int gdo0_gpio;
    int gdo2_gpio;
    uint32_t crystal_hz;
    uint32_t frequency_hz;
    int8_t tx_power_dbm;
};

struct Cc1101Info {
    uint8_t part_number;
    uint8_t version;
    uint8_t marc_state;
    int16_t rssi_dbm_x2;
    bool carrier_sense;
    bool clear_channel;
    uint32_t reset_count;
    uint32_t recovery_count;
    uint32_t ready_timeout_count;
    uint32_t state_timeout_count;
};

constexpr uint8_t kCc1101MarcStateIdle = 0x01;
constexpr uint8_t kCc1101MarcStateRx = 0x0D;
constexpr uint8_t kCc1101MarcStateRxFifoOverflow = 0x11;
constexpr uint8_t kCc1101MarcStateTx = 0x13;
constexpr uint8_t kCc1101MarcStateTxFifoUnderflow = 0x16;

uint32_t cc1101_frequency_word(uint32_t frequency_hz, uint32_t crystal_hz);
uint8_t cc1101_pa_table_value(int8_t requested_dbm);

class Cc1101 {
public:
    Cc1101() = default;
    Cc1101(const Cc1101 &) = delete;
    Cc1101 &operator=(const Cc1101 &) = delete;

    esp_err_t initialize(const Cc1101Config &config);
    esp_err_t deinitialize();

    esp_err_t reset_and_configure();
    esp_err_t enter_idle(uint32_t timeout_ms = 20);
    esp_err_t enter_receive(uint32_t timeout_ms = 20);
    esp_err_t enter_transmit(uint32_t timeout_ms = 20);
    esp_err_t recover_receive();

    esp_err_t read_info(Cc1101Info *info);
    esp_err_t read_register(uint8_t address, uint8_t *value);
    bool initialized() const;
    const Cc1101Config &config() const;

private:
    esp_err_t manual_reset();
    esp_err_t apply_profile();
    esp_err_t verify_profile();
    esp_err_t write_register(uint8_t address, uint8_t value);
    esp_err_t write_burst(uint8_t address, const uint8_t *values, std::size_t count);
    esp_err_t read_burst(uint8_t address, uint8_t *values, std::size_t count);
    esp_err_t read_status_register(uint8_t address, uint8_t *value);
    esp_err_t read_stable_status(uint8_t address, uint8_t *value);
    esp_err_t strobe(uint8_t command, uint8_t *status = nullptr);
    esp_err_t wait_for_state(uint8_t expected, uint32_t timeout_ms);
    esp_err_t wait_chip_ready(uint32_t timeout_us);
    esp_err_t transfer_selected(const uint8_t *tx, uint8_t *rx, std::size_t count);
    esp_err_t begin_transaction();
    void end_transaction();

    Cc1101Config config_{};
    spi_device_handle_t spi_device_ = nullptr;
    bool bus_initialized_ = false;
    bool gpio_initialized_ = false;
    bool selected_ = false;
    bool initialized_ = false;
    uint8_t part_number_ = 0;
    uint8_t version_ = 0;
    uint32_t reset_count_ = 0;
    uint32_t recovery_count_ = 0;
    uint32_t ready_timeout_count_ = 0;
    uint32_t state_timeout_count_ = 0;
};

}  // namespace rfbridge
