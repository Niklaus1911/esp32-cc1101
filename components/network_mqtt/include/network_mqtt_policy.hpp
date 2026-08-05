#pragma once

#include <cstdint>

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

}  // namespace rfbridge
