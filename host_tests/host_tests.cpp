#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <cstdlib>

#include "network_wifi_config.hpp"
#include "network_wifi_state.hpp"
#include "ota_update_policy.hpp"
#include "rf_automation_engine.hpp"
#include "rf_codec.hpp"
#include "rf_console_format.hpp"
#include "rf_console_parse.hpp"
#include "rf_activity_led_policy.hpp"
#include "rf_storage_format.hpp"

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void test_protocol_table()
{
    struct GoldenProtocol {
        uint16_t pulse;
        uint8_t sync_first;
        uint8_t sync_second;
        uint8_t zero_first;
        uint8_t zero_second;
        uint8_t one_first;
        uint8_t one_second;
        bool inverted;
    };
    constexpr GoldenProtocol golden[] = {
        {350, 1, 31, 1, 3, 3, 1, false},  {650, 1, 10, 1, 2, 2, 1, false},
        {100, 30, 71, 4, 11, 9, 6, false}, {380, 1, 6, 1, 3, 3, 1, false},
        {500, 6, 14, 1, 2, 2, 1, false},  {450, 23, 1, 1, 2, 2, 1, true},
        {150, 2, 62, 1, 6, 6, 1, false},  {200, 3, 130, 7, 16, 3, 16, false},
        {200, 130, 7, 16, 7, 16, 3, true}, {365, 18, 1, 3, 1, 1, 3, true},
        {270, 36, 1, 1, 2, 2, 1, true},   {320, 36, 1, 1, 2, 2, 1, true},
    };
    require(std::size(golden) == rfbridge::kRfProtocolCount, "golden protocol count");
    for (std::size_t index = 0; index < std::size(golden); ++index) {
        const rfbridge::RfProtocol *protocol = rfbridge::rf_protocol(static_cast<uint8_t>(index + 1U));
        const GoldenProtocol &expected = golden[index];
        require(protocol != nullptr && protocol->pulse_us == expected.pulse &&
                    protocol->sync.first == expected.sync_first && protocol->sync.second == expected.sync_second &&
                    protocol->zero.first == expected.zero_first && protocol->zero.second == expected.zero_second &&
                    protocol->one.first == expected.one_first && protocol->one.second == expected.one_second &&
                    protocol->inverted == expected.inverted,
                "rc-switch protocol table matches golden definitions");
    }
}

void test_protocol_round_trips()
{
    for (uint8_t protocol_number = 1; protocol_number <= rfbridge::kRfProtocolCount; ++protocol_number) {
        const rfbridge::RfProtocol *protocol = rfbridge::rf_protocol(protocol_number);
        require(protocol != nullptr, "protocol exists");
        rfbridge::DecodedSignal signal{};
        signal.code = 0xA5;
        signal.pulse_us = protocol->pulse_us;
        signal.bits = 8;
        signal.protocol = protocol_number;
        signal.inverted = protocol->inverted;
        rfbridge::RawSignal raw{};
        require(rfbridge::build_decoded_raw(signal, &raw), "protocol waveform builds");
        rfbridge::DecodedSignal identified{};
        const rfbridge::RawProtocolIdentity identity = rfbridge::identify_raw_protocol(raw, &identified);
        require(identity != rfbridge::RawProtocolIdentity::kUnknown, "protocol waveform is recognized");
        if (identity == rfbridge::RawProtocolIdentity::kDecoded) {
            require(rfbridge::decoded_signals_match(signal, identified), "unique protocol identity round trips");
        }
    }
}

