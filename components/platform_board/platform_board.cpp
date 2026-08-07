#include "platform_board.hpp"

#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

#if CONFIG_PLATFORM_BOARD_ESP32_DEVKIT
constexpr BoardProfile kConfiguredProfile = BoardProfile::kEsp32Devkit;
constexpr RfBoardImageDescriptor kConfiguredDescriptor = {
    .magic = {'R', 'F', 'B', 'D'},
    .version = kBoardImageDescriptorVersion,
    .size = sizeof(RfBoardImageDescriptor),
    .board_id = 1,
    .target_id = 1,
    .flash_mib = 4,
    .partition_layout_id = 1,
    .reserved = {},
};
#elif CONFIG_PLATFORM_BOARD_ESP32S3_DEVKITC_N16R8
constexpr BoardProfile kConfiguredProfile = BoardProfile::kEsp32s3DevkitcN16r8;
constexpr RfBoardImageDescriptor kConfiguredDescriptor = {
    .magic = {'R', 'F', 'B', 'D'},
    .version = kBoardImageDescriptorVersion,
    .size = sizeof(RfBoardImageDescriptor),
    .board_id = 2,
    .target_id = 2,
    .flash_mib = 16,
    .partition_layout_id = 3,
    .reserved = {},
};
#elif CONFIG_PLATFORM_BOARD_XIAO_ESP32S3
constexpr BoardProfile kConfiguredProfile = BoardProfile::kXiaoEsp32s3;
constexpr RfBoardImageDescriptor kConfiguredDescriptor = {
    .magic = {'R', 'F', 'B', 'D'},
    .version = kBoardImageDescriptorVersion,
    .size = sizeof(RfBoardImageDescriptor),
    .board_id = 3,
    .target_id = 2,
    .flash_mib = 8,
    .partition_layout_id = 2,
    .reserved = {},
};
#elif CONFIG_PLATFORM_BOARD_ESP32S3_SUPERMINI_FH4R2
constexpr BoardProfile kConfiguredProfile = BoardProfile::kEsp32s3SuperminiFh4r2;
constexpr RfBoardImageDescriptor kConfiguredDescriptor = {
    .magic = {'R', 'F', 'B', 'D'},
    .version = kBoardImageDescriptorVersion,
    .size = sizeof(RfBoardImageDescriptor),
    .board_id = 4,
    .target_id = 2,
    .flash_mib = 4,
    .partition_layout_id = 1,
    .reserved = {},
};
#else
#error "A supported RF bridge board profile must be selected"
#endif

extern "C" const RfBoardImageDescriptor s_rfbridge_board_image_descriptor
    __attribute__((section(".rodata_custom_desc"), used)) = kConfiguredDescriptor;

BoardHeapRegionSnapshot heap_snapshot(uint32_t capabilities)
{
    return {
        .total = static_cast<uint32_t>(heap_caps_get_total_size(capabilities)),
        .free = static_cast<uint32_t>(heap_caps_get_free_size(capabilities)),
        .minimum_free = static_cast<uint32_t>(heap_caps_get_minimum_free_size(capabilities)),
        .largest_free_block =
            static_cast<uint32_t>(heap_caps_get_largest_free_block(capabilities)),
    };
}

}  // namespace

BoardProfile configured_board_profile()
{
    return kConfiguredProfile;
}

const BoardInfo &current_board_info()
{
    static const BoardInfo configured = [] {
        BoardInfo value = *board_info(kConfiguredProfile);
        value.cc1101 = {
            .sclk = CONFIG_CC1101_SPI_SCLK_GPIO,
            .miso = CONFIG_CC1101_SPI_MISO_GPIO,
            .mosi = CONFIG_CC1101_SPI_MOSI_GPIO,
            .cs = CONFIG_CC1101_SPI_CS_GPIO,
            .gdo0 = CONFIG_CC1101_GDO0_GPIO,
            .gdo2 = CONFIG_CC1101_GDO2_GPIO,
        };
        const BoardInfo *profile = board_info(kConfiguredProfile);
        if (profile != nullptr) {
            value.cc1101.generic_tx = profile->cc1101.generic_tx;
            value.cc1101.generic_rx = profile->cc1101.generic_rx;
        }
#if defined(CONFIG_RF_ACTIVITY_LED_ENABLE)
        value.activity_led_enabled = true;
        value.activity_led_gpio = CONFIG_RF_ACTIVITY_LED_GPIO;
#if defined(CONFIG_RF_ACTIVITY_LED_ACTIVE_HIGH)
        value.activity_led_active_high = true;
#else
        value.activity_led_active_high = false;
#endif
#else
        value.activity_led_enabled = false;
        value.activity_led_gpio = -1;
#endif
        return value;
    }();
    return configured;
}

const RfBoardImageDescriptor &current_board_image_descriptor()
{
    return s_rfbridge_board_image_descriptor;
}

BoardMemorySnapshot board_memory_snapshot()
{
    return {
        .internal = heap_snapshot(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .psram = heap_snapshot(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
}

}  // namespace rfbridge
