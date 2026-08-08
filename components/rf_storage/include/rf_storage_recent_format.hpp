#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_codec.hpp"

namespace rfbridge {

constexpr std::size_t kRfRecentSignalCapacity = 5;
constexpr std::size_t kRfRecentMaxRecordSize = 125;

struct RfRecentSignal {
    uint64_t id = 0;
    DecodedSignal decoded{};
};

struct RfRecentHistory {
    uint64_t next_id = 1;
    std::size_t count = 0;
    RfRecentSignal entries[kRfRecentSignalCapacity]{};
};

enum class RfRecentFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidRecord,
    kInvalidCrc,
    kBufferTooSmall,
};

bool rf_recent_history_is_valid(const RfRecentHistory &history);
RfRecentFormatResult append_rf_recent_history(RfRecentHistory *history,
                                              const DecodedSignal &decoded,
                                              RfRecentSignal *appended = nullptr);
RfRecentFormatResult encode_rf_recent_record(const RfRecentHistory &history, uint8_t *output,
                                             std::size_t capacity, std::size_t *output_size);
RfRecentFormatResult decode_rf_recent_record(const uint8_t *record, std::size_t size,
                                             RfRecentHistory *history);

}  // namespace rfbridge