void test_decoded_transport_boundaries()
{
    for (uint8_t protocol_number = 1; protocol_number <= rfbridge::kRfProtocolCount; ++protocol_number) {
        const rfbridge::RfProtocol *protocol = rfbridge::rf_protocol(protocol_number);
        const uint8_t minimum_factor = rfbridge::rf_protocol_min_factor(protocol_number);
        const uint8_t maximum_factor = rfbridge::rf_protocol_max_factor(protocol_number);
        require(protocol != nullptr && minimum_factor > 0 && maximum_factor > 0, "protocol factors exist");

        const uint16_t minimum_unit = static_cast<uint16_t>(
            (rfbridge::kMinimumRawPulseUs + minimum_factor - 1U) / minimum_factor);
        const uint16_t maximum_unit =
            static_cast<uint16_t>(rfbridge::kMaximumPulseDurationUs / maximum_factor);
        rfbridge::DecodedSignal signal{};
        signal.code = 0xA5;
        signal.bits = 8;
        signal.protocol = protocol_number;
        signal.inverted = protocol->inverted;

        signal.pulse_us = static_cast<uint16_t>(minimum_unit - 1U);
        require(!rfbridge::decoded_signal_is_valid(signal), "decoded unit below transport floor rejected");
        signal.pulse_us = minimum_unit;
        require(rfbridge::decoded_signal_is_valid(signal), "decoded unit at transport floor accepted");
        rfbridge::RawSignal raw{};
        require(rfbridge::build_decoded_raw(signal, &raw) && rfbridge::raw_signal_is_valid(raw),
                "every valid decoded signal builds a valid raw waveform");

        signal.pulse_us = maximum_unit;
        require(rfbridge::decoded_signal_is_valid(signal), "decoded unit at transport ceiling accepted");
        signal.pulse_us = static_cast<uint16_t>(maximum_unit + 1U);
        require(!rfbridge::decoded_signal_is_valid(signal), "decoded unit above transport ceiling rejected");
    }
}

void test_raw_identity_rules()
{
    rfbridge::DecodedSignal left{};
    left.code = 0;
    left.pulse_us = 350;
    left.bits = 24;
    left.protocol = 1;
    rfbridge::DecodedSignal right = left;
    right.code = 1;
    rfbridge::RawSignal left_raw{};
    rfbridge::RawSignal right_raw{};
    require(rfbridge::build_decoded_raw(left, &left_raw), "left raw builds");
    require(rfbridge::build_decoded_raw(right, &right_raw), "right raw builds");
    require(!rfbridge::raw_signals_match(left_raw, right_raw), "one changed bit remains distinct");

    rfbridge::RawSignal rotated{};
    rotated.count = left_raw.count;
    constexpr std::size_t shift = 7;
    rotated.start_level = static_cast<uint8_t>(left_raw.start_level ^ (shift & 1U));
    for (std::size_t index = 0; index < left_raw.count; ++index) {
        rotated.durations_us[index] = left_raw.durations_us[(index + shift) % left_raw.count];
    }
    require(rfbridge::raw_signals_match(left_raw, rotated), "cyclic phase remains equivalent");

    for (uint8_t bits = 1; bits < 4; ++bits) {
        rfbridge::DecodedSignal invalid = left;
        invalid.bits = bits;
        invalid.code = 1;
        require(!rfbridge::decoded_signal_is_valid(invalid), "short decoded code rejected");
    }
}

void test_protocol_alias_policy()
{
    rfbridge::DecodedSignal midpoint{};
    midpoint.code = 0x0A;
    midpoint.pulse_us = 297;
    midpoint.bits = 4;
    midpoint.protocol = 12;
    midpoint.inverted = 1;
    rfbridge::RawSignal raw{};
    require(rfbridge::build_decoded_raw(midpoint, &raw), "midpoint waveform builds");
    require(rfbridge::identify_raw_protocol(raw, nullptr) == rfbridge::RawProtocolIdentity::kAmbiguous,
            "protocol 11/12 midpoint fails closed");
}

