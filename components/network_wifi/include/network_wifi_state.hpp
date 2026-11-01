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

enum class NetworkWifiRetryAction : uint8_t {
    kRetry,
    kFallBackToSaved,
    kGiveUp,
};

// After `retry_limit` failures without an address, a newly requested (candidate) network falls
// back to a different saved network so a remote change can't strand the device; any other
// connection gives up as before. A connection that has had an address keeps retrying.
NetworkWifiRetryAction network_wifi_retry_action(bool had_ip, uint32_t retry_count,
                                                 uint32_t retry_limit, bool candidate,
                                                 bool saved_differs);
bool format_network_wifi_connected_line(const char *ssid, uint32_t ip, uint32_t netmask,
                                        uint32_t gateway, char *output, std::size_t capacity);

}  // namespace rfbridge
