#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_codec.hpp"

namespace rfbridge {

constexpr std::size_t kRfRecentSignalCapacity = 5;
constexpr std::size_t kRfRecentMaxRecordSize = 125;
// New receptions are buffered in RAM and written to NVS no later than this after the
// first unsaved one, bounding flash wear in busy RF environments.
constexpr int64_t kRfRecentWriteBackDelayUs = 30000000;

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
// Returns how long buffered history may stay unsaved: -1 when nothing is buffered and
// 0 when the write-back is due.
int64_t rf_recent_write_back_wait_us(bool dirty, int64_t dirty_since_us, int64_t now_us);

}  // namespace rfbridge
