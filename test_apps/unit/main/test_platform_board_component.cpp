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
