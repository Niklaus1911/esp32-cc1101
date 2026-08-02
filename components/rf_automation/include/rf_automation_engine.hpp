#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_storage_format.hpp"
#include "rf_storage_rule_format.hpp"

namespace rfbridge {

enum class RfAutomationMatchResult : uint8_t {
    kNone,
    kUnique,
    kAmbiguous,
};

bool rf_stored_signals_equivalent(const RfStoredSignal &left, const RfStoredSignal &right);
RfAutomationMatchResult rf_find_unique_stored_signal_match(const RfStoredSignal &incoming,
                                                           const RfStoredSignal *const *triggers, std::size_t count,
                                                           std::size_t *matched_index);
bool rf_automation_event_is_current(uint32_t event_generation, uint32_t current_generation, int64_t captured_us,
                                    int64_t generation_changed_us);
bool rf_rule_would_create_cycle(const RfStorageRuleEntry *rules, std::size_t count, const char *trigger_name,
                                const char *target_name);
bool rf_rule_graph_has_cycle(const RfStorageRuleEntry *rules, std::size_t count);
bool rf_automation_cooldown_allows(int64_t last_fired_us, int64_t event_us, uint32_t cooldown_ms);

} // namespace rfbridge
