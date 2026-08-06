#include "mqtt_discovery.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rfbridge {
namespace {

class BoundedWriter {
public:
    BoundedWriter(char *output, std::size_t capacity) : output_(output), capacity_(capacity)
    {
        if (output_ != nullptr && capacity_ > 0) {
            output_[0] = '\0';
        }
    }

    bool append(const char *text)
    {
        if (text == nullptr) {
            return false;
        }
        return append_bytes(text, std::strlen(text));
    }

    bool append_format(const char *format, ...)
    {
        if (!valid_ || output_ == nullptr || length_ >= capacity_) {
            return false;
        }
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(output_ + length_, capacity_ - length_, format, arguments);
        va_end(arguments);
        if (written < 0 || static_cast<std::size_t>(written) >= capacity_ - length_) {
            valid_ = false;
            return false;
        }
        length_ += static_cast<std::size_t>(written);
        return true;
    }

    bool append_json_string(const char *text)
    {
        if (!append("\"")) {
            return false;
        }
        for (const uint8_t *cursor = reinterpret_cast<const uint8_t *>(text);
             cursor != nullptr && *cursor != 0; ++cursor) {
            switch (*cursor) {
                case '\"': if (!append("\\\"")) return false; break;
                case '\\': if (!append("\\\\")) return false; break;
                case '\b': if (!append("\\b")) return false; break;
                case '\f': if (!append("\\f")) return false; break;
                case '\n': if (!append("\\n")) return false; break;
                case '\r': if (!append("\\r")) return false; break;
                case '\t': if (!append("\\t")) return false; break;
                default:
                    if (*cursor < 0x20) {
                        if (!append_format("\\u%04x", static_cast<unsigned>(*cursor))) {
                            return false;
                        }
                    } else if (!append_bytes(reinterpret_cast<const char *>(cursor), 1)) {
                        return false;
                    }
                    break;
            }
        }
        return append("\"");
    }

    bool valid() const { return valid_; }

private:
    bool append_bytes(const char *data, std::size_t size)
    {
        if (!valid_ || output_ == nullptr || size >= capacity_ - length_) {
            valid_ = false;
            return false;
        }
        std::memcpy(output_ + length_, data, size);
        length_ += size;
        output_[length_] = '\0';
        return true;
    }

