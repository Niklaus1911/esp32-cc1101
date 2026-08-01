#include "rf_codec.hpp"

#include <algorithm>
#include <array>
#include <climits>

namespace rfbridge {
namespace {

constexpr uint8_t kRawTimingTolerancePercent = 15;
constexpr uint8_t kRawShapeTolerancePercent = 8;
constexpr uint8_t kRawShapeNeutralPercent = 20;
constexpr uint8_t kRawScaleTolerancePercent = 8;
constexpr uint32_t kIdentityAmbiguityMargin = 25;

// Timing and numbering compatible with sui77/rc-switch (LGPL-2.1-or-later).
constexpr std::array<RfProtocol, kRfProtocolCount> kProtocols{{
    {350, {1, 31}, {1, 3}, {3, 1}, false},
    {650, {1, 10}, {1, 2}, {2, 1}, false},
    {100, {30, 71}, {4, 11}, {9, 6}, false},
    {380, {1, 6}, {1, 3}, {3, 1}, false},
    {500, {6, 14}, {1, 2}, {2, 1}, false},
    {450, {23, 1}, {1, 2}, {2, 1}, true},
    {150, {2, 62}, {1, 6}, {6, 1}, false},
    {200, {3, 130}, {7, 16}, {3, 16}, false},
    {200, {130, 7}, {16, 7}, {16, 3}, true},
    {365, {18, 1}, {3, 1}, {1, 3}, true},
    {270, {36, 1}, {1, 2}, {2, 1}, true},
    {320, {36, 1}, {1, 2}, {2, 1}, true},
}};

struct RawDecodeCandidate {
    DecodedSignal decoded;
    uint32_t score;
};

uint32_t fnv_byte(uint32_t hash, uint8_t value)
{
    return (hash ^ value) * 16777619U;
}

bool pulse_matches(uint32_t left, uint32_t right)
{
    const uint32_t largest = std::max(left, right);
    const uint32_t difference = left > right ? left - right : right - left;
    return difference <= std::max<uint32_t>(20, largest * kRawTimingTolerancePercent / 100U);
}

uint32_t normalized_duration_error(uint32_t actual, uint32_t expected)
{
    if (expected == 0) {
        return UINT32_MAX;
    }
    const uint32_t difference = actual > expected ? actual - expected : expected - actual;
    return static_cast<uint32_t>((static_cast<uint64_t>(difference) * 1000U) / expected);
}

uint8_t pulse_level(const RawSignal &signal, std::size_t index)
{
    return static_cast<uint8_t>(signal.start_level ^ (index & 1U));
}

std::size_t shifted_index(const RawSignal &signal, std::size_t shift, std::size_t index)
{
    return (shift + index) % signal.count;
}

bool fit_raw_protocol(const RawSignal &raw, std::size_t shift, uint8_t protocol_number,
                      RawDecodeCandidate *candidate)
{
    const RfProtocol *protocol = rf_protocol(protocol_number);
    const std::size_t pair_count = raw.count / 2U;
    if (protocol == nullptr || candidate == nullptr || pair_count < 2 || pair_count - 1U > 64) {
        return false;
    }

    const uint8_t active = protocol->inverted ? 0 : 1;
    for (std::size_t index = 0; index < raw.count; ++index) {
        const uint8_t expected_level = (index & 1U) == 0 ? active : static_cast<uint8_t>(active ^ 1U);
        if (pulse_level(raw, shifted_index(raw, shift, index)) != expected_level) {
            return false;
        }
    }

    const std::size_t sync_index = pair_count * 2U - 2U;
    const bool first_is_longer = protocol->sync.first >= protocol->sync.second;
    const uint8_t unit_factor = first_is_longer ? protocol->sync.first : protocol->sync.second;
    const std::size_t unit_index = sync_index + (first_is_longer ? 0U : 1U);
    if (unit_factor == 0) {
        return false;
    }
    const uint32_t unit_duration = raw.durations_us[shifted_index(raw, shift, unit_index)];
    const uint32_t unit = (unit_duration + unit_factor / 2U) / unit_factor;
    if (unit < 20 || unit > 5000 || unit > kMaximumPulseDurationUs / rf_protocol_max_factor(protocol_number)) {
        return false;
    }

    uint64_t total_error = 0;
    const auto pair_fits = [&](std::size_t index, const PulsePair &pair) {
        const uint32_t first = static_cast<uint32_t>(unit) * pair.first;
        const uint32_t second = static_cast<uint32_t>(unit) * pair.second;
        const uint32_t actual_first = raw.durations_us[shifted_index(raw, shift, index)];
        const uint32_t actual_second = raw.durations_us[shifted_index(raw, shift, index + 1U)];
        return pulse_matches(actual_first, first) && pulse_matches(actual_second, second);
    };
    if (!pair_fits(sync_index, protocol->sync)) {
        return false;
    }
    total_error += normalized_duration_error(raw.durations_us[shifted_index(raw, shift, sync_index)],
                                             static_cast<uint32_t>(unit) * protocol->sync.first);
    total_error += normalized_duration_error(raw.durations_us[shifted_index(raw, shift, sync_index + 1U)],
                                             static_cast<uint32_t>(unit) * protocol->sync.second);

    uint64_t code = 0;
    for (std::size_t index = 0; index < sync_index; index += 2) {
        const bool zero = pair_fits(index, protocol->zero);
        const bool one = pair_fits(index, protocol->one);
        if (zero == one) {
            return false;
        }
        const PulsePair &pair = one ? protocol->one : protocol->zero;
        total_error += normalized_duration_error(raw.durations_us[shifted_index(raw, shift, index)],
                                                 static_cast<uint32_t>(unit) * pair.first);
        total_error += normalized_duration_error(raw.durations_us[shifted_index(raw, shift, index + 1U)],
                                                 static_cast<uint32_t>(unit) * pair.second);
        code = (code << 1U) | static_cast<uint64_t>(one);
    }

    DecodedSignal decoded{};
    decoded.code = code;
    decoded.pulse_us = static_cast<uint16_t>(unit);
    decoded.bits = static_cast<uint8_t>(pair_count - 1U);
    decoded.protocol = protocol_number;
    decoded.inverted = protocol->inverted;
    if (!decoded_signal_is_valid(decoded)) {
        return false;
    }

    const uint32_t residual = static_cast<uint32_t>(total_error / raw.count);
    const uint32_t nominal_error = normalized_duration_error(unit, protocol->pulse_us);
    candidate->decoded = decoded;
    candidate->score = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(residual) * 2U + nominal_error, UINT32_MAX));
    return true;
}

