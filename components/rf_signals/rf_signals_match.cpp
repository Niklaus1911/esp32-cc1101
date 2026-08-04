#include "rf_signals_match.hpp"

#include <cstring>

#include "rf_automation_engine.hpp"

namespace rfbridge {

RfLearnFrameDisposition classify_learn_frame_window(int64_t armed_us, int64_t deadline_us,
                                                   int64_t frame_event_us,
                                                   int64_t frame_capture_start_us)
{
    if (armed_us < 0 || deadline_us <= armed_us || frame_event_us < armed_us ||
        frame_capture_start_us < armed_us) {
        return RfLearnFrameDisposition::kIgnore;
    }
    return frame_event_us < deadline_us ? RfLearnFrameDisposition::kCapture
                                        : RfLearnFrameDisposition::kTimeout;
}

LearnedMatch find_learned_signal_match(const RfStoredSignal &incoming,
                                       const LearnedSignalEntry *entries, std::size_t count,
                                       bool catalog_available)
{
    LearnedMatch result{};
    if (!catalog_available || (entries == nullptr && count != 0)) {
        result.kind = LearnedMatchKind::kUnavailable;
        return result;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!rf_stored_signals_equivalent(incoming, entries[index].signal)) {
            continue;
        }
        if (result.count == 0) {
            std::memcpy(result.name, entries[index].name.value, sizeof(result.name));
        }
        ++result.count;
    }
    if (result.count == 0) {
        result.kind = LearnedMatchKind::kNone;
    } else if (result.count == 1) {
        result.kind = LearnedMatchKind::kUnique;
    } else {
        result.kind = LearnedMatchKind::kAmbiguous;
        result.name[0] = '\0';
    }
    return result;
}

}  // namespace rfbridge
