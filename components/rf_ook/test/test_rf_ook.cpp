#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>

#include "../private_include/rf_activity_led_policy.hpp"
#include "rf_ook.hpp"
#include "unity.h"

namespace {

void append_pair(uint8_t *levels, uint16_t *durations, std::size_t *count, uint8_t start_level,
                 uint16_t first, uint16_t second)
{
    levels[*count] = start_level;
    durations[(*count)++] = first;
    levels[*count] = start_level == 0 ? 1 : 0;
    durations[(*count)++] = second;
}

void append_protocol1_frame(uint8_t *levels, uint16_t *durations, std::size_t *count, uint64_t code,
                            uint8_t bits, uint16_t unit = 350)
{
    for (int bit = bits - 1; bit >= 0; --bit) {
        if (((code >> bit) & 1U) != 0) {
            append_pair(levels, durations, count, 1, unit * 3, unit);
        } else {
            append_pair(levels, durations, count, 1, unit, unit * 3);
        }
    }
    append_pair(levels, durations, count, 1, unit, unit * 31);
}

void append_protocol2_frame(uint8_t *levels, uint16_t *durations, std::size_t *count, uint64_t code,
                            uint8_t bits, uint16_t unit = 100)
{
    for (int bit = bits - 1; bit >= 0; --bit) {
        if (((code >> bit) & 1U) != 0) {
            append_pair(levels, durations, count, 1, unit * 2, unit);
        } else {
            append_pair(levels, durations, count, 1, unit, unit * 2);
        }
    }
    append_pair(levels, durations, count, 1, unit, unit * 10);
}

}  // namespace

TEST_CASE("RF activity LED policy validates GPIO polarity and deadlines", "[rf_ook][led]")
{
    const int radio_gpios[] = {18, 19, 23, 27, 26, 25};
    rfbridge::RfActivityLedConfig config{
        .enabled = true,
        .gpio = 2,
        .active_high = true,
        .pulse_ms = 25,
    };
    TEST_ASSERT_TRUE(rfbridge::rf_activity_led_config_is_valid(
        config, radio_gpios, std::size(radio_gpios)));
    TEST_ASSERT_EQUAL_UINT8(1, rfbridge::rf_activity_led_active_level(config));
    TEST_ASSERT_EQUAL_UINT8(0, rfbridge::rf_activity_led_inactive_level(config));
    TEST_ASSERT_EQUAL_UINT64(25000, rfbridge::rf_activity_led_pulse_us(config));

    config.active_high = false;
    TEST_ASSERT_EQUAL_UINT8(0, rfbridge::rf_activity_led_active_level(config));
    TEST_ASSERT_EQUAL_UINT8(1, rfbridge::rf_activity_led_inactive_level(config));
    config.gpio = 25;
    TEST_ASSERT_FALSE(rfbridge::rf_activity_led_config_is_valid(
        config, radio_gpios, std::size(radio_gpios)));
    for (const int unsupported_strapping_gpio : {0, 5, 12, 15}) {
        config.gpio = unsupported_strapping_gpio;
        TEST_ASSERT_FALSE(rfbridge::rf_activity_led_config_is_valid(
            config, radio_gpios, std::size(radio_gpios)));
    }
    config.enabled = false;
    config.gpio = -1;
    TEST_ASSERT_TRUE(rfbridge::rf_activity_led_config_is_valid(config, nullptr, 0));

    const rfbridge::RfActivityLedDeadlineDecision pending =
        rfbridge::rf_activity_led_deadline_decision(5000, 5001);
    const rfbridge::RfActivityLedDeadlineDecision expired =
        rfbridge::rf_activity_led_deadline_decision(5000, 5000);
    TEST_ASSERT_FALSE(pending.turn_off);
    TEST_ASSERT_EQUAL_UINT64(1, pending.rearm_us);
    TEST_ASSERT_TRUE(expired.turn_off);
    TEST_ASSERT_EQUAL_UINT64(0, expired.rearm_us);
}

