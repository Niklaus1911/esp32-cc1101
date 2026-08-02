#include "rf_ook.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <iterator>

#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rf_activity_led_private.hpp"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

constexpr char kTag[] = "rf_ook";
constexpr uint32_t kResolutionHz = 1000000;
constexpr std::size_t kRxSymbolCapacity = 448;
constexpr std::size_t kPulseCapacity = kRxSymbolCapacity * 2;
constexpr uint8_t kDecodeTolerancePercent = 35;
constexpr uint16_t kTerminalIdleMinimumUs = 25000;
constexpr uint16_t kRmtStopDurationUs = 30000;
constexpr uint32_t kTrustedCaptureStartIdleUs = 60000;
constexpr uint64_t kMaximumTransmissionUs = 5000000;
constexpr TickType_t kApiMutexTimeoutTicks = pdMS_TO_TICKS(1000);
constexpr TickType_t kCommandTimeoutTicks = pdMS_TO_TICKS(7000);
constexpr TickType_t kShutdownTimeoutTicks = pdMS_TO_TICKS(7000);

struct PulseBuffer {
    uint8_t levels[kPulseCapacity]{};
    uint16_t durations[kPulseCapacity]{};
    std::size_t count = 0;
};

struct RxQueueItem {
    rmt_rx_done_event_data_t event;
    uint32_t generation;
    int64_t armed_us;
    int64_t captured_us;
};

enum class ServiceState : uint8_t {
    kStopped,
    kStarting,
    kRunning,
    kStopping,
};

enum class CommandState : uint8_t {
    kIdle,
    kQueued,
    kExecuting,
    kCancelled,
    kCompleted,
};

enum class RadioCommandType : uint8_t {
    kSetReceive,
    kTransmitDecoded,
    kTransmitRaw,
    kReplayLast,
    kGetLast,
    kGetStatus,
    kReset,
    kEnterMaintenance,
    kExitMaintenance,
    kStop,
};

struct RadioCommand {
    uint32_t id;
    RadioCommandType type;
    bool enabled;
    uint16_t repeats;
    DecodedSignal decoded;
    RawSignal raw;
};

struct RadioReply {
    uint32_t id;
    esp_err_t result;
    RfFrame frame;
    RfRadioStatus status;
};

rmt_channel_handle_t s_rx_channel = nullptr;
rmt_channel_handle_t s_tx_channel = nullptr;
rmt_encoder_handle_t s_copy_encoder = nullptr;
rmt_symbol_word_t s_rx_symbols[2][kRxSymbolCapacity]{};
rmt_symbol_word_t s_symbol_snapshot[kRxSymbolCapacity]{};
rmt_symbol_word_t s_tx_symbols[(kMaxRawPulses / 2U) * 20U]{};
PulseBuffer s_pulse_buffer{};
std::array<std::atomic<uint32_t>, 2> s_rx_buffer_generations{};
std::array<std::atomic<uint32_t>, 2> s_rx_buffer_armed_low{};
std::array<std::atomic<uint32_t>, 2> s_rx_buffer_armed_high{};
std::size_t s_next_rx_buffer = 0;
QueueHandle_t s_rx_queue = nullptr;
QueueHandle_t s_command_queue = nullptr;
QueueHandle_t s_reply_queue = nullptr;
QueueHandle_t s_startup_queue = nullptr;
QueueSetHandle_t s_radio_queue_set = nullptr;
SemaphoreHandle_t s_api_mutex = nullptr;
SemaphoreHandle_t s_radio_stopped = nullptr;
std::atomic<TaskHandle_t> s_radio_task{nullptr};
RfFrameCallback s_frame_callback = nullptr;
void *s_callback_context = nullptr;
std::atomic<ServiceState> s_service_state{ServiceState::kStopped};
std::atomic<bool> s_running{false};
std::atomic<bool> s_transmitting{false};
std::atomic<bool> s_maintenance_requested{false};
std::atomic<bool> s_maintenance_active{false};
std::atomic<bool> s_rx_rearm_required{false};
std::atomic<bool> s_rmt_tx_faulted{false};
std::atomic<uint32_t> s_rx_generation{0};
std::atomic<uint32_t> s_rx_queue_drops{0};
std::atomic<uint32_t> s_command_timeouts{0};
std::atomic<uint32_t> s_command_sequence{0};
portMUX_TYPE s_command_state_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_active_command_id = 0;
CommandState s_active_command_state = CommandState::kIdle;
std::atomic_flag s_lifecycle_busy = ATOMIC_FLAG_INIT;
rmt_receive_config_t s_receive_config{};
Cc1101 s_radio{};
std::atomic<bool> s_receive_enabled{true};
std::atomic<bool> s_receive_active{false};
std::atomic<bool> s_has_last_frame{false};
RfFrame s_last_frame{};
int64_t s_last_reported_us = 0;
std::atomic<uint32_t> s_accepted_frames{0};
std::atomic<uint32_t> s_suppressed_duplicates{0};
std::atomic<uint32_t> s_truncated_captures{0};
std::atomic<bool> s_activity_led_warning_logged{false};

class LifecycleGuard {
public:
    LifecycleGuard() : acquired_(!s_lifecycle_busy.test_and_set(std::memory_order_acquire)) {}

    ~LifecycleGuard()
    {
        if (acquired_) {
            s_lifecycle_busy.clear(std::memory_order_release);
        }
    }

    bool acquired() const
    {
        return acquired_;
    }

private:
    bool acquired_;
};

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

void append_pulse(PulseBuffer *buffer, uint8_t level, uint32_t duration)
{
    if (duration < kMinimumRawPulseUs || buffer->count >= kPulseCapacity) {
        return;
    }
    duration = std::min<uint32_t>(duration, UINT16_MAX);
    if (buffer->count > 0 && buffer->levels[buffer->count - 1] == level) {
        const uint32_t merged = buffer->durations[buffer->count - 1] + duration;
        buffer->durations[buffer->count - 1] = static_cast<uint16_t>(std::min<uint32_t>(merged, UINT16_MAX));
        return;
    }
    buffer->levels[buffer->count] = level;
    buffer->durations[buffer->count] = static_cast<uint16_t>(duration);
    ++buffer->count;
}

