#include "rf_console.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rf_console_parse.hpp"
#include "sdkconfig.h"

#ifndef CONFIG_ESP_CONSOLE_UART_DEFAULT
#error "rf_console requires CONFIG_ESP_CONSOLE_UART_DEFAULT"
#endif

namespace rfbridge {
namespace {

constexpr char kTag[] = "rf_console";
constexpr std::size_t kOutputLineSize = 2048;
constexpr uint32_t kPrinterTaskStackSize = 6144;
QueueHandle_t s_frame_queue = nullptr;
TaskHandle_t s_printer_task = nullptr;
esp_console_repl_t *s_repl = nullptr;
bool s_repl_started = false;
std::atomic<bool> s_started{false};
std::atomic_flag s_starting = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> s_frame_queue_drops{0};
RawSignal s_staged_raw{};
bool s_raw_staging = false;

template <typename... Args>
bool append_to_line(char *line, std::size_t capacity, std::size_t *length, const char *format, Args... args)
{
    if (*length >= capacity) {
        return false;
    }
    const std::size_t available = capacity - *length;
    const int written = std::snprintf(line + *length, available, format, args...);
    if (written < 0 || static_cast<std::size_t>(written) >= available) {
        return false;
    }
    *length += static_cast<std::size_t>(written);
    return true;
}

void print_frame(const char *prefix, const RfFrame &frame)
{
    char line[kOutputLineSize]{};
    std::size_t length = 0;
    bool formatted = false;
    if (frame.encoding == RfEncoding::kDecoded) {
        formatted = append_to_line(
            line, sizeof(line), &length,
            "\n%s RC code=%llu hex=0x%llX bits=%u protocol=%u pulse_us=%u confidence=%s repeats=%u "
            "fingerprint=0x%08lX\n",
            prefix, static_cast<unsigned long long>(frame.decoded.code),
            static_cast<unsigned long long>(frame.decoded.code), frame.decoded.bits, frame.decoded.protocol,
            frame.decoded.pulse_us,
            frame.confidence == RfFrameConfidence::kRepeatedEvidence ? "repeated" : "single",
            frame.observed_repeats, static_cast<unsigned long>(frame.fingerprint));
    } else {
        formatted = append_to_line(line, sizeof(line), &length,
                                   "\n%s RAW start=%u count=%u repeats=%u fingerprint=0x%08lX durations=", prefix,
                                   frame.raw.start_level, frame.raw.count, frame.observed_repeats,
                                   static_cast<unsigned long>(frame.fingerprint));
        for (std::size_t index = 0; formatted && index < frame.raw.count; ++index) {
            formatted = append_to_line(line, sizeof(line), &length, "%s%u", index == 0 ? "" : ",",
                                       frame.raw.durations_us[index]);
        }
        if (formatted) {
            formatted = append_to_line(line, sizeof(line), &length, "\n");
        }
    }
    if (!formatted) {
        ESP_LOGE(kTag, "Could not format %s frame output without truncation", prefix);
        return;
    }
    std::printf("%s", line);
    std::fflush(stdout);
}

void printer_task(void *)
{
    RfFrame frame{};
    while (true) {
        if (xQueueReceive(s_frame_queue, &frame, portMAX_DELAY) == pdTRUE) {
            print_frame("RX", frame);
        }
    }
}

bool parse_bounded(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value)
{
    return parse_unsigned_value(text, maximum, value) && *value >= minimum;
}

int print_result(const char *operation, esp_err_t error)
{
    if (error == ESP_OK) {
        std::printf("OK %s\n", operation);
        return 0;
    }
    std::printf("ERROR %s: %s (0x%x)\n", operation, esp_err_to_name(error), static_cast<unsigned>(error));
    return 1;
}

int status_command(int argc, char **)
{
    if (argc != 1) {
        std::printf("usage: status\n");
        return 1;
    }
    RfRadioStatus status{};
    const esp_err_t error = get_rf_radio_status(&status);
    if (error != ESP_OK) {
        return print_result("status", error);
    }
    std::printf("STATUS running=%u rx_enabled=%u rx_active=%u tx=%u has_last=%u accepted=%lu duplicates=%lu rx_drops=%lu truncated=%lu command_timeouts=%lu console_drops=%lu\n",
                status.running, status.receive_enabled, status.receive_active, status.transmitting,
                status.has_last_frame, static_cast<unsigned long>(status.accepted_frames),
                static_cast<unsigned long>(status.suppressed_duplicates),
                static_cast<unsigned long>(status.rx_queue_drops),
                static_cast<unsigned long>(status.truncated_captures),
                static_cast<unsigned long>(status.command_timeouts),
                static_cast<unsigned long>(s_frame_queue_drops.load(std::memory_order_relaxed)));
    if (status.cc1101_info_valid) {
        std::printf("RADIO part=0x%02X version=0x%02X state=0x%02X rssi_x2=%d cs=%u cca=%u resets=%lu recoveries=%lu ready_timeouts=%lu state_timeouts=%lu frequency_hz=%d power_dbm=%d\n",
                    status.cc1101.part_number, status.cc1101.version, status.cc1101.marc_state,
                    status.cc1101.rssi_dbm_x2, status.cc1101.carrier_sense, status.cc1101.clear_channel,
                    static_cast<unsigned long>(status.cc1101.reset_count),
                    static_cast<unsigned long>(status.cc1101.recovery_count),
                    static_cast<unsigned long>(status.cc1101.ready_timeout_count),
                    static_cast<unsigned long>(status.cc1101.state_timeout_count), CONFIG_CC1101_FREQUENCY_HZ,
                    CONFIG_CC1101_TX_POWER_DBM);
    } else {
        std::printf("RADIO unavailable error=%s (0x%x) frequency_hz=%d power_dbm=%d\n",
                    esp_err_to_name(status.cc1101_error), static_cast<unsigned>(status.cc1101_error),
                    CONFIG_CC1101_FREQUENCY_HZ, CONFIG_CC1101_TX_POWER_DBM);
    }
    return 0;
}

int radio_command(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "info") == 0) {
        return status_command(1, argv);
    }
    if (argc == 2 && std::strcmp(argv[1], "reset") == 0) {
        return print_result("radio reset", reset_rf_radio());
    }
    if (argc == 2 && std::strcmp(argv[1], "start") == 0) {
        return print_result("radio start", start_rf_ook(rf_console_on_frame, nullptr));
    }
    std::printf("usage: radio <info|reset|start>\n");
    return 1;
}