TEST_CASE("one idle-bounded protocol 1 frame is marked single decoded", "[rf_ook][short_tap]")
{
    uint8_t levels[64]{};
    uint16_t durations[64]{};
    std::size_t count = 0;
    constexpr uint64_t code = 11043138;
    append_protocol1_frame(levels, durations, &count, code, 24, 386);

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfFrameConfidence::kSingleDecoded),
                      static_cast<int>(frame.confidence));
    TEST_ASSERT_EQUAL_UINT8(1, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(24, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(code, frame.decoded.code);
    TEST_ASSERT_EQUAL_UINT16(1, frame.observed_repeats);
}

TEST_CASE("unbounded capture-start suffix is not trusted", "[rf_ook][short_tap]")
{
    uint8_t levels[32]{};
    uint16_t durations[32]{};
    std::size_t count = 0;
    append_protocol1_frame(levels, durations, &count, 0xA5, 8, 386);

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, false));
}

TEST_CASE("two-frame tap uses the complete first frame before terminal idle", "[rf_ook][short_tap]")
{
    uint8_t levels[128]{};
    uint16_t durations[128]{};
    std::size_t count = 0;
    constexpr uint64_t code = 11043138;
    append_protocol1_frame(levels, durations, &count, code, 24, 386);
    append_protocol1_frame(levels, durations, &count, code, 24, 386);
    durations[count - 1U] = 30000;

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfFrameConfidence::kSingleDecoded),
                      static_cast<int>(frame.confidence));
    TEST_ASSERT_EQUAL_UINT8(1, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(24, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(code, frame.decoded.code);
    TEST_ASSERT_EQUAL_UINT16(1, frame.observed_repeats);
}

TEST_CASE("untrusted first frame plus terminal idle is not repeated evidence", "[rf_ook][short_tap]")
{
    uint8_t levels[128]{};
    uint16_t durations[128]{};
    std::size_t count = 0;
    append_protocol1_frame(levels, durations, &count, 11043138, 24, 386);
    append_protocol1_frame(levels, durations, &count, 11043138, 24, 386);
    durations[count - 1U] = 30000;

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, false));
}

TEST_CASE("terminal-only protocol alias is not guessed", "[rf_ook][short_tap]")
{
    uint8_t levels[64]{};
    uint16_t durations[64]{};
    std::size_t count = 0;
    append_protocol1_frame(levels, durations, &count, 11043138, 24, 386);
    durations[count - 1U] = 30000;

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
}

TEST_CASE("terminal idle cannot complete a protocol 8 data prefix", "[rf_ook][short_tap]")
{
    constexpr uint16_t unit = 200;
    uint8_t levels[16]{};
    uint16_t durations[16]{};
    std::size_t count = 0;
    append_pair(levels, durations, &count, 1, unit * 3, unit * 130);
    for (int bit = 0; bit < 4; ++bit) {
        append_pair(levels, durations, &count, 1, unit * 3, unit * 16);
    }
    append_pair(levels, durations, &count, 1, unit * 3, 30000);

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
}

TEST_CASE("single decoded fallback is disabled for truncation suspicion", "[rf_ook][short_tap]")
{
    uint8_t levels[64]{};
    uint16_t durations[64]{};
    std::size_t count = 0;
    append_pair(levels, durations, &count, 1, 386, 386 * 31);
    append_protocol1_frame(levels, durations, &count, 11043138, 24, 386);

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, true, true));
}

