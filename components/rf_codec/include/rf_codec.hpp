#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

constexpr std::size_t kMaxRawPulses = 256;
constexpr uint16_t kMinimumRawPulseUs = 100;
constexpr uint16_t kMaximumPulseDurationUs = 29000;
constexpr std::size_t kRfProtocolCount = 12;
constexpr uint16_t kRfProtocolAliasAmbiguousMinimumUs = 290;
constexpr uint16_t kRfProtocolAliasAmbiguousMaximumUs = 300;

struct PulsePair {
    uint8_t first;
    uint8_t second;
};

struct RfProtocol {
    uint16_t pulse_us;
    PulsePair sync;
    PulsePair zero;
    PulsePair one;
    bool inverted;
};

struct DecodedSignal {
    uint64_t code;
    uint16_t pulse_us;
    uint8_t bits;
    uint8_t protocol;
    uint8_t inverted;
    uint8_t reserved[3];
};

struct RawSignal {
    uint16_t count;
    uint8_t start_level;
    uint8_t reserved;
    uint16_t durations_us[kMaxRawPulses];
};

enum class RawProtocolIdentity {
    kUnknown,
    kDecoded,
    kAmbiguous,
};

const RfProtocol *rf_protocol(uint8_t protocol_number);
uint8_t rf_protocol_min_factor(uint8_t protocol_number);
uint8_t rf_protocol_max_factor(uint8_t protocol_number);
bool decoded_signal_is_valid(const DecodedSignal &signal);
bool decoded_signals_match(const DecodedSignal &left, const DecodedSignal &right);
bool raw_signal_is_valid(const RawSignal &signal);
RawProtocolIdentity identify_raw_protocol(const RawSignal &raw, DecodedSignal *decoded);
bool raw_signals_match(const RawSignal &left, const RawSignal &right);
bool build_decoded_raw(const DecodedSignal &signal, RawSignal *raw);
bool raw_signal_matches_decoded(const RawSignal &raw, const DecodedSignal &decoded);
uint32_t decoded_signal_fingerprint(const DecodedSignal &signal);
uint32_t raw_signal_fingerprint(const RawSignal &signal);

}  // namespace rfbridge
