#include <cstring>

#include "rf_signals_match.hpp"
#include "unity.h"

TEST_CASE("learned signal matching is bounded and ambiguity aware", "[rf_signals]")
{
    rfbridge::RfStoredSignal incoming{};
    incoming.decoded.code = 0xA88142;
    incoming.decoded.pulse_us = 386;
    incoming.decoded.bits = 24;
    incoming.decoded.protocol = 1;

    rfbridge::LearnedSignalEntry entries[2]{};
    std::strcpy(entries[0].name.value, "gate");
    entries[0].signal = incoming;
    std::strcpy(entries[1].name.value, "porch");
    entries[1].signal = incoming;

    rfbridge::LearnedMatch match =
        rfbridge::find_learned_signal_match(incoming, entries, 1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::LearnedMatchKind::kUnique),
                      static_cast<int>(match.kind));
    TEST_ASSERT_EQUAL_UINT16(1, match.count);
    TEST_ASSERT_EQUAL_STRING("gate", match.name);

    match = rfbridge::find_learned_signal_match(incoming, entries, 2, true);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::LearnedMatchKind::kAmbiguous),
                      static_cast<int>(match.kind));
    TEST_ASSERT_EQUAL_UINT16(2, match.count);
    TEST_ASSERT_EQUAL_CHAR('\0', match.name[0]);

    match = rfbridge::find_learned_signal_match(incoming, nullptr, 0, false);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::LearnedMatchKind::kUnavailable),
                      static_cast<int>(match.kind));
}

TEST_CASE("learning window excludes pre-arm and deadline frames", "[rf_signals]")
{
    using rfbridge::RfLearnFrameDisposition;
    TEST_ASSERT_EQUAL(
        static_cast<int>(RfLearnFrameDisposition::kCapture),
        static_cast<int>(rfbridge::classify_learn_frame_window(1000, 31001000, 1000, 1000)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(RfLearnFrameDisposition::kTimeout),
        static_cast<int>(rfbridge::classify_learn_frame_window(1000, 31001000, 31001000, 2000)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(RfLearnFrameDisposition::kIgnore),
        static_cast<int>(rfbridge::classify_learn_frame_window(1000, 31001000, 2000, 999)));
}