TEST_CASE("rc-switch protocol 1 repeated frame decodes", "[rf_ook]")
{
    uint8_t levels[64]{};
    uint16_t durations[64]{};
    std::size_t count = 0;
    constexpr uint8_t code = 0xA5;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int bit = 7; bit >= 0; --bit) {
            if (((code >> bit) & 1U) != 0) {
                append_pair(levels, durations, &count, 1, 1050, 350);
            } else {
                append_pair(levels, durations, &count, 1, 350, 1050);
            }
        }
        append_pair(levels, durations, &count, 1, 350, 10850);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT8(1, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(8, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(code, frame.decoded.code);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(3, frame.observed_repeats);
}

TEST_CASE("inverted rc-switch protocol decodes", "[rf_ook]")
{
    uint8_t levels[32]{};
    uint16_t durations[32]{};
    std::size_t count = 0;
    constexpr uint8_t code = 0x0A;
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (int bit = 3; bit >= 0; --bit) {
            if (((code >> bit) & 1U) != 0) {
                append_pair(levels, durations, &count, 0, 540, 270);
            } else {
                append_pair(levels, durations, &count, 0, 270, 540);
            }
        }
        append_pair(levels, durations, &count, 0, 9720, 270);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT8(11, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(4, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(code, frame.decoded.code);
}

TEST_CASE("stable unknown waveform uses raw fallback", "[rf_ook]")
{
    constexpr uint16_t pattern[] = {210, 740, 330, 910, 460, 620, 270, 830, 510, 570,
                                    390, 680, 240, 960, 430, 650, 310, 790, 550, 520};
    uint8_t levels[40]{};
    uint16_t durations[40]{};
    for (std::size_t repeat = 0; repeat < 2; ++repeat) {
        for (std::size_t index = 0; index < 20; ++index) {
            const std::size_t output = repeat * 20 + index;
            levels[output] = index & 1U;
            durations[output] = pattern[index];
        }
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, 40, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kRaw), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT16(20, frame.raw.count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(2, frame.observed_repeats);

    rfbridge::RfFrame jittered = frame;
    for (std::size_t index = 0; index < jittered.raw.count; ++index) {
        jittered.raw.durations_us[index] += 30;
    }
    TEST_ASSERT_TRUE(rfbridge::rf_frames_equivalent(frame, jittered));
}

TEST_CASE("terminal RMT stop pulse is censored for raw recovery", "[rf_ook][raw]")
{
    constexpr uint16_t pattern[] = {210, 740, 330, 910, 460, 620, 270, 830, 510, 570,
                                    390, 680, 240, 880, 430, 650, 310, 790, 550, 1200};
    uint8_t levels[41]{};
    uint16_t durations[41]{};
    for (std::size_t repeat = 0; repeat < 2; ++repeat) {
        for (std::size_t index = 0; index < 20; ++index) {
            const std::size_t output = repeat * 20 + index;
            levels[output] = output & 1U;
            durations[output] = pattern[index];
        }
    }

    durations[39] = 30000;
    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, 40, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kRaw), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT16(20, frame.raw.count);
    TEST_ASSERT_EQUAL_UINT16(2, frame.observed_repeats);
    for (std::size_t index = 0; index < frame.raw.count; ++index) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(rfbridge::kMaximumPulseDurationUs, frame.raw.durations_us[index]);
    }

    durations[39] = pattern[19];
    levels[40] = 0;
    durations[40] = 30000;
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, 41, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kRaw), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT16(20, frame.raw.count);
}

TEST_CASE("capture validation rejects malformed levels topology and durations", "[rf_ook][validation]")
{
    constexpr uint16_t pattern[] = {210, 740, 330, 910, 460, 620, 270, 830};
    uint8_t levels[16]{};
    uint16_t durations[16]{};
    for (std::size_t index = 0; index < 16; ++index) {
        levels[index] = index & 1U;
        durations[index] = pattern[index % 8U];
    }
    rfbridge::RfFrame frame{};

    levels[3] = 2;
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, 16, &frame, false, true));
    levels[3] = 1;
    levels[4] = 1;
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, 16, &frame, false, true));
    levels[4] = 0;

    durations[3] = rfbridge::kMinimumRawPulseUs - 1U;
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, 16, &frame, false, true));
    durations[3] = pattern[3];
    durations[5] = rfbridge::kMaximumPulseDurationUs + 1U;
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, 16, &frame, false, true));
    durations[5] = pattern[5];
    durations[15] = 29999;
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, 16, &frame, false, true));
}

