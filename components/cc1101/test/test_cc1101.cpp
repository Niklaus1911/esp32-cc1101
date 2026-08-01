#include "cc1101.hpp"
#include "unity.h"

TEST_CASE("CC1101 frequency word matches 433.92 MHz profile", "[cc1101]")
{
    TEST_ASSERT_EQUAL_HEX32(0x10B071, rfbridge::cc1101_frequency_word(433920000, 26000000));
    TEST_ASSERT_EQUAL_UINT32(0, rfbridge::cc1101_frequency_word(433920000, 0));
}

TEST_CASE("CC1101 PA selection never exceeds requested ceiling", "[cc1101]")
{
    TEST_ASSERT_EQUAL_HEX8(0x12, rfbridge::cc1101_pa_table_value(-30));
    TEST_ASSERT_EQUAL_HEX8(0x12, rfbridge::cc1101_pa_table_value(-29));
    TEST_ASSERT_EQUAL_HEX8(0x2A, rfbridge::cc1101_pa_table_value(-6));
    TEST_ASSERT_EQUAL_HEX8(0x60, rfbridge::cc1101_pa_table_value(1));
    TEST_ASSERT_EQUAL_HEX8(0x84, rfbridge::cc1101_pa_table_value(5));
    TEST_ASSERT_EQUAL_HEX8(0xC0, rfbridge::cc1101_pa_table_value(10));
}

TEST_CASE("CC1101 rejects flash strap UART and PSRAM pins before initialization", "[cc1101]")
{
    constexpr int reserved_pins[] = {0, 1, 2, 3, 5, 6, 11, 12, 15, 16, 17};
    for (const int pin : reserved_pins) {
        rfbridge::Cc1101 radio;
        const rfbridge::Cc1101Config config{
            .sclk_gpio = pin,
            .miso_gpio = 19,
            .mosi_gpio = 23,
            .cs_gpio = 27,
            .gdo0_gpio = 26,
            .gdo2_gpio = 25,
            .crystal_hz = 26000000,
            .frequency_hz = 433920000,
            .tx_power_dbm = 5,
        };
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, radio.initialize(config));
        TEST_ASSERT_FALSE(radio.initialized());
    }
}
