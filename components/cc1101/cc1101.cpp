#include "cc1101.hpp"

#include <array>
#include <climits>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace rfbridge {
namespace {

constexpr char kTag[] = "cc1101";
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr int kSpiClockHz = 4000000;
constexpr uint32_t kChipReadyTimeoutUs = 10000;
constexpr uint8_t kReadSingle = 0x80;
constexpr uint8_t kWriteBurst = 0x40;
constexpr uint8_t kReadBurst = 0xC0;

constexpr uint8_t kRegIocfg2 = 0x00;
constexpr uint8_t kRegIocfg1 = 0x01;
constexpr uint8_t kRegIocfg0 = 0x02;
constexpr uint8_t kRegFifothr = 0x03;
constexpr uint8_t kRegPktctrl1 = 0x07;
constexpr uint8_t kRegPktctrl0 = 0x08;
constexpr uint8_t kRegChannr = 0x0A;
constexpr uint8_t kRegFsctrl1 = 0x0B;
constexpr uint8_t kRegFsctrl0 = 0x0C;
constexpr uint8_t kRegFreq2 = 0x0D;
constexpr uint8_t kRegFreq1 = 0x0E;
constexpr uint8_t kRegFreq0 = 0x0F;
constexpr uint8_t kRegMdmcfg4 = 0x10;
constexpr uint8_t kRegMdmcfg3 = 0x11;
constexpr uint8_t kRegMdmcfg2 = 0x12;
constexpr uint8_t kRegMdmcfg1 = 0x13;
constexpr uint8_t kRegMdmcfg0 = 0x14;
constexpr uint8_t kRegDeviatn = 0x15;
constexpr uint8_t kRegMcsm2 = 0x16;
constexpr uint8_t kRegMcsm1 = 0x17;
constexpr uint8_t kRegMcsm0 = 0x18;
constexpr uint8_t kRegFoccfg = 0x19;
constexpr uint8_t kRegBscfg = 0x1A;
constexpr uint8_t kRegAgcctrl2 = 0x1B;
constexpr uint8_t kRegAgcctrl1 = 0x1C;
constexpr uint8_t kRegAgcctrl0 = 0x1D;
constexpr uint8_t kRegWorctrl = 0x20;
constexpr uint8_t kRegFrend1 = 0x21;
constexpr uint8_t kRegFrend0 = 0x22;
constexpr uint8_t kRegFscal3 = 0x23;
constexpr uint8_t kRegFscal2 = 0x24;
constexpr uint8_t kRegFscal1 = 0x25;
constexpr uint8_t kRegFscal0 = 0x26;
constexpr uint8_t kRegTest2 = 0x2C;
constexpr uint8_t kRegTest1 = 0x2D;
constexpr uint8_t kRegTest0 = 0x2E;
constexpr uint8_t kRegPartnum = 0x30;
constexpr uint8_t kRegVersion = 0x31;
constexpr uint8_t kRegRssi = 0x34;
constexpr uint8_t kRegMarcstate = 0x35;
constexpr uint8_t kRegPktstatus = 0x38;
constexpr uint8_t kRegPatable = 0x3E;

constexpr uint8_t kStrobeSres = 0x30;
constexpr uint8_t kStrobeScal = 0x33;
constexpr uint8_t kStrobeSrx = 0x34;
constexpr uint8_t kStrobeStx = 0x35;
constexpr uint8_t kStrobeSidle = 0x36;
constexpr uint8_t kStrobeSfrx = 0x3A;
constexpr uint8_t kStrobeSftx = 0x3B;

struct RegisterSetting {
    uint8_t address;
    uint8_t value;
};

bool pin_is_reserved(int pin)
{
    return (pin >= 6 && pin <= 12) || pin == 0 || pin == 1 || pin == 2 || pin == 3 || pin == 5 || pin == 15 ||
           pin == 16 || pin == 17;
}

bool pins_are_valid(const Cc1101Config &config)
{
    const int pins[] = {config.sclk_gpio, config.miso_gpio, config.mosi_gpio,
                        config.cs_gpio, config.gdo0_gpio, config.gdo2_gpio};
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config.sclk_gpio) || !GPIO_IS_VALID_GPIO(config.miso_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.mosi_gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(config.cs_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.gdo0_gpio) || !GPIO_IS_VALID_GPIO(config.gdo2_gpio)) {
        return false;
    }
    for (std::size_t left = 0; left < std::size(pins); ++left) {
        if (pin_is_reserved(pins[left])) {
            return false;
        }
        for (std::size_t right = left + 1; right < std::size(pins); ++right) {
            if (pins[left] == pins[right]) {
                return false;
            }
        }
    }
    return config.crystal_hz >= 26000000 && config.crystal_hz <= 27000000 &&
           config.frequency_hz >= 387000000 && config.frequency_hz <= 464000000;
}