int rx_command(int argc, char **argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "on") != 0 && std::strcmp(argv[1], "off") != 0)) {
        std::printf("usage: rx <on|off>\n");
        return 1;
    }
    const bool enabled = std::strcmp(argv[1], "on") == 0;
    return print_result(enabled ? "rx on" : "rx off", set_rf_receive_enabled(enabled));
}

int last_command(int argc, char **)
{
    if (argc != 1) {
        std::printf("usage: last\n");
        return 1;
    }
    RfFrame frame{};
    const esp_err_t error = get_last_rf_frame(&frame);
    if (error != ESP_OK) {
        return print_result("last", error);
    }
    print_frame("LAST", frame);
    return 0;
}

int replay_command(int argc, char **argv)
{
    if (argc > 2) {
        std::printf("usage: replay [repeats]\n");
        return 1;
    }
    uint64_t repeats = CONFIG_RF_DEFAULT_TX_REPEATS;
    if (argc == 2 && !parse_bounded(argv[1], 1, 20, &repeats)) {
        std::printf("ERROR repeats must be 1..20\n");
        return 1;
    }
    return print_result("replay", replay_last_rf_frame(static_cast<uint16_t>(repeats)));
}

int send_value_command(int argc, char **argv)
{
    if (argc < 4 || argc > 6) {
        std::printf("usage: send <code> <bits> <protocol> [pulse_us] [repeats]\n");
        return 1;
    }
    uint64_t code = 0;
    uint64_t bits = 0;
    uint64_t protocol_number = 0;
    if (!parse_unsigned_value(argv[1], UINT64_MAX, &code) || !parse_bounded(argv[2], 4, 64, &bits) ||
        !parse_bounded(argv[3], 1, kRfProtocolCount, &protocol_number)) {
        std::printf("ERROR code must be decimal/0x; bits 4..64; protocol 1..%zu\n", kRfProtocolCount);
        return 1;
    }
    if (bits < 64 && (code >> bits) != 0) {
        std::printf("ERROR code does not fit in the requested bit count\n");
        return 1;
    }
    const uint8_t selected_protocol = static_cast<uint8_t>(protocol_number);
    const RfProtocol *protocol = rf_protocol(selected_protocol);
    uint64_t pulse_us = protocol->pulse_us;
    uint64_t repeats = CONFIG_RF_DEFAULT_TX_REPEATS;
    const uint8_t minimum_factor = rf_protocol_min_factor(selected_protocol);
    const uint16_t minimum_unit =
        static_cast<uint16_t>((kMinimumRawPulseUs + minimum_factor - 1U) / minimum_factor);
    const uint16_t maximum_unit =
        static_cast<uint16_t>(kMaximumPulseDurationUs / rf_protocol_max_factor(selected_protocol));
    if (argc >= 5 && !parse_bounded(argv[4], minimum_unit, maximum_unit, &pulse_us)) {
        std::printf("ERROR pulse_us is outside the selected protocol's transport limits\n");
        return 1;
    }
    if (argc == 6 && !parse_bounded(argv[5], 1, 20, &repeats)) {
        std::printf("ERROR repeats must be 1..20\n");
        return 1;
    }
    DecodedSignal signal{};
    signal.code = code;
    signal.pulse_us = static_cast<uint16_t>(pulse_us);
    signal.bits = static_cast<uint8_t>(bits);
    signal.protocol = static_cast<uint8_t>(protocol_number);
    signal.inverted = protocol->inverted;
    return print_result("send", transmit_rf_decoded(signal, static_cast<uint16_t>(repeats)));
}

