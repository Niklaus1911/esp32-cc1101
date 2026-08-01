#include "rf_console.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "rf_console_parse.hpp"
#include "rf_storage.hpp"
#include "sdkconfig.h"

#ifndef CONFIG_ESP_CONSOLE_UART_DEFAULT
#error "rf_console requires CONFIG_ESP_CONSOLE_UART_DEFAULT"
#endif

namespace rfbridge {
namespace {

constexpr char kTag[] = "rf_console";
constexpr std::size_t kOutputLineSize = 2048;
constexpr uint32_t kEventWorkerTaskStackSize = 6144;
constexpr int64_t kLearnTimeoutUs = 30000000;
constexpr TickType_t kLearnEnqueueWaitTicks = pdMS_TO_TICKS(100);

enum class ConsoleEventType : uint8_t {
    kFrame,
    kLearnRequest,
};

struct ConsoleEvent {
    ConsoleEventType type = ConsoleEventType::kFrame;
    int64_t occurred_us = 0;
    RfFrame frame{};
    char name[kRfStorageNameCapacity]{};
};

struct PendingLearn {
    bool active = false;
    int64_t armed_us = 0;
    int64_t deadline_us = 0;
    uint32_t frame_drops_at_arm = 0;
    char name[kRfStorageNameCapacity]{};
};

QueueHandle_t s_event_queue = nullptr;
TaskHandle_t s_event_worker_task = nullptr;
esp_console_repl_t *s_repl = nullptr;
bool s_repl_started = false;
std::atomic<bool> s_started{false};
std::atomic_flag s_starting = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> s_frame_queue_drops{0};
std::atomic<uint32_t> s_frame_callbacks_in_flight{0};
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
            "\r\n%s RC code=%llu hex=0x%llX bits=%u protocol=%u pulse_us=%u confidence=%s repeats=%u "
            "fingerprint=0x%08lX\r\n",
            prefix, static_cast<unsigned long long>(frame.decoded.code),
            static_cast<unsigned long long>(frame.decoded.code), frame.decoded.bits, frame.decoded.protocol,
            frame.decoded.pulse_us,
            frame.confidence == RfFrameConfidence::kRepeatedEvidence ? "repeated" : "single",
            frame.observed_repeats, static_cast<unsigned long>(frame.fingerprint));
    } else {
        formatted = append_to_line(line, sizeof(line), &length,
                                   "\r\n%s RAW start=%u count=%u repeats=%u fingerprint=0x%08lX durations=", prefix,
                                   frame.raw.start_level, frame.raw.count, frame.observed_repeats,
                                   static_cast<unsigned long>(frame.fingerprint));
        for (std::size_t index = 0; formatted && index < frame.raw.count; ++index) {
            formatted = append_to_line(line, sizeof(line), &length, "%s%u", index == 0 ? "" : ",",
                                       frame.raw.durations_us[index]);
        }
        if (formatted) {
            formatted = append_to_line(line, sizeof(line), &length, "\r\n");
        }
    }
    if (!formatted) {
        ESP_LOGE(kTag, "Could not format %s frame output without truncation", prefix);
        return;
    }
    std::printf("%s", line);
    std::fflush(stdout);
}

void clear_pending_learn(PendingLearn *pending)
{
    *pending = {};
}

void print_learn_timeout(PendingLearn *pending)
{
    char name[kRfStorageNameCapacity]{};
    std::memcpy(name, pending->name, sizeof(name));
    clear_pending_learn(pending);
    std::printf("\r\nLEARN TIMEOUT name=%s\r\n", name);
    std::fflush(stdout);
}

RfStoredSignal stored_signal_from_frame(const RfFrame &frame)
{
    RfStoredSignal stored{};
    if (frame.encoding == RfEncoding::kDecoded) {
        stored.encoding = RfStoredEncoding::kDecoded;
        stored.decoded = frame.decoded;
    } else {
        stored.encoding = RfStoredEncoding::kRaw;
        stored.raw = frame.raw;
    }
    return stored;
}

void process_learn_request(const ConsoleEvent &event, PendingLearn *pending)
{
    RfRadioStatus status{};
    const esp_err_t status_error = get_rf_radio_status(&status);
    if (status_error != ESP_OK || !status.running || !status.receive_enabled) {
        const esp_err_t error = status_error == ESP_OK ? ESP_ERR_INVALID_STATE : status_error;
        std::printf("\r\nERROR learn name=%s: RF/RX unavailable: %s (0x%x)\r\n", event.name,
                    esp_err_to_name(error), static_cast<unsigned>(error));
        std::fflush(stdout);
        return;
    }

    bool exists = false;
    const esp_err_t exists_error = rf_storage_exists(event.name, &exists);
    if (exists_error != ESP_OK) {
        std::printf("\r\nERROR learn name=%s: %s (0x%x)\r\n", event.name, esp_err_to_name(exists_error),
                    static_cast<unsigned>(exists_error));
        return;
    }
    if (exists) {
        std::printf("\r\nERROR learn name=%s already exists\r\n", event.name);
        return;
    }
    if (pending->active) {
        char old_name[kRfStorageNameCapacity]{};
        std::memcpy(old_name, pending->name, sizeof(old_name));
        clear_pending_learn(pending);
        std::printf("\r\nLEARN REPLACED old=%s new=%s\r\n", old_name, event.name);
    }
    std::printf("\r\nLEARN ARMED name=%s timeout=30s\r\n", event.name);
    std::fflush(stdout);

    // Open the capture window only after the marker has been emitted.
    const uint32_t frame_drops_at_arm = s_frame_queue_drops.load(std::memory_order_acquire);
    const int64_t armed_us = esp_timer_get_time();
    pending->active = true;
    pending->armed_us = armed_us;
    pending->deadline_us = armed_us + kLearnTimeoutUs;
    pending->frame_drops_at_arm = frame_drops_at_arm;
    std::memcpy(pending->name, event.name, sizeof(pending->name));
}

