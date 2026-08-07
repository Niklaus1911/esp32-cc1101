#include "mqtt_telemetry.hpp"

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
        return text != nullptr && append_bytes(text, std::strlen(text));
    }

    bool append_format(const char *format, ...)
    {
        if (!valid_ || output_ == nullptr || capacity_ == 0 || length_ >= capacity_) {
            valid_ = false;
            return false;
        }
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(output_ + length_, capacity_ - length_, format,
                                           arguments);
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
        const auto *cursor = reinterpret_cast<const uint8_t *>(text == nullptr ? "" : text);
        for (; *cursor != 0; ++cursor) {
            switch (*cursor) {
                case '"': if (!append("\\\"")) return false; break;
                case '\\': if (!append("\\\\")) return false; break;
                case '\b': if (!append("\\b")) return false; break;
                case '\f': if (!append("\\f")) return false; break;
                case '\n': if (!append("\\n")) return false; break;
                case '\r': if (!append("\\r")) return false; break;
                case '\t': if (!append("\\t")) return false; break;
                default:
                    if (*cursor < 0x20U &&
                        !append_format("\\u%04x", static_cast<unsigned>(*cursor))) {
                        return false;
                    }
                    if (*cursor >= 0x20U &&
                        !append_bytes(reinterpret_cast<const char *>(cursor), 1U)) {
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
        if (!valid_ || output_ == nullptr || capacity_ == 0 || size >= capacity_ - length_) {
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

const char *encoding_name(MqttTelemetryEncoding encoding)
{
    return encoding == MqttTelemetryEncoding::kRaw ? "raw" : "decoded";
}

const char *match_name(MqttTelemetryMatch match)
{
    switch (match) {
        case MqttTelemetryMatch::kUnique: return "unique";
        case MqttTelemetryMatch::kAmbiguous: return "ambiguous";
        case MqttTelemetryMatch::kUnavailable: return "unavailable";
        case MqttTelemetryMatch::kNone: default: return "none";
    }
}

const char *automation_type_name(uint8_t type)
{
    switch (type) {
        case 0: return "triggered";
        case 1: return "action_completed";
        case 2: return "cooldown_suppressed";
        case 3: return "ambiguous_frame";
        case 4: return "stale_frame";
        case 5: return "queue_drop";
        default: return "unknown";
    }
}

const char *log_mode_name(uint8_t mode)
{
    switch (mode) {
        case 0: return "off";
        case 1: return "actions";
        case 2: return "verbose";
        default: return "unknown";
    }
}

bool append_rx_fields(BoundedWriter *writer, const MqttRxTelemetry &event, bool include_type)
{
    if (include_type && !writer->append("{\"event_type\":\"received\",")) {
        return false;
    }
    if (!include_type && !writer->append("{")) {
        return false;
    }
    if (!writer->append_format("\"sequence\":%llu,\"encoding\":\"%s\","
                               "\"fingerprint\":%lu,\"repeats\":%u,\"match\":\"%s\","
                               "\"match_count\":%u",
                               static_cast<unsigned long long>(event.sequence),
                               encoding_name(event.encoding),
                               static_cast<unsigned long>(event.fingerprint),
                               static_cast<unsigned>(event.observed_repeats), match_name(event.match),
                               static_cast<unsigned>(event.learned_count))) {
        return false;
    }
    if (event.learned_name[0] != '\0' &&
        (!writer->append(",\"name\":") || !writer->append_json_string(event.learned_name))) {
        return false;
    }
    if (event.encoding == MqttTelemetryEncoding::kDecoded) {
        return writer->append_format(",\"code\":%llu,\"bits\":%u,\"protocol\":%u,"
                                     "\"pulse_us\":%u,\"inverted\":%u}",
                                     static_cast<unsigned long long>(event.code),
                                     static_cast<unsigned>(event.bits),
                                     static_cast<unsigned>(event.protocol),
                                     static_cast<unsigned>(event.pulse_us),
                                     static_cast<unsigned>(event.inverted));
    }
    return writer->append_format(",\"pulses\":%u,\"start_level\":%u}",
                                 static_cast<unsigned>(event.pulses),
                                 static_cast<unsigned>(event.start_level));
}

}  // namespace

bool format_mqtt_rx_event_payload(const MqttRxTelemetry &event, char *output, std::size_t capacity)
{
    BoundedWriter writer(output, capacity);
    if (!append_rx_fields(&writer, event, true)) {
        return false;
    }
    return writer.valid();
}

bool format_mqtt_automation_event_payload(const MqttAutomationTelemetry &event, char *output,
                                          std::size_t capacity)
{
    BoundedWriter writer(output, capacity);
    if (!writer.append_format("{\"event_type\":\"%s\",\"sequence\":%llu,"
                              "\"action_id\":%lu,\"result\":%ld,\"elapsed_ms\":%lu,"
                              "\"value\":%lu,\"repeats\":%u",
                              automation_type_name(event.type),
                              static_cast<unsigned long long>(event.sequence),
                              static_cast<unsigned long>(event.action_id),
                              static_cast<long>(event.result),
                              static_cast<unsigned long>(event.elapsed_ms),
                              static_cast<unsigned long>(event.value),
                              static_cast<unsigned>(event.repeats))) {
        return false;
    }
    if (event.trigger_name[0] != '\0' &&
        (!writer.append(",\"trigger\":") || !writer.append_json_string(event.trigger_name))) {
        return false;
    }
    if (event.target_name[0] != '\0' &&
        (!writer.append(",\"target\":") || !writer.append_json_string(event.target_name))) {
        return false;
    }
    return writer.append("}") && writer.valid();
}

bool format_mqtt_automation_state_payload(const MqttAutomationStateTelemetry &state, char *output,
                                          std::size_t capacity)
{
    BoundedWriter writer(output, capacity);
    if (!writer.append_format("{\"enabled\":\"%s\",\"log_mode\":\"%s\","
                              "\"rules\":%u,\"frames\":%lu,\"matches\":%lu,"
                              "\"actions\":%lu,\"tx_errors\":%lu,\"cooldown\":%lu,"
                              "\"queue_drops\":%lu,\"event_drops\":%lu,\"log_drops\":%lu,"
                              "\"last_error\":%ld",
                              state.enabled_known && state.enabled ? "ON" : "OFF",
                              state.log_mode_known ? log_mode_name(state.log_mode) : "unknown",
                              static_cast<unsigned>(state.rules),
                              static_cast<unsigned long>(state.frames),
                              static_cast<unsigned long>(state.matches),
                              static_cast<unsigned long>(state.actions),
                              static_cast<unsigned long>(state.tx_errors),
                              static_cast<unsigned long>(state.cooldown_suppressed),
                              static_cast<unsigned long>(state.queue_drops),
                              static_cast<unsigned long>(state.event_drops),
                              static_cast<unsigned long>(state.log_drops),
                              static_cast<long>(state.last_error))) {
        return false;
    }
    if (state.last_trigger[0] != '\0' &&
        (!writer.append(",\"last_trigger\":") ||
         !writer.append_json_string(state.last_trigger))) {
        return false;
    }
    if (state.last_target[0] != '\0' &&
        (!writer.append(",\"last_target\":") || !writer.append_json_string(state.last_target))) {
        return false;
    }
    return writer.append("}") && writer.valid();
}

bool format_mqtt_rule_state_payload(const MqttRuleTelemetry &rule, char *output, std::size_t capacity)
{
    BoundedWriter writer(output, capacity);
    if (!writer.append_format("{\"state\":\"%s\",\"error\":%ld,\"repeats\":%u,"
                              "\"cooldown_ms\":%lu,\"target\":",
                              rule.valid ? "configured" : "invalid", static_cast<long>(rule.error),
                              static_cast<unsigned>(rule.repeats),
                              static_cast<unsigned long>(rule.cooldown_ms)) ||
        !writer.append_json_string(rule.target_name) || !writer.append("}")) {
        return false;
    }
    return writer.valid();
}

bool format_mqtt_system_state_payload(const MqttSystemTelemetry &system, char *output,
                                      std::size_t capacity)
{
    if (system.board_profile == nullptr || system.board_target == nullptr ||
        system.requested_services == nullptr || system.effective_services == nullptr) {
        return false;
    }
    BoundedWriter writer(output, capacity);
    if (system.hardware != nullptr &&
        (!writer.append("{\"hardware\":") || !writer.append_json_string(system.hardware) ||
         !writer.append(","))) {
        return false;
    }
    if (!writer.append(system.hardware == nullptr ? "{\"board\":{\"profile\":"
                                                   : "\"board\":{\"profile\":") ||
        !writer.append_json_string(system.board_profile) ||
        !writer.append(",\"target\":") || !writer.append_json_string(system.board_target) ||
        !writer.append_format(",\"flash_mib\":%u,\"psram_mib\":%u},\"services\":{\"requested\":",
                              static_cast<unsigned>(system.flash_mib),
                              static_cast<unsigned>(system.psram_mib)) ||
        !writer.append_json_string(system.requested_services) ||
        !writer.append(",\"effective\":") ||
        !writer.append_json_string(system.effective_services) ||
        !writer.append_format(",\"reboot_required\":%s},\"uptime_s\":%llu,\"memory\":{"
                              "\"internal\":{\"total\":%lu,\"free\":%lu,\"minimum\":%lu,"
                              "\"largest\":%lu},\"psram\":{\"total\":%lu,\"free\":%lu,"
                              "\"minimum\":%lu,\"largest\":%lu}}}",
                              system.reboot_required ? "true" : "false",
                              static_cast<unsigned long long>(system.uptime_s),
                              static_cast<unsigned long>(system.internal.total),
                              static_cast<unsigned long>(system.internal.free),
                              static_cast<unsigned long>(system.internal.minimum),
                              static_cast<unsigned long>(system.internal.largest),
                              static_cast<unsigned long>(system.psram.total),
                              static_cast<unsigned long>(system.psram.free),
                              static_cast<unsigned long>(system.psram.minimum),
                              static_cast<unsigned long>(system.psram.largest))) {
        return false;
    }
    return writer.valid();
}

}  // namespace rfbridge
