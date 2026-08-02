#include "network_wifi_state.hpp"

#include <algorithm>
#include <cstdio>

namespace rfbridge {

const char *network_wifi_state_name(NetworkWifiState state)
{
    switch (state) {
        case NetworkWifiState::kOff:
            return "off";
        case NetworkWifiState::kStarting:
            return "starting";
        case NetworkWifiState::kConnecting:
            return "connecting";
        case NetworkWifiState::kWaitingDhcp:
            return "waiting_dhcp";
        case NetworkWifiState::kOnline:
            return "online";
        case NetworkWifiState::kRetryWait:
            return "retry_wait";
        case NetworkWifiState::kStopping:
            return "stopping";
        case NetworkWifiState::kFault:
            return "fault";
    }
    return "invalid";
}

uint32_t network_wifi_retry_delay_ms(uint32_t retry_count)
{
    if (retry_count == 0) {
        return 0;
    }
    const uint32_t shift = std::min<uint32_t>(retry_count - 1U, 5U);
    return std::min<uint32_t>(1000U << shift, 30000U);
}

bool format_network_wifi_connected_line(const char *ssid, uint32_t ip, uint32_t netmask,
                                        uint32_t gateway, char *output, std::size_t capacity)
{
    if (ssid == nullptr || output == nullptr || capacity == 0) {
        return false;
    }
    const auto byte = [](uint32_t value, unsigned shift) {
        return static_cast<unsigned>((value >> shift) & 0xffU);
    };
    const int written = std::snprintf(
        output, capacity,
        "WIFI CONNECTED ssid=%s ip=%u.%u.%u.%u netmask=%u.%u.%u.%u gateway=%u.%u.%u.%u",
        ssid, byte(ip, 0), byte(ip, 8), byte(ip, 16), byte(ip, 24), byte(netmask, 0),
        byte(netmask, 8), byte(netmask, 16), byte(netmask, 24), byte(gateway, 0),
        byte(gateway, 8), byte(gateway, 16), byte(gateway, 24));
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

}  // namespace rfbridge