    char *output_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t length_ = 0;
    bool valid_ = true;
};

bool format_text(char *output, std::size_t capacity, const char *format, ...)
{
    if (output == nullptr || capacity == 0) {
        return false;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(output, capacity, format, arguments);
    va_end(arguments);
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

const char *event_suffix(MqttEventTopicKind kind)
{
    return kind == MqttEventTopicKind::kAutomation ? "automation" : "rx";
}

const char *state_suffix(MqttStateTopicKind kind)
{
    switch (kind) {
        case MqttStateTopicKind::kLastRx: return "last_rx";
        case MqttStateTopicKind::kLastAutomation: return "last_automation";
        case MqttStateTopicKind::kRule: return "rule";
        case MqttStateTopicKind::kSystem: return "system";
        case MqttStateTopicKind::kAutomation: default: return "automation";
    }
}

const char *command_suffix(MqttAutomationCommandKind kind)
{
    return kind == MqttAutomationCommandKind::kLogMode ? "log_mode" : "enabled";
}

bool entity_parts(MqttDiscoveryEntityKind kind, const char **component, const char **object,
                  bool *requires_rule)
{
    if (component == nullptr || object == nullptr || requires_rule == nullptr) {
        return false;
    }
    *requires_rule = false;
    switch (kind) {
        case MqttDiscoveryEntityKind::kRxEvent:
            *component = "event";
            *object = "rf_activity";
            return true;
        case MqttDiscoveryEntityKind::kAutomationEvent:
            *component = "event";
            *object = "automation_activity";
            return true;
        case MqttDiscoveryEntityKind::kAutomationSwitch:
            *component = "switch";
            *object = "automation";
            return true;
        case MqttDiscoveryEntityKind::kAutomationLogSelect:
            *component = "select";
            *object = "automation_log";
            return true;
        case MqttDiscoveryEntityKind::kRuleCountSensor:
            *component = "sensor";
            *object = "rule_count";
            return true;
        case MqttDiscoveryEntityKind::kEventDropsSensor:
            *component = "sensor";
            *object = "event_drops";
            return true;
        case MqttDiscoveryEntityKind::kRuleSensor:
            *component = "sensor";
            *object = "rule";
            *requires_rule = true;
            return true;
        case MqttDiscoveryEntityKind::kInternalFreeSensor:
            *component = "sensor";
            *object = "internal_free";
            return true;
        case MqttDiscoveryEntityKind::kInternalMinimumSensor:
            *component = "sensor";
            *object = "internal_minimum";
            return true;
        case MqttDiscoveryEntityKind::kInternalLargestSensor:
            *component = "sensor";
            *object = "internal_largest";
            return true;
        case MqttDiscoveryEntityKind::kPsramFreeSensor:
            *component = "sensor";
            *object = "psram_free";
            return true;
    }
    return false;
}

bool append_device(BoundedWriter *writer, const MqttDeviceIdentity &identity,
                   const BoardInfo &board, const char *firmware_version)
{
    if (writer == nullptr || board.model_name == nullptr || board.profile_name == nullptr ||
        firmware_version == nullptr) {
        return false;
    }
    return writer->append(",\"device\":{\"identifiers\":[") &&
           writer->append_json_string(identity.device_id) &&
           writer->append("],\"name\":") &&
           writer->append_json_string(identity.device_name) &&
           writer->append(",\"manufacturer\":\"RF Bridge\",\"model\":") &&
           writer->append_json_string(board.model_name) &&
           writer->append(",\"hw_version\":") &&
           writer->append_json_string(board.profile_name) && writer->append(",\"sw_version\":") &&
           writer->append_json_string(firmware_version) && writer->append("}");
}

bool system_entity(MqttDiscoveryEntityKind kind)
{
    return kind == MqttDiscoveryEntityKind::kInternalFreeSensor ||
           kind == MqttDiscoveryEntityKind::kInternalMinimumSensor ||
           kind == MqttDiscoveryEntityKind::kInternalLargestSensor ||
           kind == MqttDiscoveryEntityKind::kPsramFreeSensor;
}

}  // namespace

bool derive_mqtt_device_identity(const uint8_t station_mac[6], MqttDeviceIdentity *identity)
{
    if (station_mac == nullptr || identity == nullptr) {
        return false;
    }
    MqttDeviceIdentity result{};
    if (!format_text(result.mac_hex, sizeof(result.mac_hex), "%02x%02x%02x%02x%02x%02x",
                     static_cast<unsigned>(station_mac[0]),
                     static_cast<unsigned>(station_mac[1]),
                     static_cast<unsigned>(station_mac[2]),
                     static_cast<unsigned>(station_mac[3]),
                     static_cast<unsigned>(station_mac[4]),
                     static_cast<unsigned>(station_mac[5])) ||
        !format_text(result.client_id, sizeof(result.client_id), "rfbridge-%s", result.mac_hex) ||
        !format_text(result.device_id, sizeof(result.device_id), "rfbridge_%s", result.mac_hex) ||
        !format_text(result.device_name, sizeof(result.device_name), "RF Bridge %02X%02X%02X",
                     static_cast<unsigned>(station_mac[3]),
                     static_cast<unsigned>(station_mac[4]),
                     static_cast<unsigned>(station_mac[5]))) {
        return false;
    }
    *identity = result;
    return true;
}

bool format_mqtt_command_filter(const MqttDeviceIdentity &identity, char *output,
                                std::size_t capacity)
{
    return format_text(output, capacity, "rfbridge/%s/signal/+/press", identity.mac_hex);
}

bool format_mqtt_availability_topic(const MqttDeviceIdentity &identity, char *output,
                                    std::size_t capacity)
{
    return format_text(output, capacity, "rfbridge/%s/availability", identity.mac_hex);
}

bool format_mqtt_discovery_topic(const MqttDeviceIdentity &identity, const char *signal_name,
                                 char *output, std::size_t capacity)
{
    return rf_storage_name_is_valid(signal_name) &&
           format_text(output, capacity, "homeassistant/button/%s/%s/config", identity.device_id,
                       signal_name);
}

bool format_mqtt_command_topic(const MqttDeviceIdentity &identity, const char *signal_name,
                               char *output, std::size_t capacity)
{
    return rf_storage_name_is_valid(signal_name) &&
           format_text(output, capacity, "rfbridge/%s/signal/%s/press", identity.mac_hex,
                       signal_name);
}

bool format_mqtt_event_topic(const MqttDeviceIdentity &identity, MqttEventTopicKind kind,
                             char *output, std::size_t capacity)
{
    return format_text(output, capacity, "rfbridge/%s/event/%s", identity.mac_hex,
                       event_suffix(kind));
}

bool format_mqtt_state_topic(const MqttDeviceIdentity &identity, MqttStateTopicKind kind,
                             const char *rule_name, char *output, std::size_t capacity)
{
    if (kind == MqttStateTopicKind::kRule && !rf_storage_name_is_valid(rule_name)) {
        return false;
    }
    if (kind == MqttStateTopicKind::kRule) {
        return format_text(output, capacity, "rfbridge/%s/state/rule/%s", identity.mac_hex,
                           rule_name);
    }
    return format_text(output, capacity, "rfbridge/%s/state/%s", identity.mac_hex,
                       state_suffix(kind));
}

bool format_mqtt_automation_command_topic(const MqttDeviceIdentity &identity,
                                           MqttAutomationCommandKind kind, char *output,
                                           std::size_t capacity)
{
    return format_text(output, capacity, "rfbridge/%s/automation/%s/set", identity.mac_hex,
                       command_suffix(kind));
}

bool format_mqtt_entity_discovery_topic(const MqttDeviceIdentity &identity,
                                        MqttDiscoveryEntityKind kind, const char *rule_name,
                                        char *output, std::size_t capacity)
{
    const char *component = nullptr;
    const char *object = nullptr;
    bool requires_rule = false;
    if (!entity_parts(kind, &component, &object, &requires_rule) ||
        (requires_rule && !rf_storage_name_is_valid(rule_name))) {
        return false;
    }
    if (requires_rule) {
        return format_text(output, capacity, "homeassistant/%s/%s/rule_%s/config", component,
                           identity.device_id, rule_name);
    }
    return format_text(output, capacity, "homeassistant/%s/%s/%s/config", component,
                       identity.device_id, object == nullptr ? "" : object);
}

bool format_mqtt_discovery_payload(const MqttDeviceIdentity &identity, const char *signal_name,
                                   const BoardInfo &board, const char *firmware_version, char *output,
                                   std::size_t capacity)
{
    if (!rf_storage_name_is_valid(signal_name) || firmware_version == nullptr || output == nullptr) {
        return false;
    }
    char command_topic[kMqttTopicCapacity]{};
    char availability_topic[kMqttTopicCapacity]{};
    char unique_id[48]{};
    if (!format_mqtt_command_topic(identity, signal_name, command_topic, sizeof(command_topic)) ||
        !format_mqtt_availability_topic(identity, availability_topic,
                                        sizeof(availability_topic)) ||
        !format_text(unique_id, sizeof(unique_id), "%s_%s", identity.device_id, signal_name)) {
        return false;
    }
    BoundedWriter writer(output, capacity);
    writer.append("{\"name\":");
    writer.append_json_string(signal_name);
    writer.append(",\"unique_id\":");
    writer.append_json_string(unique_id);
    writer.append(",\"command_topic\":");
    writer.append_json_string(command_topic);
    writer.append(",\"payload_press\":\"PRESS\",\"qos\":0,\"retain\":false,");
    writer.append("\"availability_topic\":");
    writer.append_json_string(availability_topic);
    writer.append(",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"");
    return append_device(&writer, identity, board, firmware_version) && writer.append("}") &&
           writer.valid();
}

bool format_mqtt_entity_discovery_payload(const MqttDeviceIdentity &identity,
                                          MqttDiscoveryEntityKind kind, const char *rule_name,
                                          const BoardInfo &board, const char *firmware_version,
                                          char *output,
                                          std::size_t capacity)
{
    if (firmware_version == nullptr || output == nullptr) {
        return false;
    }
    const char *component = nullptr;
    const char *object = nullptr;
    bool requires_rule = false;
    if (!entity_parts(kind, &component, &object, &requires_rule) ||
        (requires_rule && !rf_storage_name_is_valid(rule_name))) {
        return false;
    }
    char state_topic[kMqttTopicCapacity]{};
    char availability_topic[kMqttTopicCapacity]{};
    char command_topic[kMqttTopicCapacity]{};
    char unique_id[64]{};
    if (!format_mqtt_availability_topic(identity, availability_topic, sizeof(availability_topic))) {
        return false;
    }
    if (requires_rule) {
        if (!format_mqtt_state_topic(identity, MqttStateTopicKind::kRule, rule_name, state_topic,
                                     sizeof(state_topic)) ||
            !format_text(unique_id, sizeof(unique_id), "%s_rule_%s", identity.device_id,
                         rule_name)) {
            return false;
        }
    } else if (kind == MqttDiscoveryEntityKind::kRxEvent ||
               kind == MqttDiscoveryEntityKind::kAutomationEvent) {
        const MqttEventTopicKind event_kind = kind == MqttDiscoveryEntityKind::kRxEvent
                                                   ? MqttEventTopicKind::kRx
                                                   : MqttEventTopicKind::kAutomation;
        if (!format_mqtt_event_topic(identity, event_kind, state_topic, sizeof(state_topic)) ||
            !format_text(unique_id, sizeof(unique_id), "%s_%s", identity.device_id, object)) {
            return false;
        }
    } else if (system_entity(kind)) {
        if (!format_mqtt_state_topic(identity, MqttStateTopicKind::kSystem, nullptr,
                                     state_topic, sizeof(state_topic)) ||
            !format_text(unique_id, sizeof(unique_id), "%s_%s", identity.device_id, object)) {
            return false;
        }
    } else if (!format_mqtt_state_topic(identity, MqttStateTopicKind::kAutomation, nullptr,
                                        state_topic, sizeof(state_topic)) ||
               !format_text(unique_id, sizeof(unique_id), "%s_%s", identity.device_id, object)) {
        return false;
    }
    if (kind == MqttDiscoveryEntityKind::kAutomationSwitch &&
        !format_mqtt_automation_command_topic(identity, MqttAutomationCommandKind::kEnabled,
                                               command_topic, sizeof(command_topic))) {
        return false;
    }
    if (kind == MqttDiscoveryEntityKind::kAutomationLogSelect &&
        !format_mqtt_automation_command_topic(identity, MqttAutomationCommandKind::kLogMode,
                                               command_topic, sizeof(command_topic))) {
        return false;
    }
    BoundedWriter writer(output, capacity);
    if (requires_rule) {
        return writer.append("{\"name\":\"Rule ") && writer.append(rule_name) &&
               writer.append("\",\"unique_id\":") && writer.append_json_string(unique_id) &&
               writer.append(",\"state_topic\":") && writer.append_json_string(state_topic) &&
               writer.append(",\"value_template\":\"{{ value_json.state }}\","
                             "\"json_attributes_topic\":") &&
               writer.append_json_string(state_topic) &&
               writer.append(",\"entity_category\":\"diagnostic\",\"availability_topic\":") &&
               writer.append_json_string(availability_topic) &&
               writer.append(",\"payload_available\":\"online\","
                             "\"payload_not_available\":\"offline\"") &&
               append_device(&writer, identity, board, firmware_version) && writer.append("}") &&
               writer.valid();
    }
    const char *name = object;
    if (kind == MqttDiscoveryEntityKind::kRxEvent) {
        name = "RF activity";
    } else if (kind == MqttDiscoveryEntityKind::kAutomationEvent) {
        name = "Automation activity";
    } else if (kind == MqttDiscoveryEntityKind::kAutomationSwitch) {
        name = "Automation enabled";
    } else if (kind == MqttDiscoveryEntityKind::kAutomationLogSelect) {
        name = "Automation log mode";
    } else if (kind == MqttDiscoveryEntityKind::kRuleCountSensor) {
        name = "Automation rule count";
    } else if (kind == MqttDiscoveryEntityKind::kEventDropsSensor) {
        name = "MQTT event drops";
    } else if (kind == MqttDiscoveryEntityKind::kInternalFreeSensor) {
        name = "Internal free memory";
    } else if (kind == MqttDiscoveryEntityKind::kInternalMinimumSensor) {
        name = "Internal minimum free memory";
    } else if (kind == MqttDiscoveryEntityKind::kInternalLargestSensor) {
        name = "Largest internal memory block";
    } else if (kind == MqttDiscoveryEntityKind::kPsramFreeSensor) {
        name = "PSRAM free memory";
    }
    if (!writer.append("{\"name\":") || !writer.append_json_string(name) ||
        !writer.append(",\"unique_id\":") || !writer.append_json_string(unique_id)) {
        return false;
    }
    if (kind == MqttDiscoveryEntityKind::kRxEvent ||
        kind == MqttDiscoveryEntityKind::kAutomationEvent) {
        if (!writer.append(",\"state_topic\":") || !writer.append_json_string(state_topic) ||
            !writer.append(",\"event_types\":[")) {
            return false;
        }
        if (kind == MqttDiscoveryEntityKind::kRxEvent) {
            if (!writer.append("\"received\"]")) return false;
        } else if (!writer.append("\"triggered\",\"action_completed\",\"cooldown_suppressed\","
                                 "\"ambiguous_frame\",\"stale_frame\",\"queue_drop\"]")) {
            return false;
        }
    } else if (kind == MqttDiscoveryEntityKind::kAutomationSwitch) {
        if (!writer.append(",\"state_topic\":") || !writer.append_json_string(state_topic) ||
            !writer.append(",\"value_template\":\"{{ value_json.enabled }}\","
                           "\"command_topic\":") ||
            !writer.append_json_string(command_topic) ||
            !writer.append(",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"")) {
            return false;
        }
    } else if (kind == MqttDiscoveryEntityKind::kAutomationLogSelect) {
        if (!writer.append(",\"state_topic\":") || !writer.append_json_string(state_topic) ||
            !writer.append(",\"value_template\":\"{{ value_json.log_mode }}\","
                           "\"command_topic\":") ||
            !writer.append_json_string(command_topic) ||
            !writer.append(",\"options\":[\"off\",\"actions\",\"verbose\"]")) {
            return false;
        }
    } else if (kind == MqttDiscoveryEntityKind::kRuleCountSensor) {
        if (!writer.append(",\"state_topic\":") || !writer.append_json_string(state_topic) ||
            !writer.append(",\"value_template\":\"{{ value_json.rules }}\","
                           "\"entity_category\":\"diagnostic\"")) {
            return false;
        }
    } else if (kind == MqttDiscoveryEntityKind::kEventDropsSensor) {
        if (!writer.append(",\"state_topic\":") || !writer.append_json_string(state_topic) ||
            !writer.append(",\"value_template\":\"{{ value_json.event_drops }}\","
                           "\"entity_category\":\"diagnostic\"")) {
            return false;
        }
    } else if (system_entity(kind)) {
        const char *field = kind == MqttDiscoveryEntityKind::kInternalFreeSensor
                                ? "free"
                            : kind == MqttDiscoveryEntityKind::kInternalMinimumSensor
                                ? "minimum"
                            : kind == MqttDiscoveryEntityKind::kInternalLargestSensor
                                ? "largest"
                                : "free";
        const char *region = kind == MqttDiscoveryEntityKind::kPsramFreeSensor
                                 ? "psram"
                                 : "internal";
        if (!writer.append(",\"state_topic\":") || !writer.append_json_string(state_topic) ||
            !writer.append_format(",\"value_template\":\"{{ value_json.memory.%s.%s }}\","
                                  "\"device_class\":\"data_size\","
                                  "\"unit_of_measurement\":\"B\","
                                  "\"entity_category\":\"diagnostic\",\"expire_after\":180",
                                  region, field)) {
            return false;
        }
    }
    if (!writer.append(",\"availability_topic\":") ||
        !writer.append_json_string(availability_topic) ||
        !writer.append(",\"payload_available\":\"online\","
                       "\"payload_not_available\":\"offline\"")) {
        return false;
    }
    return append_device(&writer, identity, board, firmware_version) && writer.append("}") &&
           writer.valid();
}

bool parse_mqtt_button_command(const MqttDeviceIdentity &identity,
                               const MqttIncomingMessage &message,
                               char output_name[kRfStorageNameCapacity])
{
    if (output_name == nullptr || message.topic == nullptr || message.data == nullptr ||
        message.qos != 0 || message.retain || message.duplicate ||
        message.current_data_offset != 0 || message.total_data_length != message.data_length ||
        message.data_length != 5 || std::memcmp(message.data, "PRESS", 5) != 0) {
        return false;
    }
    char prefix[48]{};
    if (!format_text(prefix, sizeof(prefix), "rfbridge/%s/signal/", identity.mac_hex)) {
        return false;
    }
    constexpr char suffix[] = "/press";
    const std::size_t prefix_length = std::strlen(prefix);
    const std::size_t suffix_length = sizeof(suffix) - 1U;
    if (message.topic_length <= prefix_length + suffix_length ||
        std::memcmp(message.topic, prefix, prefix_length) != 0 ||
        std::memcmp(message.topic + message.topic_length - suffix_length, suffix,
                    suffix_length) != 0) {
        return false;
    }
    const std::size_t name_length = message.topic_length - prefix_length - suffix_length;
    if (name_length == 0 || name_length >= kRfStorageNameCapacity) {
        return false;
    }
    char name[kRfStorageNameCapacity]{};
    std::memcpy(name, message.topic + prefix_length, name_length);
    if (std::strlen(name) != name_length || !rf_storage_name_is_valid(name)) {
        return false;
    }
    std::memcpy(output_name, name, sizeof(name));
    return true;
}

bool parse_mqtt_automation_command(const MqttDeviceIdentity &identity,
                                   const MqttIncomingMessage &message,
                                   MqttAutomationCommandKind *kind, bool *enabled,
                                   uint8_t *log_mode)
{
    if (kind == nullptr || enabled == nullptr || log_mode == nullptr || message.topic == nullptr ||
        message.data == nullptr || message.qos != 0 || message.retain || message.duplicate ||
        message.current_data_offset != 0 || message.total_data_length != message.data_length ||
        message.topic_length == 0 || message.data_length == 0) {
        return false;
    }
    char enabled_topic[kMqttTopicCapacity]{};
    char log_topic[kMqttTopicCapacity]{};
    if (!format_mqtt_automation_command_topic(identity, MqttAutomationCommandKind::kEnabled,
                                               enabled_topic, sizeof(enabled_topic)) ||
        !format_mqtt_automation_command_topic(identity, MqttAutomationCommandKind::kLogMode,
                                               log_topic, sizeof(log_topic))) {
        return false;
    }
    const bool is_enabled = message.topic_length == std::strlen(enabled_topic) &&
                            std::memcmp(message.topic, enabled_topic, message.topic_length) == 0;
    const bool is_log = message.topic_length == std::strlen(log_topic) &&
                        std::memcmp(message.topic, log_topic, message.topic_length) == 0;
    if (!is_enabled && !is_log) {
        return false;
    }
    if (is_enabled) {
        if (message.data_length == 2 && std::memcmp(message.data, "ON", 2) == 0) {
            *kind = MqttAutomationCommandKind::kEnabled;
            *enabled = true;
            return true;
        }
        if (message.data_length == 3 && std::memcmp(message.data, "OFF", 3) == 0) {
            *kind = MqttAutomationCommandKind::kEnabled;
            *enabled = false;
            return true;
        }
        return false;
    }
    if ((message.data_length == 3 && std::memcmp(message.data, "off", 3) == 0) ||
        (message.data_length == 7 && std::memcmp(message.data, "actions", 7) == 0) ||
        (message.data_length == 7 && std::memcmp(message.data, "verbose", 7) == 0)) {
        *kind = MqttAutomationCommandKind::kLogMode;
        *enabled = false;
        *log_mode = message.data_length == 3 ? 0 : (message.data[0] == 'a' ? 1 : 2);
        return true;
    }
    return false;
}

bool mqtt_message_is_home_assistant_birth(const MqttIncomingMessage &message)
{
    constexpr char topic[] = "homeassistant/status";
    constexpr char payload[] = "online";
    return message.topic != nullptr && message.data != nullptr && message.qos >= 0 &&
           message.qos <= 1 && message.current_data_offset == 0 &&
           message.total_data_length == message.data_length &&
           message.topic_length == sizeof(topic) - 1U &&
           std::memcmp(message.topic, topic, sizeof(topic) - 1U) == 0 &&
           message.data_length == sizeof(payload) - 1U &&
           std::memcmp(message.data, payload, sizeof(payload) - 1U) == 0;
}

}  // namespace rfbridge
