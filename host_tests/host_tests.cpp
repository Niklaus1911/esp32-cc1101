#include <cstdint>
#include <cstdio>
#include <iterator>
#include <cstdlib>

#include "rf_codec.hpp"
#include "rf_console_parse.hpp"

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
    std::puts("All host RF tests passed");
    return 0;
}
