#include "bridge_control.hpp"

#include <atomic>
#include <cstring>

#include "esp_random.h"
#include "rf_storage.hpp"

namespace rfbridge {
namespace {

std::atomic<bool> s_rf_hardware_switch_busy{false};

class HardwareSwitchGuard {
public:
    HardwareSwitchGuard()
    {
        bool expected = false;
        acquired_ = s_rf_hardware_switch_busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel);
    }

    ~HardwareSwitchGuard()
    {
        if (acquired_) {
            s_rf_hardware_switch_busy.store(false, std::memory_order_release);
        }
    }

    bool acquired() const
    {
        return acquired_;
    }

private:
    bool acquired_ = false;
};

void publish_tx_event(BridgeEventType type, BridgeEventSource source, uint32_t operation_id,
                      uint16_t repeats, const RfFrame *frame, const char *name,
                      esp_err_t result)
{
    BridgeEvent event{};
    event.type = type;
    event.source = source;
    event.operation_id = operation_id;
    event.repeats = repeats;
    event.result = result;
    if (frame != nullptr) {
        event.payload.rf.frame = *frame;
    }
    if (name != nullptr) {
        std::strncpy(event.payload.rf.name, name, sizeof(event.payload.rf.name) - 1U);
    }
    bridge_events_publish(event);
}

esp_err_t transmit_stored(const RfStoredSignal &stored, uint16_t repeats,
                          BridgeEventSource source, uint32_t operation_id,
                          const char *name)
{
    RfFrame frame{};
    if (stored.encoding == RfStoredEncoding::kDecoded) {
        frame.encoding = RfEncoding::kDecoded;
        frame.decoded = stored.decoded;
    } else {
        frame.encoding = RfEncoding::kRaw;
        frame.raw = stored.raw;
    }
    publish_tx_event(BridgeEventType::kTxStarted, source, operation_id, repeats, &frame, name,
                     ESP_OK);
    const esp_err_t error = stored.encoding == RfStoredEncoding::kDecoded
                                ? transmit_rf_decoded(stored.decoded, repeats)
                                : transmit_rf_raw(stored.raw, repeats);
    publish_tx_event(BridgeEventType::kTxCompleted, source, operation_id, repeats, &frame, name,
                     error);
    return error;
}

}  // namespace

bool bridge_control_decoded_request_is_valid(const DecodedTransmitRequest &request)
{
    DecodedSignal decoded{};
    return request.repeats >= 1 && request.repeats <= 20 &&
           make_decoded_signal(request.code, request.bits, request.protocol, request.pulse_us,
                               &decoded);
}

bool bridge_control_raw_request_is_valid(const RawTransmitRequest &request)
{
    return request.repeats >= 1 && request.repeats <= 20 &&
           raw_signal_is_valid(request.signal);
}

esp_err_t bridge_control_get_status(BridgeStatusSnapshot *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    BridgeStatusSnapshot result{};
    result.radio_error = get_rf_radio_status(&result.radio);
    result.signals_error = get_rf_signals_status(&result.signals);
    result.automation_error = rf_automation_get_status(&result.automation);
    result.wifi_error = get_network_wifi_status(&result.wifi);
    result.ota_error = get_ota_update_status(&result.ota);
    result.events_error = bridge_events_get_status(&result.events);
    *status = result;
    return ESP_OK;
}

esp_err_t bridge_control_start_radio()
{
    return start_rf_ook(rf_signals_on_frame, nullptr);
}

esp_err_t bridge_control_set_rf_hardware(RfHardware hardware, BridgeEventSource source)
{
    HardwareSwitchGuard switch_guard;
    BridgeEvent event{};
    event.type = BridgeEventType::kHardwareSwitch;
    event.source = source;
    event.value = static_cast<uint32_t>(hardware);
    const esp_err_t pause_error = switch_guard.acquired()
                                      ? rf_automation_set_runtime_paused(
                                            RfAutomationPauseReason::kHardwareSwitch, true)
                                      : ESP_ERR_INVALID_STATE;
    const esp_err_t switch_error = pause_error == ESP_OK ? set_rf_hardware(hardware) : pause_error;
    const esp_err_t resume_error = pause_error == ESP_OK
                                       ? rf_automation_set_runtime_paused(
                                             RfAutomationPauseReason::kHardwareSwitch, false)
                                       : ESP_OK;
    event.result = switch_error != ESP_OK ? switch_error : resume_error;
    bridge_events_publish(event);
    return event.result;
}

esp_err_t bridge_control_reset_radio()
{
    return reset_rf_radio();
}

esp_err_t bridge_control_transmit_decoded(const DecodedTransmitRequest &request,
                                          BridgeEventSource source, uint32_t operation_id)
{
    DecodedSignal decoded{};
    if (request.repeats < 1 || request.repeats > 20 ||
        !make_decoded_signal(request.code, request.bits, request.protocol, request.pulse_us,
                             &decoded)) {
        return ESP_ERR_INVALID_ARG;
    }
    RfStoredSignal stored{};
    stored.encoding = RfStoredEncoding::kDecoded;
    stored.decoded = decoded;
    return transmit_stored(stored, request.repeats, source, operation_id, nullptr);
}