void process_frame_event(const ConsoleEvent &event, PendingLearn *pending)
{
    if (!pending->active) {
        print_frame("RX", event.frame);
        return;
    }
    const LearnFrameDisposition disposition =
        classify_learn_frame(pending->armed_us, pending->deadline_us, event.occurred_us,
                             event.frame.captured_us);
    if (disposition == LearnFrameDisposition::kIgnore) {
        print_frame("RX", event.frame);
        return;
    }
    if (disposition == LearnFrameDisposition::kTimeout) {
        print_learn_timeout(pending);
        print_frame("RX", event.frame);
        return;
    }

    char name[kRfStorageNameCapacity]{};
    std::memcpy(name, pending->name, sizeof(name));
    clear_pending_learn(pending);
    print_frame("RX", event.frame);
    const RfStoredSignal stored = stored_signal_from_frame(event.frame);
    const esp_err_t error = rf_storage_create(name, stored);
    if (error == ESP_OK) {
        std::printf("\r\nLEARNED name=%s\r\n", name);
    } else if (error == ESP_ERR_INVALID_STATE) {
        std::printf("\r\nERROR learn name=%s already exists\r\n", name);
    } else {
        std::printf("\r\nERROR learn name=%s: %s (0x%x)\r\n", name, esp_err_to_name(error),
                    static_cast<unsigned>(error));
    }
    std::fflush(stdout);
}

void process_console_event(const ConsoleEvent &event, PendingLearn *pending)
{
    if (event.type == ConsoleEventType::kLearnRequest) {
        if (pending->active && event.occurred_us >= pending->deadline_us) {
            print_learn_timeout(pending);
        }
        process_learn_request(event, pending);
    } else {
        process_frame_event(event, pending);
    }
}

TickType_t learn_wait_ticks(int64_t remaining_us)
{
    const uint64_t ticks = (static_cast<uint64_t>(remaining_us) * configTICK_RATE_HZ + 999999U) / 1000000U;
    return static_cast<TickType_t>(ticks == 0 ? 1 : ticks);
}

void event_worker_task(void *)
{
    PendingLearn pending{};
    while (true) {
        ConsoleEvent event{};
        if (pending.active &&
            s_frame_queue_drops.load(std::memory_order_acquire) != pending.frame_drops_at_arm) {
            char name[kRfStorageNameCapacity]{};
            std::memcpy(name, pending.name, sizeof(name));
            clear_pending_learn(&pending);
            std::printf("\r\nLEARN ERROR name=%s: frame event queue overflowed; retry\r\n", name);
            std::fflush(stdout);
            continue;
        }
        if (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
            process_console_event(event, &pending);
            continue;
        }
        if (!pending.active) {
            if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
                process_console_event(event, &pending);
            }
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= pending.deadline_us) {
            if (s_frame_callbacks_in_flight.load(std::memory_order_acquire) != 0) {
                vTaskDelay(1);
                continue;
            }
            if (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
                process_console_event(event, &pending);
                continue;
            }
            print_learn_timeout(&pending);
            continue;
        }
        if (xQueueReceive(s_event_queue, &event, learn_wait_ticks(pending.deadline_us - now_us)) == pdTRUE) {
            process_console_event(event, &pending);
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

int list_learned_names()
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::size_t count = 0;
        esp_err_t error = rf_storage_list(nullptr, 0, &count);
        if (error != ESP_OK) {
            return print_result("learn list", error);
        }
        if (count == 0) {
            std::printf("LEARNED_NAMES count=0\n");
            return 0;
        }
        std::unique_ptr<RfStorageName[]> names(new (std::nothrow) RfStorageName[count]);
        if (!names) {
            return print_result("learn list", ESP_ERR_NO_MEM);
        }
        const std::size_t capacity = count;
        error = rf_storage_list(names.get(), capacity, &count);
        if (error == ESP_ERR_INVALID_SIZE && attempt == 0) {
            continue;
        }
        if (error != ESP_OK) {
            return print_result("learn list", error);
        }
        std::printf("LEARNED_NAMES count=%zu\n", count);
        for (std::size_t index = 0; index < count; ++index) {
            std::printf("  %s\n", names[index].value);
        }
        return 0;
    }
    return print_result("learn list", ESP_ERR_INVALID_SIZE);
}