std::array<RegisterSetting, 36> make_profile(const Cc1101Config &config)
{
    const uint32_t frequency_word = cc1101_frequency_word(config.frequency_hz, config.crystal_hz);
    return {{
        {kRegIocfg2, 0x0D},
        {kRegIocfg1, 0x2E},
        {kRegIocfg0, 0x2E},
        {kRegFifothr, 0x47},
        {kRegPktctrl1, 0x00},
        {kRegPktctrl0, 0x30},
        {kRegChannr, 0x00},
        {kRegFsctrl1, 0x06},
        {kRegFsctrl0, 0x00},
        {kRegFreq2, static_cast<uint8_t>(frequency_word >> 16U)},
        {kRegFreq1, static_cast<uint8_t>(frequency_word >> 8U)},
        {kRegFreq0, static_cast<uint8_t>(frequency_word)},
        {kRegMdmcfg4, 0x87},
        {kRegMdmcfg3, 0x83},
        {kRegMdmcfg2, 0x30},
        {kRegMdmcfg1, 0x22},
        {kRegMdmcfg0, 0xF8},
        {kRegDeviatn, 0x00},
        {kRegMcsm2, 0x07},
        {kRegMcsm1, 0x00},
        {kRegMcsm0, 0x18},
        {kRegFoccfg, 0x14},
        {kRegBscfg, 0x6C},
        {kRegAgcctrl2, 0x04},
        {kRegAgcctrl1, 0x00},
        {kRegAgcctrl0, 0x92},
        {kRegWorctrl, 0xFB},
        {kRegFrend1, 0xB6},
        {kRegFrend0, 0x11},
        {kRegFscal3, 0xE9},
        {kRegFscal2, 0x2A},
        {kRegFscal1, 0x00},
        {kRegFscal0, 0x1F},
        {kRegTest2, 0x81},
        {kRegTest1, 0x35},
        {kRegTest0, 0x09},
    }};
}

bool calibration_register(uint8_t address)
{
    return address >= kRegFscal3 && address <= kRegFscal0;
}

}  // namespace

uint32_t cc1101_frequency_word(uint32_t frequency_hz, uint32_t crystal_hz)
{
    if (crystal_hz == 0) {
        return 0;
    }
    const uint64_t scaled = (static_cast<uint64_t>(frequency_hz) << 16U) + crystal_hz / 2U;
    return static_cast<uint32_t>(scaled / crystal_hz);
}

uint8_t cc1101_pa_table_value(int8_t requested_dbm)
{
    struct PowerSetting {
        int8_t dbm;
        uint8_t value;
    };
    constexpr PowerSetting settings[] = {
        {-30, 0x12}, {-20, 0x0E}, {-15, 0x1D}, {-10, 0x34}, {-6, 0x2A},
        {0, 0x60},   {5, 0x84},   {7, 0xC8},   {10, 0xC0},
    };
    for (std::size_t index = std::size(settings); index > 0; --index) {
        if (requested_dbm >= settings[index - 1U].dbm) {
            return settings[index - 1U].value;
        }
    }
    return settings[0].value;
}

