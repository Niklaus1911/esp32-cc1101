#include "network_mqtt_policy.hpp"

namespace rfbridge {

MqttNetworkAvailability mqtt_network_availability(MqttNetworkEventKind event_kind,
                                                  bool state_online)
{
    switch (event_kind) {
        case MqttNetworkEventKind::kDisconnected:
            return MqttNetworkAvailability::kOffline;
        case MqttNetworkEventKind::kStateChanged:
        case MqttNetworkEventKind::kConnected:
            return state_online ? MqttNetworkAvailability::kOnline
                                : MqttNetworkAvailability::kOffline;
        case MqttNetworkEventKind::kOther:
            return MqttNetworkAvailability::kUnchanged;
    }
    return MqttNetworkAvailability::kUnchanged;
}

bool network_service_request_is_supported(NetworkServiceMask requested,
                                          bool combined_services_supported)
{
    return requested == NetworkServiceMask::kWeb || requested == NetworkServiceMask::kMqtt ||
           (requested == NetworkServiceMask::kBoth && combined_services_supported);
}

NetworkServiceMask network_service_boot_mask(NetworkServiceMask requested,
                                             bool combined_services_supported)
{
    if (requested == NetworkServiceMask::kBoth && !combined_services_supported) {
        return NetworkServiceMask::kWeb;
    }
    return requested == NetworkServiceMask::kWeb || requested == NetworkServiceMask::kMqtt ||
                   requested == NetworkServiceMask::kBoth
               ? requested
               : NetworkServiceMask::kWeb;
}

NetworkServiceMask network_service_recovery_mask(NetworkServiceMask boot_services,
                                                 bool mqtt_failed)
{
    if (!mqtt_failed ||
        !network_service_mask_has(boot_services, NetworkServiceMask::kMqtt)) {
        return boot_services;
    }
    return static_cast<NetworkServiceMask>(static_cast<uint8_t>(boot_services) |
                                           static_cast<uint8_t>(NetworkServiceMask::kWeb));
}

NetworkServiceMask network_service_effective_mask(NetworkServiceMask attempted,
                                                  bool web_started, bool mqtt_started)
{
    uint8_t effective = 0;
    if (web_started && network_service_mask_has(attempted, NetworkServiceMask::kWeb)) {
        effective |= static_cast<uint8_t>(NetworkServiceMask::kWeb);
    }
    if (mqtt_started && network_service_mask_has(attempted, NetworkServiceMask::kMqtt)) {
        effective |= static_cast<uint8_t>(NetworkServiceMask::kMqtt);
    }
    return static_cast<NetworkServiceMask>(effective);
}

bool mqtt_retirement_requires_reboot(NetworkServiceMask effective_services)
{
    return !network_service_mask_has(effective_services, NetworkServiceMask::kWeb);
}

}  // namespace rfbridge
