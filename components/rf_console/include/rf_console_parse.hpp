#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_automation_event.hpp"

namespace rfbridge {

enum class ReplayTarget : uint8_t {
    kRam,
    kNamed,
};

struct ReplayArguments {
    ReplayTarget target = ReplayTarget::kRam;
    uint16_t repeats = 0;
};

enum class RecentAction : uint8_t {
    kList,
    kReplay,
    kSave,
    kClear,
};

struct RecentArguments {
    RecentAction action = RecentAction::kList;
    uint64_t id = 0;
    uint16_t repeats = 0;
    const char *name = nullptr;
};

struct SaveSignalArguments {
    const char *name = nullptr;
    uint64_t code = 0;
    uint16_t pulse_us = 0;
    uint8_t bits = 0;
    uint8_t protocol = 0;
};

bool parse_unsigned_value(const char *text, uint64_t maximum, uint64_t *value);
bool parse_replay_arguments(int argc, const char *const *argv, uint16_t default_repeats,
                            ReplayArguments *arguments);
bool parse_recent_arguments(int argc, const char *const *argv, uint16_t default_repeats,
                            RecentArguments *arguments);
bool parse_save_signal_arguments(int argc, const char *const *argv,
                                 SaveSignalArguments *arguments);
bool parse_rule_add_arguments(int argc, const char *const *argv, uint8_t default_repeats, uint8_t *repeats);
bool parse_rule_log_mode(const char *text, RfAutomationLogMode *mode);
bool format_rf_automation_event(const RfAutomationEvent &event, const char *result_name, char *output,
                                std::size_t capacity);

}  // namespace rfbridge