int decode_raw_pair(const RawSignal &raw, std::size_t index, const RfProtocol &protocol, uint16_t unit)
{
    const uint8_t active = protocol.inverted ? 0 : 1;
    const std::size_t second_index = shifted_index(raw, index, 1);
    if (pulse_level(raw, index) != active || pulse_level(raw, second_index) == active) {
        return -1;
    }
    const bool zero = pulse_matches(raw.durations_us[index], static_cast<uint32_t>(unit) * protocol.zero.first) &&
                      pulse_matches(raw.durations_us[second_index],
                                    static_cast<uint32_t>(unit) * protocol.zero.second);
    const bool one = pulse_matches(raw.durations_us[index], static_cast<uint32_t>(unit) * protocol.one.first) &&
                     pulse_matches(raw.durations_us[second_index], static_cast<uint32_t>(unit) * protocol.one.second);
    if (zero == one) {
        return -1;
    }
    return one ? 1 : 0;
}

RawProtocolIdentity identify_raw_protocol_fragment(const RawSignal &raw, DecodedSignal *decoded)
{
    std::array<RawDecodeCandidate, 32> candidates{};
    std::size_t candidate_count = 0;
    bool overflowed = false;
    for (uint8_t protocol_number = 1; protocol_number <= kRfProtocolCount; ++protocol_number) {
        const RfProtocol *protocol = rf_protocol(protocol_number);
        if (protocol == nullptr) {
            continue;
        }
        const uint8_t active = protocol->inverted ? 0 : 1;
        for (std::size_t sync = 0; sync < raw.count; ++sync) {
            const std::size_t sync_second = shifted_index(raw, sync, 1);
            if (pulse_level(raw, sync) != active || pulse_level(raw, sync_second) == active) {
                continue;
            }
            const bool first_is_longer = protocol->sync.first >= protocol->sync.second;
            const uint8_t unit_factor = first_is_longer ? protocol->sync.first : protocol->sync.second;
            const std::size_t unit_index = first_is_longer ? sync : sync_second;
            if (unit_factor == 0) {
                continue;
            }
            const uint32_t unit = (raw.durations_us[unit_index] + unit_factor / 2U) / unit_factor;
            if (unit < 20 || unit > 5000 || unit > kMaximumPulseDurationUs / rf_protocol_max_factor(protocol_number) ||
                !pulse_matches(raw.durations_us[sync], static_cast<uint32_t>(unit) * protocol->sync.first) ||
                !pulse_matches(raw.durations_us[sync_second],
                               static_cast<uint32_t>(unit) * protocol->sync.second)) {
                continue;
            }

            std::size_t start = sync;
            uint8_t bits = 0;
            while (bits < 64) {
                const std::size_t previous = (start + raw.count - 2U) % raw.count;
                if (previous == sync || decode_raw_pair(raw, previous, *protocol, static_cast<uint16_t>(unit)) < 0) {
                    break;
                }
                start = previous;
                ++bits;
            }
            if (bits < 4) {
                continue;
            }

            uint64_t code = 0;
            uint64_t total_error =
                normalized_duration_error(raw.durations_us[sync],
                                          static_cast<uint32_t>(unit) * protocol->sync.first) +
                normalized_duration_error(raw.durations_us[sync_second],
                                          static_cast<uint32_t>(unit) * protocol->sync.second);
            std::size_t index = start;
            for (uint8_t bit_index = 0; bit_index < bits; ++bit_index) {
                const int bit = decode_raw_pair(raw, index, *protocol, static_cast<uint16_t>(unit));
                if (bit < 0) {
                    bits = 0;
                    break;
                }
                const PulsePair &pair = bit != 0 ? protocol->one : protocol->zero;
                const std::size_t second = shifted_index(raw, index, 1);
                total_error += normalized_duration_error(raw.durations_us[index],
                                                         static_cast<uint32_t>(unit) * pair.first);
                total_error += normalized_duration_error(raw.durations_us[second],
                                                         static_cast<uint32_t>(unit) * pair.second);
                code = (code << 1U) | static_cast<uint64_t>(bit);
                index = shifted_index(raw, index, 2);
            }
            if (bits < 4) {
                continue;
            }

            RawDecodeCandidate observed{};
            observed.decoded.code = code;
            observed.decoded.pulse_us = static_cast<uint16_t>(unit);
            observed.decoded.bits = bits;
            observed.decoded.protocol = protocol_number;
            observed.decoded.inverted = protocol->inverted;
            if (!decoded_signal_is_valid(observed.decoded)) {
                continue;
            }
            const uint32_t residual = static_cast<uint32_t>(total_error / (static_cast<uint32_t>(bits) * 2U + 2U));
            const uint32_t nominal_error = normalized_duration_error(unit, protocol->pulse_us);
            observed.score = static_cast<uint32_t>(
                std::min<uint64_t>(static_cast<uint64_t>(residual) * 2U + nominal_error, UINT32_MAX));

            RawDecodeCandidate *existing = nullptr;
            for (std::size_t candidate_index = 0; candidate_index < candidate_count; ++candidate_index) {
                if (decoded_signals_match(candidates[candidate_index].decoded, observed.decoded)) {
                    existing = &candidates[candidate_index];
                    break;
                }
            }
            if (existing != nullptr) {
                if (observed.score < existing->score) {
                    *existing = observed;
                }
            } else if (candidate_count < candidates.size()) {
                candidates[candidate_count++] = observed;
            } else {
                overflowed = true;
            }
        }
    }
    if (overflowed) {
        return RawProtocolIdentity::kAmbiguous;
    }

    const RawDecodeCandidate *best = nullptr;
    const RawDecodeCandidate *runner_up = nullptr;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const RawDecodeCandidate &candidate = candidates[index];
        if (best == nullptr || candidate.score < best->score) {
            runner_up = best;
            best = &candidate;
        } else if (runner_up == nullptr || candidate.score < runner_up->score) {
            runner_up = &candidate;
        }
    }
    if (best == nullptr) {
        return RawProtocolIdentity::kUnknown;
    }
    if (runner_up != nullptr) {
        const bool protocol_alias =
            (best->decoded.protocol == 11 && runner_up->decoded.protocol == 12) ||
            (best->decoded.protocol == 12 && runner_up->decoded.protocol == 11);
        if (protocol_alias && best->decoded.bits == runner_up->decoded.bits &&
            best->decoded.code == runner_up->decoded.code &&
            best->decoded.pulse_us >= kRfProtocolAliasAmbiguousMinimumUs &&
            best->decoded.pulse_us <= kRfProtocolAliasAmbiguousMaximumUs) {
            return RawProtocolIdentity::kAmbiguous;
        }
        if (runner_up->score <= best->score + kIdentityAmbiguityMargin) {
            return RawProtocolIdentity::kAmbiguous;
        }
    }
    if (decoded != nullptr) {
        *decoded = best->decoded;
    }
    return RawProtocolIdentity::kDecoded;
}