void test_console_parser()
{
    uint64_t value = 0;
    require(rfbridge::parse_unsigned_value("11043138", UINT64_MAX, &value) && value == 11043138,
            "decimal parser");
    require(rfbridge::parse_unsigned_value("0xA88142", UINT64_MAX, &value) && value == 0xA88142,
            "hex parser");
    require(!rfbridge::parse_unsigned_value("-1", UINT64_MAX, &value), "negative rejected");
    require(!rfbridge::parse_unsigned_value("0x", UINT64_MAX, &value), "empty hex rejected");
    require(!rfbridge::parse_unsigned_value("12junk", UINT64_MAX, &value), "trailing junk rejected");
    require(!rfbridge::parse_unsigned_value("256", 255, &value), "bound enforced");

    const char *ram[] = {"replay"};
    const char *ram_repeats[] = {"replay", "10"};
    const char *named[] = {"replay", "gate", "7"};
    const char *missing_named_repeats[] = {"replay", "gate"};
    rfbridge::ReplayArguments replay{};
    require(rfbridge::parse_replay_arguments(1, ram, 8, &replay) &&
                replay.target == rfbridge::ReplayTarget::kRam && replay.repeats == 8,
            "default RAM replay preserved");
    require(rfbridge::parse_replay_arguments(2, ram_repeats, 8, &replay) &&
                replay.target == rfbridge::ReplayTarget::kRam && replay.repeats == 10,
            "explicit RAM replay preserved");
    require(rfbridge::parse_replay_arguments(3, named, 8, &replay) &&
                replay.target == rfbridge::ReplayTarget::kNamed && replay.repeats == 7,
            "named replay parsed");
    require(!rfbridge::parse_replay_arguments(2, missing_named_repeats, 8, &replay),
            "named replay requires repeats");

    const char *rule_default[] = {"rule", "add", "B", "A"};
    const char *rule_explicit[] = {"rule", "add", "B", "A", "12"};
    const char *rule_self[] = {"rule", "add", "A", "A"};
    uint8_t rule_repeats = 0;
    require(rfbridge::parse_rule_add_arguments(4, rule_default, 8, &rule_repeats) && rule_repeats == 8,
            "rule add default repeats parsed");
    require(rfbridge::parse_rule_add_arguments(5, rule_explicit, 8, &rule_repeats) && rule_repeats == 12,
            "rule add explicit repeats parsed");
    require(!rfbridge::parse_rule_add_arguments(4, rule_self, 8, &rule_repeats),
            "rule self mapping rejected");

    rfbridge::RfAutomationLogMode log_mode{};
    require(rfbridge::parse_rule_log_mode("actions", &log_mode) &&
                log_mode == rfbridge::RfAutomationLogMode::kActions,
            "rule action logging mode parsed");
    require(rfbridge::parse_rule_log_mode("verbose", &log_mode) &&
                log_mode == rfbridge::RfAutomationLogMode::kVerbose,
            "rule verbose logging mode parsed");
    require(!rfbridge::parse_rule_log_mode("debug", &log_mode), "unknown rule logging mode rejected");

    rfbridge::RfAutomationEvent event{};
    event.type = rfbridge::RfAutomationEventType::kTriggered;
    std::strcpy(event.trigger_name, "B");
    std::strcpy(event.target_name, "A");
    event.repeats = 8;
    event.action_id = 42;
    event.received_encoding = rfbridge::RfStoredEncoding::kRaw;
    event.target_encoding = rfbridge::RfStoredEncoding::kDecoded;
    char line[256]{};
    require(rfbridge::format_rf_automation_event(event, "ESP_OK", line, sizeof(line)) &&
                std::strcmp(line, "RULE TRIGGER id=42 trigger=B target=A repeats=8 rx_encoding=raw tx_encoding=decoded") == 0,
            "automation trigger event formatted");
    event.type = rfbridge::RfAutomationEventType::kActionCompleted;
    event.elapsed_ms = 742;
    require(rfbridge::format_rf_automation_event(event, "ESP_OK", line, sizeof(line)) &&
                std::strcmp(line, "RULE ACTION id=42 trigger=B target=A result=OK elapsed_ms=742") == 0,
            "automation completion event formatted");
}

