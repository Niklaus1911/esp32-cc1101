#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

enum class BoardProfile : uint8_t {
    kEsp32Devkit = 1,
    kEsp32s3DevkitcN16r8 = 2,
    kXiaoEsp32s3 = 3,
    kEsp32s3SuperminiFh4r2 = 4,
};

enum class BoardTarget : uint8_t {
    kEsp32 = 1,
    kEsp32s3 = 2,
};

enum class ConsoleTransport : uint8_t {
    kUart0,
    kUsbSerialJtag,
};

enum class PartitionLayout : uint8_t {
    kFlash4Mb = 1,
    kFlash8Mb = 2,
    kFlash16Mb = 3,
};

struct BoardGpioMap {
    int sclk = -1;
    int miso = -1;
    int mosi = -1;
    int cs = -1;
    int gdo0 = -1;
    int gdo2 = -1;
    int generic_tx = -1;
    int generic_rx = -1;
};

struct BoardInfo {
    BoardProfile profile = BoardProfile::kEsp32Devkit;
    BoardTarget target = BoardTarget::kEsp32;
    ConsoleTransport console = ConsoleTransport::kUart0;
    PartitionLayout partition_layout = PartitionLayout::kFlash4Mb;
    const char *profile_name = nullptr;
    const char *model_name = nullptr;
    const char *target_name = nullptr;
    uint8_t flash_mib = 0;
    uint8_t psram_mib = 0;
    bool combined_services = false;
    BoardGpioMap cc1101{};
    bool activity_led_enabled = false;
    int activity_led_gpio = -1;
    bool activity_led_active_high = true;
};

const BoardInfo *board_info(BoardProfile profile);
bool board_profile_supports_combined_services(BoardProfile profile);
bool board_cc1101_gpio_map_is_valid(BoardProfile profile, const BoardGpioMap &gpios);
bool board_generic_gpio_map_is_valid(BoardProfile profile, const BoardGpioMap &cc1101,
                                     int generic_tx, int generic_rx, int activity_led_gpio = -1);
bool board_activity_led_gpio_is_valid(BoardProfile profile, int gpio,
                                      const int *unavailable_gpios,
                                      std::size_t unavailable_gpio_count);
const char *board_profile_name(BoardProfile profile);
const char *console_transport_name(ConsoleTransport transport);

}  // namespace rfbridge