TEST_CASE("long protocol hold beyond span cache still decodes", "[rf_ook][matching]")
{
    uint8_t levels[400]{};
    uint16_t durations[400]{};
    std::size_t count = 0;
    for (int repeat = 0; repeat < 33; ++repeat) {
        append_protocol1_frame(levels, durations, &count, 0x0A, 4);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT8(1, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT64(0x0A, frame.decoded.code);
    TEST_ASSERT_EQUAL_UINT16(20, frame.observed_repeats);
}

TEST_CASE("maximum raw period survives terminal RMT stop censoring", "[rf_ook][raw]")
{
    constexpr std::size_t period = rfbridge::kMaxRawPulses;
    uint8_t levels[period * 2U]{};
    uint16_t durations[period * 2U]{};
    for (std::size_t repeat = 0; repeat < 2; ++repeat) {
        for (std::size_t index = 0; index < period; ++index) {
            const std::size_t output = repeat * period + index;
            levels[output] = output & 1U;
            durations[output] = index + 1U == period ? 5000 : static_cast<uint16_t>(110 + index * 7U);
        }
    }
    durations[period * 2U - 1U] = 30000;

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, period * 2U, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kRaw), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT16(period, frame.raw.count);
    TEST_ASSERT_EQUAL_UINT16(2, frame.observed_repeats);
}

TEST_CASE("protocol 8 all-ones frame wins ambiguous decode", "[rf_ook]")
{
    uint8_t levels[64]{};
    uint16_t durations[64]{};
    std::size_t count = 0;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int bit = 0; bit < 8; ++bit) {
            append_pair(levels, durations, &count, 1, 600, 3200);
        }
        append_pair(levels, durations, &count, 1, 600, 26000);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT8(8, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(8, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(0xFFU, frame.decoded.code);
}

TEST_CASE("odd partial prefix is rejected without underflow", "[rf_ook]")
{
    uint8_t levels[16]{};
    uint16_t durations[16]{};
    std::size_t count = 0;
    levels[count] = 0;
    durations[count++] = 500;
    for (int bit = 0; bit < 4; ++bit) {
        append_pair(levels, durations, &count, 1, 350, 1050);
    }
    append_pair(levels, durations, &count, 1, 350, 10850);

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
}

TEST_CASE("protocol 9 all-ones frame preserves inverted phase", "[rf_ook]")
{
    uint8_t levels[64]{};
    uint16_t durations[64]{};
    std::size_t count = 0;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int bit = 0; bit < 8; ++bit) {
            append_pair(levels, durations, &count, 0, 3200, 600);
        }
        append_pair(levels, durations, &count, 0, 26000, 1400);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT8(9, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(8, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(0xFFU, frame.decoded.code);
}

TEST_CASE("observed 24-bit protocol 1 code stays decoded", "[rf_ook][matching]")
{
    uint8_t levels[160]{};
    uint16_t durations[160]{};
    std::size_t count = 0;
    constexpr uint64_t code = 11043138;
    for (int repeat = 0; repeat < 3; ++repeat) {
        append_protocol1_frame(levels, durations, &count, code, 24, 386);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT8(1, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(24, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(code, frame.decoded.code);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(3, frame.observed_repeats);
}

TEST_CASE("repeated protocol consensus outranks a longer prefix candidate", "[rf_ook][matching]")
{
    uint8_t levels[80]{};
    uint16_t durations[80]{};
    std::size_t count = 0;
    append_pair(levels, durations, &count, 1, 350, 1050);
    for (int repeat = 0; repeat < 3; ++repeat) {
        append_protocol1_frame(levels, durations, &count, 0xA5, 8);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT8(8, frame.decoded.bits);
    TEST_ASSERT_EQUAL_UINT64(0xA5, frame.decoded.code);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(2, frame.observed_repeats);
}

TEST_CASE("raw fallback retains the complete frame instead of an internal motif", "[rf_ook][matching]")
{
    uint8_t levels[160]{};
    uint16_t durations[160]{};
    std::size_t count = 0;
    for (int repeat = 0; repeat < 3; ++repeat) {
        const std::size_t frame_start = count;
        append_protocol1_frame(levels, durations, &count, 0, 24);
        durations[frame_start] = 700;
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kRaw), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT16(50, frame.raw.count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(3, frame.observed_repeats);
}

TEST_CASE("one changed raw bit is a different identity", "[rf_ook][matching]")
{
    rfbridge::DecodedSignal left_decoded{};
    left_decoded.code = 0;
    left_decoded.bits = 24;
    left_decoded.protocol = 1;
    left_decoded.pulse_us = 350;
    rfbridge::DecodedSignal right_decoded = left_decoded;
    right_decoded.code = 1;

    rfbridge::RawSignal left_raw{};
    rfbridge::RawSignal right_raw{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(left_decoded, &left_raw));
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(right_decoded, &right_raw));
    TEST_ASSERT_TRUE(rfbridge::raw_signal_matches_decoded(left_raw, left_decoded));
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(left_raw, right_raw));
}

TEST_CASE("truncation-suspected captures cannot become raw", "[rf_ook][matching]")
{
    constexpr uint16_t pattern[] = {210, 740, 330, 910, 460, 620, 270, 830, 510, 570,
                                    390, 680, 240, 960, 430, 650, 310, 790, 550, 520};
    uint8_t levels[40]{};
    uint16_t durations[40]{};
    for (std::size_t repeat = 0; repeat < 2; ++repeat) {
        for (std::size_t index = 0; index < 20; ++index) {
            const std::size_t output = repeat * 20 + index;
            levels[output] = index & 1U;
            durations[output] = pattern[index];
        }
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, 40, &frame, true));
}

TEST_CASE("short-pulse raw identities do not merge changed bits", "[rf_ook][matching]")
{
    rfbridge::DecodedSignal left{};
    left.code = 0;
    left.bits = 4;
    left.protocol = 2;
    left.pulse_us = 100;
    rfbridge::DecodedSignal right = left;
    right.code = 1;

    rfbridge::RawSignal left_raw{};
    rfbridge::RawSignal right_raw{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(left, &left_raw));
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(right, &right_raw));
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(left_raw, right_raw));
    TEST_ASSERT_FALSE(rfbridge::raw_signal_matches_decoded(left_raw, right));
}

TEST_CASE("long raw holds keep the canonical period", "[rf_ook][matching]")
{
    constexpr uint16_t pattern[] = {210, 740, 330, 910, 460, 620, 270, 830, 510, 570,
                                    390, 680, 240, 960, 430, 650, 310, 790, 550, 520};
    uint8_t levels[420]{};
    uint16_t durations[420]{};
    for (std::size_t repeat = 0; repeat < 21; ++repeat) {
        for (std::size_t index = 0; index < 20; ++index) {
            const std::size_t output = repeat * 20 + index;
            levels[output] = index & 1U;
            durations[output] = pattern[index];
        }
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, 420, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kRaw), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT16(20, frame.raw.count);
    TEST_ASSERT_EQUAL_UINT16(20, frame.observed_repeats);
}

TEST_CASE("canonical raw identity tolerates cyclic capture phase", "[rf_ook][matching]")
{
    rfbridge::DecodedSignal decoded{};
    decoded.code = 11043138;
    decoded.bits = 24;
    decoded.protocol = 1;
    decoded.pulse_us = 386;
    rfbridge::RawSignal original{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(decoded, &original));

    constexpr std::size_t shift = 7;
    rfbridge::RawSignal rotated{};
    rotated.count = original.count;
    rotated.start_level = static_cast<uint8_t>(original.start_level ^ (shift & 1U));
    for (std::size_t index = 0; index < original.count; ++index) {
        rotated.durations_us[index] = original.durations_us[(index + shift) % original.count];
    }
    TEST_ASSERT_TRUE(rfbridge::raw_signals_match(original, rotated));
}

TEST_CASE("nearby unknown PWM symbols remain distinct", "[rf_ook][matching]")
{
    rfbridge::RawSignal left{};
    rfbridge::RawSignal right{};
    left.count = 8;
    right.count = 8;
    left.start_level = 1;
    right.start_level = 1;
    constexpr uint16_t left_pulses[] = {1000, 1250, 400, 400, 400, 400, 400, 3000};
    constexpr uint16_t right_pulses[] = {1250, 1000, 400, 400, 400, 400, 400, 3000};
    for (std::size_t index = 0; index < left.count; ++index) {
        left.durations_us[index] = left_pulses[index];
        right.durations_us[index] = right_pulses[index];
    }
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(left, right));
}

TEST_CASE("different short-pulse frames cannot form raw consensus", "[rf_ook][matching]")
{
    uint8_t levels[24]{};
    uint16_t durations[24]{};
    std::size_t count = 0;
    append_protocol2_frame(levels, durations, &count, 0, 4);
    append_protocol2_frame(levels, durations, &count, 1, 4);

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
}

TEST_CASE("protocols 11 and 12 use nominal timing to stay distinct", "[rf_ook][matching]")
{
    for (uint8_t protocol : {static_cast<uint8_t>(11), static_cast<uint8_t>(12)}) {
        const uint16_t unit = protocol == 11 ? 270 : 320;
        uint8_t levels[32]{};
        uint16_t durations[32]{};
        std::size_t count = 0;
        for (int repeat = 0; repeat < 2; ++repeat) {
            for (int bit = 3; bit >= 0; --bit) {
                const bool one = ((0x0AU >> bit) & 1U) != 0;
                append_pair(levels, durations, &count, 0, unit * (one ? 2 : 1), unit * (one ? 1 : 2));
            }
            append_pair(levels, durations, &count, 0, unit * 36, unit);
        }

        rfbridge::RfFrame frame{};
        TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
        TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kDecoded), static_cast<int>(frame.encoding));
        TEST_ASSERT_EQUAL_UINT8(protocol, frame.decoded.protocol);
        TEST_ASSERT_EQUAL_UINT64(0x0A, frame.decoded.code);
    }
}

