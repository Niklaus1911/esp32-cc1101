#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_codec.hpp"

// Capture decoding shared by the radio task and host tests; no ESP-IDF dependencies.
namespace rfbridge {

constexpr std::size_t kRfMaxCapturePulses = 896;
// The RMT idle threshold: a final pulse of exactly this length was cut off, not measured.
constexpr uint16_t kRfCaptureStopDurationUs = 30000;
constexpr uint16_t kRfTerminalIdleMinimumUs = 25000;
// A lone frame (no repeat in the capture) must carry at least this many bits. Random and
// receiver-AGC noise fits a protocol's timing for a few bits plus a sync gap far more often than
// for a full code; short codes still decode when they repeat.
constexpr uint8_t kRfMinimumSingleFrameBits = 8;

enum class RfEncoding : uint8_t {
    kDecoded,
    kRaw,
};

enum class RfFrameConfidence : uint8_t {
    kRepeatedEvidence,
    kSingleDecoded,
};

struct RfFrame {
    RfEncoding encoding;
    RfFrameConfidence confidence = RfFrameConfidence::kRepeatedEvidence;
    uint32_t fingerprint;
    uint16_t observed_repeats;
    int64_t captured_us;
    DecodedSignal decoded;
    RawSignal raw;
};

bool decode_rf_pulses(const uint8_t *levels, const uint16_t *durations_us, std::size_t count, RfFrame *frame,
                      bool capture_may_be_truncated = false, bool capture_started_after_idle = false);
bool rf_frames_equivalent(const RfFrame &left, const RfFrame &right);
uint32_t rf_frame_fingerprint(const RfFrame &frame);

}  // namespace rfbridge
