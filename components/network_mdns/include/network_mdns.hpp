#pragma once

#include <cstdint>

#include "esp_err.h"
#include "network_hostname_config.hpp"
#include "network_mdns_policy.hpp"

namespace rfbridge {

constexpr std::size_t kNetworkMdnsEffectiveHostnameCapacity = 65;

struct NetworkMdnsStatus {
    bool available = false;
    bool initialized = false;
    bool http_service_registered = false;
    bool rfbridge_service_registered = false;
    bool hostname_custom = false;
    bool effective_known = false;
    bool conflict_renamed = false;
    NetworkMdnsState state = NetworkMdnsState::kNotStarted;
    char configured_hostname[kNetworkHostnameCapacity]{};
    char effective_hostname[kNetworkMdnsEffectiveHostnameCapacity]{};
    uint32_t configured_generation = 0;
    uint32_t applied_generation = 0;
    uint32_t owner_heartbeat_age_ms = 0;
    esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
    esp_err_t last_error = ESP_OK;
};

const char *network_mdns_state_name(NetworkMdnsState state);
esp_err_t initialize_network_mdns();
esp_err_t start_network_mdns(uint16_t http_port);
esp_err_t reconcile_network_mdns();
esp_err_t set_network_mdns_hostname(const char *hostname);
esp_err_t reset_network_mdns_hostname();
esp_err_t get_network_mdns_status(NetworkMdnsStatus *status);
bool network_mdns_is_active();

}  // namespace rfbridge