void test_console_formatter()
{
    using rfbridge::ConsoleStyle;
    using rfbridge::ConsoleTone;
    ConsoleStyle style{};
    require(rfbridge::parse_console_style("pretty", &style) && style == ConsoleStyle::kPretty,
            "pretty console style parsed");
    require(rfbridge::parse_console_style("plain", &style) && style == ConsoleStyle::kPlain,
            "plain console style parsed");
    require(!rfbridge::parse_console_style("ansi", &style), "unknown console style rejected");
    require(rfbridge::ota_progress_percent(420, 1000) == 42, "OTA percentage computed");
    require(rfbridge::ota_progress_percent(UINT32_MAX, UINT32_MAX) == 100,
            "OTA percentage handles uint32 maximum");
    require(rfbridge::ota_progress_percent(UINT32_MAX, 1) == 100,
            "OTA percentage clamps oversized received count");
    std::size_t chunk_length = 0;
    std::size_t consumed_length = 0;
    require(rfbridge::console_wrap_chunk("alpha beta gamma", 10, &chunk_length,
                                         &consumed_length) &&
                chunk_length == 10 && consumed_length == 11,
            "console wrapping uses boundary delimiter");
    require(rfbridge::console_wrap_chunk("123,456,789", 7, &chunk_length,
                                         &consumed_length) &&
                chunk_length == 4 && consumed_length == 4,
            "console wrapping preserves comma-delimited values");

    char line[256]{};
    require(rfbridge::format_ota_progress_line(ConsoleStyle::kPlain, 420, 1000, line,
                                                sizeof(line)) &&
                std::strcmp(line, "OTA PROGRESS bytes=420 total=1000") == 0,
            "plain OTA progress remains exact");
    require(rfbridge::format_ota_progress_line(ConsoleStyle::kPretty, 420, 1000, line,
                                                sizeof(line)) &&
                std::strstr(line, " 42% [########------------]") != nullptr &&
                std::strcmp(line + std::strlen(line) - 4U, "\x1b[0m") == 0,
            "pretty OTA progress has fixed bar and reset");
    require(rfbridge::format_ota_progress_line(ConsoleStyle::kPretty, 957680, 957680, line,
                                                sizeof(line)) &&
                std::strstr(line, "100% [####################] 936 KiB / 936 KiB") != nullptr,
            "completed OTA progress uses consistent KiB rounding");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPretty, ConsoleTone::kSuccess, " OK ", "OK rx on", "RX enabled",
                line, sizeof(line)) &&
                std::strstr(line, "\x1b[1;32m[ OK  ]\x1b[0m RX enabled") != nullptr &&
                std::strcmp(line + std::strlen(line) - 4U, "\x1b[0m") == 0,
            "pretty tag uses semantic color and reset");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPretty, ConsoleTone::kInfo, "", "", "confidence=repeated", line,
                sizeof(line)) &&
                std::strcmp(line,
                            "      \x1b[1;36m|\x1b[0m confidence=repeated\x1b[0m") == 0,
            "pretty continuation uses aligned colored gutter");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPretty, ConsoleTone::kInfo, "     ", "", "elapsed_ms=170", line,
                sizeof(line)) &&
                std::strcmp(line, "      \x1b[1;36m|\x1b[0m elapsed_ms=170\x1b[0m") == 0,
            "whitespace tag also uses continuation gutter");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPlain, ConsoleTone::kInfo, "", "RULE ACTION elapsed_ms=170",
                "elapsed_ms=170", line, sizeof(line)) &&
                std::strcmp(line, "RULE ACTION elapsed_ms=170") == 0,
            "plain continuation input preserves stable line");
    std::strcpy(line, "RC code=7");
    require(rfbridge::decorate_console_message(ConsoleStyle::kPlain, ConsoleTone::kInfo,
                                                "RF RX", "RX ", line, sizeof(line)) &&
                std::strcmp(line, "RX RC code=7") == 0,
            "plain in-place message preserves legacy prefix");
    std::strcpy(line, "RC code=7");
    require(rfbridge::decorate_console_message(ConsoleStyle::kPretty, ConsoleTone::kInfo,
                                                "RF RX", "RX ", line, sizeof(line)) &&
                std::strstr(line, "[RF RX]\x1b[0m RC code=7") != nullptr &&
                std::strcmp(line + std::strlen(line) - 4U, "\x1b[0m") == 0,
            "pretty in-place message decorates one bounded buffer");
    require(rfbridge::format_console_section_header("Wi-Fi", line, sizeof(line)) &&
                std::strstr(line, "+-- Wi-Fi ") != nullptr,
            "dashboard section header formatted");
    require(rfbridge::format_console_dashboard_row(
                "Last frame", "available", ConsoleTone::kInfo, "Console drops", "0",
                ConsoleTone::kMuted, line, sizeof(line)) &&
                std::strstr(line, "Console drops") != nullptr,
            "system dashboard row accepts existing labels");
    require(rfbridge::format_console_dashboard_row(
                "1234567890123", "1234567890123456789012", ConsoleTone::kSuccess,
                "1234567890123", "1234567890123456789012", ConsoleTone::kInfo, line,
                sizeof(line)),
            "dashboard accepts exact label and value limits");
    require(!rfbridge::format_console_dashboard_row(
                "12345678901234", "value", ConsoleTone::kSuccess, "label", "value",
                ConsoleTone::kInfo, line, sizeof(line)),
            "dashboard rejects overlong labels");
    require(!rfbridge::format_console_dashboard_row(
                "label", "12345678901234567890123", ConsoleTone::kSuccess, "label", "value",
                ConsoleTone::kInfo, line, sizeof(line)),
            "dashboard rejects overlong two-column values");
    require(!rfbridge::format_console_dashboard_value(
                "Network", "this value is deliberately longer than the sixty character dashboard limit 123",
                ConsoleTone::kInfo, line, sizeof(line)),
            "dashboard refuses truncation");
    require(rfbridge::format_console_wifi_disconnected_message(
                ConsoleStyle::kPlain, "iliadbox-2.4ghz", 8, "online", line, sizeof(line)) &&
                std::strcmp(line,
                            "WIFI DISCONNECTED ssid=iliadbox-2.4ghz reason=8 state=online") == 0,
            "plain Wi-Fi disconnect remains exact");
    require(rfbridge::format_console_wifi_disconnected_message(
                ConsoleStyle::kPretty, "iliadbox-2.4ghz", 8, "online", line, sizeof(line)) &&
                std::strcmp(line, "Disconnected from iliadbox-2.4ghz | reason 8") == 0 &&
                std::strstr(line, "ONLINE") == nullptr,
            "pretty Wi-Fi disconnect omits stale state");
}

