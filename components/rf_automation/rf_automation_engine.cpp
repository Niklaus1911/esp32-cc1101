#include "rf_automation_engine.hpp"

#include <cstring>

namespace rfbridge {

bool rf_automation_log_mode_is_valid(RfAutomationLogMode mode)
{
    return mode == RfAutomationLogMode::kOff || mode == RfAutomationLogMode::kActions ||
           mode == RfAutomationLogMode::kVerbose;
}

bool rf_automation_log_mode_allows(RfAutomationLogMode mode, RfAutomationEventType type)
{
    if (!rf_automation_log_mode_is_valid(mode) || mode == RfAutomationLogMode::kOff) {
        return false;
    }
    if (mode == RfAutomationLogMode::kVerbose) {
        return true;
    }
    return type == RfAutomationEventType::kTriggered || type == RfAutomationEventType::kActionCompleted;
}

const char *rf_automation_log_mode_name(RfAutomationLogMode mode)
{
    switch (mode) {
        case RfAutomationLogMode::kOff:
            return "off";
        case RfAutomationLogMode::kActions:
            return "actions";
        case RfAutomationLogMode::kVerbose:
            return "verbose";
    }
    return "invalid";
}

bool rf_stored_signals_equivalent(const RfStoredSignal &left, const RfStoredSignal &right)
{
    if (left.encoding == RfStoredEncoding::kDecoded && right.encoding == RfStoredEncoding::kDecoded) {
        return decoded_signals_match(left.decoded, right.decoded);
    }
    if (left.encoding == RfStoredEncoding::kRaw && right.encoding == RfStoredEncoding::kRaw) {
        return raw_signals_match(left.raw, right.raw);
    }
    if (left.encoding == RfStoredEncoding::kDecoded && right.encoding == RfStoredEncoding::kRaw) {
        return raw_signal_matches_decoded(right.raw, left.decoded);
    }
    if (left.encoding == RfStoredEncoding::kRaw && right.encoding == RfStoredEncoding::kDecoded) {
        return raw_signal_matches_decoded(left.raw, right.decoded);
    }
    return false;
}

RfAutomationMatchResult rf_find_unique_stored_signal_match(const RfStoredSignal &incoming,
                                                           const RfStoredSignal *const *triggers, std::size_t count,
                                                           std::size_t *matched_index,
                                                           std::size_t *match_count)
{
    if ((triggers == nullptr && count != 0) || matched_index == nullptr) {
        return RfAutomationMatchResult::kAmbiguous;
    }
    std::size_t matches = 0;
    std::size_t selected = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (triggers[index] == nullptr) {
            return RfAutomationMatchResult::kAmbiguous;
        }
        if (rf_stored_signals_equivalent(incoming, *triggers[index])) {
            selected = index;
            ++matches;
        }
    }
    if (match_count != nullptr) {
        *match_count = matches;
    }
    if (matches == 0) {
        return RfAutomationMatchResult::kNone;
    }
    if (matches > 1) {
        return RfAutomationMatchResult::kAmbiguous;
    }
    *matched_index = selected;
    return RfAutomationMatchResult::kUnique;
}

bool rf_automation_event_is_current(uint32_t event_generation, uint32_t current_generation, int64_t captured_us,
                                    int64_t generation_changed_us)
{
    return event_generation == current_generation && (captured_us <= 0 || captured_us > generation_changed_us);
}

bool rf_rule_would_create_cycle(const RfStorageRuleEntry *rules, std::size_t count, const char *trigger_name,
                                const char *target_name)
{
    if (rules == nullptr || trigger_name == nullptr || target_name == nullptr) {
        return true;
    }
    const char *current = target_name;
    for (std::size_t step = 0; step <= count; ++step) {
        if (std::strcmp(current, trigger_name) == 0) {
            return true;
        }
        const RfStorageRuleEntry *next = nullptr;
        for (std::size_t index = 0; index < count; ++index) {
            if (std::strcmp(rules[index].trigger_name, current) == 0) {
                next = &rules[index];
                break;
            }
        }
        if (next == nullptr) {
            return false;
        }
        current = next->rule.target_name;
    }
    return true;
}

bool rf_rule_graph_has_cycle(const RfStorageRuleEntry *rules, std::size_t count)
{
    if (rules == nullptr && count != 0) {
        return true;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const char *start = rules[index].trigger_name;
        const char *current = rules[index].rule.target_name;
        for (std::size_t step = 0; step <= count; ++step) {
            if (std::strcmp(current, start) == 0) {
                return true;
            }
            const RfStorageRuleEntry *next = nullptr;
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                if (std::strcmp(rules[candidate].trigger_name, current) == 0) {
                    next = &rules[candidate];
                    break;
                }
            }
            if (next == nullptr) {
                break;
            }
            current = next->rule.target_name;
        }
    }
    return false;
}

bool rf_automation_cooldown_allows(int64_t last_fired_us, int64_t event_us, uint32_t cooldown_ms)
{
    if (event_us <= 0 || cooldown_ms == 0) {
        return false;
    }
    if (last_fired_us <= 0) {
        return true;
    }
    if (event_us <= last_fired_us) {
        return false;
    }
    return event_us - last_fired_us >= static_cast<int64_t>(cooldown_ms) * 1000;
}


uint32_t rf_automation_cooldown_remaining_ms(int64_t last_fired_us, int64_t event_us,
                                             uint32_t cooldown_ms)
{
    if (last_fired_us <= 0 || event_us <= 0 || cooldown_ms == 0) {
        return 0;
    }
    if (event_us <= last_fired_us) {
        return cooldown_ms;
    }
    const int64_t cooldown_us = static_cast<int64_t>(cooldown_ms) * 1000;
    const int64_t elapsed_us = event_us - last_fired_us;
    if (elapsed_us >= cooldown_us) {
        return 0;
    }
    return static_cast<uint32_t>((cooldown_us - elapsed_us + 999) / 1000);
}

} // namespace rfbridge
