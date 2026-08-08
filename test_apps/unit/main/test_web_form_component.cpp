#include <cstring>

#include "unity.h"
#include "web_form.hpp"

TEST_CASE("responsive Web forms accept only exact bounded fields", "[web_ui]")
{
    char name[16]{};
    TEST_ASSERT_TRUE(rfbridge::parse_web_learn_form("name=gate_1", 11, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("gate_1", name);
    TEST_ASSERT_FALSE(
        rfbridge::parse_web_learn_form("name=gate_1&extra=1", 19, name, sizeof(name)));

    rfbridge::WebReplayForm replay{};
    constexpr char replay_body[] = "name=gate_1&repeats=7";
    TEST_ASSERT_TRUE(rfbridge::parse_web_replay_form(
        replay_body, sizeof(replay_body) - 1U, &replay));
    TEST_ASSERT_FALSE(replay.latest);
    TEST_ASSERT_EQUAL_UINT16(7, replay.repeats);

    rfbridge::WebSignalSaveForm signal_save{};
    constexpr char signal_save_nominal[] =
        "name=gate&code=13830801&bits=24&protocol=1";
    TEST_ASSERT_TRUE(rfbridge::parse_web_signal_save_form(
        signal_save_nominal, sizeof(signal_save_nominal) - 1U, &signal_save));
    TEST_ASSERT_EQUAL_STRING("gate", signal_save.name);
    TEST_ASSERT_EQUAL_HEX64(0xD30A91, signal_save.code);
    TEST_ASSERT_EQUAL_UINT16(350, signal_save.pulse_us);
    constexpr char signal_save_explicit[] =
        "name=gate_2&code=0xD30A91&bits=24&protocol=1&pulse_us=199";
    TEST_ASSERT_TRUE(rfbridge::parse_web_signal_save_form(
        signal_save_explicit, sizeof(signal_save_explicit) - 1U, &signal_save));
    TEST_ASSERT_EQUAL_UINT16(199, signal_save.pulse_us);
    constexpr char signal_save_overflow[] =
        "name=gate&code=0x1000000&bits=24&protocol=1";
    TEST_ASSERT_FALSE(rfbridge::parse_web_signal_save_form(
        signal_save_overflow, sizeof(signal_save_overflow) - 1U, &signal_save));

    rfbridge::WebRecentForm recent{};
    constexpr char recent_replay[] =
        "action=replay&id=18446744073709551614&repeats=20";
    TEST_ASSERT_TRUE(rfbridge::parse_web_recent_form(
        recent_replay, sizeof(recent_replay) - 1U, &recent));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::WebRecentAction::kReplay),
                      static_cast<int>(recent.action));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX - 1U, recent.id);
    constexpr char recent_save[] = "action=save&id=42&name=gate_1";
    TEST_ASSERT_TRUE(rfbridge::parse_web_recent_form(
        recent_save, sizeof(recent_save) - 1U, &recent));
    TEST_ASSERT_EQUAL_STRING("gate_1", recent.name);
    constexpr char recent_clear[] = "action=clear";
    TEST_ASSERT_TRUE(rfbridge::parse_web_recent_form(
        recent_clear, sizeof(recent_clear) - 1U, &recent));
    constexpr char recent_extra[] = "action=clear&extra=1";
    TEST_ASSERT_FALSE(rfbridge::parse_web_recent_form(
        recent_extra, sizeof(recent_extra) - 1U, &recent));

    rfbridge::WebDecodedForm decoded{};
    constexpr char decoded_body[] =
        "code=0xA88142&bits=24&protocol=1&pulse_us=0&repeats=8";
    TEST_ASSERT_TRUE(rfbridge::parse_web_decoded_form(
        decoded_body, sizeof(decoded_body) - 1U, &decoded));
    TEST_ASSERT_EQUAL_HEX64(0xA88142, decoded.code);

    rfbridge::WebRawForm raw{};
    constexpr char raw_body[] =
        "start_level=0&durations=100,200,300,400,500,600,700,800&repeats=2";
    TEST_ASSERT_TRUE(
        rfbridge::parse_web_raw_form(raw_body, sizeof(raw_body) - 1U, &raw));
    TEST_ASSERT_EQUAL_size_t(8, raw.count);

    rfbridge::WebHardwareForm hardware{};
    constexpr char generic_body[] = "hardware=generic";
    TEST_ASSERT_TRUE(rfbridge::parse_web_hardware_form(
        generic_body, sizeof(generic_body) - 1U, &hardware));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfHardware::kGeneric),
                      static_cast<int>(hardware.hardware));
    constexpr char embedded_nul_body[] = "hardware=cc1101\0junk";
    TEST_ASSERT_FALSE(rfbridge::parse_web_hardware_form(
        embedded_nul_body, sizeof(embedded_nul_body) - 1U, &hardware));
}

TEST_CASE("responsive Web origin and escaping contracts are bounded", "[web_ui]")
{
    constexpr uint32_t device_ip = 0x1101A8C0;
    TEST_ASSERT_TRUE(rfbridge::web_host_matches_ipv4("192.168.1.17", device_ip, 80));
    TEST_ASSERT_TRUE(rfbridge::web_host_matches_ipv4("192.168.1.17:80", device_ip, 80));
    TEST_ASSERT_FALSE(rfbridge::web_host_matches_ipv4("rfbridge.local", device_ip, 80));
    TEST_ASSERT_FALSE(rfbridge::web_host_matches_ipv4("192.168.1.17:8032", device_ip, 80));
    TEST_ASSERT_TRUE(
        rfbridge::web_origin_matches_ipv4("http://192.168.1.17", device_ip, 80));
    TEST_ASSERT_TRUE(
        rfbridge::web_origin_matches_ipv4("http://192.168.1.17:80", device_ip, 80));
    TEST_ASSERT_FALSE(rfbridge::web_origin_matches_ipv4("null", device_ip, 80));

    char escaped[64]{};
    TEST_ASSERT_TRUE(rfbridge::escape_web_html("<&>\"'", escaped, sizeof(escaped)));
    TEST_ASSERT_EQUAL_STRING("&lt;&amp;&gt;&quot;&#39;", escaped);
    char small[4]{};
    TEST_ASSERT_FALSE(rfbridge::escape_web_html("<", small, sizeof(small)));
}
