#include "platform_board.hpp"

#include <array>
#include <cstring>

namespace rfbridge {
namespace {

constexpr BoardInfo kBoards[] = {
    {
        .profile = BoardProfile::kEsp32Devkit,
        .target = BoardTarget::kEsp32,
        .console = ConsoleTransport::kUart0,
        .partition_layout = PartitionLayout::kFlash4Mb,
        .profile_name = "esp32-devkit",
        .model_name = "ESP32 DevKit RF Bridge",
        .target_name = "esp32",
        .flash_mib = 4,
        .psram_mib = 0,
        .combined_services = false,
        .cc1101 = {.sclk = 18, .miso = 19, .mosi = 23, .cs = 27, .gdo0 = 26, .gdo2 = 25,
                   .generic_tx = 32, .generic_rx = 33},
        .activity_led_enabled = true,
        .activity_led_gpio = 2,
        .activity_led_active_high = true,
    },
    {
        .profile = BoardProfile::kEsp32s3DevkitcN16r8,
        .target = BoardTarget::kEsp32s3,
        .console = ConsoleTransport::kUart0,
        .partition_layout = PartitionLayout::kFlash16Mb,
        .profile_name = "esp32s3-devkitc-n16r8",
        .model_name = "ESP32-S3 DevKitC N16R8 RF Bridge",
        .target_name = "esp32s3",
        .flash_mib = 16,
        .psram_mib = 8,
        .combined_services = true,
        .cc1101 = {.sclk = 12, .miso = 13, .mosi = 11, .cs = 10, .gdo0 = 4, .gdo2 = 5,
                   .generic_tx = 6, .generic_rx = 7},
        .activity_led_enabled = false,
        .activity_led_gpio = -1,
        .activity_led_active_high = true,
    },
    {
        .profile = BoardProfile::kXiaoEsp32s3,
        .target = BoardTarget::kEsp32s3,
        .console = ConsoleTransport::kUsbSerialJtag,
        .partition_layout = PartitionLayout::kFlash8Mb,
        .profile_name = "xiao-esp32s3",
        .model_name = "Seeed Studio XIAO ESP32-S3 RF Bridge",
        .target_name = "esp32s3",
        .flash_mib = 8,
        .psram_mib = 8,
        .combined_services = true,
        .cc1101 = {.sclk = 7, .miso = 8, .mosi = 9, .cs = 4, .gdo0 = 2, .gdo2 = 1,
                   .generic_tx = 5, .generic_rx = 6},
        .activity_led_enabled = true,
        .activity_led_gpio = 21,
        .activity_led_active_high = false,
    },
    {
        .profile = BoardProfile::kEsp32s3SuperminiFh4r2,
        .target = BoardTarget::kEsp32s3,
        .console = ConsoleTransport::kUsbSerialJtag,
        .partition_layout = PartitionLayout::kFlash4Mb,
        .profile_name = "esp32s3-supermini-fh4r2",
        .model_name = "ESP32-S3 SuperMini FH4R2 RF Bridge",
        .target_name = "esp32s3",
        .flash_mib = 4,
        .psram_mib = 2,
        .combined_services = true,
        .cc1101 = {.sclk = 12, .miso = 13, .mosi = 11, .cs = 10, .gdo0 = 4, .gdo2 = 5,
                   .generic_tx = 6, .generic_rx = 7},
        .activity_led_enabled = false,
        .activity_led_gpio = -1,
        .activity_led_active_high = true,
    },
};

bool classic_gpio_is_valid(int gpio, bool output)
{
    const bool exists = (gpio >= 0 && gpio <= 19) || (gpio >= 21 && gpio <= 23) ||
                        (gpio >= 25 && gpio <= 27) || (gpio >= 32 && gpio <= 39);
    return exists && (!output || gpio <= 33);
}

bool classic_gpio_is_reserved(int gpio)
{
    return gpio == 0 || gpio == 1 || gpio == 2 || gpio == 3 || gpio == 5 ||
           (gpio >= 6 && gpio <= 12) || (gpio >= 15 && gpio <= 17);
}

bool s3_gpio_is_valid(int gpio, bool output)
{
    const bool exists = (gpio >= 0 && gpio <= 21) || (gpio >= 26 && gpio <= 48);
    return exists && (!output || gpio != 46);
}

bool s3_common_gpio_is_reserved(int gpio)
{
    return gpio == 0 || gpio == 3 || gpio == 19 || gpio == 20 ||
           (gpio >= 26 && gpio <= 37) || gpio == 45 || gpio == 46;
}

bool xiao_gpio_is_exposed(int gpio)
{
    constexpr int kExposed[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 43, 44};
    for (const int exposed : kExposed) {
        if (gpio == exposed) {
            return true;
        }
    }
    return false;
}

bool supermini_gpio_is_exposed(int gpio)
{
    return (gpio >= 1 && gpio <= 13) || gpio == 43 || gpio == 44;
}

bool gpio_is_valid_for_role(BoardProfile profile, int gpio, bool output)
{
    if (profile == BoardProfile::kEsp32Devkit) {
        return classic_gpio_is_valid(gpio, output) && !classic_gpio_is_reserved(gpio);
    }
    if (!s3_gpio_is_valid(gpio, output) || s3_common_gpio_is_reserved(gpio)) {
        return false;
    }
    if (profile == BoardProfile::kEsp32s3DevkitcN16r8) {
        return gpio != 43 && gpio != 44 && gpio != 48;
    }
    if (profile == BoardProfile::kXiaoEsp32s3) {
        return xiao_gpio_is_exposed(gpio) && gpio != 21;
    }
    if (profile == BoardProfile::kEsp32s3SuperminiFh4r2) {
        return supermini_gpio_is_exposed(gpio) && gpio != 48;
    }
    return false;
}

}  // namespace

const BoardInfo *board_info(BoardProfile profile)
{
    for (const BoardInfo &board : kBoards) {
        if (board.profile == profile) {
            return &board;
        }
    }
    return nullptr;
}

bool board_profile_supports_combined_services(BoardProfile profile)
{
    const BoardInfo *board = board_info(profile);
    return board != nullptr && board->combined_services;
}

bool board_cc1101_gpio_map_is_valid(BoardProfile profile, const BoardGpioMap &gpios)
{
    const int pins[] = {gpios.sclk, gpios.miso, gpios.mosi, gpios.cs, gpios.gdo0, gpios.gdo2};
    constexpr bool kOutput[] = {true, false, true, true, true, false};
    for (std::size_t left = 0; left < std::size(pins); ++left) {
        if (!gpio_is_valid_for_role(profile, pins[left], kOutput[left])) {
            return false;
        }
        for (std::size_t right = left + 1; right < std::size(pins); ++right) {
            if (pins[left] == pins[right]) {
                return false;
            }
        }
    }
    return true;
}

bool board_generic_gpio_map_is_valid(BoardProfile profile, const BoardGpioMap &cc1101,
                                     int generic_tx, int generic_rx, int activity_led_gpio)
{
    if (!board_cc1101_gpio_map_is_valid(profile, cc1101) ||
        !gpio_is_valid_for_role(profile, generic_tx, true) ||
        !gpio_is_valid_for_role(profile, generic_rx, false) || generic_tx == generic_rx ||
        generic_tx == activity_led_gpio || generic_rx == activity_led_gpio) {
        return false;
    }
    const int cc_pins[] = {cc1101.sclk, cc1101.miso, cc1101.mosi, cc1101.cs,
                           cc1101.gdo0, cc1101.gdo2};
    for (const int pin : cc_pins) {
        if (pin == generic_tx || pin == generic_rx) {
            return false;
        }
    }
    return true;
}

bool board_activity_led_gpio_is_valid(BoardProfile profile, int gpio,
                                      const int *unavailable_gpios,
                                      std::size_t unavailable_gpio_count)
{
    if (unavailable_gpio_count != 0 && unavailable_gpios == nullptr) {
        return false;
    }
    const bool valid = profile == BoardProfile::kEsp32Devkit
                           ? classic_gpio_is_valid(gpio, true) &&
                                 !classic_gpio_is_reserved(gpio) && gpio != 2
                           : s3_gpio_is_valid(gpio, true) &&
                                 !s3_common_gpio_is_reserved(gpio) &&
                                 ((profile == BoardProfile::kEsp32s3DevkitcN16r8 &&
                                   gpio != 43 && gpio != 44 && gpio != 48) ||
                                  (profile == BoardProfile::kXiaoEsp32s3 &&
                                   (gpio == 21 || xiao_gpio_is_exposed(gpio))) ||
                                  (profile == BoardProfile::kEsp32s3SuperminiFh4r2 &&
                                   supermini_gpio_is_exposed(gpio) && gpio != 48));
    if (!valid && !(profile == BoardProfile::kEsp32Devkit && gpio == 2)) {
        return false;
    }
    for (std::size_t index = 0; index < unavailable_gpio_count; ++index) {
        if (gpio == unavailable_gpios[index]) {
            return false;
        }
    }
    return true;
}

const char *board_profile_name(BoardProfile profile)
{
    const BoardInfo *board = board_info(profile);
    return board == nullptr ? "invalid" : board->profile_name;
}

const char *console_transport_name(ConsoleTransport transport)
{
    return transport == ConsoleTransport::kUsbSerialJtag ? "usb-serial-jtag" : "uart0";
}

bool board_image_descriptor_is_valid(const RfBoardImageDescriptor &descriptor)
{
    if (std::memcmp(descriptor.magic, kBoardImageDescriptorMagic, 4) != 0 ||
        descriptor.version != kBoardImageDescriptorVersion ||
        descriptor.size != sizeof(RfBoardImageDescriptor)) {
        return false;
    }
    const BoardInfo *board = board_info(static_cast<BoardProfile>(descriptor.board_id));
    if (board == nullptr || descriptor.target_id != static_cast<uint8_t>(board->target) ||
        descriptor.flash_mib != board->flash_mib ||
        descriptor.partition_layout_id != static_cast<uint8_t>(board->partition_layout)) {
        return false;
    }
    for (const uint8_t value : descriptor.reserved) {
        if (value != 0) {
            return false;
        }
    }
    return true;
}

bool board_image_descriptor_is_compatible(const RfBoardImageDescriptor &candidate,
                                          const RfBoardImageDescriptor &running)
{
    return board_image_descriptor_is_valid(candidate) &&
           board_image_descriptor_is_valid(running) &&
           std::memcmp(&candidate, &running, sizeof(candidate)) == 0;
}

}  // namespace rfbridge
