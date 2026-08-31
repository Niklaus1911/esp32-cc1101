#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_codec.hpp"

namespace rfbridge {

enum class RfHardware : uint8_t {
    kCc1101 = 0,
    kGeneric = 1,
};

const char *rf_hardware_name(RfHardware hardware);
bool rf_hardware_from_name(const char *name, RfHardware *hardware);

constexpr std::size_t kRfStorageNameCapacity = 16;
constexpr std::size_t kRfStorageMaxRecordSize = 527;

enum class RfStoredEncoding : uint8_t {
    kDecoded,
    kRaw,
};

struct RfStoredSignal {
    RfStoredEncoding encoding = RfStoredEncoding::kDecoded;
    DecodedSignal decoded{};
    RawSignal raw{};
};

struct RfStorageName {
    char value[kRfStorageNameCapacity]{};
};

enum class RfStorageFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidRecord,
    kInvalidCrc,
    kBufferTooSmall,
};

enum class RfHardwareFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidRecord,
    kInvalidCrc,
    kBufferTooSmall,
};

enum class RfGenericGpioFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidRecord,
    kInvalidCrc,
    kBufferTooSmall,
};

bool rf_storage_name_is_valid(const char *name);
RfStorageFormatResult encode_rf_storage_record(const RfStoredSignal &signal, uint8_t *output,
                                               std::size_t capacity, std::size_t *output_size);
RfStorageFormatResult decode_rf_storage_record(const uint8_t *record, std::size_t size,
                                               RfStoredSignal *signal);
RfHardwareFormatResult encode_rf_hardware_record(RfHardware hardware, uint8_t *output,
                                                 std::size_t capacity, std::size_t *output_size);
RfHardwareFormatResult decode_rf_hardware_record(const uint8_t *record, std::size_t size,
                                                 RfHardware *hardware);
RfGenericGpioFormatResult encode_rf_generic_gpio_record(uint8_t profile_id, uint8_t tx_gpio,
                                                        uint8_t rx_gpio, uint8_t *output,
                                                        std::size_t capacity,
                                                        std::size_t *output_size);
RfGenericGpioFormatResult decode_rf_generic_gpio_record(const uint8_t *record, std::size_t size,
                                                        uint8_t *profile_id, uint8_t *tx_gpio,
                                                        uint8_t *rx_gpio);

}  // namespace rfbridge
