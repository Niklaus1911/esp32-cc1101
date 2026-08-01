#pragma once

#include <cstddef>
#include <cstdint>

#include "cc1101.hpp"
#include "esp_err.h"
#include "rf_codec.hpp"

namespace rfbridge {

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

struct RfRadioStatus {
    bool running;
    bool receive_enabled;
    bool receive_active;
    bool transmitting;
    bool has_last_frame;
    uint32_t accepted_frames;
    uint32_t suppressed_duplicates;
    uint32_t rx_queue_drops;
    uint32_t truncated_captures;
    uint32_t command_timeouts;
    bool cc1101_info_valid;
    esp_err_t cc1101_error;
    Cc1101Info cc1101;
};

using RfFrameCallback = void (*)(const RfFrame &frame, void *context);

esp_err_t start_rf_ook(RfFrameCallback callback, void *context);
void stop_rf_ook();
esp_err_t set_rf_receive_enabled(bool enabled);
esp_err_t transmit_rf_decoded(const DecodedSignal &signal, uint16_t repeats);
esp_err_t transmit_rf_raw(const RawSignal &signal, uint16_t repeats);
esp_err_t replay_last_rf_frame(uint16_t repeats);
esp_err_t get_last_rf_frame(RfFrame *frame);
esp_err_t get_rf_radio_status(RfRadioStatus *status);
esp_err_t reset_rf_radio();

bool decode_rf_pulses(const uint8_t *levels, const uint16_t *durations_us, std::size_t count, RfFrame *frame,
                      bool capture_may_be_truncated = false, bool capture_started_after_idle = false);
bool rf_frames_equivalent(const RfFrame &left, const RfFrame &right);
uint32_t rf_frame_fingerprint(const RfFrame &frame);

}  // namespace rfbridge