bool aligned_raw_pulses_match(const RawSignal &left, const RawSignal &right, std::size_t shift)
{
    for (std::size_t index = 0; index < left.count; ++index) {
        const std::size_t right_index = shifted_index(right, shift, index);
        if (pulse_level(left, index) != pulse_level(right, right_index) ||
            !pulse_matches(left.durations_us[index], right.durations_us[right_index])) {
            return false;
        }
    }
    return true;
}

bool aligned_raw_scale_matches(const RawSignal &left, const RawSignal &right, std::size_t shift)
{
    std::array<uint32_t, kMaxRawPulses> ratios{};
    for (std::size_t index = 0; index < left.count; ++index) {
        const uint32_t left_duration = left.durations_us[index];
        const uint32_t right_duration = right.durations_us[shifted_index(right, shift, index)];
        const uint32_t largest = std::max(left_duration, right_duration);
        const uint32_t smallest = std::min(left_duration, right_duration);
        ratios[index] = (largest * 1000U + smallest / 2U) / smallest;
    }
    std::sort(ratios.begin(), ratios.begin() + left.count);
    const uint32_t median = (left.count & 1U) != 0
                                ? ratios[left.count / 2U]
                                : (ratios[left.count / 2U - 1U] + ratios[left.count / 2U]) / 2U;
    return median <= 1000U + static_cast<uint32_t>(kRawScaleTolerancePercent) * 10U;
}