bool IRAM_ATTR rx_done_callback(rmt_channel_handle_t, const rmt_rx_done_event_data_t *event, void *)
{
    BaseType_t high_priority_woken = pdFALSE;
    if (s_rx_queue == nullptr || event == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < std::size(s_rx_symbols); ++index) {
        if (event->received_symbols != s_rx_symbols[index]) {
            continue;
        }
        const uint64_t armed_high = s_rx_buffer_armed_high[index].load(std::memory_order_acquire);
        const uint64_t armed_low = s_rx_buffer_armed_low[index].load(std::memory_order_relaxed);
        const RxQueueItem item{*event,
                               s_rx_buffer_generations[index].load(std::memory_order_relaxed),
                               static_cast<int64_t>((armed_high << 32U) | armed_low),
                               esp_timer_get_time()};
        if (item.generation == s_rx_generation.load(std::memory_order_relaxed)) {
            s_receive_active.store(false, std::memory_order_release);
        }
        if (xQueueSendFromISR(s_rx_queue, &item, &high_priority_woken) != pdTRUE) {
            s_rx_queue_drops.fetch_add(1, std::memory_order_relaxed);
            s_rx_rearm_required.store(true, std::memory_order_release);
        }
        break;
    }
    return high_priority_woken == pdTRUE;
}

