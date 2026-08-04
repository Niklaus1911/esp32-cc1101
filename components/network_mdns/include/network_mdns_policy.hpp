#pragma once

#include <cstdint>

namespace rfbridge {

enum class NetworkMdnsState : uint8_t {
    kNotStarted,
    kStarting,
    kReady,
    kFaulted,
    kStalled,
};

bool network_mdns_hostname_apply_needed(uint32_t applied_generation,
                                        uint32_t configured_generation,
                                        bool failure_latched,
                                        uint32_t failed_generation,
                                        bool lifecycle_retry);
bool network_mdns_registration_is_complete(bool initialized, bool http_registered,
                                           bool rfbridge_registered);
NetworkMdnsState network_mdns_reported_state(NetworkMdnsState stored_state,
                                             bool heartbeat_known,
                                             uint32_t heartbeat_age_ms,
                                             uint32_t stalled_after_ms);

}  // namespace rfbridge