uint16_t raw_median_duration(const RawSignal &signal)
{
    std::array<uint16_t, kMaxRawPulses> samples{};
    std::copy(signal.durations_us, signal.durations_us + signal.count, samples.begin());
    std::sort(samples.begin(), samples.begin() + signal.count);
    return (signal.count & 1U) != 0
               ? samples[signal.count / 2U]
               : static_cast<uint16_t>((static_cast<uint32_t>(samples[signal.count / 2U - 1U]) +
                                        samples[signal.count / 2U]) /
                                       2U);
}

int raw_symbol_relation(uint32_t first, uint32_t second, uint16_t median)
{
    const uint32_t difference = first > second ? first - second : second - first;
    const uint32_t tolerance = std::max<uint32_t>(20, static_cast<uint32_t>(median) * 20U / 100U);
    if (difference <= tolerance) {
        return 0;
    }
    return first < second ? -1 : 1;
}

bool aligned_raw_shape_matches(const RawSignal &left, const RawSignal &right, std::size_t shift,
                               uint16_t left_median, uint16_t right_median)
{
    for (std::size_t index = 0; index < left.count; ++index) {
        const std::size_t next = (index + 1U) % left.count;
        const int left_relation =
            raw_symbol_relation(left.durations_us[index], left.durations_us[next], left_median);
        const int right_relation =
            raw_symbol_relation(right.durations_us[shifted_index(right, shift, index)],
                                right.durations_us[shifted_index(right, shift, next)], right_median);
        if (left_relation != right_relation) {
            return false;
        }

        const uint32_t left_duration = left.durations_us[index];
        const uint32_t right_duration = right.durations_us[shifted_index(right, shift, index)];
        const bool left_neutral =
            static_cast<uint64_t>(left_duration) * 100U >=
                static_cast<uint64_t>(left_median) * (100U - kRawShapeNeutralPercent) &&
            static_cast<uint64_t>(left_duration) * 100U <=
                static_cast<uint64_t>(left_median) * (100U + kRawShapeNeutralPercent);
        const bool right_neutral =
            static_cast<uint64_t>(right_duration) * 100U >=
                static_cast<uint64_t>(right_median) * (100U - kRawShapeNeutralPercent) &&
            static_cast<uint64_t>(right_duration) * 100U <=
                static_cast<uint64_t>(right_median) * (100U + kRawShapeNeutralPercent);
        if (left_neutral || right_neutral) {
            if (left_neutral != right_neutral) {
                return false;
            }
            continue;
        }
        const uint64_t left_normalized = static_cast<uint64_t>(left_duration) * right_median;
        const uint64_t right_normalized = static_cast<uint64_t>(right_duration) * left_median;
        const uint64_t largest = std::max(left_normalized, right_normalized);
        const uint64_t difference = left_normalized > right_normalized ? left_normalized - right_normalized
                                                                       : right_normalized - left_normalized;
        if (difference * 100U > largest * kRawShapeTolerancePercent) {
            return false;
        }
    }
    return true;
}