esp_err_t arm_receiver_owned()
{
    if (s_rx_channel == nullptr || !s_receive_enabled || s_receive_active ||
        s_transmitting.load(std::memory_order_relaxed) ||
        s_maintenance_active.load(std::memory_order_relaxed) ||
        !s_running.load(std::memory_order_relaxed)) {
        return ESP_ERR_INVALID_STATE;
    }
    const std::size_t buffer_index = s_next_rx_buffer;
    s_rx_buffer_generations[buffer_index].store(s_rx_generation.load(std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
    const uint64_t armed_us = static_cast<uint64_t>(esp_timer_get_time());
    s_rx_buffer_armed_low[buffer_index].store(static_cast<uint32_t>(armed_us), std::memory_order_relaxed);
    s_rx_buffer_armed_high[buffer_index].store(static_cast<uint32_t>(armed_us >> 32U),
                                               std::memory_order_release);
    const esp_err_t error = rmt_receive(s_rx_channel, s_rx_symbols[buffer_index], sizeof(s_rx_symbols[buffer_index]),
                                        &s_receive_config);
    if (error == ESP_OK) {
        s_next_rx_buffer = (buffer_index + 1U) % std::size(s_rx_symbols);
        s_receive_active = true;
    }
    return error;
}

esp_err_t stop_rmt_receive_owned()
{
    s_rx_generation.fetch_add(1, std::memory_order_relaxed);
    s_receive_active = false;
    if (s_rx_channel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = rmt_disable(s_rx_channel);
    return error == ESP_ERR_INVALID_STATE ? ESP_OK : error;
}

esp_err_t restore_receive_owned()
{
    s_receive_active = false;
    if (!s_receive_enabled) {
        return s_radio.enter_idle();
    }
    esp_err_t error = s_radio.enter_receive(50);
    if (error != ESP_OK) {
        error = s_radio.recover_receive();
    }
    if (error == ESP_OK) {
        error = rmt_enable(s_rx_channel);
        if (error == ESP_ERR_INVALID_STATE) {
            error = ESP_OK;
        }
    }
    if (error == ESP_OK) {
        error = arm_receiver_owned();
    }
    if (error != ESP_OK) {
        s_receive_active = false;
        rmt_disable(s_rx_channel);
        s_radio.enter_idle(50);
    }
    return error;
}

esp_err_t set_receive_enabled_owned(bool enabled)
{
    if (enabled && s_receive_enabled && s_receive_active) {
        return ESP_OK;
    }
    if (!enabled) {
        s_receive_enabled = false;
        s_receive_active = false;
        const esp_err_t rmt_error = stop_rmt_receive_owned();
        const esp_err_t radio_error = s_radio.enter_idle(50);
        return rmt_error != ESP_OK ? rmt_error : radio_error;
    }

    s_receive_enabled = true;
    return restore_receive_owned();
}

void recover_dropped_receive_owned()
{
    if (!s_rx_rearm_required.exchange(false, std::memory_order_acq_rel) ||
        s_maintenance_requested.load(std::memory_order_acquire)) {
        return;
    }
    ESP_LOGW(kTag, "Recovering RX after a completed capture could not be queued");
    const esp_err_t stop_error = stop_rmt_receive_owned();
    if (stop_error != ESP_OK) {
        ESP_LOGW(kTag, "Could not stop dropped RX transaction: %s", esp_err_to_name(stop_error));
    }
    if (s_receive_enabled) {
        const esp_err_t restore_error = restore_receive_owned();
        if (restore_error != ESP_OK) {
            ESP_LOGE(kTag, "Could not rearm RX after queue drop: %s", esp_err_to_name(restore_error));
        }
    }
}

bool make_symbol(const PulsePair &pair, uint16_t unit, bool inverted, rmt_symbol_word_t *symbol)
{
    const uint32_t first = static_cast<uint32_t>(pair.first) * unit;
    const uint32_t second = static_cast<uint32_t>(pair.second) * unit;
    if (symbol == nullptr || first == 0 || second == 0 || first > kMaximumPulseDurationUs || second > kMaximumPulseDurationUs) {
        return false;
    }
    symbol->level0 = inverted ? 0 : 1;
    symbol->duration0 = first;
    symbol->level1 = inverted ? 1 : 0;
    symbol->duration1 = second;
    return true;
}

bool build_decoded_symbols(const DecodedSignal &signal, rmt_symbol_word_t *symbols, std::size_t capacity,
                           std::size_t *count)
{
    const RfProtocol *protocol_pointer = rf_protocol(signal.protocol);
    if (protocol_pointer == nullptr || symbols == nullptr || count == nullptr ||
        capacity < static_cast<std::size_t>(signal.bits) + 1U) {
        return false;
    }
    const RfProtocol &protocol = *protocol_pointer;
    DecodedSignal normalized = signal;
    normalized.pulse_us = signal.pulse_us == 0 ? protocol.pulse_us : signal.pulse_us;
    if (!decoded_signal_is_valid(normalized)) {
        return false;
    }
    const uint16_t unit = normalized.pulse_us;
    for (uint8_t bit = 0; bit < signal.bits; ++bit) {
        const uint8_t shift = signal.bits - bit - 1U;
        const bool one = ((signal.code >> shift) & 1U) != 0;
        if (!make_symbol(one ? protocol.one : protocol.zero, unit, protocol.inverted, &symbols[bit])) {
            return false;
        }
    }
    if (!make_symbol(protocol.sync, unit, protocol.inverted, &symbols[signal.bits])) {
        return false;
    }
    *count = static_cast<std::size_t>(signal.bits) + 1U;
    return true;
}

bool build_raw_symbols(const RawSignal &raw, rmt_symbol_word_t *symbols, std::size_t capacity, std::size_t *count)
{
    if (!raw_signal_is_valid(raw) || symbols == nullptr || count == nullptr || capacity < raw.count / 2U) {
        return false;
    }
    for (std::size_t pulse = 0; pulse < raw.count; pulse += 2U) {
        rmt_symbol_word_t &symbol = symbols[pulse / 2U];
        symbol.level0 = raw.start_level;
        symbol.duration0 = raw.durations_us[pulse];
        symbol.level1 = raw.start_level == 0 ? 1 : 0;
        symbol.duration1 = raw.durations_us[pulse + 1U];
    }
    *count = raw.count / 2U;
    return true;
}

esp_err_t transmit_symbols_owned(const rmt_symbol_word_t *symbols, std::size_t count, uint16_t repeats)
{
    if (symbols == nullptr || count == 0 || repeats == 0 || repeats > 20 || s_tx_channel == nullptr ||
        s_copy_encoder == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_maintenance_requested.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t frame_duration_us = 0;
    for (std::size_t index = 0; index < count; ++index) {
        frame_duration_us += symbols[index].duration0 + symbols[index].duration1;
    }
    const std::size_t total_symbols = count * static_cast<std::size_t>(repeats);
    if (frame_duration_us == 0 || frame_duration_us > kMaximumTransmissionUs ||
        frame_duration_us * repeats > kMaximumTransmissionUs || total_symbols > std::size(s_tx_symbols)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint16_t repeat = 0; repeat < repeats; ++repeat) {
        std::memcpy(s_tx_symbols + static_cast<std::size_t>(repeat) * count, symbols, count * sizeof(*symbols));
    }

    esp_err_t error = stop_rmt_receive_owned();
    if (error != ESP_OK) {
        return error;
    }
    s_transmitting.store(true, std::memory_order_release);
    error = s_radio.enter_transmit(50);

    const uint64_t total_duration_us = frame_duration_us * repeats;
    const int wait_timeout_ms =
        static_cast<int>(std::min<uint64_t>((total_duration_us + 999U) / 1000U + 1000U, INT_MAX));
    rmt_transmit_config_t transmit_config{};
    transmit_config.loop_count = 0;
    transmit_config.flags.eot_level = 0;
    if (error == ESP_OK) {
        error = rmt_transmit(s_tx_channel, s_copy_encoder, s_tx_symbols, total_symbols * sizeof(*s_tx_symbols),
                             &transmit_config);
    }
    if (error == ESP_OK) {
        error = rmt_tx_wait_all_done(s_tx_channel, wait_timeout_ms);
    }
    if (error == ESP_ERR_TIMEOUT) {
        const esp_err_t idle_error = s_radio.enter_idle(50);
        const esp_err_t reset_error = idle_error == ESP_OK ? ESP_OK : s_radio.reset_and_configure();
        s_transmitting.store(false, std::memory_order_release);
        s_rmt_tx_faulted.store(true, std::memory_order_release);
        s_service_state.store(ServiceState::kStopping, std::memory_order_release);
        s_running.store(false, std::memory_order_release);
        if (idle_error == ESP_OK || reset_error == ESP_OK) {
            ESP_LOGE(kTag, "RMT TX did not stop on time; RF was forced idle and service restart requires a reboot");
        } else {
            ESP_LOGE(kTag, "RMT TX and CC1101 shutdown both failed; carrier state is unknown and reboot is required");
        }
        return error;
    }

    esp_rom_delay_us(3000);
    const esp_err_t idle_error = s_radio.enter_idle(50);
    s_transmitting.store(false, std::memory_order_release);
    if (error == ESP_OK && idle_error != ESP_OK) {
        error = idle_error;
    }
    if (idle_error != ESP_OK) {
        const esp_err_t recovery_error = s_radio.reset_and_configure();
        if (error == ESP_OK) {
            error = recovery_error;
        }
    }

    if (s_receive_enabled) {
        const esp_err_t restore_error = restore_receive_owned();
        if (restore_error != ESP_OK) {
            ESP_LOGE(kTag, "Could not restore RX after TX: %s", esp_err_to_name(restore_error));
            if (error == ESP_OK) {
                error = restore_error;
            }
        }
    }
    return error;
}

esp_err_t transmit_decoded_owned(const DecodedSignal &signal, uint16_t repeats)
{
    rmt_symbol_word_t symbols[65]{};
    std::size_t count = 0;
    if (!build_decoded_symbols(signal, symbols, std::size(symbols), &count)) {
        return ESP_ERR_INVALID_ARG;
    }
    return transmit_symbols_owned(symbols, count, repeats);
}

esp_err_t transmit_raw_owned(const RawSignal &raw, uint16_t repeats)
{
    rmt_symbol_word_t symbols[kMaxRawPulses / 2U]{};
    std::size_t count = 0;
    if (!build_raw_symbols(raw, symbols, std::size(symbols), &count)) {
        return ESP_ERR_INVALID_ARG;
    }
    return transmit_symbols_owned(symbols, count, repeats);
}

void process_receive_item(const RxQueueItem &item)
{
    if (!s_receive_enabled || s_transmitting.load(std::memory_order_relaxed) ||
        s_maintenance_requested.load(std::memory_order_relaxed) ||
        item.generation != s_rx_generation.load(std::memory_order_relaxed)) {
        return;
    }
    s_receive_active = false;
    const std::size_t symbol_count = std::min<std::size_t>(item.event.num_symbols, kRxSymbolCapacity);
    std::memcpy(s_symbol_snapshot, item.event.received_symbols, symbol_count * sizeof(rmt_symbol_word_t));
    esp_err_t arm_error = arm_receiver_owned();
    if (arm_error != ESP_OK) {
        ESP_LOGW(kTag, "RMT rearm failed: %s", esp_err_to_name(arm_error));
        rmt_disable(s_rx_channel);
        if (rmt_enable(s_rx_channel) == ESP_OK) {
            arm_error = arm_receiver_owned();
        }
        if (arm_error != ESP_OK) {
            ESP_LOGE(kTag, "RMT recovery failed: %s", esp_err_to_name(arm_error));
        }
    }

    s_pulse_buffer = {};
    uint64_t transaction_duration_us = 0;
    for (std::size_t index = 0; index < symbol_count; ++index) {
        transaction_duration_us += s_symbol_snapshot[index].duration0;
        transaction_duration_us += s_symbol_snapshot[index].duration1;
        append_pulse(&s_pulse_buffer, s_symbol_snapshot[index].level0, s_symbol_snapshot[index].duration0);
        append_pulse(&s_pulse_buffer, s_symbol_snapshot[index].level1, s_symbol_snapshot[index].duration1);
    }
    const bool capture_time_valid = item.captured_us > static_cast<int64_t>(transaction_duration_us);
    const int64_t capture_start_us =
        capture_time_valid ? item.captured_us - static_cast<int64_t>(transaction_duration_us) : 0;
    const bool capture_started_after_idle = capture_time_valid && item.armed_us > 0 &&
                                            capture_start_us >= item.armed_us + kTrustedCaptureStartIdleUs;
    const bool truncated = item.event.num_symbols >= kRxSymbolCapacity;
    if (truncated) {
        ++s_truncated_captures;
    }

    RfFrame frame{};
    if (!decode_rf_pulses(s_pulse_buffer.levels, s_pulse_buffer.durations, s_pulse_buffer.count, &frame,
                          truncated, capture_started_after_idle)) {
        return;
    }
    frame.captured_us = capture_start_us;
    const int64_t duplicate_window_us = static_cast<int64_t>(CONFIG_RF_DUPLICATE_WINDOW_MS) * 1000;
    const bool duplicate = duplicate_window_us > 0 && s_has_last_frame && s_last_reported_us > 0 &&
                           item.captured_us - s_last_reported_us <= duplicate_window_us &&
                           rf_frames_equivalent(s_last_frame, frame);
    if (duplicate) {
        s_last_reported_us = item.captured_us;
        ++s_suppressed_duplicates;
        return;
    }
    s_last_frame = frame;
    s_has_last_frame = true;
    s_last_reported_us = item.captured_us;
    ++s_accepted_frames;
    notify_rf_activity_led();
    if (s_frame_callback != nullptr) {
        s_frame_callback(frame, s_callback_context);
    }
}

void fill_software_status(RfRadioStatus *status)
{
    *status = {};
    status->running = s_service_state.load(std::memory_order_acquire) == ServiceState::kRunning;
    status->receive_enabled = s_receive_enabled.load(std::memory_order_relaxed);
    status->receive_active = s_receive_active.load(std::memory_order_relaxed);
    status->transmitting = s_transmitting.load(std::memory_order_relaxed);
    status->maintenance_active = s_maintenance_requested.load(std::memory_order_relaxed);
    status->has_last_frame = s_has_last_frame.load(std::memory_order_relaxed);
    status->accepted_frames = s_accepted_frames.load(std::memory_order_relaxed);
    status->suppressed_duplicates = s_suppressed_duplicates.load(std::memory_order_relaxed);
    status->rx_queue_drops = s_rx_queue_drops.load(std::memory_order_relaxed);
    status->truncated_captures = s_truncated_captures.load(std::memory_order_relaxed);
    status->command_timeouts = s_command_timeouts.load(std::memory_order_relaxed);
}

esp_err_t fill_status_owned(RfRadioStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_software_status(status);
    status->cc1101_error = s_radio.read_info(&status->cc1101);
    status->cc1101_info_valid = status->cc1101_error == ESP_OK;
    return ESP_OK;
}

void set_active_command(uint32_t id, CommandState state)
{
    portENTER_CRITICAL(&s_command_state_mux);
    s_active_command_id = id;
    s_active_command_state = state;
    portEXIT_CRITICAL(&s_command_state_mux);
}

bool transition_active_command(uint32_t id, CommandState from, CommandState to)
{
    bool transitioned = false;
    portENTER_CRITICAL(&s_command_state_mux);
    if (s_active_command_id == id && s_active_command_state == from) {
        s_active_command_state = to;
        transitioned = true;
    }
    portEXIT_CRITICAL(&s_command_state_mux);
    return transitioned;
}

bool execute_command(const RadioCommand &command)
{
    if (!transition_active_command(command.id, CommandState::kQueued, CommandState::kExecuting)) {
        return false;
    }

    RadioReply reply{};
    reply.id = command.id;
    switch (command.type) {
        case RadioCommandType::kSetReceive:
            reply.result = set_receive_enabled_owned(command.enabled);
            break;
        case RadioCommandType::kTransmitDecoded:
            reply.result = transmit_decoded_owned(command.decoded, command.repeats);
            break;
        case RadioCommandType::kTransmitRaw:
            reply.result = transmit_raw_owned(command.raw, command.repeats);
            break;
        case RadioCommandType::kReplayLast:
            if (!s_has_last_frame) {
                reply.result = ESP_ERR_NOT_FOUND;
            } else if (s_last_frame.encoding == RfEncoding::kDecoded) {
                reply.result = transmit_decoded_owned(s_last_frame.decoded, command.repeats);
            } else {
                reply.result = transmit_raw_owned(s_last_frame.raw, command.repeats);
            }
            break;
        case RadioCommandType::kGetLast:
            if (!s_has_last_frame) {
                reply.result = ESP_ERR_NOT_FOUND;
            } else {
                reply.frame = s_last_frame;
                reply.result = ESP_OK;
            }
            break;
        case RadioCommandType::kGetStatus:
            reply.result = fill_status_owned(&reply.status);
            break;
        case RadioCommandType::kReset: {
            const esp_err_t stop_error = stop_rmt_receive_owned();
            reply.result = stop_error == ESP_OK ? s_radio.reset_and_configure() : stop_error;
            if (reply.result == ESP_OK && s_receive_enabled) {
                reply.result = restore_receive_owned();
            }
            break;
        }
        case RadioCommandType::kEnterMaintenance: {
            if (s_maintenance_active.exchange(true, std::memory_order_acq_rel)) {
                reply.result = ESP_ERR_INVALID_STATE;
                break;
            }
            const esp_err_t receive_error = stop_rmt_receive_owned();
            const esp_err_t idle_error = s_radio.enter_idle(50);
            reply.result = receive_error != ESP_OK ? receive_error : idle_error;
            if (reply.result != ESP_OK) {
                s_maintenance_active.store(false, std::memory_order_release);
                if (s_receive_enabled) {
                    restore_receive_owned();
                }
            }
            break;
        }
        case RadioCommandType::kExitMaintenance:
            if (!s_maintenance_active.load(std::memory_order_acquire)) {
                reply.result = ESP_ERR_INVALID_STATE;
                break;
            }
            s_maintenance_active.store(false, std::memory_order_release);
            reply.result = s_receive_enabled ? restore_receive_owned() : s_radio.enter_idle(50);
            if (reply.result != ESP_OK) {
                s_maintenance_active.store(true, std::memory_order_release);
            }
            break;
        case RadioCommandType::kStop: {
            const esp_err_t receive_error = stop_rmt_receive_owned();
            const esp_err_t idle_error = s_radio.enter_idle(50);
            reply.result = receive_error != ESP_OK ? receive_error : idle_error;
            break;
        }
    }
    transition_active_command(command.id, CommandState::kExecuting, CommandState::kCompleted);
    if (xQueueSend(s_reply_queue, &reply, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(kTag, "Could not return reply for command %lu", static_cast<unsigned long>(command.id));
    }
    return command.type == RadioCommandType::kStop;
}

void radio_task(void *)
{
    s_radio_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    s_receive_active = false;
    esp_err_t startup_result = restore_receive_owned();
    xQueueSend(s_startup_queue, &startup_result, 0);
    if (startup_result != ESP_OK) {
        s_running.store(false, std::memory_order_release);
        xSemaphoreGive(s_radio_stopped);
        s_radio_task.store(nullptr, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    bool stop = false;
    while (!stop && s_running.load(std::memory_order_relaxed)) {
        const QueueSetMemberHandle_t ready = xQueueSelectFromSet(s_radio_queue_set, pdMS_TO_TICKS(500));
        if (ready == s_rx_queue) {
            RxQueueItem item{};
            if (xQueueReceive(s_rx_queue, &item, 0) == pdTRUE) {
                process_receive_item(item);
            }
        } else if (ready == s_command_queue) {
            RadioCommand command{};
            if (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) {
                stop = execute_command(command);
            }
        }
        recover_dropped_receive_owned();
        if (ready == nullptr && s_receive_enabled && !s_receive_active &&
            !s_maintenance_requested.load(std::memory_order_relaxed)) {
            const esp_err_t recovery_error = restore_receive_owned();
            if (recovery_error != ESP_OK) {
                ESP_LOGW(kTag, "Periodic RX recovery failed: %s", esp_err_to_name(recovery_error));
            }
        }
    }
    s_receive_active = false;
    s_running.store(false, std::memory_order_release);
    xSemaphoreGive(s_radio_stopped);
    s_radio_task.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}

bool wait_for_radio_task_exit(TickType_t timeout)
{
    if (s_radio_stopped == nullptr || xSemaphoreTake(s_radio_stopped, timeout) != pdTRUE) {
        return false;
    }
    const TickType_t started = xTaskGetTickCount();
    while (s_radio_task.load(std::memory_order_acquire) != nullptr && xTaskGetTickCount() - started < timeout) {
        taskYIELD();
    }
    return s_radio_task.load(std::memory_order_acquire) == nullptr;
}

esp_err_t initialize_rmt()
{
    rmt_rx_channel_config_t rx_config{};
    rx_config.gpio_num = static_cast<gpio_num_t>(CONFIG_CC1101_GDO2_GPIO);
    rx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_config.resolution_hz = kResolutionHz;
    rx_config.mem_block_symbols = kRxSymbolCapacity;
#ifdef CONFIG_CC1101_RX_INVERT
    rx_config.flags.invert_in = true;
#endif
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_config, &s_rx_channel), kTag, "create RMT RX");

    rmt_tx_channel_config_t tx_config{};
    tx_config.gpio_num = static_cast<gpio_num_t>(CONFIG_CC1101_GDO0_GPIO);
    tx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_config.resolution_hz = kResolutionHz;
    tx_config.mem_block_symbols = 64;
    tx_config.trans_queue_depth = 1;
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_config, &s_tx_channel), kTag, "create RMT TX");

    rmt_copy_encoder_config_t encoder_config{};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&encoder_config, &s_copy_encoder), kTag, "create RMT encoder");
    rmt_rx_event_callbacks_t callbacks{};
    callbacks.on_recv_done = rx_done_callback;
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_channel, &callbacks, nullptr), kTag,
                        "register RMT callback");
    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_channel), kTag, "enable RMT TX");

    s_receive_config.signal_range_min_ns = 2500;
    s_receive_config.signal_range_max_ns = static_cast<uint32_t>(kRmtStopDurationUs) * 1000U;
    return ESP_OK;
}

