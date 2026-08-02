#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_storage_format.hpp"

namespace rfbridge {

constexpr uint32_t kRfAutomationCooldownMs = 1000;
constexpr std::size_t kRfStorageMaxRuleRecordSize = 33;

struct RfStoredRule {
    char target_name[kRfStorageNameCapacity]{};
    uint8_t repeats = 0;
    uint32_t cooldown_ms = kRfAutomationCooldownMs;
};

struct RfStorageRuleEntry {
    char trigger_name[kRfStorageNameCapacity]{};
    RfStoredRule rule{};
};

enum class RfRuleFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidRecord,
    kInvalidCrc,
    kBufferTooSmall,
};

bool rf_stored_rule_is_valid(const RfStoredRule &rule);
RfRuleFormatResult encode_rf_rule_record(const RfStoredRule &rule, uint8_t *output, std::size_t capacity,
                                         std::size_t *output_size);
RfRuleFormatResult decode_rf_rule_record(const uint8_t *record, std::size_t size, RfStoredRule *rule);

} // namespace rfbridge
