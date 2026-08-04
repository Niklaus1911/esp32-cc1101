#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_learned_match.hpp"
#include "rf_storage_format.hpp"

namespace rfbridge {

enum class RfLearnFrameDisposition : uint8_t {
    kIgnore,
    kCapture,
    kTimeout,
};

struct LearnedSignalEntry {
    RfStorageName name{};
    RfStoredSignal signal{};
};

RfLearnFrameDisposition classify_learn_frame_window(int64_t armed_us, int64_t deadline_us,
                                                   int64_t frame_event_us,
                                                   int64_t frame_capture_start_us);
LearnedMatch find_learned_signal_match(const RfStoredSignal &incoming,
                                       const LearnedSignalEntry *entries, std::size_t count,
                                       bool catalog_available = true);

}  // namespace rfbridge
