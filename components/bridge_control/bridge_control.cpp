#include "bridge_control.hpp"

#include <cstring>

#include "rf_storage.hpp"

namespace rfbridge {
namespace {

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
    if (request.bits < 4 || request.bits > 64 || request.protocol < 1 ||
        request.protocol > kRfProtocolCount || request.repeats < 1 || request.repeats > 20) {
        return false;
    }
    if (request.bits < 64 && (request.code >> request.bits) != 0) {
        return false;
    }
    const RfProtocol *protocol = rf_protocol(request.protocol);
    if (protocol == nullptr) {
        return false;
    }
    const uint8_t minimum_factor = rf_protocol_min_factor(request.protocol);
    const uint16_t minimum_unit =
        static_cast<uint16_t>((kMinimumRawPulseUs + minimum_factor - 1U) / minimum_factor);
    const uint16_t maximum_unit = static_cast<uint16_t>(
        kMaximumPulseDurationUs / rf_protocol_max_factor(request.protocol));
    const uint16_t pulse_us = request.pulse_us == 0 ? protocol->pulse_us : request.pulse_us;
    return pulse_us >= minimum_unit && pulse_us <= maximum_unit;
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

esp_err_t bridge_control_reset_radio()
{
    return reset_rf_radio();
}

esp_err_t bridge_control_transmit_decoded(const DecodedTransmitRequest &request,
                                          BridgeEventSource source, uint32_t operation_id)
{
    if (!bridge_control_decoded_request_is_valid(request)) {
        return ESP_ERR_INVALID_ARG;
    }
    const RfProtocol *protocol = rf_protocol(request.protocol);
    DecodedSignal decoded{};
    decoded.code = request.code;
    decoded.bits = request.bits;
    decoded.protocol = request.protocol;
    decoded.pulse_us = request.pulse_us == 0 ? protocol->pulse_us : request.pulse_us;
    decoded.inverted = protocol->inverted;
    RfStoredSignal stored{};
    stored.encoding = RfStoredEncoding::kDecoded;
    stored.decoded = decoded;
    return transmit_stored(stored, request.repeats, source, operation_id, nullptr);
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

esp_err_t bridge_control_forget_signal(const char *name)
{
    return rf_signals_forget(name);
}

}  // namespace rfbridge
