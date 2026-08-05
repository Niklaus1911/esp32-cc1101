#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "mqtt_config_format.hpp"

namespace rfbridge {

enum class NetworkServiceProfile : uint8_t {
    kWeb,
    kMqtt,
};

enum class NetworkMqttDiscoveryState : uint8_t {
    kStopped,
    kDisconnected,
    kSubscribing,
    kPublishingAvailability,
    kReconciling,
    kReady,
    kRetiring,
    kFaulted,
};

struct NetworkMqttStatus {
    bool profile_initialized = false;
    bool configured = false;
    bool runtime_available = false;
    bool connected = false;
    bool subscribed = false;
    bool reboot_required = false;
    bool current_boot_fallback = false;
    bool retirement_pending = false;
    bool ledger_known = false;
    bool network_ready = false;
    bool client_start_pending = false;
    NetworkServiceProfile requested_profile = NetworkServiceProfile::kWeb;
    NetworkServiceProfile effective_profile = NetworkServiceProfile::kWeb;
    NetworkMqttDiscoveryState discovery_state = NetworkMqttDiscoveryState::kStopped;
    uint32_t broker_ipv4 = 0;
    uint32_t persisted_generation = 0;
    uint32_t boot_generation = 0;
    uint32_t connections = 0;
    uint32_t disconnects = 0;
    uint32_t reconciliations = 0;
    uint32_t commands_accepted = 0;
    uint32_t commands_rejected = 0;
    uint32_t command_queue_drops = 0;
    uint32_t publish_failures = 0;
    uint32_t outbox_deleted = 0;
    uint32_t outbox_bytes = 0;
    uint32_t telemetry_published = 0;
    uint32_t telemetry_drops = 0;
    uint32_t state_publish_failures = 0;
    uint32_t mqtt_stack_minimum_free = 0;
    uint32_t worker_stack_minimum_free = 0;
    uint32_t heap_free = 0;
    uint32_t heap_minimum = 0;
    uint32_t heap_largest = 0;
    uint16_t port = 0;
    uint8_t advertised_count = 0;
    uint8_t advertised_rule_count = 0;
    uint8_t current_count = 0;
    uint8_t current_rule_count = 0;
    char username[kMqttUsernameCapacity]{};
    esp_err_t profile_error = ESP_OK;
    esp_err_t runtime_error = ESP_ERR_INVALID_STATE;
    esp_err_t reconciliation_error = ESP_OK;
};

esp_err_t initialize_network_service_profile();
NetworkServiceProfile requested_network_service_profile();
esp_err_t prepare_network_mqtt();
esp_err_t activate_network_mqtt();
esp_err_t stop_network_mqtt_for_web_fallback();
void mark_network_service_web_fallback(esp_err_t error);

esp_err_t set_network_service_profile(NetworkServiceProfile profile);
esp_err_t configure_network_mqtt(uint32_t broker_ipv4, uint16_t port, const char *username,
                                 const char *password);
esp_err_t forget_network_mqtt();
esp_err_t get_network_mqtt_status(NetworkMqttStatus *status);

const char *network_service_profile_name(NetworkServiceProfile profile);
const char *network_mqtt_discovery_state_name(NetworkMqttDiscoveryState state);

}  // namespace rfbridge