esp_err_t bridge_control_save_decoded(const char *name,
                                      const DecodedSignalSaveRequest &request,
                                      BridgeEventSource source)
{
    DecodedSignal decoded{};
    if (!rf_storage_name_is_valid(name) ||
        !make_decoded_signal(request.code, request.bits, request.protocol, request.pulse_us,
                             &decoded)) {
        return ESP_ERR_INVALID_ARG;
    }
    return rf_signals_save_decoded(name, decoded, source);
}

esp_err_t bridge_control_generate_and_save_random_decoded(
    BridgeEventSource source, RandomSignalSaveResult *result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    for (std::size_t attempt = 0; attempt < kRandomSignalMaximumAttempts; ++attempt) {
        RandomSignalSaveResult candidate{};
        if (!make_random_signal_candidate(esp_random(), &candidate)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        RfFrame frame{};
        frame.encoding = RfEncoding::kDecoded;
        frame.decoded = candidate.decoded;
        const LearnedMatch match = rf_signals_match_frame(frame);
        if (match.kind == LearnedMatchKind::kUnavailable) {
            return ESP_ERR_INVALID_STATE;
        }
        if (match.kind != LearnedMatchKind::kNone) {
            continue;
        }

        const esp_err_t save_error =
            rf_signals_save_decoded(candidate.name.value, candidate.decoded, source);
        if (save_error == ESP_OK) {
            *result = candidate;
            return ESP_OK;
        }
        if (save_error == ESP_ERR_INVALID_STATE) {
            bool exists = false;
            const esp_err_t exists_error = rf_storage_exists(candidate.name.value, &exists);
            if (exists_error != ESP_OK) {
                return exists_error;
            }
            if (exists) {
                continue;
            }
        }
        return save_error;
    }
    return ESP_ERR_NOT_FINISHED;
}

esp_err_t bridge_control_transmit_raw(const RawTransmitRequest &request,
                                      BridgeEventSource source, uint32_t operation_id)
{
    if (!bridge_control_raw_request_is_valid(request)) {
        return ESP_ERR_INVALID_ARG;
    }
    RfStoredSignal stored{};
    stored.encoding = RfStoredEncoding::kRaw;
    stored.raw = request.signal;
    return transmit_stored(stored, request.repeats, source, operation_id, nullptr);
}

esp_err_t bridge_control_replay_last(uint16_t repeats, BridgeEventSource source,
                                     uint32_t operation_id)
{
    if (repeats < 1 || repeats > 20) {
        return ESP_ERR_INVALID_ARG;
    }
    RfFrame frame{};
    const esp_err_t load_error = get_last_rf_frame(&frame);
    if (load_error != ESP_OK) {
        return load_error;
    }
    publish_tx_event(BridgeEventType::kTxStarted, source, operation_id, repeats, &frame, nullptr,
                     ESP_OK);
    const esp_err_t error = replay_last_rf_frame(repeats);
    publish_tx_event(BridgeEventType::kTxCompleted, source, operation_id, repeats, &frame, nullptr,
                     error);
    return error;
}

esp_err_t bridge_control_replay_named(const char *name, uint16_t repeats,
                                      BridgeEventSource source, uint32_t operation_id)
{
    if (!rf_storage_name_is_valid(name) || repeats < 1 || repeats > 20) {
        return ESP_ERR_INVALID_ARG;
    }
    RfStoredSignal stored{};
    const esp_err_t error = rf_storage_load(name, &stored);
    return error == ESP_OK ? transmit_stored(stored, repeats, source, operation_id, name) : error;
}

esp_err_t bridge_control_replay_recent(uint64_t id, uint16_t repeats,
                                      BridgeEventSource source, uint32_t operation_id)
{
    if (id == 0 || repeats < 1 || repeats > 20) {
        return ESP_ERR_INVALID_ARG;
    }
    RfRecentSignal recent{};
    const esp_err_t error = rf_storage_recent_load(id, &recent);
    if (error != ESP_OK) {
        return error;
    }
    RfStoredSignal stored{};
    stored.encoding = RfStoredEncoding::kDecoded;
    stored.decoded = recent.decoded;
    return transmit_stored(stored, repeats, source, operation_id, nullptr);
}

esp_err_t bridge_control_save_recent(uint64_t id, const char *name,
                                    BridgeEventSource source)
{
    return rf_signals_save_recent(id, name, source);
}

esp_err_t bridge_control_clear_recent()
{
    return rf_signals_clear_recent();
}

esp_err_t bridge_control_forget_signal(const char *name)
{
    return rf_signals_forget(name);
}

}  // namespace rfbridge