void print_staged_raw()
{
    char line[kOutputLineSize]{};
    std::size_t length = 0;
    bool formatted = false;
    if (!s_raw_staging) {
        formatted = append_to_line(line, sizeof(line), &length, "RAW_STAGE empty\n");
    } else {
        formatted = append_to_line(line, sizeof(line), &length, "RAW_STAGE start=%u count=%u durations=",
                                   s_staged_raw.start_level, s_staged_raw.count);
        for (std::size_t index = 0; formatted && index < s_staged_raw.count; ++index) {
            formatted = append_to_line(line, sizeof(line), &length, "%s%u", index == 0 ? "" : ",",
                                       s_staged_raw.durations_us[index]);
        }
        if (formatted) {
            formatted = append_to_line(line, sizeof(line), &length, "\n");
        }
    }
    if (!formatted) {
        ESP_LOGE(kTag, "Could not format staged raw output without truncation");
        return;
    }
    std::printf("%s", line);
}

int raw_command(int argc, char **argv)
{
    if (argc < 2) {
        std::printf("usage: raw <begin|append|show|send|clear> ...\n");
        return 1;
    }
    if (std::strcmp(argv[1], "clear") == 0 && argc == 2) {
        s_staged_raw = {};
        s_raw_staging = false;
        std::printf("OK raw clear\n");
        return 0;
    }
    if (std::strcmp(argv[1], "begin") == 0) {
        uint64_t start_level = 0;
        if (argc != 3 || !parse_bounded(argv[2], 0, 1, &start_level)) {
            std::printf("usage: raw begin <start_level:0|1>\n");
            return 1;
        }
        s_staged_raw = {};
        s_staged_raw.start_level = static_cast<uint8_t>(start_level);
        s_raw_staging = true;
        std::printf("OK raw begin\n");
        return 0;
    }
    if (std::strcmp(argv[1], "append") == 0) {
        if (!s_raw_staging || argc < 3) {
            std::printf("usage: raw begin <0|1>, then raw append <duration_us>...\n");
            return 1;
        }
        const std::size_t additions = static_cast<std::size_t>(argc - 2);
        if (static_cast<std::size_t>(s_staged_raw.count) + additions > kMaxRawPulses) {
            std::printf("ERROR raw pulse limit is %zu\n", kMaxRawPulses);
            return 1;
        }
        uint16_t parsed[kMaxRawPulses]{};
        for (std::size_t index = 0; index < additions; ++index) {
            uint64_t duration = 0;
            if (!parse_bounded(argv[index + 2U], kMinimumRawPulseUs, kMaximumPulseDurationUs, &duration)) {
                std::printf("ERROR every duration must be 100..29000 us\n");
                return 1;
            }
            parsed[index] = static_cast<uint16_t>(duration);
        }
        for (std::size_t index = 0; index < additions; ++index) {
            s_staged_raw.durations_us[s_staged_raw.count++] = parsed[index];
        }
        std::printf("OK raw append count=%u\n", s_staged_raw.count);
        return 0;
    }
    if (std::strcmp(argv[1], "show") == 0 && argc == 2) {
        print_staged_raw();
        return 0;
    }
    if (std::strcmp(argv[1], "send") == 0) {
        uint64_t repeats = CONFIG_RF_DEFAULT_TX_REPEATS;
        if (!s_raw_staging || argc > 3 ||
            (argc == 3 && !parse_bounded(argv[2], 1, 20, &repeats))) {
            std::printf("usage: raw send [repeats]\n");
            return 1;
        }
        if (!raw_signal_is_valid(s_staged_raw)) {
            std::printf("ERROR raw frame needs 8..%zu alternating pulses and an even count\n", kMaxRawPulses);
            return 1;
        }
        return print_result("raw send", transmit_rf_raw(s_staged_raw, static_cast<uint16_t>(repeats)));
    }
    std::printf("usage: raw <begin|append|show|send|clear> ...\n");
    return 1;
}

esp_err_t register_command(const char *name, const char *help, const char *hint, esp_console_cmd_func_t handler)
{
    esp_console_cmd_t command{};
    command.command = name;
    command.help = help;
    command.hint = hint;
    command.func = handler;
    return esp_console_cmd_register(&command);
}

