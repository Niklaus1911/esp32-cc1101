#pragma once

#include <cstdint>

#include "esp_err.h"
#include "network_hostname_config.hpp"
#include "network_wifi_config.hpp"
#include "network_wifi_state.hpp"

namespace rfbridge {

enum class NetworkWifiEventType : uint8_t {
    kStateChanged,
    kConnected,
    kDisconnected,
    kScanResult,
    kScanCompleted,
    kError,
};

struct NetworkWifiEvent {
    NetworkWifiEventType type = NetworkWifiEventType::kStateChanged;
    NetworkWifiState state = NetworkWifiState::kOff;
    char ssid[kWifiSsidCapacity]{};
    uint32_t ip = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    int32_t reason = 0;
    esp_err_t error = ESP_OK;
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint8_t auth_mode = 0;
};

struct NetworkWifiStatus {
    bool available = false;
    bool driver_initialized = false;
    bool driver_started = false;
    bool saved_known = false;
    bool saved = false;
    bool active_saved = false;
    bool scan_running = false;
    bool ota_locked = false;
    NetworkWifiState state = NetworkWifiState::kOff;
    char active_ssid[kWifiSsidCapacity]{};
    char saved_ssid[kWifiSsidCapacity]{};
    uint32_t ip = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    uint32_t dns = 0;
    uint32_t retry_count = 0;
    uint32_t event_drops = 0;
    int32_t disconnect_reason = 0;
    int8_t rssi = 0;
    esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
    esp_err_t persistence_error = ESP_OK;
    esp_err_t last_error = ESP_OK;
};

struct NetworkHostnameStatus {
    char default_hostname[kNetworkHostnameCapacity]{};
    char configured_hostname[kNetworkHostnameCapacity]{};
    char mac_suffix[kNetworkHostnameMacSuffixCapacity]{};
    bool custom = false;
    uint32_t configured_generation = 0;
    uint32_t netif_applied_generation = 0;
    esp_err_t persistence_error = ESP_OK;
    esp_err_t last_apply_error = ESP_OK;
};

// Runs on the network owner task. Sinks must use only bounded zero-wait operations and must not call
// network Wi-Fi APIs. Returning false asks the owner to retry critical connection events.
using NetworkWifiEventSink = bool (*)(const NetworkWifiEvent &event, void *context);
using NetworkWifiOnlineSink = void (*)(bool online, void *context);

esp_err_t initialize_network_wifi();
// Initializes only the TCP/IP core so socket clients can be started before the Wi-Fi driver.
esp_err_t prepare_network_wifi_stack();
esp_err_t start_saved_network_wifi();
esp_err_t connect_network_wifi(const WifiCredentials &credentials);
esp_err_t stop_network_wifi();
esp_err_t forget_network_wifi();
esp_err_t scan_network_wifi();
esp_err_t get_network_wifi_status(NetworkWifiStatus *status);
esp_err_t get_network_hostname_status(NetworkHostnameStatus *status);
esp_err_t set_network_hostname(const char *hostname);
esp_err_t reset_network_hostname();
esp_err_t set_network_wifi_event_sink(NetworkWifiEventSink sink, void *context);
esp_err_t set_network_wifi_online_sink(NetworkWifiOnlineSink sink, void *context);
esp_err_t set_network_wifi_ota_lock(bool enabled);
bool network_wifi_is_online();

}  // namespace rfbridge
