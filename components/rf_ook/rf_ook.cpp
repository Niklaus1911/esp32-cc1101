#include "rf_ook.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <iterator>

#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rf_activity_led_private.hpp"
#include "rf_storage.hpp"
#include "platform_board.hpp"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

constexpr char kTag[] = "rf_ook";
constexpr uint32_t kResolutionHz = 1000000;
constexpr std::size_t kRxSymbolCapacity = 448;
constexpr std::size_t kPulseCapacity = kRxSymbolCapacity * 2;
constexpr uint16_t kTerminalIdleMinimumUs = kRfTerminalIdleMinimumUs;
constexpr uint16_t kRmtStopDurationUs = kRfCaptureStopDurationUs;
static_assert(kPulseCapacity == kRfMaxCapturePulses, "decoder and RMT capture capacity differ");
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
    kTransmitDecoded,
    kTransmitRaw,
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
#if CONFIG_IDF_TARGET_ESP32S3
rmt_symbol_word_t *s_rx_symbols[2]{};
#else
rmt_symbol_word_t s_rx_symbol_storage[2][kRxSymbolCapacity]{};
rmt_symbol_word_t *s_rx_symbols[2] = {s_rx_symbol_storage[0], s_rx_symbol_storage[1]};
#endif
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
std::atomic<bool> s_hardware_switch_busy{false};
std::atomic<RfHardware> s_hardware{RfHardware::kCc1101};
std::atomic<bool> s_start_override_valid{false};
std::atomic<RfHardware> s_start_override{RfHardware::kCc1101};
std::atomic<esp_err_t> s_hardware_switch_error{ESP_OK};
std::atomic<uint32_t> s_hardware_switches{0};
std::atomic<bool> s_generic_gpio_initialized{false};
std::atomic_flag s_generic_gpio_initializing = ATOMIC_FLAG_INIT;
portMUX_TYPE s_generic_gpio_mux = portMUX_INITIALIZER_UNLOCKED;
// The active GPIO map has a single writer: load_generic_gpio_config() assigns it once
// during boot initialization before publishing s_generic_gpio_initialized. Later calls
// (including set_rf_generic_gpio_config()) only mutate the saved/pending fields of
// s_generic_gpio_config, never the active map, so lock-free reads are safe. Any future
// write to the active map after publication must take s_generic_gpio_mux and re-audit
// every active_gpio_map() reader.
BoardGpioMap s_active_gpio_map{};
RfGenericGpioConfig s_generic_gpio_config{};
rmt_receive_config_t s_receive_config{};
Cc1101 s_radio{};
std::atomic<bool> s_receive_active{false};
std::atomic<bool> s_has_last_frame{false};
RfFrame s_last_frame{};
int64_t s_last_reported_us = 0;
std::atomic<uint32_t> s_accepted_frames{0};
std::atomic<uint32_t> s_suppressed_duplicates{0};
std::atomic<uint32_t> s_truncated_captures{0};
std::atomic<uint32_t> s_completed_captures{0};
std::atomic<int64_t> s_receive_armed_us{0};
// Sampled RX DATA line activity. A capture only completes after a kRmtStopDurationUs idle gap, so a
// receiver that never idles (no squelch) leaves RX armed forever; these tell that apart from a dead line.
std::atomic<uint32_t> s_line_samples{0};
std::atomic<uint32_t> s_line_high_samples{0};
std::atomic<uint32_t> s_line_transitions{0};
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

class HardwareSwitchGuard {
public:
    HardwareSwitchGuard()
    {
        bool expected = false;
        acquired_ = s_hardware_switch_busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel);
    }

    ~HardwareSwitchGuard()
    {
        if (acquired_) {
            s_hardware_switch_busy.store(false, std::memory_order_release);
        }
    }

    bool acquired() const
    {
        return acquired_;
    }

private:
    bool acquired_ = false;
};

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

bool using_generic_hardware()
{
    return s_hardware.load(std::memory_order_acquire) == RfHardware::kGeneric;
}

bool generic_gpio_overlaps_cc1101(const BoardGpioMap &gpios)
{
    const int cc_pins[] = {gpios.sclk, gpios.miso, gpios.mosi, gpios.cs, gpios.gdo0,
                           gpios.gdo2};
    for (const int cc_pin : cc_pins) {
        if (cc_pin == gpios.generic_tx || cc_pin == gpios.generic_rx) {
            return true;
        }
    }
    return false;
}

bool gpio_is_cc1101_pin(const BoardGpioMap &gpios, int gpio)
{
    return gpio == gpios.sclk || gpio == gpios.miso || gpio == gpios.mosi || gpio == gpios.cs ||
           gpio == gpios.gdo0 || gpio == gpios.gdo2;
}