bool append_pair(RawSignal *raw, uint16_t unit, uint8_t active, const PulsePair &pair)
{
    if (static_cast<std::size_t>(raw->count) + 2U > kMaxRawPulses) {
        return false;
    }
    const uint32_t first = static_cast<uint32_t>(unit) * pair.first;
    const uint32_t second = static_cast<uint32_t>(unit) * pair.second;
    if (first == 0 || first > kMaximumPulseDurationUs || second == 0 || second > kMaximumPulseDurationUs) {
        return false;
    }
    if (raw->count == 0) {
        raw->start_level = active;
    }
    raw->durations_us[raw->count++] = static_cast<uint16_t>(first);
    raw->durations_us[raw->count++] = static_cast<uint16_t>(second);
    return true;
}

}  // namespace

const RfProtocol *rf_protocol(uint8_t protocol_number)
{
    if (protocol_number == 0 || protocol_number > kProtocols.size()) {
        return nullptr;
    }
    return &kProtocols[protocol_number - 1];
}

uint8_t rf_protocol_min_factor(uint8_t protocol_number)
{
    const RfProtocol *protocol = rf_protocol(protocol_number);
    if (protocol == nullptr) {
        return 0;
    }
    return std::min({protocol->sync.first, protocol->sync.second, protocol->zero.first, protocol->zero.second,
                     protocol->one.first, protocol->one.second});
}

uint8_t rf_protocol_max_factor(uint8_t protocol_number)
{
    const RfProtocol *protocol = rf_protocol(protocol_number);
    if (protocol == nullptr) {
        return 0;
    }
    return std::max({protocol->sync.first, protocol->sync.second, protocol->zero.first, protocol->zero.second,
                     protocol->one.first, protocol->one.second});
}

bool decoded_signal_is_valid(const DecodedSignal &signal)
{
    const RfProtocol *protocol = rf_protocol(signal.protocol);
    const uint8_t minimum_factor = rf_protocol_min_factor(signal.protocol);
    const uint8_t maximum_factor = rf_protocol_max_factor(signal.protocol);
    const uint16_t minimum_unit = minimum_factor == 0
                                      ? 0
                                      : static_cast<uint16_t>((kMinimumRawPulseUs + minimum_factor - 1U) /
                                                              minimum_factor);
    return protocol != nullptr && signal.bits >= 4 && signal.bits <= 64 && minimum_unit > 0 &&
           signal.pulse_us >= minimum_unit && maximum_factor > 0 &&
           signal.pulse_us <= kMaximumPulseDurationUs / maximum_factor &&
           (signal.bits == 64 || (signal.code >> signal.bits) == 0);
}

bool decoded_signals_match(const DecodedSignal &left, const DecodedSignal &right)
{
    return decoded_signal_is_valid(left) && decoded_signal_is_valid(right) && left.code == right.code &&
           left.bits == right.bits && left.protocol == right.protocol;
}

