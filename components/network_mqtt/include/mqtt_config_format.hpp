#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "rf_storage_format.hpp"

namespace rfbridge {

constexpr std::size_t kMqttUsernameCapacity = 64;
constexpr std::size_t kMqttPasswordCapacity = 128;
constexpr std::size_t kMqttServiceMaxRecordSize = 214;
constexpr std::size_t kMqttMaximumAdvertisedSignals = 64;
constexpr uint8_t kMqttAdvertisedFormatVersion = 2;
constexpr std::size_t kMqttAdvertisedMaxRecordSize =
    20U + kMqttMaximumAdvertisedSignals * kRfStorageNameCapacity;

enum class MqttServiceState : uint8_t {
    kWeb = 0,
    kMqtt = 1,
    kRetiring = 2,
    kRetired = 3,
};

struct MqttServiceConfig {
    MqttServiceState state = MqttServiceState::kWeb;
    uint32_t broker_ipv4 = 0;
    uint32_t generation = 0;
    uint16_t port = 0;
    char username[kMqttUsernameCapacity]{};
    char password[kMqttPasswordCapacity]{};
};

struct MqttAdvertisedLedger {
    uint32_t broker_ipv4 = 0;
    uint16_t port = 0;
    uint8_t count = 0;
    uint8_t rule_count = 0;
    uint8_t format_version = kMqttAdvertisedFormatVersion;
    std::array<RfStorageName, kMqttMaximumAdvertisedSignals> names{};
};

constexpr std::size_t mqtt_advertised_ledger_total_count(const MqttAdvertisedLedger &ledger)
{
    return static_cast<std::size_t>(ledger.count) + ledger.rule_count;
}

enum class MqttConfigFormatResult : uint8_t {
    kOk,
    kInvalidArgument,
    kInvalidVersion,
    kInvalidCrc,
    kBufferTooSmall,
    kInvalidRecord,
};

bool mqtt_broker_ipv4_is_valid(uint32_t address);
bool parse_mqtt_broker_ipv4(const char *text, uint32_t *address);
bool format_mqtt_broker_ipv4(uint32_t address, char *output, std::size_t capacity);
bool mqtt_service_config_is_valid(const MqttServiceConfig &config);
bool mqtt_service_has_credentials(const MqttServiceConfig &config);
bool mqtt_advertised_ledger_is_valid(const MqttAdvertisedLedger &ledger);
bool mqtt_advertised_ledger_matches_endpoint(const MqttAdvertisedLedger &ledger,
                                             uint32_t broker_ipv4, uint16_t port);
MqttConfigFormatResult merge_mqtt_advertised_ledgers(
    const MqttAdvertisedLedger &left, const MqttAdvertisedLedger &right,
    MqttAdvertisedLedger *merged);

MqttConfigFormatResult encode_mqtt_service_record(const MqttServiceConfig &config,
                                                  uint8_t *output, std::size_t capacity,
                                                  std::size_t *encoded_size);
MqttConfigFormatResult decode_mqtt_service_record(const uint8_t *record, std::size_t size,
                                                  MqttServiceConfig *config);
MqttConfigFormatResult encode_mqtt_advertised_record(const MqttAdvertisedLedger &ledger,
                                                     uint8_t *output, std::size_t capacity,
                                                     std::size_t *encoded_size);
MqttConfigFormatResult decode_mqtt_advertised_record(const uint8_t *record, std::size_t size,
                                                     MqttAdvertisedLedger *ledger);

}  // namespace rfbridge