void test_rf_activity_led_policy()
{
    const int radio_gpios[] = {18, 19, 23, 27, 26, 25};
    rfbridge::RfActivityLedConfig config{
        .enabled = true,
        .gpio = 2,
        .active_high = true,
        .pulse_ms = 25,
    };
    require(rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                       std::size(radio_gpios)),
            "GPIO2 activity LED defaults are valid");
    require(rfbridge::rf_activity_led_active_level(config) == 1 &&
                rfbridge::rf_activity_led_inactive_level(config) == 0 &&
                rfbridge::rf_activity_led_pulse_us(config) == 25000,
            "active-high LED levels and pulse conversion are exact");
    config.active_high = false;
    require(rfbridge::rf_activity_led_active_level(config) == 0 &&
                rfbridge::rf_activity_led_inactive_level(config) == 1,
            "active-low LED levels are exact");
    config.active_high = true;
    config.gpio = 25;
    require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                        std::size(radio_gpios)),
            "activity LED rejects CC1101 pin conflicts");
    for (const int reserved_gpio : {0, 1, 3, 5, 6, 11, 12, 15, 16, 17, 20, 24, 28, 34}) {
        config.gpio = reserved_gpio;
        require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                            std::size(radio_gpios)),
                "activity LED rejects unavailable output GPIO");
    }
    config.gpio = 2;
    config.pulse_ms = rfbridge::kRfActivityLedMinimumPulseMs - 1U;
    require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                        std::size(radio_gpios)),
            "activity LED rejects short pulses");
    config.pulse_ms = rfbridge::kRfActivityLedMaximumPulseMs + 1U;
    require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                        std::size(radio_gpios)),
            "activity LED rejects long pulses");
    config.enabled = false;
    config.gpio = -1;
    require(rfbridge::rf_activity_led_config_is_valid(config, nullptr, 0),
            "disabled activity LED ignores hardware configuration");

    const rfbridge::RfActivityLedDeadlineDecision pending =
        rfbridge::rf_activity_led_deadline_decision(1000, 1001);
    const rfbridge::RfActivityLedDeadlineDecision expired =
        rfbridge::rf_activity_led_deadline_decision(1000, 1000);
    require(!pending.turn_off && pending.rearm_us == 1,
            "activity LED rearms until the latest deadline");
    require(expired.turn_off && expired.rearm_us == 0,
            "activity LED turns off at the latest deadline");
}

void test_learn_deadline_classification()
{
    using rfbridge::LearnFrameDisposition;
    require(rfbridge::classify_learn_frame(1000, 31001000, 1000, 1000) ==
                LearnFrameDisposition::kCapture,
            "frame at arm boundary captured");
    require(rfbridge::classify_learn_frame(1000, 31001000, 31000999, 2000) ==
                LearnFrameDisposition::kCapture,
            "frame before learn deadline captured");
    require(rfbridge::classify_learn_frame(1000, 31001000, 31001000, 2000) ==
                LearnFrameDisposition::kTimeout,
            "frame at learn deadline rejected");
    require(rfbridge::classify_learn_frame(1000, 31001000, 999, 999) ==
                LearnFrameDisposition::kIgnore,
            "frame accepted before arm ignored");
    require(rfbridge::classify_learn_frame(1000, 31001000, 2000, 999) ==
                LearnFrameDisposition::kIgnore,
            "capture that started before learning is ignored");
}

