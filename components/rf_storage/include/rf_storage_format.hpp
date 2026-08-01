#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_codec.hpp"

namespace rfbridge {

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

bool rf_storage_name_is_valid(const char *name);
RfStorageFormatResult encode_rf_storage_record(const RfStoredSignal &signal, uint8_t *output,
                                               std::size_t capacity, std::size_t *output_size);
RfStorageFormatResult decode_rf_storage_record(const uint8_t *record, std::size_t size,
                                               RfStoredSignal *signal);

}  // namespace rfbridge
