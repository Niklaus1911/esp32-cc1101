#pragma once

#include <cstddef>
#include <cstdint>

#include "platform_board_policy.hpp"
#include "rf_storage_format.hpp"

namespace rfbridge {

constexpr std::size_t kMqttMacHexCapacity = 13;
constexpr std::size_t kMqttClientIdCapacity = 22;
constexpr std::size_t kMqttDeviceIdCapacity = 22;
constexpr std::size_t kMqttDeviceNameCapacity = 23;
constexpr std::size_t kMqttTopicCapacity = 112;
constexpr std::size_t kMqttDiscoveryPayloadCapacity = 1024;

enum class MqttEventTopicKind : uint8_t {
    kRx,
    kAutomation,
};

enum class MqttStateTopicKind : uint8_t {
    kAutomation,
    kLastRx,
    kLastAutomation,
    kRule,
    kSystem,
};

enum class MqttAutomationCommandKind : uint8_t {
    kEnabled,
    kLogMode,
};

enum class MqttDiscoveryEntityKind : uint8_t {
    kRxEvent,
    kAutomationEvent,
    kAutomationSwitch,
    kAutomationLogSelect,
    kRuleCountSensor,
    kEventDropsSensor,
    kRuleSensor,
    kInternalFreeSensor,
    kInternalMinimumSensor,
    kInternalLargestSensor,
    kPsramFreeSensor,
};

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
bool format_mqtt_event_topic(const MqttDeviceIdentity &identity, MqttEventTopicKind kind,
                             char *output, std::size_t capacity);
bool format_mqtt_state_topic(const MqttDeviceIdentity &identity, MqttStateTopicKind kind,
                             const char *rule_name, char *output, std::size_t capacity);
bool format_mqtt_automation_command_topic(const MqttDeviceIdentity &identity,
                                           MqttAutomationCommandKind kind, char *output,
                                           std::size_t capacity);
bool format_mqtt_entity_discovery_topic(const MqttDeviceIdentity &identity,
                                        MqttDiscoveryEntityKind kind, const char *rule_name,
                                        char *output, std::size_t capacity);
bool format_mqtt_discovery_payload(const MqttDeviceIdentity &identity, const char *signal_name,
                                   const BoardInfo &board, const char *firmware_version, char *output,
                                   std::size_t capacity);
bool format_mqtt_entity_discovery_payload(const MqttDeviceIdentity &identity,
                                          MqttDiscoveryEntityKind kind, const char *rule_name,
                                          const BoardInfo &board, const char *firmware_version,
                                          char *output,
                                          std::size_t capacity);
bool parse_mqtt_button_command(const MqttDeviceIdentity &identity,
                               const MqttIncomingMessage &message,
                               char output_name[kRfStorageNameCapacity]);
bool parse_mqtt_automation_command(const MqttDeviceIdentity &identity,
                                   const MqttIncomingMessage &message,
                                   MqttAutomationCommandKind *kind, bool *enabled,
                                   uint8_t *log_mode);
bool mqtt_message_is_home_assistant_birth(const MqttIncomingMessage &message);

}  // namespace rfbridge