void test_storage_format()
{
    require(rfbridge::rf_storage_name_is_valid("Remote_1"), "valid storage name");
    require(!rfbridge::rf_storage_name_is_valid("list"), "list name reserved");
    require(!rfbridge::rf_storage_name_is_valid("1remote"), "numeric-leading name rejected");
    require(!rfbridge::rf_storage_name_is_valid("a-23456789012345"), "overlength name rejected");

    rfbridge::RfStoredSignal decoded{};
    decoded.encoding = rfbridge::RfStoredEncoding::kDecoded;
    decoded.decoded.code = 0xA88142;
    decoded.decoded.pulse_us = 386;
    decoded.decoded.bits = 24;
    decoded.decoded.protocol = 1;
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t record_size = 0;
    require(rfbridge::encode_rf_storage_record(decoded, record, sizeof(record), &record_size) ==
                rfbridge::RfStorageFormatResult::kOk &&
                record_size == 25,
            "decoded storage record encodes");
    constexpr uint8_t golden_record[] = {0x52, 0x46, 0x53, 0x52, 0x01, 0x01, 0x0D, 0x00, 0x42,
                                         0x81, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x01,
                                         0x18, 0x01, 0x00, 0xFA, 0x83, 0x81, 0x07};
    require(record_size == sizeof(golden_record) && std::memcmp(record, golden_record, sizeof(golden_record)) == 0,
            "version-one decoded storage record matches golden bytes");
    rfbridge::RfStoredSignal loaded{};
    require(rfbridge::decode_rf_storage_record(record, record_size, &loaded) ==
                rfbridge::RfStorageFormatResult::kOk &&
                loaded.encoding == rfbridge::RfStoredEncoding::kDecoded &&
                rfbridge::decoded_signals_match(decoded.decoded, loaded.decoded),
            "decoded storage record round trips");
    record[8] ^= 1U;
    require(rfbridge::decode_rf_storage_record(record, record_size, &loaded) ==
                rfbridge::RfStorageFormatResult::kInvalidCrc,
            "storage record CRC corruption rejected");

    rfbridge::RfStoredSignal raw{};
    raw.encoding = rfbridge::RfStoredEncoding::kRaw;
    raw.raw.count = rfbridge::kMaxRawPulses;
    raw.raw.start_level = 1;
    for (std::size_t index = 0; index < raw.raw.count; ++index) {
        raw.raw.durations_us[index] = static_cast<uint16_t>(100U + index);
    }
    require(rfbridge::encode_rf_storage_record(raw, record, sizeof(record), &record_size) ==
                rfbridge::RfStorageFormatResult::kOk &&
                record_size == rfbridge::kRfStorageMaxRecordSize,
            "maximum raw storage record encodes");
    require(rfbridge::decode_rf_storage_record(record, record_size, &loaded) ==
                rfbridge::RfStorageFormatResult::kOk &&
                loaded.encoding == rfbridge::RfStoredEncoding::kRaw &&
                rfbridge::raw_signals_match(raw.raw, loaded.raw),
            "maximum raw storage record round trips");
}


rfbridge::RfStorageRuleEntry make_rule(const char *trigger, const char *target)
{
    rfbridge::RfStorageRuleEntry entry{};
    std::strncpy(entry.trigger_name, trigger, sizeof(entry.trigger_name) - 1U);
    std::strncpy(entry.rule.target_name, target, sizeof(entry.rule.target_name) - 1U);
    entry.rule.repeats = 8;
    entry.rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    return entry;
}

