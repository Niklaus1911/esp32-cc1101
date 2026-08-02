#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_automation_event.hpp"

namespace rfbridge {

enum class LearnFrameDisposition : uint8_t {
    kIgnore,
    kCapture,
    kTimeout,
};

enum class ReplayTarget : uint8_t {
    kRam,
    kNamed,
};

struct ReplayArguments {
    ReplayTarget target = ReplayTarget::kRam;
    uint16_t repeats = 0;
};

bool parse_unsigned_value(const char *text, uint64_t maximum, uint64_t *value);
bool parse_replay_arguments(int argc, const char *const *argv, uint16_t default_repeats,
                            ReplayArguments *arguments);
bool parse_rule_add_arguments(int argc, const char *const *argv, uint8_t default_repeats, uint8_t *repeats);
bool parse_rule_log_mode(const char *text, RfAutomationLogMode *mode);
bool format_rf_automation_event(const RfAutomationEvent &event, const char *result_name, char *output,
                                std::size_t capacity);
LearnFrameDisposition classify_learn_frame(int64_t armed_us, int64_t deadline_us, int64_t frame_event_us,
                                           int64_t frame_capture_start_us);

}  // namespace rfbridge
