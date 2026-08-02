#include "rf_console_parse.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "rf_storage_format.hpp"

namespace rfbridge {

bool parse_unsigned_value(const char *text, uint64_t maximum, uint64_t *value)
{
    if (text == nullptr || value == nullptr || text[0] == '\0' || text[0] == '-' ||
        std::isspace(static_cast<unsigned char>(text[0])) != 0) {
        return false;
    }
    const bool hexadecimal = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hexadecimal && text[2] == '\0') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, hexadecimal ? 16 : 10);
    if (errno == ERANGE || end == text || end == nullptr || *end != '\0' || parsed > maximum) {
        return false;
    }
    *value = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_replay_arguments(int argc, const char *const *argv, uint16_t default_repeats,
                            ReplayArguments *arguments)
{
    if (argv == nullptr || arguments == nullptr || default_repeats < 1 || default_repeats > 20) {
        return false;
    }
    ReplayArguments parsed{};
    uint64_t repeats = default_repeats;
    if (argc == 1) {
        parsed.target = ReplayTarget::kRam;
    } else if (argc == 2) {
        if (!parse_unsigned_value(argv[1], 20, &repeats) || repeats < 1) {
            return false;
        }
        parsed.target = ReplayTarget::kRam;
    } else if (argc == 3) {
        if (!rf_storage_name_is_valid(argv[1]) || !parse_unsigned_value(argv[2], 20, &repeats) || repeats < 1) {
            return false;
        }
        parsed.target = ReplayTarget::kNamed;
    } else {
        return false;
    }
    parsed.repeats = static_cast<uint16_t>(repeats);
    *arguments = parsed;
    return true;
}


bool parse_rule_add_arguments(int argc, const char *const *argv, uint8_t default_repeats, uint8_t *repeats)
{
    if (argv == nullptr || repeats == nullptr || default_repeats < 1 || default_repeats > 20 || argc < 4 ||
        argc > 5 || std::strcmp(argv[1], "add") != 0 || !rf_storage_name_is_valid(argv[2]) ||
        !rf_storage_name_is_valid(argv[3]) || std::strcmp(argv[2], argv[3]) == 0) {
        return false;
    }
    uint64_t parsed = default_repeats;
    if (argc == 5 && (!parse_unsigned_value(argv[4], 20, &parsed) || parsed < 1)) {
        return false;
    }
    *repeats = static_cast<uint8_t>(parsed);
    return true;
}


bool parse_rule_log_mode(const char *text, RfAutomationLogMode *mode)
{
    if (text == nullptr || mode == nullptr) {
        return false;
    }
    RfAutomationLogMode parsed{};
    if (std::strcmp(text, "off") == 0) {
        parsed = RfAutomationLogMode::kOff;
    } else if (std::strcmp(text, "actions") == 0) {
        parsed = RfAutomationLogMode::kActions;
    } else if (std::strcmp(text, "verbose") == 0) {
        parsed = RfAutomationLogMode::kVerbose;
    } else {
        return false;
    }
    *mode = parsed;
    return true;
}

bool format_rf_automation_event(const RfAutomationEvent &event, const char *result_name, char *output,
                                std::size_t capacity)
{
    if (output == nullptr || capacity == 0) {
        return false;
    }
    const char *received_encoding =
        event.received_encoding == RfStoredEncoding::kDecoded ? "decoded" : "raw";
    const char *target_encoding =
        event.target_encoding == RfStoredEncoding::kDecoded ? "decoded" : "raw";
    int written = -1;
    switch (event.type) {
        case RfAutomationEventType::kTriggered:
            written = std::snprintf(output, capacity,
                                    "RULE TRIGGER id=%lu trigger=%s target=%s repeats=%u rx_encoding=%s tx_encoding=%s",
                                    static_cast<unsigned long>(event.action_id), event.trigger_name,
                                    event.target_name, event.repeats, received_encoding, target_encoding);
            break;
        case RfAutomationEventType::kActionCompleted:
            if (event.result == 0) {
                written = std::snprintf(output, capacity,
                                        "RULE ACTION id=%lu trigger=%s target=%s result=OK elapsed_ms=%lu",
                                        static_cast<unsigned long>(event.action_id), event.trigger_name,
                                        event.target_name, static_cast<unsigned long>(event.elapsed_ms));
            } else {
                written = std::snprintf(output, capacity,
                                        "RULE ACTION id=%lu trigger=%s target=%s result=ERROR error=%s (0x%lx) elapsed_ms=%lu",
                                        static_cast<unsigned long>(event.action_id), event.trigger_name,
                                        event.target_name, result_name == nullptr ? "UNKNOWN" : result_name,
                                        static_cast<unsigned long>(static_cast<uint32_t>(event.result)),
                                        static_cast<unsigned long>(event.elapsed_ms));
            }
            break;
        case RfAutomationEventType::kCooldownSuppressed:
            written = std::snprintf(output, capacity,
                                    "RULE SUPPRESS trigger=%s reason=cooldown remaining_ms=%lu",
                                    event.trigger_name, static_cast<unsigned long>(event.value));
            break;
        case RfAutomationEventType::kAmbiguousFrame:
            written = std::snprintf(output, capacity, "RULE SKIP reason=ambiguous matches=%lu",
                                    static_cast<unsigned long>(event.value));
            break;
        case RfAutomationEventType::kStaleFrame:
            written = std::snprintf(output, capacity, "RULE SKIP reason=stale event_generation=%lu",
                                    static_cast<unsigned long>(event.value));
            break;
        case RfAutomationEventType::kQueueDrop:
            written = std::snprintf(output, capacity, "RULE DROP source=automation_queue total=%lu",
                                    static_cast<unsigned long>(event.value));
            break;
    }
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

LearnFrameDisposition classify_learn_frame(int64_t armed_us, int64_t deadline_us, int64_t frame_event_us,
                                           int64_t frame_capture_start_us)
{
    if (armed_us < 0 || deadline_us <= armed_us || frame_event_us < armed_us ||
        frame_capture_start_us < armed_us) {
        return LearnFrameDisposition::kIgnore;
    }
    return frame_event_us < deadline_us ? LearnFrameDisposition::kCapture : LearnFrameDisposition::kTimeout;
}

}  // namespace rfbridge