void test_wifi_config()
{
    rfbridge::WifiCredentials credentials{};
    std::strcpy(credentials.ssid, "Workshop WiFi");
    std::strcpy(credentials.password, "personal-password");
    uint8_t record[rfbridge::kWifiConfigMaxRecordSize]{};
    std::size_t size = 0;
    require(rfbridge::wifi_credentials_are_valid(credentials) &&
                rfbridge::encode_wifi_config_record(credentials, record, sizeof(record), &size) ==
                    rfbridge::WifiConfigFormatResult::kOk,
            "Wi-Fi credentials encode");
    rfbridge::WifiCredentials decoded{};
    require(rfbridge::decode_wifi_config_record(record, size, &decoded) ==
                    rfbridge::WifiConfigFormatResult::kOk &&
                std::strcmp(decoded.ssid, credentials.ssid) == 0 &&
                std::strcmp(decoded.password, credentials.password) == 0,
            "Wi-Fi credentials round trip");
    record[8] ^= 1U;
    require(rfbridge::decode_wifi_config_record(record, size, &decoded) ==
                rfbridge::WifiConfigFormatResult::kInvalidCrc,
            "Wi-Fi credential CRC rejects corruption");
    std::strcpy(credentials.password, "short");
    require(!rfbridge::wifi_credentials_are_valid(credentials), "short Wi-Fi passphrase rejected");
    credentials.password[0] = '\0';
    require(rfbridge::wifi_credentials_are_valid(credentials), "open Wi-Fi credentials accepted");
    require(rfbridge::network_wifi_retry_delay_ms(0) == 0 &&
                rfbridge::network_wifi_retry_delay_ms(1) == 1000 &&
                rfbridge::network_wifi_retry_delay_ms(5) == 16000 &&
                rfbridge::network_wifi_retry_delay_ms(6) == 30000 &&
                rfbridge::network_wifi_retry_delay_ms(20) == 30000,
            "Wi-Fi reconnect backoff is bounded");
    char connected_line[192]{};
    require(rfbridge::format_network_wifi_connected_line(
                "Workshop WiFi", 0x2a01a8c0U, 0x00ffffffU, 0x0101a8c0U,
                connected_line, sizeof(connected_line)) &&
                std::strcmp(connected_line,
                            "WIFI CONNECTED ssid=Workshop WiFi ip=192.168.1.42 "
                            "netmask=255.255.255.0 gateway=192.168.1.1") == 0,
            "Wi-Fi DHCP success line is stable");
}

void test_ota_policy()
{
    require(rfbridge::ota_http_upload_request_is_valid("application/octet-stream", 4096, 8192, 512) &&
                !rfbridge::ota_http_upload_request_is_valid("application/json", 4096, 8192, 512) &&
                !rfbridge::ota_http_upload_request_is_valid("application/octet-stream", 511, 8192, 512) &&
                !rfbridge::ota_http_upload_request_is_valid("application/octet-stream", 8193, 8192, 512),
            "OTA HTTP upload policy is strict");
    require(rfbridge::ota_project_name_is_compatible("esp32-cc1101", "esp32-cc1101") &&
                !rfbridge::ota_project_name_is_compatible("other", "esp32-cc1101"),
            "OTA project identity is exact");
}