TEST_CASE("single protocol 11 and 12 timing midpoint stays ambiguous", "[rf_ook][matching][short_tap]")
{
    constexpr uint16_t unit = 295;
    uint8_t levels[16]{};
    uint16_t durations[16]{};
    std::size_t count = 0;
    append_pair(levels, durations, &count, 0, unit * 36, unit);
    for (int bit = 3; bit >= 0; --bit) {
        const bool one = ((0x0AU >> bit) & 1U) != 0;
        append_pair(levels, durations, &count, 0, unit * (one ? 2 : 1), unit * (one ? 1 : 2));
    }
    append_pair(levels, durations, &count, 0, unit * 36, unit);

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
}

TEST_CASE("protocol 11 and 12 timing midpoint stays ambiguous", "[rf_ook][matching]")
{
    constexpr uint16_t unit = 295;
    uint8_t levels[32]{};
    uint16_t durations[32]{};
    std::size_t count = 0;
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (int bit = 3; bit >= 0; --bit) {
            const bool one = ((0x0AU >> bit) & 1U) != 0;
            append_pair(levels, durations, &count, 0, unit * (one ? 2 : 1), unit * (one ? 1 : 2));
        }
        append_pair(levels, durations, &count, 0, unit * 36, unit);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_FALSE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
}