bool raw_signal_is_valid(const RawSignal &signal)
{
    if (signal.count < 8 || signal.count > kMaxRawPulses || (signal.count & 1U) != 0 || signal.start_level > 1) {
        return false;
    }
    for (std::size_t index = 0; index < signal.count; ++index) {
        if (signal.durations_us[index] < kMinimumRawPulseUs ||
            signal.durations_us[index] > kMaximumPulseDurationUs) {
            return false;
        }
    }
    return true;
}

RawProtocolIdentity identify_raw_protocol(const RawSignal &raw, DecodedSignal *decoded)
{
    if (!raw_signal_is_valid(raw) || raw.count / 2U <= 1 || raw.count / 2U - 1U > 64) {
        return RawProtocolIdentity::kUnknown;
    }

    std::array<RawDecodeCandidate, 32> candidates{};
    std::size_t candidate_count = 0;
    bool overflowed = false;
    for (uint8_t protocol = 1; protocol <= kRfProtocolCount; ++protocol) {
        for (std::size_t shift = 0; shift < raw.count; ++shift) {
            RawDecodeCandidate observed{};
            if (!fit_raw_protocol(raw, shift, protocol, &observed)) {
                continue;
            }
            RawDecodeCandidate *existing = nullptr;
            for (std::size_t index = 0; index < candidate_count; ++index) {
                if (decoded_signals_match(candidates[index].decoded, observed.decoded)) {
                    existing = &candidates[index];
                    break;
                }
            }
            if (existing != nullptr) {
                if (observed.score < existing->score) {
                    *existing = observed;
                }
            } else if (candidate_count < candidates.size()) {
                candidates[candidate_count++] = observed;
            } else {
                overflowed = true;
            }
        }
    }
    if (overflowed) {
        return RawProtocolIdentity::kAmbiguous;
    }

    const RawDecodeCandidate *best = nullptr;
    const RawDecodeCandidate *runner_up = nullptr;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const RawDecodeCandidate &candidate = candidates[index];
        if (best == nullptr || candidate.score < best->score) {
            runner_up = best;
            best = &candidate;
        } else if (runner_up == nullptr || candidate.score < runner_up->score) {
            runner_up = &candidate;
        }
    }
    if (best == nullptr) {
        return RawProtocolIdentity::kUnknown;
    }
    for (std::size_t left = 0; left < candidate_count; ++left) {
        for (std::size_t right = left + 1U; right < candidate_count; ++right) {
            const DecodedSignal &left_signal = candidates[left].decoded;
            const DecodedSignal &right_signal = candidates[right].decoded;
            const bool protocol_alias =
                (left_signal.protocol == 11 && right_signal.protocol == 12) ||
                (left_signal.protocol == 12 && right_signal.protocol == 11);
            if (protocol_alias && left_signal.bits == right_signal.bits && left_signal.code == right_signal.code) {
                const uint16_t unit = static_cast<uint16_t>(
                    (static_cast<uint32_t>(left_signal.pulse_us) + right_signal.pulse_us) / 2U);
                if (unit >= kRfProtocolAliasAmbiguousMinimumUs && unit <= kRfProtocolAliasAmbiguousMaximumUs) {
                    return RawProtocolIdentity::kAmbiguous;
                }
            }
        }
    }
    if (runner_up != nullptr && runner_up->score <= best->score + kIdentityAmbiguityMargin) {
        return RawProtocolIdentity::kAmbiguous;
    }
    if (decoded != nullptr) {
        *decoded = best->decoded;
    }
    return RawProtocolIdentity::kDecoded;
}

