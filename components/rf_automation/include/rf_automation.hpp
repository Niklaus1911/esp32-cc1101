#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "rf_automation_event.hpp"
#include "rf_ook.hpp"
#include "rf_storage_rule_format.hpp"

namespace rfbridge {

struct RfAutomationStatus {
    bool available = false;
    bool enabled = false;
    bool enabled_known = false;
    bool rule_count_known = false;
    bool log_mode_known = false;
    bool runtime_paused = false;
    RfAutomationLogMode log_mode = RfAutomationLogMode::kActions;
    uint16_t rule_count = 0;
    uint32_t frames_seen = 0;
    uint32_t stale_frames = 0;
    uint32_t ambiguous_frames = 0;
    uint32_t matches = 0;
    uint32_t actions_succeeded = 0;
    uint32_t cooldown_suppressed = 0;
    uint32_t queue_drops = 0;
    uint32_t tx_errors = 0;
    uint32_t log_events = 0;
    uint32_t log_drops = 0;
    uint32_t configuration_revision = 0;
    esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
    esp_err_t last_error = ESP_OK;
    char last_trigger[kRfStorageNameCapacity]{};
    char last_target[kRfStorageNameCapacity]{};
};

struct RfAutomationRuleInfo {
    RfStorageRuleEntry entry{};
    esp_err_t validation_error = ESP_OK;
};

enum class RfAutomationPauseReason : uint8_t {
    kOtaMaintenance = 1U << 0U,
    kHardwareSwitch = 1U << 1U,
};

esp_err_t initialize_rf_automation();
esp_err_t rf_automation_add_rule(const char *trigger_name, const char *target_name, uint8_t repeats);
esp_err_t rf_automation_remove_rule(const char *trigger_name);
esp_err_t rf_automation_list_rules(RfStorageRuleEntry *rules, std::size_t capacity, std::size_t *count);
esp_err_t rf_automation_list_rule_info(RfAutomationRuleInfo *rules, std::size_t capacity, std::size_t *count);
esp_err_t rf_automation_set_enabled(bool enabled);
esp_err_t rf_automation_set_log_mode(RfAutomationLogMode mode);
esp_err_t rf_automation_set_runtime_paused(RfAutomationPauseReason reason, bool paused);
esp_err_t rf_automation_set_event_sink(RfAutomationEventSink sink, void *context);
esp_err_t rf_automation_get_status(RfAutomationStatus *status);
void rf_automation_on_frame(const RfFrame &frame);

}  // namespace rfbridge
