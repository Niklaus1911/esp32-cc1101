#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

constexpr std::size_t kNetworkHostnameMaxLength = 32;
constexpr std::size_t kNetworkHostnameCapacity = kNetworkHostnameMaxLength + 1U;
constexpr std::size_t kNetworkHostnameMacSuffixCapacity = 7;
constexpr std::size_t kNetworkHostnameRecordMaxSize = 44;

enum class NetworkHostnameFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidCrc,
    kBufferTooSmall,
    kInvalidRecord,
};

bool canonicalize_network_hostname(const char *input, char *output, std::size_t capacity);
bool derive_default_network_hostname(const uint8_t mac[6], char *hostname,
                                     std::size_t hostname_capacity, char *mac_suffix,
                                     std::size_t suffix_capacity);
NetworkHostnameFormatResult encode_network_hostname_record(const char *hostname, uint8_t *output,
                                                           std::size_t capacity,
                                                           std::size_t *encoded_size);
NetworkHostnameFormatResult decode_network_hostname_record(const uint8_t *record, std::size_t size,
                                                           char *hostname,
                                                           std::size_t hostname_capacity);

}  // namespace rfbridge