esp_err_t Cc1101::initialize(const Cc1101Config &config)
{
    if (spi_device_ != nullptr || bus_initialized_ || !pins_are_valid(config)) {
        return spi_device_ != nullptr || bus_initialized_ ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    config_ = config;

    gpio_config_t cs_config{};
    cs_config.pin_bit_mask = 1ULL << config_.cs_gpio;
    cs_config.mode = GPIO_MODE_OUTPUT;
    cs_config.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&cs_config), kTag, "configure CSN");
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(config_.cs_gpio), 1), kTag, "idle CSN");
    gpio_initialized_ = true;

    gpio_config_t gdo_config{};
    gdo_config.pin_bit_mask = (1ULL << config_.gdo0_gpio) | (1ULL << config_.gdo2_gpio);
    gdo_config.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&gdo_config), kTag, "configure GDO pins");

    gpio_config_t reset_lines{};
    reset_lines.pin_bit_mask = (1ULL << config_.sclk_gpio) | (1ULL << config_.mosi_gpio);
    reset_lines.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&reset_lines), kTag, "configure reset lines");
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(config_.sclk_gpio), 1), kTag, "reset SCLK high");
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(config_.mosi_gpio), 0), kTag, "reset MOSI low");
    gpio_set_level(static_cast<gpio_num_t>(config_.cs_gpio), 0);
    esp_rom_delay_us(10);
    gpio_set_level(static_cast<gpio_num_t>(config_.cs_gpio), 1);
    esp_rom_delay_us(45);

    spi_bus_config_t bus_config{};
    bus_config.mosi_io_num = config_.mosi_gpio;
    bus_config.miso_io_num = config_.miso_gpio;
    bus_config.sclk_io_num = config_.sclk_gpio;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz = 16;
    esp_err_t error = spi_bus_initialize(kSpiHost, &bus_config, SPI_DMA_DISABLED);
    if (error != ESP_OK) {
        return error;
    }
    bus_initialized_ = true;

    spi_device_interface_config_t device_config{};
    device_config.clock_speed_hz = kSpiClockHz;
    device_config.mode = 0;
    device_config.spics_io_num = -1;
    device_config.queue_size = 1;
    error = spi_bus_add_device(kSpiHost, &device_config, &spi_device_);
    if (error != ESP_OK) {
        deinitialize();
        return error;
    }
    error = reset_and_configure();
    if (error != ESP_OK) {
        deinitialize();
        return error;
    }
    initialized_ = true;
    ESP_LOGI(kTag, "CC1101 part=0x%02X version=0x%02X frequency=%lu Hz power=%d dBm", part_number_,
             version_, static_cast<unsigned long>(config_.frequency_hz), config_.tx_power_dbm);
    return ESP_OK;
}

esp_err_t Cc1101::deinitialize()
{
    esp_err_t cleanup_error = ESP_OK;
    const auto remember_error = [&](esp_err_t error, const char *operation) {
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "%s failed during shutdown: %s", operation, esp_err_to_name(error));
            if (cleanup_error == ESP_OK) {
                cleanup_error = error;
            }
        }
    };

    if (gpio_initialized_) {
        remember_error(gpio_set_direction(static_cast<gpio_num_t>(config_.gdo0_gpio), GPIO_MODE_OUTPUT),
                       "configure GDO0 low");
        remember_error(gpio_set_level(static_cast<gpio_num_t>(config_.gdo0_gpio), 0), "drive GDO0 low");
    }
    if (initialized_ && spi_device_ != nullptr && enter_idle(50) != ESP_OK) {
        ESP_LOGW(kTag, "Could not enter IDLE during shutdown; resetting radio");
        remember_error(manual_reset(), "reset radio");
    }
    initialized_ = false;
    if (selected_) {
        end_transaction();
    }
    if (spi_device_ != nullptr) {
        const esp_err_t remove_error = spi_bus_remove_device(spi_device_);
        if (remove_error == ESP_OK) {
            spi_device_ = nullptr;
        } else {
            remember_error(remove_error, "remove SPI device");
        }
    }
    if (bus_initialized_ && spi_device_ == nullptr) {
        const esp_err_t free_error = spi_bus_free(kSpiHost);
        if (free_error == ESP_OK) {
            bus_initialized_ = false;
        } else {
            remember_error(free_error, "free SPI bus");
        }
    }
    if (gpio_initialized_) {
        remember_error(gpio_set_level(static_cast<gpio_num_t>(config_.cs_gpio), 1), "idle CSN");
        if (spi_device_ == nullptr && !bus_initialized_) {
            gpio_initialized_ = false;
        }
    }
    return cleanup_error;
}

