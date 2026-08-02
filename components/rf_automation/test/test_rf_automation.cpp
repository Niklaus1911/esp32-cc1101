#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "rf_automation_engine.hpp"
#include "unity.h"

namespace {

rfbridge::RfStoredSignal decoded(uint64_t code)
{
    rfbridge::RfStoredSignal signal{};
    signal.encoding = rfbridge::RfStoredEncoding::kDecoded;
    signal.decoded.code = code;
    signal.decoded.pulse_us = 350;
    signal.decoded.bits = 24;
    signal.decoded.protocol = 1;
    return signal;
}

rfbridge::RfStorageRuleEntry rule(const char *trigger, const char *target)
{
    rfbridge::RfStorageRuleEntry entry{};
    std::strncpy(entry.trigger_name, trigger, sizeof(entry.trigger_name) - 1U);
    std::strncpy(entry.rule.target_name, target, sizeof(entry.rule.target_name) - 1U);
    entry.rule.repeats = 8;
    entry.rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    return entry;
}

rfbridge::RfStoredSignal uniform_raw(uint16_t duration_us)
{
    rfbridge::RfStoredSignal signal{};
    signal.encoding = rfbridge::RfStoredEncoding::kRaw;
    signal.raw.start_level = 1;
    signal.raw.count = 8;
    for (std::size_t index = 0; index < signal.raw.count; ++index) {
        signal.raw.durations_us[index] = duration_us;
    }
    return signal;
}

} // namespace

TEST_CASE("automation matching supports decoded raw equivalence", "[rf_automation]")
{
    const rfbridge::RfStoredSignal decoded_signal = decoded(0xA88142);
    rfbridge::RfStoredSignal raw_signal{};
    raw_signal.encoding = rfbridge::RfStoredEncoding::kRaw;
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(decoded_signal.decoded, &raw_signal.raw));
    TEST_ASSERT_TRUE(rfbridge::rf_stored_signals_equivalent(decoded_signal, raw_signal));

    rfbridge::RfStoredSignal different = decoded(0xA88141);
    TEST_ASSERT_FALSE(rfbridge::rf_stored_signals_equivalent(decoded_signal, different));
}

TEST_CASE("automation graph rejects self and directed cycles", "[rf_automation]")
{
    rfbridge::RfStorageRuleEntry rules[] = {rule("B", "A"), rule("C", "B")};
    TEST_ASSERT_FALSE(rfbridge::rf_rule_graph_has_cycle(rules, 2));
    TEST_ASSERT_FALSE(rfbridge::rf_rule_would_create_cycle(rules, 2, "D", "A"));
    TEST_ASSERT_TRUE(rfbridge::rf_rule_would_create_cycle(rules, 2, "A", "C"));
    TEST_ASSERT_TRUE(rfbridge::rf_rule_would_create_cycle(rules, 2, "A", "A"));

    rfbridge::RfStorageRuleEntry cyclic[] = {rule("A", "C"), rule("B", "A"), rule("C", "B")};
    TEST_ASSERT_TRUE(rfbridge::rf_rule_graph_has_cycle(cyclic, 3));
}

TEST_CASE("automation cooldown is exact and rejects stale events", "[rf_automation]")
{
    TEST_ASSERT_TRUE(rfbridge::rf_automation_cooldown_allows(0, 1000000, 1000));
    TEST_ASSERT_FALSE(rfbridge::rf_automation_cooldown_allows(1000000, 1999999, 1000));
    TEST_ASSERT_TRUE(rfbridge::rf_automation_cooldown_allows(1000000, 2000000, 1000));
    TEST_ASSERT_FALSE(rfbridge::rf_automation_cooldown_allows(1000000, 1000000, 1000));
    TEST_ASSERT_FALSE(rfbridge::rf_automation_cooldown_allows(1000000, 999999, 1000));
}

TEST_CASE("automation suppresses non-transitive ambiguous raw matches", "[rf_automation]")
{
    const rfbridge::RfStoredSignal low = uniform_raw(100);
    const rfbridge::RfStoredSignal middle = uniform_raw(108);
    const rfbridge::RfStoredSignal high = uniform_raw(116);
    TEST_ASSERT_TRUE(rfbridge::rf_stored_signals_equivalent(low, middle));
    TEST_ASSERT_TRUE(rfbridge::rf_stored_signals_equivalent(middle, high));
    TEST_ASSERT_FALSE(rfbridge::rf_stored_signals_equivalent(low, high));

    const rfbridge::RfStoredSignal *triggers[] = {&low, &high};
    std::size_t matched_index = 99;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfAutomationMatchResult::kAmbiguous),
                      static_cast<int>(rfbridge::rf_find_unique_stored_signal_match(
                          middle, triggers, std::size(triggers), &matched_index)));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfAutomationMatchResult::kUnique),
                      static_cast<int>(rfbridge::rf_find_unique_stored_signal_match(low, triggers, std::size(triggers),
                                                                                    &matched_index)));
    TEST_ASSERT_EQUAL_UINT32(0, matched_index);
}

TEST_CASE("automation rejects frames from prior rule generations", "[rf_automation]")
{
    TEST_ASSERT_TRUE(rfbridge::rf_automation_event_is_current(4, 4, 2001, 2000));
    TEST_ASSERT_TRUE(rfbridge::rf_automation_event_is_current(4, 4, 0, 2000));
    TEST_ASSERT_FALSE(rfbridge::rf_automation_event_is_current(3, 4, 3000, 2000));
    TEST_ASSERT_FALSE(rfbridge::rf_automation_event_is_current(4, 4, 2000, 2000));
    TEST_ASSERT_FALSE(rfbridge::rf_automation_event_is_current(4, 4, 1999, 2000));
}