TEST_CASE("raw promotion requires one complete known protocol period", "[rf_ook][matching]")
{
    rfbridge::DecodedSignal embedded{};
    embedded.code = 0xA5;
    embedded.bits = 8;
    embedded.protocol = 1;
    embedded.pulse_us = 350;
    rfbridge::RawSignal period{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(embedded, &period));
    period.durations_us[period.count++] = 500;
    period.durations_us[period.count++] = 20000;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RawProtocolIdentity::kUnknown),
                      static_cast<int>(rfbridge::identify_raw_protocol(period, nullptr)));

    uint8_t levels[64]{};
    uint16_t durations[64]{};
    for (std::size_t repeat = 0; repeat < 3; ++repeat) {
        for (std::size_t index = 0; index < period.count; ++index) {
            const std::size_t output = repeat * period.count + index;
            levels[output] = static_cast<uint8_t>(period.start_level ^ (index & 1U));
            durations[output] = period.durations_us[index];
        }
    }
    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, period.count * 3U, &frame, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfEncoding::kRaw), static_cast<int>(frame.encoding));
    TEST_ASSERT_EQUAL_UINT16(period.count, frame.raw.count);
}

TEST_CASE("raw identity cannot bridge protocols 3 and 5", "[rf_ook][matching]")
{
    rfbridge::DecodedSignal protocol3{};
    protocol3.code = 0xA5;
    protocol3.bits = 8;
    protocol3.protocol = 3;
    protocol3.pulse_us = 100;
    rfbridge::DecodedSignal protocol5 = protocol3;
    protocol5.protocol = 5;
    protocol5.pulse_us = 500;

    rfbridge::RawSignal raw3{};
    rfbridge::RawSignal raw5{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(protocol3, &raw3));
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(protocol5, &raw5));
    TEST_ASSERT_TRUE(rfbridge::raw_signal_matches_decoded(raw3, protocol3));
    TEST_ASSERT_TRUE(rfbridge::raw_signal_matches_decoded(raw5, protocol5));
    TEST_ASSERT_FALSE(rfbridge::raw_signal_matches_decoded(raw3, protocol5));
    TEST_ASSERT_FALSE(rfbridge::raw_signal_matches_decoded(raw5, protocol3));
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(raw3, raw5));
}