esp_err_t Cc1101::wait_chip_ready(uint32_t timeout_us)
{
    const int64_t deadline = esp_timer_get_time() + timeout_us;
    while (gpio_get_level(static_cast<gpio_num_t>(config_.miso_gpio)) != 0) {
        if (esp_timer_get_time() >= deadline) {
            ++ready_timeout_count_;
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(10);
    }
    return ESP_OK;
}

esp_err_t Cc1101::transfer_selected(const uint8_t *tx, uint8_t *rx, std::size_t count)
{
    if (!selected_ || spi_device_ == nullptr || tx == nullptr || count == 0 || count > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_transaction_t transaction{};
    transaction.length = count * CHAR_BIT;
    transaction.tx_buffer = tx;
    transaction.rx_buffer = rx;
    return spi_device_polling_transmit(spi_device_, &transaction);
}

esp_err_t Cc1101::begin_transaction()
{
    if (selected_ || spi_device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(spi_device_, portMAX_DELAY), kTag, "acquire SPI");
    const esp_err_t select_error = gpio_set_level(static_cast<gpio_num_t>(config_.cs_gpio), 0);
    if (select_error != ESP_OK) {
        spi_device_release_bus(spi_device_);
        return select_error;
    }
    selected_ = true;
    const esp_err_t ready_error = wait_chip_ready(kChipReadyTimeoutUs);
    if (ready_error != ESP_OK) {
        end_transaction();
    }
    return ready_error;
}

void Cc1101::end_transaction()
{
    if (!selected_) {
        return;
    }
    gpio_set_level(static_cast<gpio_num_t>(config_.cs_gpio), 1);
    selected_ = false;
    spi_device_release_bus(spi_device_);
}

esp_err_t Cc1101::manual_reset()
{
    if (spi_device_ == nullptr || selected_) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(spi_device_, portMAX_DELAY), kTag, "acquire reset SPI");
    gpio_set_level(static_cast<gpio_num_t>(config_.cs_gpio), 0);
    selected_ = true;

    esp_err_t error = wait_chip_ready(kChipReadyTimeoutUs);
    const uint8_t command = kStrobeSres;
    if (error == ESP_OK) {
        error = transfer_selected(&command, nullptr, 1);
    }
    if (error == ESP_OK) {
        error = wait_chip_ready(kChipReadyTimeoutUs);
    }
    end_transaction();
    if (error == ESP_OK) {
        esp_rom_delay_us(100);
    }
    return error;
}

esp_err_t Cc1101::write_register(uint8_t address, uint8_t value)
{
    if (address > kRegTest0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(begin_transaction(), kTag, "select register write");
    const uint8_t tx[] = {address, value};
    const esp_err_t error = transfer_selected(tx, nullptr, std::size(tx));
    end_transaction();
    return error;
}

esp_err_t Cc1101::write_burst(uint8_t address, const uint8_t *values, std::size_t count)
{
    if (values == nullptr || count == 0 || count > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    std::array<uint8_t, 9> tx{};
    tx[0] = static_cast<uint8_t>(address | kWriteBurst);
    for (std::size_t index = 0; index < count; ++index) {
        tx[index + 1U] = values[index];
    }
    ESP_RETURN_ON_ERROR(begin_transaction(), kTag, "select burst write");
    const esp_err_t error = transfer_selected(tx.data(), nullptr, count + 1U);
    end_transaction();
    return error;
}

esp_err_t Cc1101::read_burst(uint8_t address, uint8_t *values, std::size_t count)
{
    if (values == nullptr || count == 0 || count > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    std::array<uint8_t, 9> tx{};
    std::array<uint8_t, 9> rx{};
    tx[0] = static_cast<uint8_t>(address | kReadBurst);
    ESP_RETURN_ON_ERROR(begin_transaction(), kTag, "select burst read");
    const esp_err_t error = transfer_selected(tx.data(), rx.data(), count + 1U);
    end_transaction();
    if (error == ESP_OK) {
        for (std::size_t index = 0; index < count; ++index) {
            values[index] = rx[index + 1U];
        }
    }
    return error;
}

esp_err_t Cc1101::read_register(uint8_t address, uint8_t *value)
{
    if (value == nullptr || address > kRegTest0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t tx[] = {static_cast<uint8_t>(address | kReadSingle), 0};
    uint8_t rx[2]{};
    ESP_RETURN_ON_ERROR(begin_transaction(), kTag, "select register read");
    const esp_err_t error = transfer_selected(tx, rx, std::size(tx));
    end_transaction();
    if (error == ESP_OK) {
        *value = rx[1];
    }
    return error;
}

esp_err_t Cc1101::read_status_register(uint8_t address, uint8_t *value)
{
    if (value == nullptr || address < kRegPartnum || address > 0x3D) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t tx[] = {static_cast<uint8_t>(address | kReadBurst), 0};
    uint8_t rx[2]{};
    ESP_RETURN_ON_ERROR(begin_transaction(), kTag, "select status read");
    const esp_err_t error = transfer_selected(tx, rx, std::size(tx));
    end_transaction();
    if (error == ESP_OK) {
        *value = rx[1];
    }
    return error;
}

esp_err_t Cc1101::read_stable_status(uint8_t address, uint8_t *value)
{
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t previous = 0;
    ESP_RETURN_ON_ERROR(read_status_register(address, &previous), kTag, "initial status read");
    for (int attempt = 0; attempt < 7; ++attempt) {
        uint8_t current = 0;
        ESP_RETURN_ON_ERROR(read_status_register(address, &current), kTag, "repeated status read");
        if (current == previous) {
            *value = current;
            return ESP_OK;
        }
        previous = current;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t Cc1101::strobe(uint8_t command, uint8_t *status)
{
    if (command < 0x30 || command > 0x3D) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t received = 0;
    ESP_RETURN_ON_ERROR(begin_transaction(), kTag, "select strobe");
    const esp_err_t error = transfer_selected(&command, &received, 1);
    end_transaction();
    if (error == ESP_OK && status != nullptr) {
        *status = received;
    }
    return error;
}

esp_err_t Cc1101::apply_profile()
{
    const auto profile = make_profile(config_);
    for (const RegisterSetting &setting : profile) {
        ESP_RETURN_ON_ERROR(write_register(setting.address, setting.value), kTag, "write register 0x%02X",
                            setting.address);
    }
    const uint8_t pa_table[8] = {0x00, cc1101_pa_table_value(config_.tx_power_dbm), 0, 0, 0, 0, 0, 0};
    return write_burst(kRegPatable, pa_table, std::size(pa_table));
}

esp_err_t Cc1101::verify_profile()
{
    const auto profile = make_profile(config_);
    for (const RegisterSetting &setting : profile) {
        if (calibration_register(setting.address)) {
            continue;
        }
        uint8_t observed = 0;
        ESP_RETURN_ON_ERROR(read_register(setting.address, &observed), kTag, "read back 0x%02X",
                            setting.address);
        if (observed != setting.value) {
            ESP_LOGE(kTag, "Register 0x%02X readback 0x%02X != 0x%02X", setting.address, observed,
                     setting.value);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    uint8_t pa_table[2]{};
    ESP_RETURN_ON_ERROR(read_burst(kRegPatable, pa_table, std::size(pa_table)), kTag, "read back PATABLE");
    if (pa_table[0] != 0 || pa_table[1] != cc1101_pa_table_value(config_.tx_power_dbm)) {
        ESP_LOGE(kTag, "PATABLE readback 0x%02X 0x%02X is invalid", pa_table[0], pa_table[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t Cc1101::reset_and_configure()
{
    if (spi_device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    initialized_ = false;
    ++reset_count_;
    ESP_RETURN_ON_ERROR(manual_reset(), kTag, "reset radio");
    ESP_RETURN_ON_ERROR(write_register(kRegIocfg0, 0x2E), kTag, "disable reset clock output");
    ESP_RETURN_ON_ERROR(apply_profile(), kTag, "apply OOK profile");
    ESP_RETURN_ON_ERROR(read_status_register(kRegPartnum, &part_number_), kTag, "read PARTNUM");
    ESP_RETURN_ON_ERROR(read_status_register(kRegVersion, &version_), kTag, "read VERSION");
    ESP_RETURN_ON_ERROR(verify_profile(), kTag, "verify OOK profile");
    ESP_RETURN_ON_ERROR(strobe(kStrobeScal), kTag, "calibrate");
    ESP_RETURN_ON_ERROR(wait_for_state(kCc1101MarcStateIdle, 50), kTag, "wait for calibration");
    initialized_ = true;
    return ESP_OK;
}

esp_err_t Cc1101::wait_for_state(uint8_t expected, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    do {
        uint8_t state = 0;
        const esp_err_t error = read_stable_status(kRegMarcstate, &state);
        if (error == ESP_OK && (state & 0x1FU) == expected) {
            return ESP_OK;
        }
        if (error != ESP_OK && error != ESP_ERR_INVALID_RESPONSE) {
            return error;
        }
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    ++state_timeout_count_;
    return ESP_ERR_TIMEOUT;
}

esp_err_t Cc1101::enter_idle(uint32_t timeout_ms)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(strobe(kStrobeSidle), kTag, "SIDLE");
    esp_err_t error = wait_for_state(kCc1101MarcStateIdle, timeout_ms);
    if (error == ESP_OK) {
        return ESP_OK;
    }
    uint8_t state = 0;
    if (read_stable_status(kRegMarcstate, &state) != ESP_OK) {
        return error;
    }
    state &= 0x1FU;
    if (state == kCc1101MarcStateRxFifoOverflow) {
        ESP_RETURN_ON_ERROR(strobe(kStrobeSfrx), kTag, "flush RX overflow");
    } else if (state == kCc1101MarcStateTxFifoUnderflow) {
        ESP_RETURN_ON_ERROR(strobe(kStrobeSftx), kTag, "flush TX underflow");
    } else {
        return error;
    }
    return wait_for_state(kCc1101MarcStateIdle, timeout_ms);
}

esp_err_t Cc1101::enter_receive(uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(enter_idle(timeout_ms), kTag, "idle before RX");
    ESP_RETURN_ON_ERROR(strobe(kStrobeSfrx), kTag, "flush RX");
    ESP_RETURN_ON_ERROR(strobe(kStrobeSrx), kTag, "SRX");
    return wait_for_state(kCc1101MarcStateRx, timeout_ms);
}

esp_err_t Cc1101::enter_transmit(uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(enter_idle(timeout_ms), kTag, "idle before TX");
    ESP_RETURN_ON_ERROR(strobe(kStrobeSftx), kTag, "flush TX");
    ESP_RETURN_ON_ERROR(strobe(kStrobeStx), kTag, "STX");
    return wait_for_state(kCc1101MarcStateTx, timeout_ms);
}

esp_err_t Cc1101::recover_receive()
{
    ++recovery_count_;
    esp_err_t error = enter_receive(30);
    if (error == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(kTag, "Soft RX recovery failed (%s), resetting radio", esp_err_to_name(error));
    error = reset_and_configure();
    if (error != ESP_OK) {
        return error;
    }
    return enter_receive(50);
}

esp_err_t Cc1101::read_info(Cc1101Info *info)
{
    if (!initialized_ || info == nullptr) {
        return info == nullptr ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    uint8_t marc_state = 0;
    uint8_t rssi = 0;
    uint8_t packet_status = 0;
    ESP_RETURN_ON_ERROR(read_stable_status(kRegMarcstate, &marc_state), kTag, "read MARCSTATE");
    ESP_RETURN_ON_ERROR(read_stable_status(kRegRssi, &rssi), kTag, "read RSSI");
    ESP_RETURN_ON_ERROR(read_status_register(kRegPktstatus, &packet_status), kTag, "read PKTSTATUS");

    const int16_t signed_rssi = static_cast<int8_t>(rssi);
    *info = {};
    info->part_number = part_number_;
    info->version = version_;
    info->marc_state = marc_state & 0x1FU;
    info->rssi_dbm_x2 = static_cast<int16_t>(signed_rssi - 148);
    info->carrier_sense = (packet_status & 0x40U) != 0;
    info->clear_channel = (packet_status & 0x10U) != 0;
    info->reset_count = reset_count_;
    info->recovery_count = recovery_count_;
    info->ready_timeout_count = ready_timeout_count_;
    info->state_timeout_count = state_timeout_count_;
    return ESP_OK;
}

bool Cc1101::initialized() const
{
    return initialized_;
}

const Cc1101Config &Cc1101::config() const
{
    return config_;
}

}  // namespace rfbridge
