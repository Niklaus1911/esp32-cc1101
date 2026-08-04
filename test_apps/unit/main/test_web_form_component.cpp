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
