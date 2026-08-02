#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

enum class NetworkWifiState : uint8_t {
    kOff,
    kStarting,
    kConnecting,
    kWaitingDhcp,
    kOnline,
    kRetryWait,
    kStopping,
    kFault,
};

const char *network_wifi_state_name(NetworkWifiState state);
uint32_t network_wifi_retry_delay_ms(uint32_t retry_count);
bool format_network_wifi_connected_line(const char *ssid, uint32_t ip, uint32_t netmask,
                                        uint32_t gateway, char *output, std::size_t capacity);

}  // namespace rfbridge