void test_automation_rules()
{
    rfbridge::RfStoredRule rule{};
    std::strcpy(rule.target_name, "A");
    rule.repeats = 8;
    rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    uint8_t record[rfbridge::kRfStorageMaxRuleRecordSize]{};
    std::size_t size = 0;
    require(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size) ==
                rfbridge::RfRuleFormatResult::kOk,
            "rule record encodes");
    constexpr uint8_t golden[] = {0x52, 0x46, 0x52, 0x4C, 0x01, 0x00, 0x07, 0x00, 0x01, 0x08,
                                  0xE8, 0x03, 0x00, 0x00, 0x41, 0x6E, 0xFB, 0x09, 0xF0};
    require(size == sizeof(golden) && std::memcmp(record, golden, sizeof(golden)) == 0,
            "rule record matches golden bytes");
    rfbridge::RfStoredRule loaded{};
    require(rfbridge::decode_rf_rule_record(record, size, &loaded) == rfbridge::RfRuleFormatResult::kOk &&
                std::strcmp(loaded.target_name, "A") == 0 && loaded.repeats == 8 &&
                loaded.cooldown_ms == rfbridge::kRfAutomationCooldownMs,
            "rule record round trips");
    record[10] ^= 1U;
    require(rfbridge::decode_rf_rule_record(record, size, &loaded) == rfbridge::RfRuleFormatResult::kInvalidCrc,
            "rule CRC corruption rejected");

    rfbridge::RfStorageRuleEntry graph[] = {make_rule("B", "A"), make_rule("C", "B")};
    require(!rfbridge::rf_rule_graph_has_cycle(graph, 2), "acyclic rule graph accepted");
    require(rfbridge::rf_rule_would_create_cycle(graph, 2, "A", "C"), "directed cycle rejected");
    require(!rfbridge::rf_rule_would_create_cycle(graph, 2, "D", "A"), "shared target remains valid");
    require(!rfbridge::rf_automation_cooldown_allows(1000000, 1999999, 1000),
            "cooldown suppresses before boundary");
    require(rfbridge::rf_automation_cooldown_allows(1000000, 2000000, 1000),
            "cooldown allows exact boundary");

    rfbridge::RfStoredSignal decoded{};
    decoded.encoding = rfbridge::RfStoredEncoding::kDecoded;
    decoded.decoded.code = 0xA88142;
    decoded.decoded.pulse_us = 350;
    decoded.decoded.bits = 24;
    decoded.decoded.protocol = 1;
    rfbridge::RfStoredSignal raw{};
    raw.encoding = rfbridge::RfStoredEncoding::kRaw;
    require(rfbridge::build_decoded_raw(decoded.decoded, &raw.raw) &&
                rfbridge::rf_stored_signals_equivalent(decoded, raw),
            "decoded and raw stored signals match canonically");

    auto uniform_raw = [](uint16_t duration_us) {
        rfbridge::RfStoredSignal signal{};
        signal.encoding = rfbridge::RfStoredEncoding::kRaw;
        signal.raw.start_level = 1;
        signal.raw.count = 8;
        for (std::size_t index = 0; index < signal.raw.count; ++index) {
            signal.raw.durations_us[index] = duration_us;
        }
        return signal;
    };
    const rfbridge::RfStoredSignal low = uniform_raw(100);
    const rfbridge::RfStoredSignal middle = uniform_raw(108);
    const rfbridge::RfStoredSignal high = uniform_raw(116);
    const rfbridge::RfStoredSignal *triggers[] = {&low, &high};
    std::size_t matched_index = 0;
    require(rfbridge::rf_stored_signals_equivalent(low, middle) &&
                rfbridge::rf_stored_signals_equivalent(middle, high) &&
                !rfbridge::rf_stored_signals_equivalent(low, high),
            "raw matching has the expected non-transitive boundary");
    require(rfbridge::rf_find_unique_stored_signal_match(middle, triggers, 2, &matched_index) ==
                rfbridge::RfAutomationMatchResult::kAmbiguous,
            "overlapping raw trigger match is suppressed as ambiguous");
    require(rfbridge::rf_automation_event_is_current(4, 4, 2001, 2000) &&
                !rfbridge::rf_automation_event_is_current(3, 4, 3000, 2000) &&
                !rfbridge::rf_automation_event_is_current(4, 4, 2000, 2000),
            "stale rule-generation events are rejected");
    require(rfbridge::rf_automation_log_mode_allows(rfbridge::RfAutomationLogMode::kActions,
                                                     rfbridge::RfAutomationEventType::kTriggered) &&
                !rfbridge::rf_automation_log_mode_allows(rfbridge::RfAutomationLogMode::kActions,
                                                          rfbridge::RfAutomationEventType::kCooldownSuppressed) &&
                rfbridge::rf_automation_log_mode_allows(rfbridge::RfAutomationLogMode::kVerbose,
                                                         rfbridge::RfAutomationEventType::kCooldownSuppressed),
            "automation log mode filtering is exact");
    require(rfbridge::rf_automation_cooldown_remaining_ms(1000000, 1499001, 1000) == 501 &&
                rfbridge::rf_automation_cooldown_remaining_ms(1000000, 2000000, 1000) == 0,
            "automation cooldown logging remainder is rounded up");
}

}  // namespace

int main()
{
    test_protocol_table();
    test_protocol_round_trips();
    test_decoded_transport_boundaries();
    test_raw_identity_rules();
    test_protocol_alias_policy();
    test_console_parser();
    test_console_formatter();
    test_rf_activity_led_policy();
    test_learn_deadline_classification();
    test_storage_format();
    test_wifi_config();
    test_ota_policy();
    test_automation_rules();
    std::puts("All host RF tests passed");
    return 0;
}