esp_err_t load_generic_gpio_config()
{
    const BoardInfo &board = current_board_info();
    BoardGpioMap active = board.cc1101;
    RfGenericGpioConfig config{};
    config.active_tx_gpio = active.generic_tx;
    config.active_rx_gpio = active.generic_rx;
    config.default_tx_gpio = active.generic_tx;
    config.default_rx_gpio = active.generic_rx;
    config.saved_tx_gpio = active.generic_tx;
    config.saved_rx_gpio = active.generic_rx;

    uint8_t profile_id = 0;
    uint8_t tx_gpio = 0;
    uint8_t rx_gpio = 0;
    bool persisted = false;
    esp_err_t load_error = rf_storage_generic_gpio_get(&profile_id, &tx_gpio, &rx_gpio, &persisted);
    if (load_error == ESP_OK && persisted) {
        const bool profile_matches =
            profile_id == static_cast<uint8_t>(configured_board_profile());
        const bool valid = profile_matches &&
                           board_generic_gpio_map_is_valid(
                               configured_board_profile(), board.cc1101, tx_gpio, rx_gpio,
                               board.activity_led_enabled ? board.activity_led_gpio : -1);
        if (valid) {
            active.generic_tx = tx_gpio;
            active.generic_rx = rx_gpio;
            config.active_tx_gpio = tx_gpio;
            config.active_rx_gpio = rx_gpio;
            config.saved_tx_gpio = tx_gpio;
            config.saved_rx_gpio = rx_gpio;
            config.saved = true;
        } else {
            load_error = profile_matches ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_INVALID_STATE;
            config.configuration_error = load_error;
        }
    } else if (load_error != ESP_OK) {
        config.configuration_error = load_error;
    }
    if (load_error == ESP_OK && !persisted) {
        config.configuration_error = ESP_OK;
    }
    config.overlap_cc1101 = generic_gpio_overlaps_cc1101(active);
    portENTER_CRITICAL(&s_generic_gpio_mux);
    s_active_gpio_map = active;
    s_generic_gpio_config = config;
    s_generic_gpio_initialized.store(true, std::memory_order_release);
    portEXIT_CRITICAL(&s_generic_gpio_mux);
    return load_error;
}

const BoardGpioMap &active_gpio_map()
{
    return s_active_gpio_map;
}

int active_rx_gpio()
{
    return using_generic_hardware() ? active_gpio_map().generic_rx : active_gpio_map().gdo2;
}

int active_tx_gpio()
{
    return using_generic_hardware() ? active_gpio_map().generic_tx : active_gpio_map().gdo0;
}

esp_err_t configure_generic_tx_idle()
{
    const int gpio = active_gpio_map().generic_tx;
    ESP_RETURN_ON_ERROR(gpio_set_direction(static_cast<gpio_num_t>(gpio), GPIO_MODE_OUTPUT),
                        kTag, "configure generic TX idle");
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(gpio), 0), kTag,
                        "drive generic TX idle");
    return ESP_OK;
}

void force_generic_tx_idle()
{
    const int gpio = active_gpio_map().generic_tx;
    if (gpio < 0) {
        return;
    }
    (void)gpio_reset_pin(static_cast<gpio_num_t>(gpio));
    (void)gpio_set_direction(static_cast<gpio_num_t>(gpio), GPIO_MODE_OUTPUT);
    (void)gpio_set_level(static_cast<gpio_num_t>(gpio), 0);
}

esp_err_t prepare_backend_gpio_ownership(RfHardware hardware)
{
    const BoardGpioMap &gpios = active_gpio_map();
    const bool generic_tx_shared = gpio_is_cc1101_pin(gpios, gpios.generic_tx);
    const bool generic_rx_shared = gpio_is_cc1101_pin(gpios, gpios.generic_rx);
    if (generic_tx_shared) {
        ESP_RETURN_ON_ERROR(gpio_reset_pin(static_cast<gpio_num_t>(gpios.generic_tx)), kTag,
                            "release shared generic TX and CC1101 GPIO");
    }
    if (generic_rx_shared && gpios.generic_rx != gpios.generic_tx) {
        ESP_RETURN_ON_ERROR(gpio_reset_pin(static_cast<gpio_num_t>(gpios.generic_rx)), kTag,
                            "release shared generic RX and CC1101 GPIO");
    }
    return hardware == RfHardware::kGeneric || !generic_tx_shared
               ? configure_generic_tx_idle()
               : ESP_OK;
}

