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