esp_err_t cleanup_resources()
{
    if (s_radio_task.load(std::memory_order_acquire) != nullptr) {
        ESP_LOGE(kTag, "Refusing to clean up while the radio task is alive");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rmt_tx_faulted.load(std::memory_order_acquire)) {
        s_service_state.store(ServiceState::kStopping, std::memory_order_release);
        ESP_LOGE(kTag, "Refusing unbounded cleanup of a faulted ESP32 RMT TX channel; reboot required");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t cleanup_error = ESP_OK;
    const auto remember_error = [&](esp_err_t error, const char *operation) {
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "%s failed during cleanup: %s", operation, esp_err_to_name(error));
            if (cleanup_error == ESP_OK) {
                cleanup_error = error;
            }
        }
    };
    if (s_rx_channel != nullptr) {
        const esp_err_t disable_error = rmt_disable(s_rx_channel);
        if (disable_error != ESP_OK && disable_error != ESP_ERR_INVALID_STATE) {
            remember_error(disable_error, "disable RMT RX");
        }
        const esp_err_t delete_error = rmt_del_channel(s_rx_channel);
        if (delete_error == ESP_OK) {
            s_rx_channel = nullptr;
        } else {
            remember_error(delete_error, "delete RMT RX");
        }
    }
    if (s_tx_channel != nullptr) {
        const esp_err_t disable_error = rmt_disable(s_tx_channel);
        if (disable_error != ESP_OK && disable_error != ESP_ERR_INVALID_STATE) {
            remember_error(disable_error, "disable RMT TX");
        }
        const esp_err_t delete_error = rmt_del_channel(s_tx_channel);
        if (delete_error == ESP_OK) {
            s_tx_channel = nullptr;
        } else {
            remember_error(delete_error, "delete RMT TX");
        }
    }
    if (s_copy_encoder != nullptr) {
        const esp_err_t delete_error = rmt_del_encoder(s_copy_encoder);
        if (delete_error == ESP_OK) {
            s_copy_encoder = nullptr;
        } else {
            remember_error(delete_error, "delete RMT encoder");
        }
    }
    if (s_rx_channel != nullptr || s_tx_channel != nullptr || s_copy_encoder != nullptr) {
        s_service_state.store(ServiceState::kStopping, std::memory_order_release);
        return cleanup_error == ESP_OK ? ESP_FAIL : cleanup_error;
    }

    const esp_err_t radio_cleanup_error = s_radio.deinitialize();
    if (radio_cleanup_error != ESP_OK) {
        s_service_state.store(ServiceState::kStopping, std::memory_order_release);
        return radio_cleanup_error;
    }

    if (s_radio_queue_set != nullptr) {
        QueueSetMemberHandle_t ready = nullptr;
        while ((ready = xQueueSelectFromSet(s_radio_queue_set, 0)) != nullptr) {
            if (ready == s_rx_queue) {
                RxQueueItem discarded{};
                xQueueReceive(s_rx_queue, &discarded, 0);
            } else if (ready == s_command_queue) {
                RadioCommand discarded{};
                xQueueReceive(s_command_queue, &discarded, 0);
            }
        }
        if (s_rx_queue != nullptr && xQueueRemoveFromSet(s_rx_queue, s_radio_queue_set) != pdPASS) {
            ESP_LOGW(kTag, "Could not remove RX queue from queue set");
        }
        if (s_command_queue != nullptr && xQueueRemoveFromSet(s_command_queue, s_radio_queue_set) != pdPASS) {
            ESP_LOGW(kTag, "Could not remove command queue from queue set");
        }
        vQueueDelete(s_radio_queue_set);
        s_radio_queue_set = nullptr;
    }
    for (QueueHandle_t *queue : {&s_rx_queue, &s_command_queue, &s_reply_queue, &s_startup_queue}) {
        if (*queue != nullptr) {
            vQueueDelete(*queue);
            *queue = nullptr;
        }
    }
    if (s_radio_stopped != nullptr) {
        vSemaphoreDelete(s_radio_stopped);
        s_radio_stopped = nullptr;
    }
    s_frame_callback = nullptr;
    s_callback_context = nullptr;
    s_maintenance_active.store(false, std::memory_order_release);
    set_active_command(0, CommandState::kIdle);
    s_service_state.store(ServiceState::kStopped, std::memory_order_release);
    return cleanup_error;
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

