#include "network_mdns_policy.hpp"

namespace rfbridge {

bool network_mdns_hostname_apply_needed(uint32_t applied_generation,
                                        uint32_t configured_generation,
                                        bool failure_latched,
                                        uint32_t failed_generation,
                                        bool lifecycle_retry)
{
    return applied_generation != configured_generation &&
           (lifecycle_retry || !failure_latched || failed_generation != configured_generation);
}

bool network_mdns_registration_is_complete(bool initialized, bool http_registered,
                                           bool rfbridge_registered)
{
    return initialized && http_registered && rfbridge_registered;
}

NetworkMdnsState network_mdns_reported_state(NetworkMdnsState stored_state,
                                             bool heartbeat_known,
                                             uint32_t heartbeat_age_ms,
                                             uint32_t stalled_after_ms)
{
    if (heartbeat_known && heartbeat_age_ms > stalled_after_ms &&
        (stored_state == NetworkMdnsState::kStarting || stored_state == NetworkMdnsState::kReady)) {
        return NetworkMdnsState::kStalled;
    }
    return stored_state;
}

}  // namespace rfbridge