TEST_CASE("raw topology cannot collapse a long symbol to equality", "[rf_ook][matching]")
{
    rfbridge::RawSignal distinct{};
    rfbridge::RawSignal collapsed{};
    distinct.count = collapsed.count = 8;
    distinct.start_level = collapsed.start_level = 1;
    constexpr uint16_t distinct_pulses[] = {1000, 1250, 400, 400, 400, 400, 400, 3000};
    constexpr uint16_t collapsed_pulses[] = {1125, 1125, 400, 400, 400, 400, 400, 3000};
    for (std::size_t index = 0; index < distinct.count; ++index) {
        distinct.durations_us[index] = distinct_pulses[index];
        collapsed.durations_us[index] = collapsed_pulses[index];
    }
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(distinct, collapsed));
}

TEST_CASE("unknown extensions cannot merge protocol 11 and 12 timing", "[rf_ook][matching]")
{
    rfbridge::DecodedSignal protocol11{};
    protocol11.code = 0x0A;
    protocol11.bits = 4;
    protocol11.protocol = 11;
    protocol11.pulse_us = 289;
    rfbridge::DecodedSignal protocol12 = protocol11;
    protocol12.protocol = 12;
    protocol12.pulse_us = 301;

    rfbridge::RawSignal raw11{};
    rfbridge::RawSignal raw12{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(protocol11, &raw11));
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(protocol12, &raw12));
    raw11.durations_us[raw11.count++] = 500;
    raw11.durations_us[raw11.count++] = 20000;
    raw12.durations_us[raw12.count++] = 500;
    raw12.durations_us[raw12.count++] = 20000;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RawProtocolIdentity::kUnknown),
                      static_cast<int>(rfbridge::identify_raw_protocol(raw11, nullptr)));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RawProtocolIdentity::kUnknown),
                      static_cast<int>(rfbridge::identify_raw_protocol(raw12, nullptr)));
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(raw11, raw12));
}

TEST_CASE("raw protocol 11 and 12 midpoint uses decoder ambiguity policy", "[rf_ook][matching]")
{
    rfbridge::DecodedSignal midpoint{};
    midpoint.code = 0x0A;
    midpoint.bits = 4;
    midpoint.protocol = 12;
    midpoint.pulse_us = 297;
    rfbridge::RawSignal raw{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(midpoint, &raw));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RawProtocolIdentity::kAmbiguous),
                      static_cast<int>(rfbridge::identify_raw_protocol(raw, nullptr)));
}

TEST_CASE("raw topology cannot change from weak to strongly ordered", "[rf_ook][matching]")
{
    rfbridge::RawSignal weak{};
    rfbridge::RawSignal strong{};
    weak.count = strong.count = 8;
    weak.start_level = strong.start_level = 1;
    constexpr uint16_t weak_pulses[] = {560, 500, 400, 400, 400, 400, 400, 3000};
    constexpr uint16_t strong_pulses[] = {620, 500, 400, 400, 400, 400, 400, 3000};
    for (std::size_t index = 0; index < weak.count; ++index) {
        weak.durations_us[index] = weak_pulses[index];
        strong.durations_us[index] = strong_pulses[index];
    }
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(weak, strong));
}

TEST_CASE("maximum raw periods preserve exact cyclic identity", "[rf_ook][matching]")
{
    rfbridge::RawSignal original{};
    rfbridge::RawSignal rotated{};
    original.count = rotated.count = rfbridge::kMaxRawPulses;
    original.start_level = rotated.start_level = 1;
    for (std::size_t index = 0; index < original.count; ++index) {
        const int phase = static_cast<int>(index % 12U);
        const int distance = phase > 6 ? phase - 6 : 6 - phase;
        original.durations_us[index] = static_cast<uint16_t>(850 + 50 * distance);
    }
    constexpr std::size_t shift = 2;
    for (std::size_t index = 0; index < original.count; ++index) {
        rotated.durations_us[index] = original.durations_us[(index + shift) % original.count];
    }
    TEST_ASSERT_TRUE(rfbridge::raw_signals_match(original, rotated));
}