bool command_is_blocked_by_maintenance(RadioCommandType type)
{
    return type == RadioCommandType::kSetReceive || type == RadioCommandType::kTransmitDecoded ||
           type == RadioCommandType::kTransmitRaw || type == RadioCommandType::kReplayLast ||
           type == RadioCommandType::kReset;
}

esp_err_t send_command(RadioCommand command, RadioReply *reply, bool internal = false)
{
    if (s_maintenance_requested.load(std::memory_order_acquire) &&
        command_is_blocked_by_maintenance(command.type)) {
        return ESP_ERR_INVALID_STATE;
    }
    const ServiceState initial_state = s_service_state.load(std::memory_order_acquire);
    if ((!internal && initial_state != ServiceState::kRunning) ||
        (internal && initial_state == ServiceState::kStopped) || s_api_mutex == nullptr ||
        xTaskGetCurrentTaskHandle() == s_radio_task.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t mutex_timeout = internal ? kCommandTimeoutTicks : kApiMutexTimeoutTicks;
    if (xSemaphoreTake(s_api_mutex, mutex_timeout) != pdTRUE) {
        s_command_timeouts.fetch_add(1, std::memory_order_relaxed);
        return ESP_ERR_TIMEOUT;
    }
    const ServiceState locked_state = s_service_state.load(std::memory_order_acquire);
    if ((!internal && locked_state != ServiceState::kRunning) ||
        (internal && locked_state == ServiceState::kStopped) || s_command_queue == nullptr ||
        s_reply_queue == nullptr || !s_running.load(std::memory_order_acquire) ||
        (s_maintenance_requested.load(std::memory_order_acquire) &&
         command_is_blocked_by_maintenance(command.type))) {
        xSemaphoreGive(s_api_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    command.id = s_command_sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    set_active_command(command.id, CommandState::kQueued);
    if (xQueueSend(s_command_queue, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
        set_active_command(0, CommandState::kIdle);
        s_command_timeouts.fetch_add(1, std::memory_order_relaxed);
        xSemaphoreGive(s_api_mutex);
        return ESP_ERR_TIMEOUT;
    }

    RadioReply observed{};
    const auto receive_reply_until = [&](TickType_t timeout) {
        const TickType_t started = xTaskGetTickCount();
        while (xTaskGetTickCount() - started < timeout) {
            const TickType_t elapsed = xTaskGetTickCount() - started;
            if (xQueueReceive(s_reply_queue, &observed, timeout - elapsed) != pdTRUE) {
                break;
            }
            if (observed.id == command.id) {
                return true;
            }
        }
        return false;
    };

    bool received = receive_reply_until(kCommandTimeoutTicks);
    if (!received && transition_active_command(command.id, CommandState::kQueued, CommandState::kCancelled)) {
        if (internal && command.type == RadioCommandType::kStop) {
            s_running.store(false, std::memory_order_release);
        }
        s_command_timeouts.fetch_add(1, std::memory_order_relaxed);
        xSemaphoreGive(s_api_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (!received) {
        received = receive_reply_until(kCommandTimeoutTicks);
    }
    if (!received) {
        s_command_timeouts.fetch_add(1, std::memory_order_relaxed);
        s_service_state.store(ServiceState::kStopping, std::memory_order_release);
        s_running.store(false, std::memory_order_release);
        xSemaphoreGive(s_api_mutex);
        return ESP_ERR_TIMEOUT;
    }

    set_active_command(0, CommandState::kIdle);
    const esp_err_t result = observed.result;
    if (reply != nullptr) {
        *reply = observed;
    }
    xSemaphoreGive(s_api_mutex);
    return result;
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
    return decode_known_protocol(levels, durations_us, count, frame, capture_started_after_idle, 1) ==
           KnownDecodeResult::kDecoded;
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

esp_err_t start_rf_ook(RfFrameCallback callback, void *context)
{
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_maintenance_requested.load(std::memory_order_acquire) ||
        s_service_state.load(std::memory_order_acquire) != ServiceState::kStopped ||
        s_running.load(std::memory_order_relaxed) ||
        s_radio_task.load(std::memory_order_acquire) != nullptr || s_rx_queue != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_service_state.store(ServiceState::kStarting, std::memory_order_release);

    s_rx_queue = xQueueCreate(4, sizeof(RxQueueItem));
    s_command_queue = xQueueCreate(4, sizeof(RadioCommand));
    s_reply_queue = xQueueCreate(4, sizeof(RadioReply));
    s_startup_queue = xQueueCreate(1, sizeof(esp_err_t));
    s_radio_queue_set = xQueueCreateSet(8);
    if (s_api_mutex == nullptr) {
        s_api_mutex = xSemaphoreCreateMutex();
    }
    s_radio_stopped = xSemaphoreCreateBinary();
    if (s_rx_queue == nullptr || s_command_queue == nullptr || s_reply_queue == nullptr ||
        s_startup_queue == nullptr || s_radio_queue_set == nullptr || s_api_mutex == nullptr ||
        s_radio_stopped == nullptr || xQueueAddToSet(s_rx_queue, s_radio_queue_set) != pdPASS ||
        xQueueAddToSet(s_command_queue, s_radio_queue_set) != pdPASS) {
        cleanup_resources();
        return ESP_ERR_NO_MEM;
    }

    s_frame_callback = callback;
    s_callback_context = context;
    s_receive_enabled = true;
    s_receive_active = false;
    s_has_last_frame = false;
    s_last_frame = {};
    s_last_reported_us = 0;
    s_accepted_frames = 0;
    s_suppressed_duplicates = 0;
    s_truncated_captures = 0;
    s_next_rx_buffer = 0;
    s_rx_generation.store(0, std::memory_order_relaxed);
    s_rx_queue_drops.store(0, std::memory_order_relaxed);
    s_command_timeouts.store(0, std::memory_order_relaxed);
    s_rx_rearm_required.store(false, std::memory_order_relaxed);
    s_rmt_tx_faulted.store(false, std::memory_order_relaxed);
    s_maintenance_active.store(false, std::memory_order_relaxed);

    const esp_err_t activity_led_error = initialize_rf_activity_led();
    if (activity_led_error != ESP_OK &&
        !s_activity_led_warning_logged.exchange(true, std::memory_order_acq_rel)) {
        ESP_LOGW(kTag, "RX activity LED unavailable: %s", esp_err_to_name(activity_led_error));
    }

    const Cc1101Config radio_config{
        .sclk_gpio = CONFIG_CC1101_SPI_SCLK_GPIO,
        .miso_gpio = CONFIG_CC1101_SPI_MISO_GPIO,
        .mosi_gpio = CONFIG_CC1101_SPI_MOSI_GPIO,
        .cs_gpio = CONFIG_CC1101_SPI_CS_GPIO,
        .gdo0_gpio = CONFIG_CC1101_GDO0_GPIO,
        .gdo2_gpio = CONFIG_CC1101_GDO2_GPIO,
        .crystal_hz = CONFIG_CC1101_XTAL_HZ,
        .frequency_hz = CONFIG_CC1101_FREQUENCY_HZ,
        .tx_power_dbm = CONFIG_CC1101_TX_POWER_DBM,
    };
    esp_err_t error = s_radio.initialize(radio_config);
    if (error == ESP_OK) {
        error = initialize_rmt();
    }
    if (error != ESP_OK) {
        cleanup_resources();
        return error;
    }

    s_running.store(true, std::memory_order_release);
    if (xTaskCreate(radio_task, "rf_radio", 12288, nullptr, 12, nullptr) != pdPASS) {
        s_running.store(false, std::memory_order_release);
        cleanup_resources();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t startup_result = ESP_ERR_TIMEOUT;
    const bool startup_received =
        xQueueReceive(s_startup_queue, &startup_result, pdMS_TO_TICKS(1000)) == pdTRUE;
    if (!startup_received) {
        RadioCommand stop_command{};
        stop_command.type = RadioCommandType::kStop;
        const esp_err_t stop_error = send_command(stop_command, nullptr, true);
        if (stop_error != ESP_OK) {
            ESP_LOGW(kTag, "Startup-timeout stop operation reported: %s", esp_err_to_name(stop_error));
        }
        if (wait_for_radio_task_exit(kShutdownTimeoutTicks)) {
            cleanup_resources();
        } else {
            s_service_state.store(ServiceState::kStopping, std::memory_order_release);
            ESP_LOGE(kTag, "Radio startup timed out and safe shutdown failed; resources retained");
        }
        return ESP_ERR_TIMEOUT;
    }
    if (startup_result != ESP_OK) {
        if (wait_for_radio_task_exit(pdMS_TO_TICKS(1000))) {
            cleanup_resources();
        } else {
            ESP_LOGE(kTag, "Radio startup failed but task did not stop; resources retained");
        }
        return startup_result;
    }
    s_service_state.store(ServiceState::kRunning, std::memory_order_release);
    ESP_LOGI(kTag, "RX GPIO%d, TX GPIO%d; listening at %lu Hz", CONFIG_CC1101_GDO2_GPIO,
             CONFIG_CC1101_GDO0_GPIO, static_cast<unsigned long>(CONFIG_CC1101_FREQUENCY_HZ));
    return ESP_OK;
}

void stop_rf_ook()
{
    const TaskHandle_t task = s_radio_task.load(std::memory_order_acquire);
    if (task != nullptr && xTaskGetCurrentTaskHandle() == task) {
        ESP_LOGE(kTag, "stop_rf_ook cannot be called from the radio callback task");
        return;
    }
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired()) {
        ESP_LOGW(kTag, "Radio lifecycle operation already in progress");
        return;
    }
    if (task == nullptr) {
        if (s_service_state.load(std::memory_order_acquire) != ServiceState::kStopped &&
            !wait_for_radio_task_exit(kShutdownTimeoutTicks)) {
            ESP_LOGE(kTag, "Radio task exit was not confirmed; resources retained");
            return;
        }
        const esp_err_t cleanup_error = cleanup_resources();
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(kTag, "Radio cleanup remains incomplete: %s", esp_err_to_name(cleanup_error));
        }
        return;
    }
    s_service_state.store(ServiceState::kStopping, std::memory_order_release);
    if (s_running.load(std::memory_order_acquire)) {
        RadioCommand command{};
        command.type = RadioCommandType::kStop;
        const esp_err_t command_error = send_command(command, nullptr, true);
        if (command_error != ESP_OK) {
            ESP_LOGW(kTag, "Radio stop operation reported: %s", esp_err_to_name(command_error));
        }
    }
    if (!wait_for_radio_task_exit(kShutdownTimeoutTicks)) {
        ESP_LOGE(kTag, "Radio task did not stop safely; resources retained");
        return;
    }
    const esp_err_t cleanup_error = cleanup_resources();
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(kTag, "Radio cleanup remains incomplete: %s", esp_err_to_name(cleanup_error));
    }
}

esp_err_t set_rf_receive_enabled(bool enabled)
{
    RadioCommand command{};
    command.type = RadioCommandType::kSetReceive;
    command.enabled = enabled;
    return send_command(command, nullptr);
}

esp_err_t transmit_rf_decoded(const DecodedSignal &signal, uint16_t repeats)
{
    RadioCommand command{};
    command.type = RadioCommandType::kTransmitDecoded;
    command.decoded = signal;
    command.repeats = repeats;
    return send_command(command, nullptr);
}

esp_err_t transmit_rf_raw(const RawSignal &signal, uint16_t repeats)
{
    RadioCommand command{};
    command.type = RadioCommandType::kTransmitRaw;
    command.raw = signal;
    command.repeats = repeats;
    return send_command(command, nullptr);
}

esp_err_t replay_last_rf_frame(uint16_t repeats)
{
    RadioCommand command{};
    command.type = RadioCommandType::kReplayLast;
    command.repeats = repeats;
    return send_command(command, nullptr);
}

esp_err_t get_last_rf_frame(RfFrame *frame)
{
    if (frame == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    RadioCommand command{};
    command.type = RadioCommandType::kGetLast;
    RadioReply reply{};
    const esp_err_t error = send_command(command, &reply);
    if (error == ESP_OK) {
        *frame = reply.frame;
    }
    return error;
}

esp_err_t get_rf_radio_status(RfRadioStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    RadioCommand command{};
    command.type = RadioCommandType::kGetStatus;
    RadioReply reply{};
    const esp_err_t error = send_command(command, &reply);
    if (error == ESP_OK) {
        *status = reply.status;
        return ESP_OK;
    }

    fill_software_status(status);
    status->cc1101_info_valid = false;
    status->cc1101_error = error;
    return ESP_OK;
}

esp_err_t reset_rf_radio()
{
    RadioCommand command{};
    command.type = RadioCommandType::kReset;
    return send_command(command, nullptr);
}

esp_err_t begin_rf_maintenance()
{
    bool expected = false;
    if (!s_maintenance_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired()) {
        s_maintenance_requested.store(false, std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    const ServiceState state = s_service_state.load(std::memory_order_acquire);
    if (state == ServiceState::kStopped) {
        return ESP_OK;
    }
    if (state != ServiceState::kRunning) {
        s_maintenance_requested.store(false, std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    RadioCommand command{};
    command.type = RadioCommandType::kEnterMaintenance;
    const esp_err_t error = send_command(command, nullptr, true);
    if (error != ESP_OK) {
        s_maintenance_requested.store(false, std::memory_order_release);
    }
    return error;
}

esp_err_t end_rf_maintenance()
{
    if (!s_maintenance_requested.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired()) {
        return ESP_ERR_INVALID_STATE;
    }
    const ServiceState state = s_service_state.load(std::memory_order_acquire);
    esp_err_t error = ESP_OK;
    if (state == ServiceState::kRunning) {
        RadioCommand command{};
        command.type = RadioCommandType::kExitMaintenance;
        error = send_command(command, nullptr, true);
    } else if (state != ServiceState::kStopped) {
        return ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        s_maintenance_requested.store(false, std::memory_order_release);
    }
    return error;
}

bool rf_maintenance_is_active()
{
    return s_maintenance_requested.load(std::memory_order_acquire);
}

}  // namespace rfbridge