esp_err_t arm_receiver_owned()
{
    if (s_rx_channel == nullptr || s_receive_active ||
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
    const esp_err_t error = rmt_receive(s_rx_channel, s_rx_symbols[buffer_index],
                                        kRxSymbolCapacity * sizeof(rmt_symbol_word_t),
                                        &s_receive_config);
    if (error == ESP_OK) {
        s_next_rx_buffer = (buffer_index + 1U) % std::size(s_rx_symbols);
        s_receive_armed_us.store(static_cast<int64_t>(armed_us), std::memory_order_relaxed);
        s_receive_active = true;
    }
    return error;
}

void sample_rx_line_owned()
{
    constexpr uint32_t kSamples = 250;
    constexpr uint32_t kSampleSpacingUs = 16;
    const gpio_num_t gpio = static_cast<gpio_num_t>(active_rx_gpio());
    if (!s_receive_active || gpio < 0) {
        return;
    }
    int previous = gpio_get_level(gpio);
    uint32_t high = previous != 0 ? 1U : 0U;
    uint32_t transitions = 0;
    for (uint32_t index = 1; index < kSamples; ++index) {
        esp_rom_delay_us(kSampleSpacingUs);
        const int level = gpio_get_level(gpio);
        high += level != 0 ? 1U : 0U;
        transitions += level != previous ? 1U : 0U;
        previous = level;
    }
    s_line_samples.fetch_add(kSamples, std::memory_order_relaxed);
    s_line_high_samples.fetch_add(high, std::memory_order_relaxed);
    s_line_transitions.fetch_add(transitions, std::memory_order_relaxed);
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
    esp_err_t error = ESP_OK;
    if (!using_generic_hardware()) {
        error = s_radio.enter_receive(50);
        if (error != ESP_OK) {
            error = s_radio.recover_receive();
        }
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
        if (!using_generic_hardware()) {
            s_radio.enter_idle(50);
        }
    }
    return error;
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
    const esp_err_t restore_error = restore_receive_owned();
    if (restore_error != ESP_OK) {
        ESP_LOGE(kTag, "Could not rearm RX after queue drop: %s", esp_err_to_name(restore_error));
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
    if (!using_generic_hardware()) {
        error = s_radio.enter_transmit(50);
    }

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
        const esp_err_t idle_error = using_generic_hardware() ? ESP_OK : s_radio.enter_idle(50);
        const esp_err_t reset_error = idle_error == ESP_OK || using_generic_hardware()
                                          ? ESP_OK
                                          : s_radio.reset_and_configure();
        s_transmitting.store(false, std::memory_order_release);
        s_rmt_tx_faulted.store(true, std::memory_order_release);
        s_service_state.store(ServiceState::kStopping, std::memory_order_release);
        s_running.store(false, std::memory_order_release);
        if (using_generic_hardware()) {
            force_generic_tx_idle();
        }
        if (idle_error == ESP_OK || reset_error == ESP_OK) {
            ESP_LOGE(kTag, "RMT TX did not stop on time; RF was forced idle and service restart requires a reboot");
        } else {
            ESP_LOGE(kTag, "RMT TX and CC1101 shutdown both failed; carrier state is unknown and reboot is required");
        }
        return error;
    }

    esp_rom_delay_us(3000);
    const esp_err_t idle_error = using_generic_hardware() ? ESP_OK : s_radio.enter_idle(50);
    s_transmitting.store(false, std::memory_order_release);
    if (error == ESP_OK && idle_error != ESP_OK) {
        error = idle_error;
    }
    if (idle_error != ESP_OK) {
        const esp_err_t recovery_error = using_generic_hardware() ? ESP_OK : s_radio.reset_and_configure();
        if (error == ESP_OK) {
            error = recovery_error;
        }
    }

    const esp_err_t restore_error = restore_receive_owned();
    if (restore_error != ESP_OK) {
        ESP_LOGE(kTag, "Could not restore RX after TX: %s", esp_err_to_name(restore_error));
        if (error == ESP_OK) {
            error = restore_error;
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
    if (s_transmitting.load(std::memory_order_relaxed) ||
        s_maintenance_requested.load(std::memory_order_relaxed) ||
        item.generation != s_rx_generation.load(std::memory_order_relaxed)) {
        return;
    }
    s_receive_active = false;
    ++s_completed_captures;
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
    status->hardware = s_hardware.load(std::memory_order_acquire);
    status->generic_tx_gpio = active_gpio_map().generic_tx;
    status->generic_rx_gpio = active_gpio_map().generic_rx;
    status->hardware_switch_error = s_hardware_switch_error.load(std::memory_order_relaxed);
    status->hardware_switches = s_hardware_switches.load(std::memory_order_relaxed);
    status->running = s_service_state.load(std::memory_order_acquire) == ServiceState::kRunning;
    status->receive_enabled = true;
    status->receive_active = s_receive_active.load(std::memory_order_relaxed);
    status->transmitting = s_transmitting.load(std::memory_order_relaxed);
    status->maintenance_active = s_maintenance_requested.load(std::memory_order_relaxed);
    status->has_last_frame = s_has_last_frame.load(std::memory_order_relaxed);
    status->accepted_frames = s_accepted_frames.load(std::memory_order_relaxed);
    status->suppressed_duplicates = s_suppressed_duplicates.load(std::memory_order_relaxed);
    status->rx_queue_drops = s_rx_queue_drops.load(std::memory_order_relaxed);
    status->truncated_captures = s_truncated_captures.load(std::memory_order_relaxed);
    status->completed_captures = s_completed_captures.load(std::memory_order_relaxed);
    if (status->receive_active) {
        const int64_t open_us = esp_timer_get_time() - s_receive_armed_us.load(std::memory_order_relaxed);
        status->capture_open_ms = static_cast<uint32_t>(std::clamp<int64_t>(open_us / 1000, 0, UINT32_MAX));
    }
    status->line_samples = s_line_samples.load(std::memory_order_relaxed);
    status->line_high_samples = s_line_high_samples.load(std::memory_order_relaxed);
    status->line_transitions = s_line_transitions.load(std::memory_order_relaxed);
    status->command_timeouts = s_command_timeouts.load(std::memory_order_relaxed);
}

esp_err_t fill_status_owned(RfRadioStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_software_status(status);
    if (using_generic_hardware()) {
        status->cc1101_error = ESP_ERR_NOT_SUPPORTED;
        status->cc1101_info_valid = false;
    } else {
        status->cc1101_error = s_radio.read_info(&status->cc1101);
        status->cc1101_info_valid = status->cc1101_error == ESP_OK;
    }
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
        case RadioCommandType::kTransmitDecoded:
            reply.result = transmit_decoded_owned(command.decoded, command.repeats);
            break;
        case RadioCommandType::kTransmitRaw:
            reply.result = transmit_raw_owned(command.raw, command.repeats);
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
            reply.result = stop_error == ESP_OK
                               ? (using_generic_hardware() ? ESP_OK : s_radio.reset_and_configure())
                               : stop_error;
            if (reply.result == ESP_OK) {
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
            const esp_err_t idle_error = using_generic_hardware() ? ESP_OK : s_radio.enter_idle(50);
            reply.result = receive_error != ESP_OK ? receive_error : idle_error;
            if (reply.result != ESP_OK) {
                s_maintenance_active.store(false, std::memory_order_release);
                restore_receive_owned();
            }
            break;
        }
        case RadioCommandType::kExitMaintenance:
            if (!s_maintenance_active.load(std::memory_order_acquire)) {
                reply.result = ESP_ERR_INVALID_STATE;
                break;
            }
            s_maintenance_active.store(false, std::memory_order_release);
            reply.result = restore_receive_owned();
            if (reply.result != ESP_OK) {
                s_maintenance_active.store(true, std::memory_order_release);
            }
            break;
        case RadioCommandType::kStop: {
            const esp_err_t receive_error = stop_rmt_receive_owned();
            const esp_err_t idle_error = using_generic_hardware() ? ESP_OK : s_radio.enter_idle(50);
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
        if (ready == nullptr) {
            sample_rx_line_owned();
        }
        if (ready == nullptr && !s_receive_active &&
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
    rx_config.gpio_num = static_cast<gpio_num_t>(active_rx_gpio());
    if (using_generic_hardware()) {
        ESP_RETURN_ON_ERROR(
            gpio_set_direction(static_cast<gpio_num_t>(active_gpio_map().generic_rx), GPIO_MODE_INPUT),
            kTag, "configure generic RX input");
        ESP_RETURN_ON_ERROR(
            gpio_set_pull_mode(static_cast<gpio_num_t>(active_gpio_map().generic_rx), GPIO_PULLDOWN_ONLY),
            kTag, "configure generic RX pull-down");
    }
    rx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_config.resolution_hz = kResolutionHz;
#if CONFIG_IDF_TARGET_ESP32S3
    rx_config.mem_block_symbols = 48;
    rx_config.flags.with_dma = true;
#else
    rx_config.mem_block_symbols = kRxSymbolCapacity;
#endif
#ifdef CONFIG_CC1101_RX_INVERT
    rx_config.flags.invert_in = !using_generic_hardware();
#endif
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_config, &s_rx_channel), kTag, "create RMT RX");

    rmt_tx_channel_config_t tx_config{};
    tx_config.gpio_num = static_cast<gpio_num_t>(active_tx_gpio());
    tx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_config.resolution_hz = kResolutionHz;
#if CONFIG_IDF_TARGET_ESP32S3
    tx_config.mem_block_symbols = 48;
    tx_config.flags.with_dma = true;
#else
    tx_config.mem_block_symbols = 64;
#endif
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

esp_err_t allocate_rx_symbol_buffers()
{
#if CONFIG_IDF_TARGET_ESP32S3
    constexpr uint32_t kCapabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    constexpr std::size_t kBufferSize = kRxSymbolCapacity * sizeof(rmt_symbol_word_t);
    for (rmt_symbol_word_t *&buffer : s_rx_symbols) {
        if (buffer == nullptr) {
            buffer = static_cast<rmt_symbol_word_t *>(
                heap_caps_aligned_alloc(64, kBufferSize, kCapabilities));
            if (buffer == nullptr) {
                for (rmt_symbol_word_t *&allocated : s_rx_symbols) {
                    heap_caps_free(allocated);
                    allocated = nullptr;
                }
                return ESP_ERR_NO_MEM;
            }
            std::memset(buffer, 0, kBufferSize);
        }
    }
#endif
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

#if CONFIG_IDF_TARGET_ESP32S3
    for (rmt_symbol_word_t *&buffer : s_rx_symbols) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
#endif

    const esp_err_t radio_cleanup_error = s_radio.deinitialize();
    if (radio_cleanup_error != ESP_OK) {
        s_service_state.store(ServiceState::kStopping, std::memory_order_release);
        return radio_cleanup_error;
    }

    const bool generic_tx_shared = gpio_is_cc1101_pin(active_gpio_map(),
                                                        active_gpio_map().generic_tx);
    if (using_generic_hardware() || !generic_tx_shared) {
        force_generic_tx_idle();
    } else {
        (void)gpio_reset_pin(static_cast<gpio_num_t>(active_gpio_map().generic_tx));
    }
    if (!using_generic_hardware() &&
        gpio_is_cc1101_pin(active_gpio_map(), active_gpio_map().generic_rx) &&
        active_gpio_map().generic_rx != active_gpio_map().generic_tx) {
        (void)gpio_reset_pin(static_cast<gpio_num_t>(active_gpio_map().generic_rx));
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

bool command_is_blocked_by_maintenance(RadioCommandType type)
{
    return type == RadioCommandType::kTransmitDecoded ||
           type == RadioCommandType::kTransmitRaw || type == RadioCommandType::kReset;
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

esp_err_t start_rf_ook_owned(RfFrameCallback callback, void *context)
{
    if (callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_maintenance_requested.load(std::memory_order_acquire) ||
        s_service_state.load(std::memory_order_acquire) != ServiceState::kStopped ||
        s_running.load(std::memory_order_relaxed) ||
        s_radio_task.load(std::memory_order_acquire) != nullptr || s_rx_queue != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    RfHardware selected_hardware = RfHardware::kCc1101;
    if (s_start_override_valid.load(std::memory_order_acquire)) {
        selected_hardware = s_start_override.load(std::memory_order_acquire);
    } else {
        const esp_err_t hardware_load_error = rf_storage_hardware_get(&selected_hardware);
        if (hardware_load_error != ESP_OK) {
            ESP_LOGW(kTag, "RF hardware selection unavailable (%s); defaulting to CC1101",
                     esp_err_to_name(hardware_load_error));
            selected_hardware = RfHardware::kCc1101;
        }
    }
    (void)initialize_rf_generic_gpio_config();
    const BoardInfo &board = current_board_info();
    if (!board_generic_gpio_map_is_valid(
            configured_board_profile(), board.cc1101, active_gpio_map().generic_tx,
            active_gpio_map().generic_rx,
            board.activity_led_enabled ? board.activity_led_gpio : -1)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hardware.store(selected_hardware, std::memory_order_release);
    ESP_RETURN_ON_ERROR(prepare_backend_gpio_ownership(selected_hardware), kTag,
                        "prepare RF backend GPIO ownership");
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
    s_receive_active = false;
    s_has_last_frame = false;
    s_last_frame = {};
    s_last_reported_us = 0;
    s_accepted_frames = 0;
    s_suppressed_duplicates = 0;
    s_truncated_captures = 0;
    s_completed_captures = 0;
    s_line_samples = 0;
    s_line_high_samples = 0;
    s_line_transitions = 0;
    s_next_rx_buffer = 0;
    s_rx_generation.store(0, std::memory_order_relaxed);
    s_rx_queue_drops.store(0, std::memory_order_relaxed);
    s_command_timeouts.store(0, std::memory_order_relaxed);
    s_rx_rearm_required.store(false, std::memory_order_relaxed);
    s_rmt_tx_faulted.store(false, std::memory_order_relaxed);
    s_maintenance_active.store(false, std::memory_order_relaxed);

    const esp_err_t activity_led_error = initialize_rf_activity_led(active_gpio_map());
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
    esp_err_t error = allocate_rx_symbol_buffers();
    if (error == ESP_OK && selected_hardware == RfHardware::kCc1101) {
        error = s_radio.initialize(radio_config);
    }
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
    ESP_LOGI(kTag, "RF hardware=%s RX GPIO%d, TX GPIO%d; listening at %lu Hz",
             rf_hardware_name(selected_hardware), active_rx_gpio(), active_tx_gpio(),
             static_cast<unsigned long>(CONFIG_CC1101_FREQUENCY_HZ));
    return ESP_OK;
}

esp_err_t start_rf_ook(RfFrameCallback callback, void *context)
{
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired() || s_hardware_switch_busy.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    return start_rf_ook_owned(callback, context);
}

esp_err_t stop_rf_ook_owned()
{
    const TaskHandle_t task = s_radio_task.load(std::memory_order_acquire);
    if (task != nullptr && xTaskGetCurrentTaskHandle() == task) {
        ESP_LOGE(kTag, "stop_rf_ook cannot be called from the radio callback task");
        return ESP_ERR_INVALID_STATE;
    }
    if (task == nullptr) {
        if (s_service_state.load(std::memory_order_acquire) != ServiceState::kStopped &&
            !wait_for_radio_task_exit(kShutdownTimeoutTicks)) {
            ESP_LOGE(kTag, "Radio task exit was not confirmed; resources retained");
            return ESP_ERR_TIMEOUT;
        }
        const esp_err_t cleanup_error = cleanup_resources();
        return cleanup_error;
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
        return ESP_ERR_TIMEOUT;
    }
    return cleanup_resources();
}

void stop_rf_ook()
{
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired() || s_hardware_switch_busy.load(std::memory_order_acquire)) {
        ESP_LOGW(kTag, "Radio lifecycle operation already in progress");
        return;
    }
    const esp_err_t error = stop_rf_ook_owned();
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Radio cleanup remains incomplete: %s", esp_err_to_name(error));
    }
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
    if (s_service_state.load(std::memory_order_acquire) == ServiceState::kStopped &&
        !s_start_override_valid.load(std::memory_order_acquire)) {
        RfHardware stored_hardware = status->hardware;
        if (rf_storage_hardware_get(&stored_hardware) == ESP_OK) {
            status->hardware = stored_hardware;
        }
    }
    status->cc1101_info_valid = false;
    status->cc1101_error = error;
    return ESP_OK;
}

esp_err_t initialize_rf_generic_gpio_config()
{
    if (s_generic_gpio_initialized.load(std::memory_order_acquire)) {
        portENTER_CRITICAL(&s_generic_gpio_mux);
        const esp_err_t error = s_generic_gpio_config.configuration_error;
        portEXIT_CRITICAL(&s_generic_gpio_mux);
        return error;
    }
    if (s_generic_gpio_initializing.test_and_set(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = load_generic_gpio_config();
    s_generic_gpio_initializing.clear(std::memory_order_release);
    return error;
}

esp_err_t get_rf_generic_gpio_config(RfGenericGpioConfig *config)
{
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_error = initialize_rf_generic_gpio_config();
    if (!s_generic_gpio_initialized.load(std::memory_order_acquire)) {
        return init_error;
    }
    portENTER_CRITICAL(&s_generic_gpio_mux);
    *config = s_generic_gpio_config;
    portEXIT_CRITICAL(&s_generic_gpio_mux);
    return ESP_OK;
}

esp_err_t set_rf_generic_gpio_config(int tx_gpio, int rx_gpio, bool *reboot_required)
{
    if (reboot_required != nullptr) {
        *reboot_required = false;
    }
    const esp_err_t init_error = initialize_rf_generic_gpio_config();
    if (!s_generic_gpio_initialized.load(std::memory_order_acquire)) {
        return init_error;
    }
    const BoardInfo &board = current_board_info();
    if (!board_generic_gpio_map_is_valid(
            configured_board_profile(), board.cc1101, tx_gpio, rx_gpio,
            board.activity_led_enabled ? board.activity_led_gpio : -1)) {
        return ESP_ERR_INVALID_ARG;
    }

    RfGenericGpioConfig current{};
    portENTER_CRITICAL(&s_generic_gpio_mux);
    current = s_generic_gpio_config;
    portEXIT_CRITICAL(&s_generic_gpio_mux);
    if (current.pending) {
        return ESP_ERR_INVALID_STATE;
    }
    if (current.active_tx_gpio == tx_gpio && current.active_rx_gpio == rx_gpio && current.saved) {
        return ESP_OK;
    }
    const esp_err_t persist_error = rf_storage_generic_gpio_set(
        static_cast<uint8_t>(configured_board_profile()), static_cast<uint8_t>(tx_gpio),
        static_cast<uint8_t>(rx_gpio));
    if (persist_error != ESP_OK) {
        return persist_error;
    }
    const bool pending = tx_gpio != current.active_tx_gpio || rx_gpio != current.active_rx_gpio;
    portENTER_CRITICAL(&s_generic_gpio_mux);
    s_generic_gpio_config.saved_tx_gpio = tx_gpio;
    s_generic_gpio_config.saved_rx_gpio = rx_gpio;
    s_generic_gpio_config.saved = true;
    s_generic_gpio_config.pending = pending;
    s_generic_gpio_config.overlap_cc1101 =
        generic_gpio_overlaps_cc1101(BoardGpioMap{
            .sclk = board.cc1101.sclk,
            .miso = board.cc1101.miso,
            .mosi = board.cc1101.mosi,
            .cs = board.cc1101.cs,
            .gdo0 = board.cc1101.gdo0,
            .gdo2 = board.cc1101.gdo2,
            .generic_tx = tx_gpio,
            .generic_rx = rx_gpio,
        });
    s_generic_gpio_config.configuration_error = ESP_OK;
    portEXIT_CRITICAL(&s_generic_gpio_mux);
    if (reboot_required != nullptr) {
        *reboot_required = pending;
    }
    return ESP_OK;
}

esp_err_t get_rf_hardware(RfHardware *hardware)
{
    if (hardware == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *hardware = s_hardware.load(std::memory_order_acquire);
    if (s_service_state.load(std::memory_order_acquire) == ServiceState::kStopped &&
        !s_start_override_valid.load(std::memory_order_acquire)) {
        RfHardware stored_hardware = *hardware;
        if (rf_storage_hardware_get(&stored_hardware) == ESP_OK) {
            *hardware = stored_hardware;
        }
    }
    return ESP_OK;
}

esp_err_t set_rf_hardware(RfHardware hardware, RfFrameCallback restart_callback,
                           void *restart_context)
{
    if (hardware != RfHardware::kCc1101 && hardware != RfHardware::kGeneric) {
        return ESP_ERR_INVALID_ARG;
    }
    HardwareSwitchGuard switch_guard;
    LifecycleGuard lifecycle;
    if (!switch_guard.acquired() || !lifecycle.acquired() ||
        s_maintenance_requested.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const ServiceState state = s_service_state.load(std::memory_order_acquire);
    if (state != ServiceState::kStopped && state != ServiceState::kRunning) {
        s_hardware_switch_error.store(ESP_ERR_INVALID_STATE, std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    const RfHardware previous = s_hardware.load(std::memory_order_acquire);
    const bool was_running = state == ServiceState::kRunning;
    const RfFrameCallback callback = s_frame_callback != nullptr ? s_frame_callback : restart_callback;
    void *context = s_frame_callback != nullptr ? s_callback_context : restart_context;
    const auto restart_with = [&](RfHardware selected) {
        s_hardware.store(selected, std::memory_order_release);
        s_start_override.store(selected, std::memory_order_release);
        s_start_override_valid.store(true, std::memory_order_release);
        const esp_err_t error = start_rf_ook_owned(callback, context);
        s_start_override_valid.store(false, std::memory_order_release);
        return error;
    };
    const bool should_start = callback != nullptr;
    if (previous == hardware) {
        if (state == ServiceState::kStopped) {
            const esp_err_t ownership_error = prepare_backend_gpio_ownership(hardware);
            if (ownership_error != ESP_OK) {
                s_hardware_switch_error.store(ownership_error, std::memory_order_release);
                return ownership_error;
            }
            if (should_start) {
                const esp_err_t start_error = restart_with(hardware);
                if (start_error != ESP_OK) {
                    s_hardware.store(previous, std::memory_order_release);
                    const esp_err_t rollback_error = prepare_backend_gpio_ownership(previous);
                    s_hardware_switch_error.store(
                        rollback_error == ESP_OK ? start_error : rollback_error,
                        std::memory_order_release);
                    return rollback_error == ESP_OK ? start_error : rollback_error;
                }
            }
        }
        const esp_err_t persist_error = rf_storage_hardware_set(hardware);
        if (persist_error != ESP_OK && state == ServiceState::kStopped && should_start) {
            const esp_err_t stop_error = stop_rf_ook_owned();
            if (stop_error != ESP_OK) {
                s_hardware_switch_error.store(stop_error, std::memory_order_release);
                return stop_error;
            }
            s_hardware.store(previous, std::memory_order_release);
            const esp_err_t rollback_error = prepare_backend_gpio_ownership(previous);
            if (rollback_error != ESP_OK) {
                s_hardware_switch_error.store(rollback_error, std::memory_order_release);
                return rollback_error;
            }
        }
        s_hardware_switch_error.store(persist_error, std::memory_order_release);
        return persist_error;
    }
    const BoardInfo &board = current_board_info();
    if (hardware == RfHardware::kGeneric &&
        !board_generic_gpio_map_is_valid(configured_board_profile(), board.cc1101,
                                         active_gpio_map().generic_tx,
                                         active_gpio_map().generic_rx,
                                         board.activity_led_enabled ? board.activity_led_gpio : -1)) {
        s_hardware_switch_error.store(ESP_ERR_INVALID_ARG, std::memory_order_release);
        return ESP_ERR_INVALID_ARG;
    }
    if (was_running) {
        const esp_err_t stop_error = stop_rf_ook_owned();
        if (stop_error != ESP_OK) {
            s_hardware_switch_error.store(stop_error, std::memory_order_release);
            return stop_error;
        }
        if (s_service_state.load(std::memory_order_acquire) != ServiceState::kStopped) {
            s_hardware_switch_error.store(ESP_ERR_INVALID_STATE, std::memory_order_release);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (!was_running) {
        const esp_err_t ownership_error = prepare_backend_gpio_ownership(hardware);
        if (ownership_error != ESP_OK) {
            s_hardware_switch_error.store(ownership_error, std::memory_order_release);
            return ownership_error;
        }
    }
    s_hardware.store(hardware, std::memory_order_release);
    if (was_running || should_start) {
        const esp_err_t start_error = restart_with(hardware);
        if (start_error != ESP_OK) {
            esp_err_t rollback_error = ESP_OK;
            if (was_running) {
                rollback_error = restart_with(previous);
            } else {
                s_hardware.store(previous, std::memory_order_release);
                rollback_error = prepare_backend_gpio_ownership(previous);
            }
            if (rollback_error != ESP_OK) {
                ESP_LOGE(kTag, "RF hardware rollback to %s failed: %s",
                         rf_hardware_name(previous), esp_err_to_name(rollback_error));
                s_hardware_switch_error.store(rollback_error, std::memory_order_release);
                return rollback_error;
            }
            s_hardware_switch_error.store(start_error, std::memory_order_release);
            return start_error;
        }
    }
    const esp_err_t persist_error = rf_storage_hardware_set(hardware);
    if (persist_error != ESP_OK) {
        if (was_running) {
            const esp_err_t stop_error = stop_rf_ook_owned();
            if (stop_error != ESP_OK) {
                s_hardware_switch_error.store(stop_error, std::memory_order_release);
                return stop_error;
            }
            const esp_err_t rollback_error = restart_with(previous);
            if (rollback_error != ESP_OK) {
                ESP_LOGE(kTag, "RF hardware rollback to %s failed after persistence error: %s",
                         rf_hardware_name(previous), esp_err_to_name(rollback_error));
                s_hardware_switch_error.store(rollback_error, std::memory_order_release);
                return rollback_error;
            }
        } else if (should_start) {
            const esp_err_t stop_error = stop_rf_ook_owned();
            if (stop_error != ESP_OK) {
                s_hardware_switch_error.store(stop_error, std::memory_order_release);
                return stop_error;
            }
            s_hardware.store(previous, std::memory_order_release);
            const esp_err_t rollback_error = prepare_backend_gpio_ownership(previous);
            if (rollback_error != ESP_OK) {
                s_hardware_switch_error.store(rollback_error, std::memory_order_release);
                return rollback_error;
            }
        }
        s_hardware_switch_error.store(persist_error, std::memory_order_release);
        return persist_error;
    }
    s_hardware_switch_error.store(ESP_OK, std::memory_order_release);
    s_hardware_switches.fetch_add(1, std::memory_order_relaxed);
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
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired() || s_hardware_switch_busy.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!s_maintenance_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
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
    LifecycleGuard lifecycle;
    if (!lifecycle.acquired() || s_hardware_switch_busy.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_maintenance_requested.load(std::memory_order_acquire)) {
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
