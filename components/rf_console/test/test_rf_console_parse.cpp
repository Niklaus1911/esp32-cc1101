#include <cstdint>
#include <cstring>

#include "rf_console_format.hpp"
#include "rf_console_parse.hpp"
#include "unity.h"

TEST_CASE("console formatter handles styles percentages and bounded bars", "[rf_console][format]")
{
    using rfbridge::ConsoleStyle;
    ConsoleStyle style{};
    TEST_ASSERT_TRUE(rfbridge::parse_console_style("pretty", &style));
    TEST_ASSERT_EQUAL(static_cast<int>(ConsoleStyle::kPretty), static_cast<int>(style));
    TEST_ASSERT_TRUE(rfbridge::parse_console_style("plain", &style));
    TEST_ASSERT_EQUAL(static_cast<int>(ConsoleStyle::kPlain), static_cast<int>(style));
    TEST_ASSERT_FALSE(rfbridge::parse_console_style("color", &style));
    TEST_ASSERT_EQUAL_UINT8(0, rfbridge::ota_progress_percent(10, 0));
    TEST_ASSERT_EQUAL_UINT8(42, rfbridge::ota_progress_percent(420, 1000));
    TEST_ASSERT_EQUAL_UINT8(100, rfbridge::ota_progress_percent(UINT32_MAX, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT8(100, rfbridge::ota_progress_percent(UINT32_MAX, 1));
    std::size_t chunk_length = 0;
    std::size_t consumed_length = 0;
    TEST_ASSERT_TRUE(rfbridge::console_wrap_chunk("alpha beta gamma", 10, &chunk_length,
                                                  &consumed_length));
    TEST_ASSERT_EQUAL_size_t(10, chunk_length);
    TEST_ASSERT_EQUAL_size_t(11, consumed_length);
    TEST_ASSERT_TRUE(rfbridge::console_wrap_chunk("123,456,789", 7, &chunk_length,
                                                  &consumed_length));
    TEST_ASSERT_EQUAL_size_t(4, chunk_length);
    TEST_ASSERT_EQUAL_size_t(4, consumed_length);

    char line[256]{};
    TEST_ASSERT_TRUE(rfbridge::format_ota_progress_line(ConsoleStyle::kPlain, 420, 1000, line,
                                                        sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("OTA PROGRESS bytes=420 total=1000", line);
    TEST_ASSERT_TRUE(rfbridge::format_ota_progress_line(ConsoleStyle::kPretty, 420, 1000, line,
                                                        sizeof(line)));
    TEST_ASSERT_NOT_NULL(std::strstr(line, " 42% [########------------]"));
    TEST_ASSERT_NOT_NULL(std::strstr(line, "\x1b[0m"));
    TEST_ASSERT_TRUE(rfbridge::format_ota_progress_line(ConsoleStyle::kPretty, 957680, 957680,
                                                        line, sizeof(line)));
    TEST_ASSERT_NOT_NULL(std::strstr(line, "100% [####################] 936 KiB / 936 KiB"));

    char short_line[16]{};
    TEST_ASSERT_FALSE(rfbridge::format_ota_progress_line(ConsoleStyle::kPretty, 420, 1000,
                                                         short_line, sizeof(short_line)));
}

TEST_CASE("console formatter resets ANSI and rejects dashboard truncation", "[rf_console][format]")
{
    using rfbridge::ConsoleStyle;
    using rfbridge::ConsoleTone;
    char line[256]{};
    TEST_ASSERT_TRUE(rfbridge::format_console_tagged_line(
        ConsoleStyle::kPretty, ConsoleTone::kSuccess, " OK ", "OK rx on", "RX enabled", line,
        sizeof(line)));
    TEST_ASSERT_NOT_NULL(std::strstr(line, "\x1b[1;32m[ OK  ]\x1b[0m RX enabled"));
    TEST_ASSERT_EQUAL_STRING("\x1b[0m", line + std::strlen(line) - 4U);
    TEST_ASSERT_TRUE(rfbridge::format_console_tagged_line(
        ConsoleStyle::kPlain, ConsoleTone::kSuccess, " OK ", "OK rx on", "RX enabled", line,
        sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("OK rx on", line);
    TEST_ASSERT_TRUE(rfbridge::format_console_tagged_line(
        ConsoleStyle::kPretty, ConsoleTone::kInfo, "", "", "confidence=repeated", line,
        sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("      \x1b[1;36m|\x1b[0m confidence=repeated\x1b[0m", line);
    TEST_ASSERT_TRUE(rfbridge::format_console_tagged_line(
        ConsoleStyle::kPretty, ConsoleTone::kInfo, "     ", "", "elapsed_ms=170", line,
        sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("      \x1b[1;36m|\x1b[0m elapsed_ms=170\x1b[0m", line);
    TEST_ASSERT_TRUE(rfbridge::format_console_tagged_line(
        ConsoleStyle::kPlain, ConsoleTone::kInfo, "", "RULE ACTION elapsed_ms=170",
        "elapsed_ms=170", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("RULE ACTION elapsed_ms=170", line);
    std::strcpy(line, "RC code=7");
    TEST_ASSERT_TRUE(rfbridge::decorate_console_message(
        ConsoleStyle::kPlain, ConsoleTone::kInfo, "RF RX", "RX ", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("RX RC code=7", line);
    std::strcpy(line, "RC code=7");
    TEST_ASSERT_TRUE(rfbridge::decorate_console_message(
        ConsoleStyle::kPretty, ConsoleTone::kInfo, "RF RX", "RX ", line, sizeof(line)));
    TEST_ASSERT_NOT_NULL(std::strstr(line, "[RF RX]\x1b[0m RC code=7"));
    TEST_ASSERT_EQUAL_STRING("\x1b[0m", line + std::strlen(line) - 4U);

    TEST_ASSERT_TRUE(rfbridge::format_console_section_header("Wi-Fi", line, sizeof(line)));
    TEST_ASSERT_NOT_NULL(std::strstr(line, "+-- Wi-Fi "));
    TEST_ASSERT_TRUE(rfbridge::format_console_dashboard_row(
        "Last frame", "available", ConsoleTone::kInfo, "Console drops", "0",
        ConsoleTone::kMuted, line, sizeof(line)));
    TEST_ASSERT_NOT_NULL(std::strstr(line, "Console drops"));
    TEST_ASSERT_TRUE(rfbridge::format_console_dashboard_row(
        "1234567890123", "1234567890123456789012", ConsoleTone::kSuccess,
        "1234567890123", "1234567890123456789012", ConsoleTone::kInfo, line,
        sizeof(line)));
    TEST_ASSERT_FALSE(rfbridge::format_console_dashboard_row(
        "12345678901234", "value", ConsoleTone::kSuccess, "label", "value",
        ConsoleTone::kInfo, line, sizeof(line)));
    TEST_ASSERT_FALSE(rfbridge::format_console_dashboard_row(
        "label", "12345678901234567890123", ConsoleTone::kSuccess, "label", "value",
        ConsoleTone::kInfo, line, sizeof(line)));
    TEST_ASSERT_TRUE(rfbridge::format_console_dashboard_value(
        "Network", "iliadbox-2.4ghz  192.168.1.17", ConsoleTone::kInfo, line, sizeof(line)));
    TEST_ASSERT_FALSE(rfbridge::format_console_dashboard_value(
        "Network", "this value is deliberately longer than the sixty character dashboard limit 123",
        ConsoleTone::kInfo, line, sizeof(line)));
    TEST_ASSERT_TRUE(rfbridge::format_console_wifi_disconnected_message(
        ConsoleStyle::kPlain, "iliadbox-2.4ghz", 8, "online", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "WIFI DISCONNECTED ssid=iliadbox-2.4ghz reason=8 state=online", line);
    TEST_ASSERT_TRUE(rfbridge::format_console_wifi_disconnected_message(
        ConsoleStyle::kPretty, "iliadbox-2.4ghz", 8, "online", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("Disconnected from iliadbox-2.4ghz | reason 8", line);
    TEST_ASSERT_NULL(std::strstr(line, "ONLINE"));
}

TEST_CASE("console unsigned parser accepts decimal and hexadecimal", "[rf_console]")
{
    uint64_t value = 0;
    TEST_ASSERT_TRUE(rfbridge::parse_unsigned_value("11043138", UINT64_MAX, &value));
    TEST_ASSERT_EQUAL_UINT64(11043138, value);
    TEST_ASSERT_TRUE(rfbridge::parse_unsigned_value("0xA88142", UINT64_MAX, &value));
    TEST_ASSERT_EQUAL_HEX64(0xA88142, value);
}

TEST_CASE("console unsigned parser rejects malformed and overflowing input", "[rf_console]")
{
    uint64_t value = 0;
    TEST_ASSERT_FALSE(rfbridge::parse_unsigned_value("", UINT64_MAX, &value));
    TEST_ASSERT_FALSE(rfbridge::parse_unsigned_value("-1", UINT64_MAX, &value));
    TEST_ASSERT_FALSE(rfbridge::parse_unsigned_value(" 1", UINT64_MAX, &value));
    TEST_ASSERT_FALSE(rfbridge::parse_unsigned_value("0x", UINT64_MAX, &value));
    TEST_ASSERT_FALSE(rfbridge::parse_unsigned_value("12junk", UINT64_MAX, &value));
    TEST_ASSERT_FALSE(rfbridge::parse_unsigned_value("256", 255, &value));
}

TEST_CASE("replay parser preserves RAM forms and requires repeats for names", "[rf_console]")
{
    const char *ram_default[] = {"replay"};
    const char *ram_repeats[] = {"replay", "10"};
    const char *named[] = {"replay", "gate", "7"};
    const char *named_missing_repeats[] = {"replay", "gate"};
    const char *bad_repeats[] = {"replay", "gate", "21"};
    rfbridge::ReplayArguments arguments{};
    TEST_ASSERT_TRUE(rfbridge::parse_replay_arguments(1, ram_default, 8, &arguments));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::ReplayTarget::kRam), static_cast<int>(arguments.target));
    TEST_ASSERT_EQUAL_UINT16(8, arguments.repeats);
    TEST_ASSERT_TRUE(rfbridge::parse_replay_arguments(2, ram_repeats, 8, &arguments));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::ReplayTarget::kRam), static_cast<int>(arguments.target));
    TEST_ASSERT_EQUAL_UINT16(10, arguments.repeats);
    TEST_ASSERT_TRUE(rfbridge::parse_replay_arguments(3, named, 8, &arguments));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::ReplayTarget::kNamed), static_cast<int>(arguments.target));
    TEST_ASSERT_EQUAL_UINT16(7, arguments.repeats);
    TEST_ASSERT_FALSE(rfbridge::parse_replay_arguments(2, named_missing_repeats, 8, &arguments));
    TEST_ASSERT_FALSE(rfbridge::parse_replay_arguments(3, bad_repeats, 8, &arguments));
}

TEST_CASE("learn capture window is post-arm and half open", "[rf_console]")
{
    using rfbridge::LearnFrameDisposition;
    TEST_ASSERT_EQUAL(static_cast<int>(LearnFrameDisposition::kCapture),
                      static_cast<int>(rfbridge::classify_learn_frame(1000, 31001000, 31000999, 2000)));
    TEST_ASSERT_EQUAL(static_cast<int>(LearnFrameDisposition::kTimeout),
                      static_cast<int>(rfbridge::classify_learn_frame(1000, 31001000, 31001000, 2000)));
    TEST_ASSERT_EQUAL(static_cast<int>(LearnFrameDisposition::kIgnore),
                      static_cast<int>(rfbridge::classify_learn_frame(1000, 31001000, 999, 999)));
    TEST_ASSERT_EQUAL(static_cast<int>(LearnFrameDisposition::kIgnore),
                      static_cast<int>(rfbridge::classify_learn_frame(1000, 31001000, 2000, 999)));
}


TEST_CASE("rule add parser validates names and repeats", "[rf_console][rf_automation]")
{
    const char *default_rule[] = {"rule", "add", "B", "A"};
    const char *explicit_rule[] = {"rule", "add", "B", "A", "12"};
    const char *self_rule[] = {"rule", "add", "A", "A"};
    const char *bad_repeat[] = {"rule", "add", "B", "A", "21"};
    const char *missing_target[] = {"rule", "add", "B"};
    uint8_t repeats = 0;
    TEST_ASSERT_TRUE(rfbridge::parse_rule_add_arguments(4, default_rule, 8, &repeats));
    TEST_ASSERT_EQUAL_UINT8(8, repeats);
    TEST_ASSERT_TRUE(rfbridge::parse_rule_add_arguments(5, explicit_rule, 8, &repeats));
    TEST_ASSERT_EQUAL_UINT8(12, repeats);
    TEST_ASSERT_FALSE(rfbridge::parse_rule_add_arguments(4, self_rule, 8, &repeats));
    TEST_ASSERT_FALSE(rfbridge::parse_rule_add_arguments(5, bad_repeat, 8, &repeats));
    TEST_ASSERT_FALSE(rfbridge::parse_rule_add_arguments(3, missing_target, 8, &repeats));
}


TEST_CASE("rule log parser accepts exact modes", "[rf_console][rf_automation][logging]")
{
    rfbridge::RfAutomationLogMode mode{};
    TEST_ASSERT_TRUE(rfbridge::parse_rule_log_mode("off", &mode));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfAutomationLogMode::kOff), static_cast<int>(mode));
    TEST_ASSERT_TRUE(rfbridge::parse_rule_log_mode("actions", &mode));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfAutomationLogMode::kActions), static_cast<int>(mode));
    TEST_ASSERT_TRUE(rfbridge::parse_rule_log_mode("verbose", &mode));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfAutomationLogMode::kVerbose), static_cast<int>(mode));
    TEST_ASSERT_FALSE(rfbridge::parse_rule_log_mode("action", &mode));
    TEST_ASSERT_FALSE(rfbridge::parse_rule_log_mode("VERBOSE", &mode));
    TEST_ASSERT_FALSE(rfbridge::parse_rule_log_mode(nullptr, &mode));
}

TEST_CASE("automation events format as stable terminal lines", "[rf_console][rf_automation][logging]")
{
    rfbridge::RfAutomationEvent event{};
    std::strcpy(event.trigger_name, "B");
    std::strcpy(event.target_name, "A");
    event.repeats = 8;
    event.action_id = 42;
    event.received_encoding = rfbridge::RfStoredEncoding::kRaw;
    event.target_encoding = rfbridge::RfStoredEncoding::kDecoded;
    char line[256]{};

    event.type = rfbridge::RfAutomationEventType::kTriggered;
    TEST_ASSERT_TRUE(rfbridge::format_rf_automation_event(event, "ESP_OK", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("RULE TRIGGER id=42 trigger=B target=A repeats=8 rx_encoding=raw tx_encoding=decoded", line);

    event.type = rfbridge::RfAutomationEventType::kActionCompleted;
    event.elapsed_ms = 742;
    TEST_ASSERT_TRUE(rfbridge::format_rf_automation_event(event, "ESP_OK", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("RULE ACTION id=42 trigger=B target=A result=OK elapsed_ms=742", line);
    event.result = 0x105;
    TEST_ASSERT_TRUE(rfbridge::format_rf_automation_event(event, "ESP_ERR_NOT_FOUND", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "RULE ACTION id=42 trigger=B target=A result=ERROR error=ESP_ERR_NOT_FOUND (0x105) elapsed_ms=742", line);

    event.type = rfbridge::RfAutomationEventType::kCooldownSuppressed;
    event.value = 418;
    TEST_ASSERT_TRUE(rfbridge::format_rf_automation_event(event, nullptr, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("RULE SUPPRESS trigger=B reason=cooldown remaining_ms=418", line);
    event.type = rfbridge::RfAutomationEventType::kAmbiguousFrame;
    event.value = 2;
    TEST_ASSERT_TRUE(rfbridge::format_rf_automation_event(event, nullptr, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("RULE SKIP reason=ambiguous matches=2", line);
    event.type = rfbridge::RfAutomationEventType::kQueueDrop;
    event.value = 7;
    TEST_ASSERT_TRUE(rfbridge::format_rf_automation_event(event, nullptr, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("RULE DROP source=automation_queue total=7", line);

    char short_line[8]{};
    TEST_ASSERT_FALSE(rfbridge::format_rf_automation_event(event, nullptr, short_line, sizeof(short_line)));
}
