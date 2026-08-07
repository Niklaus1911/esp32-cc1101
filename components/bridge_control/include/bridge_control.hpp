#pragma once

#include <cstdint>

#include "bridge_events.hpp"
#include "esp_err.h"
#include "network_wifi.hpp"
#include "ota_update.hpp"
#include "rf_automation.hpp"
#include "rf_codec.hpp"
#include "rf_ook.hpp"
#include "rf_signals.hpp"

namespace rfbridge {

struct BridgeStatusSnapshot {
    RfRadioStatus radio{};
    RfSignalsStatus signals{};
    RfAutomationStatus automation{};
    NetworkWifiStatus wifi{};
    OtaUpdateStatus ota{};
    BridgeEventBrokerStatus events{};
    esp_err_t radio_error = ESP_ERR_INVALID_STATE;
    esp_err_t signals_error = ESP_ERR_INVALID_STATE;
    esp_err_t automation_error = ESP_ERR_INVALID_STATE;
    esp_err_t wifi_error = ESP_ERR_INVALID_STATE;
    esp_err_t ota_error = ESP_ERR_INVALID_STATE;
    esp_err_t events_error = ESP_ERR_INVALID_STATE;
};

struct DecodedTransmitRequest {
    uint64_t code = 0;
    uint16_t pulse_us = 0;
    uint16_t repeats = 0;
    uint8_t bits = 0;
    uint8_t protocol = 0;
};

struct RawTransmitRequest {
    RawSignal signal{};
    uint16_t repeats = 0;
};

bool bridge_control_decoded_request_is_valid(const DecodedTransmitRequest &request);
bool bridge_control_raw_request_is_valid(const RawTransmitRequest &request);
esp_err_t bridge_control_get_status(BridgeStatusSnapshot *status);
esp_err_t bridge_control_start_radio();
esp_err_t bridge_control_set_rf_hardware(RfHardware hardware, BridgeEventSource source);
esp_err_t bridge_control_reset_radio();
esp_err_t bridge_control_transmit_decoded(const DecodedTransmitRequest &request,
                                          BridgeEventSource source,
                                          uint32_t operation_id = 0);
esp_err_t bridge_control_transmit_raw(const RawTransmitRequest &request,
                                      BridgeEventSource source,
                                      uint32_t operation_id = 0);
esp_err_t bridge_control_replay_last(uint16_t repeats, BridgeEventSource source,
                                     uint32_t operation_id = 0);
esp_err_t bridge_control_replay_named(const char *name, uint16_t repeats,
                                      BridgeEventSource source,
                                      uint32_t operation_id = 0);
esp_err_t bridge_control_forget_signal(const char *name);

}  // namespace rfbridge
