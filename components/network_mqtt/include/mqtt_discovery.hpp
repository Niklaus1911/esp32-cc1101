#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_storage_format.hpp"

namespace rfbridge {

constexpr std::size_t kMqttMacHexCapacity = 13;
constexpr std::size_t kMqttClientIdCapacity = 22;
constexpr std::size_t kMqttDeviceIdCapacity = 22;
constexpr std::size_t kMqttDeviceNameCapacity = 23;
constexpr std::size_t kMqttTopicCapacity = 112;
constexpr std::size_t kMqttDiscoveryPayloadCapacity = 1024;

struct MqttDeviceIdentity {
    char mac_hex[kMqttMacHexCapacity]{};
    char client_id[kMqttClientIdCapacity]{};
    char device_id[kMqttDeviceIdCapacity]{};
    char device_name[kMqttDeviceNameCapacity]{};
};

struct MqttIncomingMessage {
    const char *topic = nullptr;
    std::size_t topic_length = 0;
    const char *data = nullptr;
    std::size_t data_length = 0;
    std::size_t total_data_length = 0;
    std::size_t current_data_offset = 0;
    int qos = 0;
    bool retain = false;
    bool duplicate = false;
};

bool derive_mqtt_device_identity(const uint8_t station_mac[6], MqttDeviceIdentity *identity);
bool format_mqtt_command_filter(const MqttDeviceIdentity &identity, char *output,
                                std::size_t capacity);
bool format_mqtt_availability_topic(const MqttDeviceIdentity &identity, char *output,
                                    std::size_t capacity);
bool format_mqtt_discovery_topic(const MqttDeviceIdentity &identity, const char *signal_name,
                                 char *output, std::size_t capacity);
bool format_mqtt_command_topic(const MqttDeviceIdentity &identity, const char *signal_name,
                               char *output, std::size_t capacity);
bool format_mqtt_discovery_payload(const MqttDeviceIdentity &identity, const char *signal_name,
                                   const char *firmware_version, char *output,
                                   std::size_t capacity);
bool parse_mqtt_button_command(const MqttDeviceIdentity &identity,
                               const MqttIncomingMessage &message,
                               char output_name[kRfStorageNameCapacity]);
bool mqtt_message_is_home_assistant_birth(const MqttIncomingMessage &message);

}  // namespace rfbridge
