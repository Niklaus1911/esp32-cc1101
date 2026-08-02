#include <cstdint>

#include "rf_console_parse.hpp"
#include "unity.h"

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
