#pragma once

#include <cstdint>

#include "rf_storage_format.hpp"

namespace rfbridge {

enum class RfAutomationLogMode : uint8_t {
    kOff = 0,
    kActions = 1,
    kVerbose = 2,
};

enum class RfAutomationEventType : uint8_t {
    kTriggered,
    kActionCompleted,
    kCooldownSuppressed,
    kAmbiguousFrame,
    kStaleFrame,
    kQueueDrop,
};

struct RfAutomationEvent {
    RfAutomationEventType type = RfAutomationEventType::kTriggered;
    int64_t occurred_us = 0;
    char trigger_name[kRfStorageNameCapacity]{};
    char target_name[kRfStorageNameCapacity]{};
    RfStoredEncoding received_encoding = RfStoredEncoding::kDecoded;
    RfStoredEncoding target_encoding = RfStoredEncoding::kDecoded;
    uint8_t repeats = 0;
    int32_t result = 0;
    uint32_t action_id = 0;
    uint32_t elapsed_ms = 0;
    uint32_t value = 0;
};

// Runs on the rf_auto task while automation state is locked. A sink must use only zero-wait bounded
// operations, must not perform I/O or call automation APIs, and must not retain references after return.
using RfAutomationEventSink = bool (*)(const RfAutomationEvent &event, void *context);

bool rf_automation_log_mode_is_valid(RfAutomationLogMode mode);
bool rf_automation_log_mode_allows(RfAutomationLogMode mode, RfAutomationEventType type);
const char *rf_automation_log_mode_name(RfAutomationLogMode mode);

}  // namespace rfbridge
