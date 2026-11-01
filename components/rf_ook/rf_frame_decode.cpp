#include "rf_frame_decode.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>

// Pure capture decoding, kept free of ESP-IDF so host tests exercise the same code the radio runs.
namespace rfbridge {
namespace {

constexpr std::size_t kPulseCapacity = kRfMaxCapturePulses;
constexpr uint8_t kDecodeTolerancePercent = 35;
constexpr uint16_t kTerminalIdleMinimumUs = kRfTerminalIdleMinimumUs;
constexpr uint16_t kRmtStopDurationUs = kRfCaptureStopDurationUs;

bool duration_matches(uint32_t actual, uint32_t expected, uint8_t tolerance_percent = kDecodeTolerancePercent)
{
    const uint32_t difference = actual > expected ? actual - expected : expected - actual;
    const uint32_t tolerance = std::max<uint32_t>(80, expected * tolerance_percent / 100U);
    return difference <= tolerance;
}

int decode_pair(const uint8_t *levels, const uint16_t *durations, std::size_t index, const RfProtocol &protocol,
                uint16_t unit)
{
    const uint8_t active = protocol.inverted ? 0 : 1;
    if (levels[index] != active || levels[index + 1] == active) {
        return -1;
    }
    const bool zero = duration_matches(durations[index], unit * protocol.zero.first) &&
                      duration_matches(durations[index + 1], unit * protocol.zero.second);
    const bool one = duration_matches(durations[index], unit * protocol.one.first) &&
                     duration_matches(durations[index + 1], unit * protocol.one.second);
    if (zero == one) {
        return -1;
    }
    return one ? 1 : 0;
}

uint32_t normalized_duration_error(uint32_t actual, uint32_t expected)
{
    if (expected == 0) {
        return UINT32_MAX;
    }
    const uint32_t difference = actual > expected ? actual - expected : expected - actual;
    return static_cast<uint32_t>((static_cast<uint64_t>(difference) * 1000U) / expected);
}

uint32_t decoded_residual(const uint16_t *durations, std::size_t sync_index, uint8_t bits,
                          const RfProtocol &protocol, uint16_t unit)
{
    const std::size_t start = sync_index - static_cast<std::size_t>(bits) * 2U;
    uint64_t total = normalized_duration_error(durations[sync_index], unit * protocol.sync.first) +
                     normalized_duration_error(durations[sync_index + 1], unit * protocol.sync.second);
    std::size_t samples = 2;
    for (std::size_t index = start; index < sync_index; index += 2) {
        const bool zero = duration_matches(durations[index], unit * protocol.zero.first) &&
                          duration_matches(durations[index + 1], unit * protocol.zero.second);
        const PulsePair &pair = zero ? protocol.zero : protocol.one;
        total += normalized_duration_error(durations[index], unit * pair.first);
        total += normalized_duration_error(durations[index + 1], unit * pair.second);
        samples += 2;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(total / samples, UINT32_MAX));
}

bool decode_before_sync(const uint8_t *levels, const uint16_t *durations, std::size_t count,
                        std::size_t sync_index, const RfProtocol &protocol, bool capture_started_after_idle,
                        uint64_t *code, uint8_t *bits, uint16_t *unit, bool *trusted_boundary,
                        std::size_t *frame_start)
{
    const uint8_t active = protocol.inverted ? 0 : 1;
    if (levels[sync_index] != active || levels[sync_index + 1] == active ||
        (sync_index + 2U == count && durations[sync_index + 1] >= kTerminalIdleMinimumUs)) {
        return false;
    }
    const bool first_is_longer = protocol.sync.first >= protocol.sync.second;
    const uint8_t long_factor = first_is_longer ? protocol.sync.first : protocol.sync.second;
    const uint16_t long_duration = first_is_longer ? durations[sync_index] : durations[sync_index + 1];
    if (long_factor == 0) {
        return false;
    }
    const uint16_t candidate_unit = static_cast<uint16_t>((long_duration + long_factor / 2U) / long_factor);
    if (candidate_unit < 20 || candidate_unit > 5000 ||
        !duration_matches(durations[sync_index], candidate_unit * protocol.sync.first, 45) ||
        !duration_matches(durations[sync_index + 1], candidate_unit * protocol.sync.second, 45)) {
        return false;
    }

    std::size_t start = sync_index;
    uint8_t bit_count = 0;
    while (start >= 2 && bit_count < 64 &&
           decode_pair(levels, durations, start - 2, protocol, candidate_unit) >= 0) {
        start -= 2;
        ++bit_count;
    }
    if (bit_count < 4) {
        return false;
    }
    const bool has_previous_sync = start != 0;
    if (has_previous_sync) {
        if (start < 2) {
            return false;
        }
        const std::size_t previous_sync = start - 2;
        if (levels[previous_sync] != active || levels[previous_sync + 1] == active ||
            !duration_matches(durations[previous_sync], candidate_unit * protocol.sync.first, 45) ||
            !duration_matches(durations[previous_sync + 1], candidate_unit * protocol.sync.second, 45)) {
            return false;
        }
    }

    uint64_t decoded = 0;
    for (std::size_t index = start; index < sync_index; index += 2) {
        const int bit = decode_pair(levels, durations, index, protocol, candidate_unit);
        if (bit < 0) {
            return false;
        }
        decoded = (decoded << 1U) | static_cast<uint64_t>(bit);
    }
    *code = decoded;
    *bits = bit_count;
    *unit = candidate_unit;
    *trusted_boundary = has_previous_sync || (start == 0 && capture_started_after_idle);
    *frame_start = start;
    return true;
}

enum class KnownDecodeResult {
    kNoMatch,
    kDecoded,
    kAmbiguous,
};

KnownDecodeResult decode_known_protocol(const uint8_t *levels, const uint16_t *durations, std::size_t count,
                                         RfFrame *frame, bool capture_started_after_idle,
                                         uint16_t required_occurrences = 2)
{
    struct Candidate {
        DecodedSignal decoded;
        uint32_t fingerprint;
        uint32_t pulse_sum;
        uint32_t residual_sum;
        uint32_t nominal_error_sum;
        uint16_t supported_occurrences;
    };
    struct CandidateSpan {
        std::size_t start;
        std::size_t sync;
        bool trusted;
    };
    std::array<Candidate, 32> candidates{};
    std::array<CandidateSpan, 32> spans{};
    std::size_t candidate_count = 0;
    std::size_t span_count = 0;
    bool candidates_overflowed = false;
    bool spans_overflowed = false;

    for (std::size_t protocol_index = 0; protocol_index < kRfProtocolCount; ++protocol_index) {
        const RfProtocol *protocol_pointer = rf_protocol(static_cast<uint8_t>(protocol_index + 1));
        if (protocol_pointer == nullptr) {
            continue;
        }
        const RfProtocol &protocol = *protocol_pointer;
        for (std::size_t sync = 0; sync + 1 < count; ++sync) {
            uint64_t code = 0;
            uint8_t bits = 0;
            uint16_t unit = 0;
            bool trusted_boundary = false;
            std::size_t frame_start = 0;
            if (!decode_before_sync(levels, durations, count, sync, protocol, capture_started_after_idle, &code,
                                    &bits, &unit, &trusted_boundary, &frame_start)) {
                continue;
            }
            DecodedSignal decoded{};
            decoded.code = code;
            decoded.bits = bits;
            decoded.protocol = static_cast<uint8_t>(protocol_index + 1);
            decoded.pulse_us = unit;
            decoded.inverted = protocol.inverted;
            if (!decoded_signal_is_valid(decoded)) {
                continue;
            }

            CandidateSpan *span = nullptr;
            for (std::size_t index = 0; index < span_count; ++index) {
                if (spans[index].start == frame_start && spans[index].sync == sync) {
                    span = &spans[index];
                    break;
                }
            }
            if (span == nullptr) {
                if (span_count >= spans.size()) {
                    spans_overflowed = true;
                } else {
                    span = &spans[span_count++];
                    span->start = frame_start;
                    span->sync = sync;
                }
            }
            if (span != nullptr) {
                span->trusted = span->trusted || trusted_boundary;
            }

            Candidate *candidate = nullptr;
            for (std::size_t index = 0; index < candidate_count; ++index) {
                if (decoded_signals_match(candidates[index].decoded, decoded)) {
                    candidate = &candidates[index];
                    break;
                }
            }
            if (candidate == nullptr) {
                if (candidate_count >= candidates.size()) {
                    candidates_overflowed = true;
                    continue;
                }
                candidate = &candidates[candidate_count++];
                candidate->decoded = decoded;
                candidate->fingerprint = decoded_signal_fingerprint(decoded);
            }
            if (trusted_boundary) {
                candidate->pulse_sum += unit;
                candidate->residual_sum += decoded_residual(durations, sync, bits, protocol, unit);
                candidate->nominal_error_sum +=
                    unit > protocol.pulse_us ? unit - protocol.pulse_us : protocol.pulse_us - unit;
                ++candidate->supported_occurrences;
            }
        }
    }

    if (candidates_overflowed) {
        return KnownDecodeResult::kAmbiguous;
    }
    if (required_occurrences == 1) {
        if (spans_overflowed) {
            return KnownDecodeResult::kAmbiguous;
        }
        if (span_count == 0) {
            return KnownDecodeResult::kNoMatch;
        }
        if (span_count != 1) {
            return KnownDecodeResult::kAmbiguous;
        }
        if (!spans[0].trusted) {
            return KnownDecodeResult::kNoMatch;
        }
    }

    const Candidate *best = nullptr;
    const Candidate *runner_up = nullptr;
    const auto better = [](const Candidate &left, const Candidate &right) {
        const uint32_t left_residual = left.residual_sum / left.supported_occurrences;
        const uint32_t right_residual = right.residual_sum / right.supported_occurrences;
        const uint32_t left_nominal = left.nominal_error_sum / left.supported_occurrences;
        const uint32_t right_nominal = right.nominal_error_sum / right.supported_occurrences;
        return left.supported_occurrences > right.supported_occurrences ||
               (left.supported_occurrences == right.supported_occurrences && left_residual < right_residual) ||
               (left.supported_occurrences == right.supported_occurrences && left_residual == right_residual &&
                left.decoded.bits > right.decoded.bits) ||
               (left.supported_occurrences == right.supported_occurrences && left_residual == right_residual &&
                left.decoded.bits == right.decoded.bits && left_nominal < right_nominal);
    };
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const Candidate &candidate = candidates[index];
        if (candidate.supported_occurrences == 0) {
            continue;
        }
        if (best == nullptr || better(candidate, *best)) {
            runner_up = best;
            best = &candidate;
        } else if (runner_up == nullptr || better(candidate, *runner_up)) {
            runner_up = &candidate;
        }
    }
    if (best == nullptr || best->supported_occurrences < required_occurrences) {
        return KnownDecodeResult::kNoMatch;
    }
    if (runner_up != nullptr && runner_up->supported_occurrences == best->supported_occurrences) {
        const bool protocol_alias =
            (best->decoded.protocol == 11 && runner_up->decoded.protocol == 12) ||
            (best->decoded.protocol == 12 && runner_up->decoded.protocol == 11);
        const uint16_t average_unit = static_cast<uint16_t>(
            (best->pulse_sum + best->supported_occurrences / 2U) / best->supported_occurrences);
        if (protocol_alias && best->decoded.bits == runner_up->decoded.bits &&
            best->decoded.code == runner_up->decoded.code &&
            average_unit >= kRfProtocolAliasAmbiguousMinimumUs &&
            average_unit <= kRfProtocolAliasAmbiguousMaximumUs) {
            return KnownDecodeResult::kAmbiguous;
        }
        const uint32_t best_residual = best->residual_sum / best->supported_occurrences;
        const uint32_t runner_residual = runner_up->residual_sum / runner_up->supported_occurrences;
        const uint32_t residual_difference = best_residual > runner_residual ? best_residual - runner_residual
                                                                             : runner_residual - best_residual;
        const uint32_t best_nominal = best->nominal_error_sum / best->supported_occurrences;
        const uint32_t runner_nominal = runner_up->nominal_error_sum / runner_up->supported_occurrences;
        const uint32_t nominal_difference = best_nominal > runner_nominal ? best_nominal - runner_nominal
                                                                          : runner_nominal - best_nominal;
        if (residual_difference <= 10 && nominal_difference <= 10) {
            return KnownDecodeResult::kAmbiguous;
        }
    }

