#pragma once

#include <cstddef>
#include <cstdint>

#include "bridge_events.hpp"
#include "esp_err.h"
#include "rf_ook.hpp"
#include "rf_signals_match.hpp"
#include "rf_storage.hpp"

namespace rfbridge {

enum class RfLearningState : uint8_t {
    kIdle,
    kArmed,
    kCompleted,
    kCancelled,
    kTimedOut,
    kFailed,
};

struct RfSignalsStatus {
    bool available = false;
    bool catalog_available = false;
    bool learning_armed = false;
    uint16_t learned_count = 0;
    uint32_t queue_drops = 0;
    uint32_t catalog_errors = 0;
    uint32_t learning_revision = 0;
    RfLearningState learning_state = RfLearningState::kIdle;
    esp_err_t learning_result = ESP_OK;
    char pending_name[kRfStorageNameCapacity]{};
    char learning_name[kRfStorageNameCapacity]{};
    esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
};

esp_err_t initialize_rf_signals();
void rf_signals_on_frame(const RfFrame &frame, void *context);
esp_err_t rf_signals_arm_learning(const char *name, BridgeEventSource source,
                                  uint32_t operation_id = 0);
esp_err_t rf_signals_cancel_learning(BridgeEventSource source, uint32_t operation_id = 0);
esp_err_t rf_signals_forget(const char *name);
esp_err_t rf_signals_refresh_catalog();
LearnedMatch rf_signals_match_frame(const RfFrame &frame);
esp_err_t get_last_rf_frame_with_match(RfFrame *frame, LearnedMatch *match);
esp_err_t get_rf_signals_status(RfSignalsStatus *status);

}  // namespace rfbridge