int learn_command(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "list") == 0) {
        return list_learned_names();
    }
    if (argc != 2 || !rf_storage_name_is_valid(argv[1])) {
        std::printf("usage: learn <name>|list (name: letter then up to 14 letters/digits/_/-)\n");
        return 1;
    }
    if (!rf_storage_is_available()) {
        return print_result("learn", rf_storage_initialization_error());
    }
    bool exists = false;
    esp_err_t error = rf_storage_exists(argv[1], &exists);
    if (error != ESP_OK) {
        return print_result("learn", error);
    }
    if (exists) {
        std::printf("ERROR learn name=%s already exists\n", argv[1]);
        return 1;
    }
    RfRadioStatus status{};
    error = get_rf_radio_status(&status);
    if (error != ESP_OK) {
        return print_result("learn", error);
    }
    if (!status.running || !status.receive_enabled) {
        std::printf("ERROR learn requires running RF with RX enabled\n");
        return 1;
    }

    ConsoleEvent event{};
    event.type = ConsoleEventType::kLearnRequest;
    event.occurred_us = esp_timer_get_time();
    std::memcpy(event.name, argv[1], std::strlen(argv[1]) + 1U);
    if (s_event_queue == nullptr || xQueueSend(s_event_queue, &event, kLearnEnqueueWaitTicks) != pdTRUE) {
        std::printf("ERROR learn event queue busy\n");
        return 1;
    }
    return 0;
}

int forget_command(int argc, char **argv)
{
    if (argc != 2 || !rf_storage_name_is_valid(argv[1])) {
        std::printf("usage: forget <name>\n");
        return 1;
    }
    return print_result("forget", rf_storage_forget(argv[1]));
}

int replay_command(int argc, char **argv)
{
    ReplayArguments arguments{};
    if (!parse_replay_arguments(argc, argv, CONFIG_RF_DEFAULT_TX_REPEATS, &arguments)) {
        std::printf("usage: replay [repeats:1..20] | replay <name> <repeats:1..20>\n");
        return 1;
    }
    if (arguments.target == ReplayTarget::kRam) {
        return print_result("replay", replay_last_rf_frame(arguments.repeats));
    }

    RfStoredSignal stored{};
    const esp_err_t load_error = rf_storage_load(argv[1], &stored);
    if (load_error != ESP_OK) {
        return print_result("replay named load", load_error);
    }
    const esp_err_t transmit_error = stored.encoding == RfStoredEncoding::kDecoded
                                         ? transmit_rf_decoded(stored.decoded, arguments.repeats)
                                         : transmit_rf_raw(stored.raw, arguments.repeats);
    return print_result("replay named", transmit_error);
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
    {"learn", "Learn the next accepted frame or list names", "<name>|list", learn_command},
    {"forget", "Delete one learned NVS frame", "<name>", forget_command},
    {"replay", "Replay the latest RAM frame or a learned name", "[repeats] | <name> <repeats>", replay_command},
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
    if (s_event_worker_task != nullptr) {
        vTaskDelete(s_event_worker_task);
        s_event_worker_task = nullptr;
    }
    if (s_event_queue != nullptr) {
        vQueueDelete(s_event_queue);
        s_event_queue = nullptr;
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
    if (s_started.load(std::memory_order_acquire) || s_repl != nullptr || s_event_queue != nullptr ||
        s_event_worker_task != nullptr) {
        s_starting.clear(std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }

    // Linenoise cursor redraw is not safe when RF events print from another task.
    linenoiseSetDumbMode(1);
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

    s_event_queue = xQueueCreate(8, sizeof(ConsoleEvent));
    if (s_event_queue == nullptr) {
        return fail_start(ESP_ERR_NO_MEM, registered_count);
    }
    s_frame_queue_drops.store(0, std::memory_order_relaxed);
    s_frame_callbacks_in_flight.store(0, std::memory_order_relaxed);
    if (xTaskCreate(event_worker_task, "rf_events", kEventWorkerTaskStackSize, nullptr, 3,
                    &s_event_worker_task) != pdPASS) {
        return fail_start(ESP_ERR_NO_MEM, registered_count);
    }

    std::printf("\nNative ESP32 + CC1101 RF console ready. Type 'help'.\n");
    std::fflush(stdout);
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
    return ESP_OK;
}

void rf_console_on_frame(const RfFrame &frame, void *)
{
    s_frame_callbacks_in_flight.fetch_add(1, std::memory_order_acq_rel);
    ConsoleEvent event{};
    event.type = ConsoleEventType::kFrame;
    event.frame = frame;
    event.occurred_us = esp_timer_get_time();
    if (s_event_queue == nullptr || xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        s_frame_queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
    s_frame_callbacks_in_flight.fetch_sub(1, std::memory_order_release);
}

}  // namespace rfbridge