    *frame = {};
    frame->encoding = RfEncoding::kDecoded;
    frame->confidence = best->supported_occurrences == 1 ? RfFrameConfidence::kSingleDecoded
                                                         : RfFrameConfidence::kRepeatedEvidence;
    frame->decoded = best->decoded;
    frame->decoded.pulse_us = static_cast<uint16_t>(
        (best->pulse_sum + best->supported_occurrences / 2U) / best->supported_occurrences);
    frame->fingerprint = best->fingerprint;
    frame->observed_repeats = std::min<uint16_t>(best->supported_occurrences, 20);
    return KnownDecodeResult::kDecoded;
}

bool is_censored_terminal_pulse(const uint16_t *durations, std::size_t count, std::size_t index)
{
    return index + 1U == count && durations[index] == kRmtStopDurationUs;
}

bool similar_raw_pulse(const uint8_t *levels, const uint16_t *durations, std::size_t count, std::size_t left,
                       std::size_t right, uint32_t delimiter_floor)
{
    if (levels[left] != levels[right]) {
        return false;
    }
    if (is_censored_terminal_pulse(durations, count, left)) {
        return durations[right] >= delimiter_floor;
    }
    if (is_censored_terminal_pulse(durations, count, right)) {
        return durations[left] >= delimiter_floor;
    }
    const uint32_t largest = std::max(durations[left], durations[right]);
    const uint32_t difference = durations[left] > durations[right] ? durations[left] - durations[right]
                                                                    : durations[right] - durations[left];
    return difference <= std::max<uint32_t>(20, largest * 20U / 100U);
}

bool make_raw_frame(const uint8_t *levels, const uint16_t *durations, std::size_t count, RfFrame *frame)
{
    if (levels == nullptr || durations == nullptr || frame == nullptr || count < 16) {
        return false;
    }

    std::size_t longest_index = 0;
    const std::size_t delimiter_search_count =
        is_censored_terminal_pulse(durations, count, count - 1U) ? count - 1U : count;
    for (std::size_t index = 1; index < delimiter_search_count; ++index) {
        if (durations[index] > durations[longest_index]) {
            longest_index = index;
        }
    }
    const uint32_t delimiter_floor = static_cast<uint32_t>(durations[longest_index]) * 8U / 10U;

    std::size_t best_start = 0;
    std::size_t best_period = 0;
    std::size_t best_coverage = 0;
    uint16_t best_repeats = 0;
    bool best_uses_censored_terminal = false;
    const std::size_t maximum_period = std::min<std::size_t>(kMaxRawPulses, count / 2);
    const std::size_t maximum_start = std::min<std::size_t>(64, count / 4);

    for (std::size_t period = 8; period <= maximum_period; period += 2) {
        for (std::size_t start = 0; start <= maximum_start && start + period * 2 <= count; ++start) {
            bool contains_delimiter = false;
            for (std::size_t index = 0; index < period; ++index) {
                if (durations[start + index] >= delimiter_floor) {
                    contains_delimiter = true;
                    break;
                }
            }
            if (!contains_delimiter) {
                continue;
            }

            bool second_matches = true;
            for (std::size_t index = 0; index < period; ++index) {
                if (!similar_raw_pulse(levels, durations, count, start + index, start + period + index,
                                       delimiter_floor)) {
                    second_matches = false;
                    break;
                }
            }
            if (!second_matches) {
                continue;
            }

            uint16_t repeats = 2;
            while (start + (static_cast<std::size_t>(repeats) + 1U) * period <= count) {
                bool repeat_matches = true;
                for (std::size_t index = 0; index < period; ++index) {
                    if (!similar_raw_pulse(levels, durations, count, start + index,
                                           start + static_cast<std::size_t>(repeats) * period + index,
                                           delimiter_floor)) {
                        repeat_matches = false;
                        break;
                    }
                }
                if (!repeat_matches) {
                    break;
                }
                ++repeats;
            }

            const std::size_t coverage = static_cast<std::size_t>(repeats) * period;
            const std::size_t suffix = count - start - coverage;
            if (suffix >= period || coverage * 4U < count * 3U) {
                continue;
            }
            if (coverage > best_coverage ||
                (coverage == best_coverage && repeats > best_repeats) ||
                (coverage == best_coverage && repeats == best_repeats && period > best_period)) {
                best_start = start;
                best_period = period;
                best_coverage = coverage;
                best_repeats = repeats;
                best_uses_censored_terminal =
                    start + coverage == count && is_censored_terminal_pulse(durations, count, count - 1U);
            }
        }
    }

    if (best_period == 0 || best_repeats < 2) {
        return false;
    }

    std::size_t phase = 0;
    for (std::size_t index = 1; index < best_period; ++index) {
        if (durations[best_start + index] > durations[best_start + phase]) {
            phase = index;
        }
    }
    phase = (phase + 1U) % best_period;

    RawSignal raw{};
    raw.count = static_cast<uint16_t>(best_period);
    raw.start_level = levels[best_start + phase];
    for (std::size_t output = 0; output < best_period; ++output) {
        const std::size_t source = (output + phase) % best_period;
        std::array<uint16_t, kPulseCapacity / 8> samples{};
        std::size_t sample_count = 0;
        for (std::size_t repeat = 0; repeat < best_repeats; ++repeat) {
            const std::size_t sample_index = best_start + repeat * best_period + source;
            if (!is_censored_terminal_pulse(durations, count, sample_index)) {
                samples[sample_count++] = durations[sample_index];
            }
        }
        if (sample_count == 0) {
            return false;
        }
        std::sort(samples.begin(), samples.begin() + sample_count);
        const uint32_t median = (sample_count & 1U) != 0
                                    ? samples[sample_count / 2U]
                                    : (static_cast<uint32_t>(samples[sample_count / 2U - 1U]) +
                                       samples[sample_count / 2U]) /
                                          2U;
        const uint32_t quantized = (median + 5U) / 10U * 10U;
        raw.durations_us[output] = static_cast<uint16_t>(std::min<uint32_t>(quantized, kMaximumPulseDurationUs));
    }
    if (!raw_signal_is_valid(raw)) {
        return false;
    }

    DecodedSignal decoded{};
    const RawProtocolIdentity identity = identify_raw_protocol(raw, &decoded);
    if (identity == RawProtocolIdentity::kAmbiguous) {
        return false;
    }
    if (identity == RawProtocolIdentity::kDecoded) {
        if (best_uses_censored_terminal) {
            return false;
        }
        *frame = {};
        frame->encoding = RfEncoding::kDecoded;
        frame->confidence = RfFrameConfidence::kRepeatedEvidence;
        frame->decoded = decoded;
        frame->fingerprint = decoded_signal_fingerprint(decoded);
        frame->observed_repeats = std::min<uint16_t>(best_repeats, 20);
        return true;
    }

    *frame = {};
    frame->encoding = RfEncoding::kRaw;
    frame->confidence = RfFrameConfidence::kRepeatedEvidence;
    frame->observed_repeats = std::min<uint16_t>(best_repeats, 20);
    frame->raw = raw;
    frame->fingerprint = raw_signal_fingerprint(frame->raw);
    return true;
}

bool capture_input_is_valid(const uint8_t *levels, const uint16_t *durations, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) {
        if (levels[index] > 1 ||
            (index > 0 && levels[index] == levels[index - 1U])) {
            return false;
        }
        const bool censored_terminal = index + 1U == count && durations[index] == kRmtStopDurationUs;
        if (!censored_terminal &&
            (durations[index] < kMinimumRawPulseUs || durations[index] > kMaximumPulseDurationUs)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool decode_rf_pulses(const uint8_t *levels, const uint16_t *durations_us, std::size_t count, RfFrame *frame,
                      bool capture_may_be_truncated, bool capture_started_after_idle)
{
    if (levels == nullptr || durations_us == nullptr || frame == nullptr || count < 8 || count > kPulseCapacity ||
        !capture_input_is_valid(levels, durations_us, count)) {
        return false;
    }
    *frame = {};
    const KnownDecodeResult decoded =
        decode_known_protocol(levels, durations_us, count, frame, capture_started_after_idle);
    if (decoded == KnownDecodeResult::kDecoded) {
        return true;
    }
    if (decoded == KnownDecodeResult::kAmbiguous || capture_may_be_truncated) {
        return false;
    }
    if (make_raw_frame(levels, durations_us, count, frame)) {
        return true;
    }
    // Decoded in place: a second RfFrame would cost the radio task about 560 bytes of stack.
    if (decode_known_protocol(levels, durations_us, count, frame, capture_started_after_idle, 1) !=
            KnownDecodeResult::kDecoded ||
        frame->decoded.bits < kRfMinimumSingleFrameBits) {
        *frame = {};
        return false;
    }
    return true;
}

bool rf_frames_equivalent(const RfFrame &left, const RfFrame &right)
{
    if (left.encoding == RfEncoding::kDecoded && right.encoding == RfEncoding::kDecoded) {
        return decoded_signals_match(left.decoded, right.decoded);
    }
    if (left.encoding == RfEncoding::kRaw && right.encoding == RfEncoding::kRaw) {
        return raw_signals_match(left.raw, right.raw);
    }
    if (left.encoding == RfEncoding::kDecoded && right.encoding == RfEncoding::kRaw) {
        return raw_signal_matches_decoded(right.raw, left.decoded);
    }
    if (left.encoding == RfEncoding::kRaw && right.encoding == RfEncoding::kDecoded) {
        return raw_signal_matches_decoded(left.raw, right.decoded);
    }
    return false;
}

uint32_t rf_frame_fingerprint(const RfFrame &frame)
{
    return frame.encoding == RfEncoding::kDecoded ? decoded_signal_fingerprint(frame.decoded)
                                                   : raw_signal_fingerprint(frame.raw);
}

}  // namespace rfbridge
