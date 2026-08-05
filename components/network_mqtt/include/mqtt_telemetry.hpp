#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

enum class MqttTelemetryEncoding : uint8_t {
    kDecoded,
    kRaw,
};

enum class MqttTelemetryMatch : uint8_t {
    kNone,
    kUnique,
    kAmbiguous,
    kUnavailable,
};

struct MqttRxTelemetry {
    uint64_t sequence = 0;
    MqttTelemetryEncoding encoding = MqttTelemetryEncoding::kDecoded;
    MqttTelemetryMatch match = MqttTelemetryMatch::kNone;
    uint32_t fingerprint = 0;
    uint16_t observed_repeats = 0;
    char learned_name[16]{};
    uint16_t learned_count = 0;
    uint64_t code = 0;
    uint16_t pulse_us = 0;
    uint8_t bits = 0;
    uint8_t protocol = 0;
    uint8_t inverted = 0;
    uint16_t pulses = 0;
    uint8_t start_level = 0;
};

struct MqttAutomationTelemetry {
    uint64_t sequence = 0;
    uint8_t type = 0;
    int32_t result = 0;
    uint32_t action_id = 0;
    uint32_t elapsed_ms = 0;
    uint32_t value = 0;
    uint8_t repeats = 0;
    char trigger_name[16]{};
    char target_name[16]{};
};

struct MqttAutomationStateTelemetry {
    bool enabled = false;
    bool enabled_known = false;
    bool log_mode_known = false;
    uint8_t log_mode = 0;
    uint16_t rules = 0;
    uint32_t frames = 0;
    uint32_t matches = 0;
    uint32_t actions = 0;
    uint32_t tx_errors = 0;
    uint32_t cooldown_suppressed = 0;
    uint32_t queue_drops = 0;
    uint32_t event_drops = 0;
    uint32_t log_drops = 0;
    int32_t last_error = 0;
    char last_trigger[16]{};
    char last_target[16]{};
};

struct MqttRuleTelemetry {
    bool valid = false;
    int32_t error = 0;
    uint8_t repeats = 0;
    uint32_t cooldown_ms = 0;
    char trigger_name[16]{};
    char target_name[16]{};
};

bool format_mqtt_rx_event_payload(const MqttRxTelemetry &event, char *output,
                                  std::size_t capacity);
bool format_mqtt_automation_event_payload(const MqttAutomationTelemetry &event, char *output,
                                          std::size_t capacity);
bool format_mqtt_automation_state_payload(const MqttAutomationStateTelemetry &state, char *output,
                                          std::size_t capacity);
bool format_mqtt_rule_state_payload(const MqttRuleTelemetry &rule, char *output,
                                    std::size_t capacity);

}  // namespace rfbridge
