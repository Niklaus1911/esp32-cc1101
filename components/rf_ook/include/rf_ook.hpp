#pragma once

#include <cstddef>
#include <cstdint>

#include "cc1101.hpp"
#include "esp_err.h"
#include "rf_codec.hpp"
#include "rf_frame_decode.hpp"
#include "rf_storage_format.hpp"

namespace rfbridge {

struct RfRadioStatus {
    RfHardware hardware = RfHardware::kCc1101;
    bool running;
    bool receive_enabled;
    bool receive_active;
    bool transmitting;
    bool maintenance_active;
    bool has_last_frame;
    uint32_t accepted_frames;
    uint32_t suppressed_duplicates;
    uint32_t rx_queue_drops;
    uint32_t truncated_captures;
    uint32_t completed_captures;
    uint32_t capture_open_ms;
    uint32_t line_samples;
    uint32_t line_high_samples;
    uint32_t line_transitions;
    uint32_t command_timeouts;
    bool cc1101_info_valid;
    esp_err_t cc1101_error;
    Cc1101Info cc1101;
    int generic_tx_gpio = -1;
    int generic_rx_gpio = -1;
    esp_err_t hardware_switch_error = ESP_OK;
    uint32_t hardware_switches = 0;
};

struct RfGenericGpioConfig {
    int active_tx_gpio = -1;
    int active_rx_gpio = -1;
    int saved_tx_gpio = -1;
    int saved_rx_gpio = -1;
    int default_tx_gpio = -1;
    int default_rx_gpio = -1;
    bool saved = false;
    bool pending = false;
    bool overlap_cc1101 = false;
    esp_err_t configuration_error = ESP_OK;
};

using RfFrameCallback = void (*)(const RfFrame &frame, void *context);

esp_err_t start_rf_ook(RfFrameCallback callback, void *context);
void stop_rf_ook();
esp_err_t transmit_rf_decoded(const DecodedSignal &signal, uint16_t repeats);
esp_err_t transmit_rf_raw(const RawSignal &signal, uint16_t repeats);
esp_err_t get_last_rf_frame(RfFrame *frame);
esp_err_t get_rf_radio_status(RfRadioStatus *status);
esp_err_t initialize_rf_generic_gpio_config();
esp_err_t get_rf_generic_gpio_config(RfGenericGpioConfig *config);
esp_err_t set_rf_generic_gpio_config(int tx_gpio, int rx_gpio,
                                     bool *reboot_required = nullptr);
esp_err_t get_rf_hardware(RfHardware *hardware);
esp_err_t set_rf_hardware(RfHardware hardware, RfFrameCallback restart_callback = nullptr,
                          void *restart_context = nullptr);
esp_err_t reset_rf_radio();
esp_err_t begin_rf_maintenance();
esp_err_t end_rf_maintenance();
bool rf_maintenance_is_active();

}  // namespace rfbridge
