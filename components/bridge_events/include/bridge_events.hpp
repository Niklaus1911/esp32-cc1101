#pragma once

#include <cstdint>
#include <new>

#include "esp_err.h"
#include "network_wifi.hpp"
#include "ota_update.hpp"
#include "rf_automation_event.hpp"
#include "rf_learned_match.hpp"
#include "rf_ook.hpp"

namespace rfbridge {

enum class BridgeEventType : uint8_t {
    kRx,
    kLearnArmed,
    kLearnReplaced,
    kLearnCompleted,
    kLearnCancelled,
    kLearnTimeout,
    kLearnFailed,
    kTxStarted,
    kTxCompleted,
    kAutomation,
    kAutomationConfig,
    kNetwork,
    kOta,
    kOperationCompleted,
    kWebStarted,
    kWebStopped,
    kSignalCatalogChanged,
    kHardwareSwitch,
};

enum class BridgeEventSource : uint8_t {
    kSystem,
    kUart,
    kWeb,
    kAutomation,
    kMqtt,
};

struct BridgeRfEventPayload {
    char name[kRfStorageNameCapacity]{};
    char target[kRfStorageNameCapacity]{};
    RfFrame frame{};
    LearnedMatch learned{};
};

union BridgeEventPayload {
    BridgeRfEventPayload rf;
    RfAutomationEvent automation;
    RfAutomationConfigEvent automation_config;
    NetworkWifiEvent network;
    OtaUpdateEvent ota;

    BridgeEventPayload() : rf{} {}

    void set_automation(const RfAutomationEvent &value) {
        new (&automation) RfAutomationEvent(value);
    }

    void set_automation_config(const RfAutomationConfigEvent &value) {
        new (&automation_config) RfAutomationConfigEvent(value);
    }

    void set_network(const NetworkWifiEvent &value) {
        new (&network) NetworkWifiEvent(value);
    }

    void set_ota(const OtaUpdateEvent &value) {
        new (&ota) OtaUpdateEvent(value);
    }
};

struct BridgeEvent {
    uint64_t sequence = 0;
    int64_t occurred_us = 0;
    BridgeEventType type = BridgeEventType::kRx;
    BridgeEventSource source = BridgeEventSource::kSystem;
    esp_err_t result = ESP_OK;
    uint32_t operation_id = 0;
    uint32_t value = 0;
    uint32_t total = 0;
    uint16_t repeats = 0;
    BridgeEventPayload payload{};
};

using BridgeEventSink = bool (*)(const BridgeEvent &event, void *context);

struct BridgeEventBrokerStatus {
    bool available = false;
    uint64_t published = 0;
    uint32_t sink_count = 0;
    uint32_t queue_drops = 0;
    uint32_t sink_drops = 0;
};

esp_err_t initialize_bridge_events();
esp_err_t bridge_events_bind_available_sources();
esp_err_t bridge_events_add_sink(BridgeEventSink sink, void *context, uint8_t *sink_id);
esp_err_t bridge_events_remove_sink(uint8_t sink_id);
bool bridge_events_publish(BridgeEvent event);
esp_err_t bridge_events_get_status(BridgeEventBrokerStatus *status);

}  // namespace rfbridge