struct CommandDefinition {
    const char *name;
    const char *help;
    const char *hint;
    esp_console_cmd_func_t handler;
};

constexpr CommandDefinition kCommands[] = {
    {"status", "Show RF and CC1101 diagnostics", nullptr, status_command},
    {"radio", "Start, show, or reset the CC1101", "<start|info|reset>", radio_command},
    {"rx", "Enable or disable reception", "<on|off>", rx_command},
    {"last", "Print the latest RAM frame", nullptr, last_command},
    {"replay", "Replay the latest RAM frame", "[repeats]", replay_command},
    {"send", "Send an rc-switch-compatible value", "<code> <bits> <protocol> [pulse_us] [repeats]",
     send_value_command},
    {"raw", "Stage and send a raw alternating OOK period", "<begin|append|show|send|clear> ...", raw_command},
};

esp_err_t register_commands(std::size_t *registered_count)
{
    *registered_count = 0;
    for (const CommandDefinition &command : kCommands) {
        const esp_err_t error = register_command(command.name, command.help, command.hint, command.handler);
        if (error != ESP_OK) {
            return error;
        }
        ++*registered_count;
    }
    return ESP_OK;
}

void deregister_commands(std::size_t registered_count)
{
    while (registered_count > 0) {
        const CommandDefinition &command = kCommands[--registered_count];
        const esp_err_t error = esp_console_cmd_deregister(command.name);
        if (error != ESP_OK && error != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(kTag, "Could not deregister %s: %s", command.name, esp_err_to_name(error));
        }
    }
}

void cleanup_failed_start(std::size_t registered_count)
{
    if (s_printer_task != nullptr) {
        vTaskDelete(s_printer_task);
        s_printer_task = nullptr;
    }
    if (s_frame_queue != nullptr) {
        vQueueDelete(s_frame_queue);
        s_frame_queue = nullptr;
    }

    deregister_commands(registered_count);
    if (s_repl != nullptr) {
        if (!s_repl_started) {
            const esp_err_t start_error = esp_console_start_repl(s_repl);
            if (start_error == ESP_OK) {
                s_repl_started = true;
            } else {
                ESP_LOGE(kTag, "Could not start failed UART REPL for safe deletion: %s",
                         esp_err_to_name(start_error));
            }
        }
        if (s_repl_started) {
            const esp_err_t delete_error = esp_console_stop_repl(s_repl);
            if (delete_error == ESP_OK) {
                s_repl = nullptr;
                s_repl_started = false;
            } else {
                ESP_LOGE(kTag, "Could not delete failed UART REPL: %s", esp_err_to_name(delete_error));
            }
        }
    }
    s_started.store(false, std::memory_order_release);
    s_starting.clear(std::memory_order_release);
}

esp_err_t fail_start(esp_err_t error, std::size_t registered_count)
{
    cleanup_failed_start(registered_count);
    return error;
}

}  // namespace

esp_err_t start_rf_console()
{
    if (s_starting.test_and_set(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started.load(std::memory_order_acquire) || s_repl != nullptr || s_frame_queue != nullptr ||
        s_printer_task != nullptr) {
        s_starting.clear(std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "rf>";
    repl_config.max_cmdline_length = 2048;
    repl_config.max_cmdline_args = kMaxRawPulses + 4U;
    repl_config.task_stack_size = 8192;
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    uart_config.baud_rate = 115200;
    esp_err_t error = esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not create UART REPL: %s", esp_err_to_name(error));
        return fail_start(error, 0);
    }

    std::size_t registered_count = 0;
    error = register_commands(&registered_count);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not register commands: %s", esp_err_to_name(error));
        return fail_start(error, registered_count);
    }

    s_frame_queue = xQueueCreate(4, sizeof(RfFrame));
    if (s_frame_queue == nullptr) {
        return fail_start(ESP_ERR_NO_MEM, registered_count);
    }
    if (xTaskCreate(printer_task, "rf_print", kPrinterTaskStackSize, nullptr, 3, &s_printer_task) != pdPASS) {
        return fail_start(ESP_ERR_NO_MEM, registered_count);
    }

    error = esp_console_start_repl(s_repl);
    if (error == ESP_OK) {
        s_repl_started = true;
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not start UART REPL: %s", esp_err_to_name(error));
        return fail_start(error, registered_count);
    }

    s_started.store(true, std::memory_order_release);
    s_starting.clear(std::memory_order_release);
    std::printf("\nNative ESP32 + CC1101 RF console ready. Type 'help'.\n");
    return ESP_OK;
}

void rf_console_on_frame(const RfFrame &frame, void *)
{
    if (s_frame_queue == nullptr || xQueueSend(s_frame_queue, &frame, 0) != pdTRUE) {
        s_frame_queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace rfbridge