bool raw_signals_match(const RawSignal &left, const RawSignal &right)
{
    if (!raw_signal_is_valid(left) || !raw_signal_is_valid(right) || left.count != right.count) {
        return false;
    }

    DecodedSignal left_decoded{};
    DecodedSignal right_decoded{};
    const RawProtocolIdentity left_identity = identify_raw_protocol(left, &left_decoded);
    const RawProtocolIdentity right_identity = identify_raw_protocol(right, &right_decoded);
    if (left_identity == RawProtocolIdentity::kAmbiguous || right_identity == RawProtocolIdentity::kAmbiguous) {
        return false;
    }
    if (left_identity == RawProtocolIdentity::kDecoded || right_identity == RawProtocolIdentity::kDecoded) {
        return left_identity == RawProtocolIdentity::kDecoded && right_identity == RawProtocolIdentity::kDecoded &&
               decoded_signals_match(left_decoded, right_decoded);
    }

    DecodedSignal left_fragment{};
    DecodedSignal right_fragment{};
    const RawProtocolIdentity left_fragment_identity = identify_raw_protocol_fragment(left, &left_fragment);
    const RawProtocolIdentity right_fragment_identity = identify_raw_protocol_fragment(right, &right_fragment);
    if (left_fragment_identity == RawProtocolIdentity::kAmbiguous ||
        right_fragment_identity == RawProtocolIdentity::kAmbiguous) {
        return false;
    }
    if (left_fragment_identity == RawProtocolIdentity::kDecoded ||
        right_fragment_identity == RawProtocolIdentity::kDecoded) {
        if (left_fragment_identity != RawProtocolIdentity::kDecoded ||
            right_fragment_identity != RawProtocolIdentity::kDecoded ||
            !decoded_signals_match(left_fragment, right_fragment)) {
            return false;
        }
    }

    const uint16_t left_median = raw_median_duration(left);
    const uint16_t right_median = raw_median_duration(right);
    for (std::size_t shift = 0; shift < right.count; ++shift) {
        if (aligned_raw_pulses_match(left, right, shift) &&
            aligned_raw_shape_matches(left, right, shift, left_median, right_median) &&
            aligned_raw_scale_matches(left, right, shift)) {
            return true;
        }
    }
    return false;
}

bool build_decoded_raw(const DecodedSignal &signal, RawSignal *raw)
{
    if (raw == nullptr || !decoded_signal_is_valid(signal) ||
        static_cast<std::size_t>(signal.bits + 1U) * 2U > kMaxRawPulses) {
        return false;
    }
    const RfProtocol *protocol = rf_protocol(signal.protocol);
    *raw = {};
    const uint8_t active = protocol->inverted ? 0 : 1;
    for (int bit = signal.bits - 1; bit >= 0; --bit) {
        const PulsePair &pair = ((signal.code >> bit) & 1U) != 0 ? protocol->one : protocol->zero;
        if (!append_pair(raw, signal.pulse_us, active, pair)) {
            return false;
        }
    }
    return append_pair(raw, signal.pulse_us, active, protocol->sync);
}

bool raw_signal_matches_decoded(const RawSignal &raw, const DecodedSignal &decoded)
{
    DecodedSignal identified{};
    return decoded_signal_is_valid(decoded) &&
           identify_raw_protocol(raw, &identified) == RawProtocolIdentity::kDecoded &&
           decoded_signals_match(identified, decoded);
}

uint32_t decoded_signal_fingerprint(const DecodedSignal &signal)
{
    uint32_t hash = fnv_byte(2166136261U, 0);
    hash = fnv_byte(hash, signal.protocol);
    hash = fnv_byte(hash, signal.bits);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash = fnv_byte(hash, static_cast<uint8_t>(signal.code >> shift));
    }
    return hash;
}

uint32_t raw_signal_fingerprint(const RawSignal &signal)
{
    if (!raw_signal_is_valid(signal)) {
        return 0;
    }
    uint16_t base = UINT16_MAX;
    for (std::size_t index = 0; index < signal.count; ++index) {
        if (signal.durations_us[index] >= kMinimumRawPulseUs) {
            base = std::min(base, signal.durations_us[index]);
        }
    }
    if (base == UINT16_MAX) {
        base = 1;
    }
    uint32_t hash = fnv_byte(2166136261U, 1);
    hash = fnv_byte(hash, signal.start_level);
    hash = fnv_byte(hash, static_cast<uint8_t>(signal.count));
    hash = fnv_byte(hash, static_cast<uint8_t>(signal.count >> 8));
    for (std::size_t index = 0; index < signal.count; ++index) {
        const uint32_t scaled = (static_cast<uint32_t>(signal.durations_us[index]) * 8U + base / 2U) / base;
        hash = fnv_byte(hash, static_cast<uint8_t>(std::min<uint32_t>(scaled, 255)));
    }
    return hash;
}

}  // namespace rfbridge
