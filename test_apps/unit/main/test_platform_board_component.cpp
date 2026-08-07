#include "platform_board.hpp"

#include "sdkconfig.h"
#include "unity.h"

using namespace rfbridge;

TEST_CASE("running board metadata reflects compiled GPIO configuration", "[platform_board]")
{
    const BoardInfo &board = current_board_info();
    TEST_ASSERT_EQUAL(CONFIG_CC1101_SPI_SCLK_GPIO, board.cc1101.sclk);
    TEST_ASSERT_EQUAL(CONFIG_CC1101_SPI_MISO_GPIO, board.cc1101.miso);
    TEST_ASSERT_EQUAL(CONFIG_CC1101_SPI_MOSI_GPIO, board.cc1101.mosi);
    TEST_ASSERT_EQUAL(CONFIG_CC1101_SPI_CS_GPIO, board.cc1101.cs);
    TEST_ASSERT_EQUAL(CONFIG_CC1101_GDO0_GPIO, board.cc1101.gdo0);
    TEST_ASSERT_EQUAL(CONFIG_CC1101_GDO2_GPIO, board.cc1101.gdo2);
    TEST_ASSERT_GREATER_OR_EQUAL(0, board.cc1101.generic_tx);
    TEST_ASSERT_GREATER_OR_EQUAL(0, board.cc1101.generic_rx);
    TEST_ASSERT_TRUE(board_generic_gpio_map_is_valid(
        board.profile, board.cc1101, board.cc1101.generic_tx, board.cc1101.generic_rx,
        board.activity_led_enabled ? board.activity_led_gpio : -1));
#if defined(CONFIG_RF_ACTIVITY_LED_ENABLE)
    TEST_ASSERT_TRUE(board.activity_led_enabled);
    TEST_ASSERT_EQUAL(CONFIG_RF_ACTIVITY_LED_GPIO, board.activity_led_gpio);
#if defined(CONFIG_RF_ACTIVITY_LED_ACTIVE_HIGH)
    TEST_ASSERT_TRUE(board.activity_led_active_high);
#else
    TEST_ASSERT_FALSE(board.activity_led_active_high);
#endif
#else
    TEST_ASSERT_FALSE(board.activity_led_enabled);
    TEST_ASSERT_EQUAL(-1, board.activity_led_gpio);
#endif
}

TEST_CASE("FH4R2 board policy is available to the Unity image", "[platform_board]")
{
    const BoardInfo *board = board_info(BoardProfile::kEsp32s3SuperminiFh4r2);
    TEST_ASSERT_NOT_NULL(board);
    TEST_ASSERT_EQUAL_UINT8(4, board->flash_mib);
    TEST_ASSERT_EQUAL_UINT8(2, board->psram_mib);
    TEST_ASSERT_TRUE(board->combined_services);
    TEST_ASSERT_EQUAL(ConsoleTransport::kUsbSerialJtag, board->console);
    TEST_ASSERT_TRUE(board_cc1101_gpio_map_is_valid(board->profile, board->cc1101));
    TEST_ASSERT_EQUAL(6, board->cc1101.generic_tx);
    TEST_ASSERT_EQUAL(7, board->cc1101.generic_rx);
    TEST_ASSERT_TRUE(board_generic_gpio_map_is_valid(
        board->profile, board->cc1101, board->cc1101.generic_tx, board->cc1101.generic_rx));
    TEST_ASSERT_FALSE(board_generic_gpio_map_is_valid(
        board->profile, board->cc1101, board->cc1101.gdo0, board->cc1101.generic_rx));
    TEST_ASSERT_FALSE(board_activity_led_gpio_is_valid(board->profile, 48, nullptr, 0));
}
