#pragma once

#include <cstdint>

#include "mqtt_config_format.hpp"

namespace rfbridge {

enum class MqttNetworkEventKind : uint8_t {
    kStateChanged,
    kConnected,
    kDisconnected,
    kOther,
};

enum class MqttNetworkAvailability : uint8_t {
    kUnchanged,
    kOnline,
    kOffline,
};

MqttNetworkAvailability mqtt_network_availability(MqttNetworkEventKind event_kind,
                                                  bool state_online);
bool network_service_request_is_supported(NetworkServiceMask requested,
                                          bool combined_services_supported);
NetworkServiceMask network_service_boot_mask(NetworkServiceMask requested,
                                             bool combined_services_supported);
NetworkServiceMask network_service_recovery_mask(NetworkServiceMask boot_services,
                                                 bool mqtt_failed);
NetworkServiceMask network_service_effective_mask(NetworkServiceMask attempted,
                                                  bool web_started, bool mqtt_started);
bool mqtt_retirement_requires_reboot(NetworkServiceMask effective_services);

}  // namespace rfbridge
