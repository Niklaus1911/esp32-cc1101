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

bool format_mqtt_discovery_payload(const MqttDeviceIdentity &identity, const char *signal_name,
                                   const char *firmware_version, char *output,
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
    writer.append(",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",");
    writer.append("\"device\":{\"identifiers\":[");
    writer.append_json_string(identity.device_id);
    writer.append("],\"name\":");
    writer.append_json_string(identity.device_name);
    writer.append(",\"manufacturer\":\"RF Bridge\",\"model\":\"ESP32 + CC1101\",\"sw_version\":");
    writer.append_json_string(firmware_version);
    writer.append("}}");
    return writer.valid();
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
