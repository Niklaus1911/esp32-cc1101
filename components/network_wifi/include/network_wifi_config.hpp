#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

constexpr std::size_t kWifiSsidCapacity = 33;
constexpr std::size_t kWifiPasswordCapacity = 65;
constexpr std::size_t kWifiConfigMaxRecordSize = 108;

struct WifiCredentials {
    char ssid[kWifiSsidCapacity]{};
    char password[kWifiPasswordCapacity]{};
};

enum class WifiConfigFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidCrc,
    kBufferTooSmall,
    kInvalidRecord,
};

bool wifi_credentials_are_valid(const WifiCredentials &credentials);
WifiConfigFormatResult encode_wifi_config_record(const WifiCredentials &credentials, uint8_t *output,
                                                  std::size_t capacity, std::size_t *encoded_size);
WifiConfigFormatResult decode_wifi_config_record(const uint8_t *record, std::size_t size,
                                                  WifiCredentials *credentials);

}  // namespace rfbridge