TEST_CASE("protocol alias averaging preserves a canonical decoded unit", "[rf_ook][matching]")
{
    uint8_t levels[32]{};
    uint16_t durations[32]{};
    std::size_t count = 0;
    for (uint16_t unit : {static_cast<uint16_t>(300), static_cast<uint16_t>(301)}) {
        for (int bit = 3; bit >= 0; --bit) {
            const bool one = ((0x0AU >> bit) & 1U) != 0;
            append_pair(levels, durations, &count, 0, unit * (one ? 2 : 1), unit * (one ? 1 : 2));
        }
        append_pair(levels, durations, &count, 0, unit * 36, unit);
    }

    rfbridge::RfFrame frame{};
    TEST_ASSERT_TRUE(rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true));
    TEST_ASSERT_EQUAL_UINT8(12, frame.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT16(301, frame.decoded.pulse_us);
    rfbridge::RawSignal raw{};
    rfbridge::DecodedSignal identified{};
    TEST_ASSERT_TRUE(rfbridge::build_decoded_raw(frame.decoded, &raw));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RawProtocolIdentity::kDecoded),
                      static_cast<int>(rfbridge::identify_raw_protocol(raw, &identified)));
    TEST_ASSERT_TRUE(rfbridge::decoded_signals_match(frame.decoded, identified));
}

TEST_CASE("raw matching examines every compatible cyclic alignment", "[rf_ook][matching]")
{
    constexpr uint16_t left_pulses[] = {580, 570, 540, 610, 530, 570, 550, 600, 610, 610, 580, 620};
    constexpr uint16_t right_pulses[] = {620, 530, 530, 610, 570, 590, 600, 620, 530, 590, 580, 620};
    rfbridge::RawSignal left{};
    rfbridge::RawSignal right{};
    left.count = right.count = 12;
    left.start_level = 1;
    right.start_level = 0;
    for (std::size_t index = 0; index < left.count; ++index) {
        left.durations_us[index] = left_pulses[index];
        right.durations_us[index] = right_pulses[index];
    }
    TEST_ASSERT_TRUE(rfbridge::raw_signals_match(left, right));
}

TEST_CASE("decoded rc-switch codes require at least four bits", "[rf_ook][matching]")
{
    for (uint8_t bits : {static_cast<uint8_t>(1), static_cast<uint8_t>(2), static_cast<uint8_t>(3)}) {
        rfbridge::DecodedSignal signal{};
        signal.code = (1U << bits) - 1U;
        signal.bits = bits;
        signal.protocol = 1;
        signal.pulse_us = 350;
        rfbridge::RawSignal raw{};
        TEST_ASSERT_FALSE(rfbridge::decoded_signal_is_valid(signal));
        TEST_ASSERT_FALSE(rfbridge::build_decoded_raw(signal, &raw));

        uint8_t levels[32]{};
        uint16_t durations[32]{};
        std::size_t count = 0;
        for (int repeat = 0; repeat < 3; ++repeat) {
            append_protocol1_frame(levels, durations, &count, signal.code, bits);
        }
        rfbridge::RfFrame frame{};
        const bool received = rfbridge::decode_rf_pulses(levels, durations, count, &frame, false, true);
        TEST_ASSERT_FALSE(received && frame.encoding == rfbridge::RfEncoding::kDecoded);
    }
}

TEST_CASE("raw PWM ordering cannot collapse into equal symbols", "[rf_ook][matching]")
{
    rfbridge::RawSignal ordered{};
    rfbridge::RawSignal equal{};
    ordered.count = equal.count = 10;
    ordered.start_level = equal.start_level = 1;
    for (std::size_t index = 0; index < 9; ++index) {
        ordered.durations_us[index] = (index & 1U) != 0 ? 1105 : 870;
        equal.durations_us[index] = 940;
    }
    ordered.durations_us[9] = equal.durations_us[9] = 4000;
    TEST_ASSERT_FALSE(rfbridge::raw_signals_match(ordered, equal));
}

TEST_CASE("software RF status remains available while service is stopped", "[rf_ook][status]")
{
    rfbridge::RfRadioStatus status{};
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::get_rf_radio_status(&status));
    TEST_ASSERT_FALSE(status.running);
    TEST_ASSERT_FALSE(status.transmitting);
    TEST_ASSERT_FALSE(status.cc1101_info_valid);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, status.cc1101_error);
}
