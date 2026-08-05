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

}  // namespace rfbridge
