#include "rf_console.hpp"

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

#include <fcntl.h>
#include <unistd.h>

#include "bridge_control.hpp"
#include "bridge_events.hpp"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_log_color.h"
#include "esp_log_write.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_wifi.hpp"
#include "ota_update.hpp"
#include "rf_automation.hpp"
#include "rf_console_format.hpp"
#include "rf_console_linenoise.h"
#include "rf_console_parse.hpp"
#include "rf_signals.hpp"
#include "rf_storage.hpp"
#include "sdkconfig.h"
#include "web_auth.hpp"

#ifndef CONFIG_ESP_CONSOLE_UART_DEFAULT
#error "rf_console requires CONFIG_ESP_CONSOLE_UART_DEFAULT"
#endif

namespace rfbridge {
namespace {

constexpr char kTag[] = "rf_console";
constexpr std::size_t kOutputLineSize = 2048;
constexpr uint32_t kEventWorkerTaskStackSize = 6144;
constexpr UBaseType_t kFrameEventQueueDepth = 8;
constexpr UBaseType_t kAutomationLogQueueDepth = 8;
constexpr UBaseType_t kNetworkEventQueueDepth = 8;
constexpr UBaseType_t kOtaEventQueueDepth = 8;
constexpr UBaseType_t kBridgeEventQueueDepth = 8;
constexpr std::size_t kAutomationLogLineSize = 256;
constexpr std::size_t kSystemEventLineSize = 256;
constexpr uint8_t kFrameEventBurst = 4;

struct ConsoleEvent {
    int64_t occurred_us = 0;
    RfFrame frame{};
    LearnedMatch learned{};
};

struct ConsoleOtaEvent {
    OtaUpdateEvent event{};
    uint32_t series_generation = 0;
};

struct ConsoleSystemEvent {
    BridgeEventType type = BridgeEventType::kLearnArmed;
    BridgeEventSource source = BridgeEventSource::kSystem;
    esp_err_t result = ESP_OK;
    uint32_t value = 0;
    uint16_t repeats = 0;
    char name[kRfStorageNameCapacity]{};
    char target[kRfStorageNameCapacity]{};
};

static_assert(std::is_trivially_copyable_v<ConsoleSystemEvent>);
static_assert(sizeof(ConsoleSystemEvent) <= 48U);
static_assert(kBridgeEventQueueDepth * sizeof(ConsoleSystemEvent) <= 384U);

QueueHandle_t s_event_queue = nullptr;
QueueHandle_t s_automation_log_queue = nullptr;
QueueHandle_t s_network_event_queue = nullptr;
QueueHandle_t s_ota_event_queue = nullptr;
QueueHandle_t s_bridge_event_queue = nullptr;
SemaphoreHandle_t s_output_mutex = nullptr;
TaskHandle_t s_event_worker_task = nullptr;
TaskHandle_t s_repl_task = nullptr;
bool s_uart_driver_installed = false;
bool s_console_initialized = false;
bool s_help_registered = false;
bool s_log_hook_installed = false;
vprintf_like_t s_previous_log_vprintf = nullptr;
std::atomic<TaskHandle_t> s_log_output_owner{nullptr};
uint32_t s_output_depth = 0;
uint8_t s_log_output_fragments = 0;
char s_repl_prompt[24]{};
bool s_bridge_sink_registered = false;
uint8_t s_bridge_sink_id = 0;
std::atomic<bool> s_started{false};
std::atomic_flag s_starting = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> s_frame_queue_drops{0};
std::atomic<uint32_t> s_ota_series_generation{0};
std::atomic<ConsoleStyle> s_console_style{ConsoleStyle::kPretty};
RawSignal s_staged_raw{};
bool s_raw_staging = false;
bool s_ota_progress_series_active = false;
uint32_t s_rendered_ota_series_generation = 0;

class OutputGuard {
public:
    OutputGuard()
        : locked_(s_output_mutex != nullptr &&
                  xSemaphoreTakeRecursive(s_output_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (locked_ && s_output_depth++ == 0) {
            rf_linenoiseSuspendActiveLine();
        }
    }
    ~OutputGuard()
    {
        if (locked_) {
            if (s_output_depth > 0 && --s_output_depth == 0) {
                rf_linenoiseResumeActiveLine();
            }
            xSemaphoreGiveRecursive(s_output_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};

void editor_lock(void *)
{
    if (s_output_mutex != nullptr) {
        xSemaphoreTakeRecursive(s_output_mutex, portMAX_DELAY);
    }
}

void editor_unlock(void *)
{
    if (s_output_mutex != nullptr) {
        xSemaphoreGiveRecursive(s_output_mutex);
    }
}

bool log_fragment_has_line_break(const char *format, va_list arguments)
{
    char preview[256]{};
    va_list preview_arguments;
    va_copy(preview_arguments, arguments);
    const int length = std::vsnprintf(preview, sizeof(preview), format, preview_arguments);
    va_end(preview_arguments);
    if (length < 0) {
        return true;
    }
    if (static_cast<std::size_t>(length) < sizeof(preview)) {
        return std::strchr(preview, '\n') != nullptr;
    }

    std::unique_ptr<char[]> expanded(new (std::nothrow) char[static_cast<std::size_t>(length) + 1U]);
    if (!expanded) {
        return true;
    }
    va_copy(preview_arguments, arguments);
    const int expanded_length = std::vsnprintf(expanded.get(), static_cast<std::size_t>(length) + 1U,
                                               format, preview_arguments);
    va_end(preview_arguments);
    return expanded_length < 0 || std::strchr(expanded.get(), '\n') != nullptr;
}

bool begin_log_output(TaskHandle_t task)
{
    if (task != nullptr && s_log_output_owner.load(std::memory_order_acquire) == task) {
        ++s_log_output_fragments;
        return true;
    }
    if (s_output_mutex == nullptr ||
        xSemaphoreTakeRecursive(s_output_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (s_output_depth++ == 0) {
        rf_linenoiseSuspendActiveLine();
    }
    s_log_output_fragments = 1;
    s_log_output_owner.store(task, std::memory_order_release);
    return true;
}

void finish_log_output(TaskHandle_t task)
{
    if (s_log_output_owner.load(std::memory_order_acquire) != task) {
        return;
    }
    s_log_output_owner.store(nullptr, std::memory_order_release);
    s_log_output_fragments = 0;
    if (s_output_depth > 0 && --s_output_depth == 0) {
        rf_linenoiseResumeActiveLine();
    }
    xSemaphoreGiveRecursive(s_output_mutex);
}

int coordinated_log_vprintf(const char *format, va_list arguments)
{
    vprintf_like_t writer = s_previous_log_vprintf;
    if (writer == nullptr || writer == coordinated_log_vprintf) {
        writer = &vprintf;
    }
    const bool line_break = log_fragment_has_line_break(format, arguments);
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    const bool coordinated = begin_log_output(task);
    const int result = writer(format, arguments);
    if (coordinated && (line_break || result < 0 || s_log_output_fragments >= 8U)) {
        finish_log_output(task);
    }
    return result;
}

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

void format_ipv4(uint32_t address, char *output, std::size_t capacity)
{
    std::snprintf(output, capacity, "%u.%u.%u.%u", static_cast<unsigned>(address & 0xffU),
                  static_cast<unsigned>((address >> 8U) & 0xffU),
                  static_cast<unsigned>((address >> 16U) & 0xffU),
                  static_cast<unsigned>((address >> 24U) & 0xffU));
}

ConsoleStyle current_console_style()
{
    return s_console_style.load(std::memory_order_acquire);
}

const char *wifi_state_display(NetworkWifiState state);
const char *ota_state_display(OtaUpdateState state);

ConsoleTone wifi_state_tone(NetworkWifiState state)
{
    switch (state) {
        case NetworkWifiState::kOnline:
            return ConsoleTone::kSuccess;
        case NetworkWifiState::kFault:
            return ConsoleTone::kError;
        case NetworkWifiState::kWaitingDhcp:
        case NetworkWifiState::kRetryWait:
        case NetworkWifiState::kStopping:
            return ConsoleTone::kWarning;
        case NetworkWifiState::kOff:
            return ConsoleTone::kMuted;
        case NetworkWifiState::kStarting:
        case NetworkWifiState::kConnecting:
            return ConsoleTone::kInfo;
    }
    return ConsoleTone::kError;
}

ConsoleTone ota_state_tone(OtaUpdateState state)
{
    switch (state) {
        case OtaUpdateState::kIdle:
            return ConsoleTone::kSuccess;
        case OtaUpdateState::kReceiving:
        case OtaUpdateState::kValidating:
            return ConsoleTone::kWarning;
        case OtaUpdateState::kPendingReboot:
            return ConsoleTone::kAction;
        case OtaUpdateState::kFailed:
        case OtaUpdateState::kUnavailable:
            return ConsoleTone::kError;
    }
    return ConsoleTone::kError;
}

void print_tagged_line(ConsoleTone tone, const char *tag, const char *plain_line,
                       const char *pretty_message, bool separate = true)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    if (current_console_style() == ConsoleStyle::kPlain) {
        std::printf(separate ? "\n%s\n" : "%s\n", plain_line);
        return;
    }

    constexpr std::size_t kPrettyBodyWidth = 66;
    const char *cursor = pretty_message;
    bool first = true;
    do {
        std::size_t chunk_length = 0;
        std::size_t consumed = 0;
        if (!console_wrap_chunk(cursor, kPrettyBodyWidth, &chunk_length, &consumed)) {
            ESP_LOGE(kTag, "Could not wrap console event tag=%s", tag);
            return;
        }
        while (chunk_length > 0 && cursor[chunk_length - 1U] == ' ') {
            --chunk_length;
        }
        char chunk[kPrettyBodyWidth + 1U]{};
        std::memcpy(chunk, cursor, chunk_length);
        char output[192]{};
        if (!format_console_tagged_line(ConsoleStyle::kPretty, tone, first ? tag : "", "", chunk,
                                         output, sizeof(output))) {
            ESP_LOGE(kTag, "Could not format console event tag=%s", tag);
            return;
        }
        std::printf(first && separate ? "\n%s\n" : "%s\n", output);
        cursor += consumed;
        while (*cursor == ' ') {
            ++cursor;
        }
        first = false;
    } while (*cursor != '\0');
}

void print_dashboard_header(const char *title)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char line[192]{};
    if (format_console_section_header(title, line, sizeof(line))) {
        std::printf("%s\n", line);
    }
}

void print_dashboard_footer()
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char line[192]{};
    if (format_console_section_footer(line, sizeof(line))) {
        std::printf("%s\n", line);
    }
}

void print_dashboard_row(const char *left_label, const char *left_value, ConsoleTone left_tone,
                         const char *right_label, const char *right_value, ConsoleTone right_tone)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char line[256]{};
    if (format_console_dashboard_row(left_label, left_value, left_tone, right_label, right_value,
                                     right_tone, line, sizeof(line))) {
        std::printf("%s\n", line);
    } else {
        ESP_LOGE(kTag, "Could not format dashboard row labels=%s,%s", left_label, right_label);
    }
}

void print_dashboard_value(const char *label, const char *value, ConsoleTone tone)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char line[256]{};
    if (format_console_dashboard_value(label, value, tone, line, sizeof(line))) {
        std::printf("%s\n", line);
    } else {
        ESP_LOGE(kTag, "Could not format dashboard value label=%s", label);
    }
}

void print_frame(const char *prefix, const RfFrame &frame, const LearnedMatch *learned = nullptr)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char line[kOutputLineSize]{};
    std::size_t length = 0;
    bool formatted = true;
    if (learned != nullptr && learned->kind == LearnedMatchKind::kUnique) {
        formatted = append_to_line(line, sizeof(line), &length, "learned=%s ", learned->name);
    } else if (learned != nullptr && learned->kind == LearnedMatchKind::kAmbiguous) {
        formatted = append_to_line(line, sizeof(line), &length, "learned=ambiguous(%u) ",
                                   learned->count);
    }
    if (frame.encoding == RfEncoding::kDecoded) {
        formatted = formatted && append_to_line(
            line, sizeof(line), &length,
            "RC code=%llu hex=0x%llX bits=%u protocol=%u pulse_us=%u confidence=%s repeats=%u "
            "fingerprint=0x%08lX",
            static_cast<unsigned long long>(frame.decoded.code),
            static_cast<unsigned long long>(frame.decoded.code), frame.decoded.bits,
            frame.decoded.protocol, frame.decoded.pulse_us,
            frame.confidence == RfFrameConfidence::kRepeatedEvidence ? "repeated" : "single",
            frame.observed_repeats, static_cast<unsigned long>(frame.fingerprint));
    } else {
        formatted = formatted && append_to_line(line, sizeof(line), &length,
                                    "RAW start=%u count=%u repeats=%u fingerprint=0x%08lX durations=",
                                   frame.raw.start_level, frame.raw.count, frame.observed_repeats,
                                   static_cast<unsigned long>(frame.fingerprint));
        for (std::size_t index = 0; formatted && index < frame.raw.count; ++index) {
            formatted = append_to_line(line, sizeof(line), &length, "%s%u", index == 0 ? "" : ",",
                                       frame.raw.durations_us[index]);
        }
    }
    if (!formatted) {
        ESP_LOGE(kTag, "Could not format %s frame output without truncation", prefix);
        return;
    }
    const char *pretty_tag = std::strcmp(prefix, "LAST") == 0 ? "LAST" : "RF RX";
    if (current_console_style() == ConsoleStyle::kPretty) {
        print_tagged_line(ConsoleTone::kInfo, pretty_tag, "", line);
    } else {
        char plain_prefix[8]{};
        const int prefix_written = std::snprintf(plain_prefix, sizeof(plain_prefix), "%s ", prefix);
        if (prefix_written < 0 || static_cast<std::size_t>(prefix_written) >= sizeof(plain_prefix) ||
            !decorate_console_message(ConsoleStyle::kPlain, ConsoleTone::kInfo, pretty_tag,
                                      plain_prefix, line, sizeof(line))) {
            ESP_LOGE(kTag, "Could not format %s frame prefix", prefix);
            return;
        }
        std::printf("\n%s\n", line);
    }
    std::fflush(stdout);
}


void process_console_event(const ConsoleEvent &event)
{
    print_frame("RX", event.frame, &event.learned);
}



void process_automation_log_event(const RfAutomationEvent &event)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char line[kAutomationLogLineSize]{};
    const esp_err_t result = static_cast<esp_err_t>(event.result);
    if (!format_rf_automation_event(event, esp_err_to_name(result), line, sizeof(line))) {
        print_tagged_line(ConsoleTone::kError, "RULE", "RULE LOG ERROR type=invalid",
                          "Could not format automation event");
        std::fflush(stdout);
        return;
    }
    ConsoleTone tone = ConsoleTone::kWarning;
    if (event.type == RfAutomationEventType::kTriggered) {
        tone = ConsoleTone::kAction;
    } else if (event.type == RfAutomationEventType::kActionCompleted) {
        tone = result == ESP_OK ? ConsoleTone::kSuccess : ConsoleTone::kError;
    } else if (event.type == RfAutomationEventType::kQueueDrop) {
        tone = ConsoleTone::kError;
    }
    const char *pretty = std::strncmp(line, "RULE ", 5U) == 0 ? line + 5U : line;
    print_tagged_line(tone, "RULE", line, pretty);
    std::fflush(stdout);
}

bool enqueue_automation_log_event(const RfAutomationEvent &event, void *)
{
    if (s_automation_log_queue == nullptr || xQueueSend(s_automation_log_queue, &event, 0) != pdTRUE) {
        return false;
    }
    if (s_event_worker_task != nullptr) {
        xTaskNotifyGive(s_event_worker_task);
    }
    return true;
}

void process_network_event(const NetworkWifiEvent &event)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char plain[kSystemEventLineSize]{};
    char pretty[kSystemEventLineSize]{};
    char ip[16]{};
    char gateway[16]{};
    switch (event.type) {
        case NetworkWifiEventType::kConnected:
            if (!format_network_wifi_connected_line(event.ssid, event.ip, event.netmask,
                                                     event.gateway, plain, sizeof(plain))) {
                ESP_LOGE(kTag, "Could not format connected Wi-Fi event");
                break;
            }
            format_ipv4(event.ip, ip, sizeof(ip));
            format_ipv4(event.gateway, gateway, sizeof(gateway));
            if (current_console_style() == ConsoleStyle::kPretty) {
                std::snprintf(pretty, sizeof(pretty), "Connected to %s", event.ssid);
                print_tagged_line(ConsoleTone::kSuccess, "WIFI", plain, pretty);
                std::snprintf(pretty, sizeof(pretty), "IP %s | gateway %s", ip, gateway);
                print_tagged_line(ConsoleTone::kInfo, "", "", pretty, false);
            } else {
                print_tagged_line(ConsoleTone::kSuccess, "WIFI", plain, plain);
            }
            if (event.error != ESP_OK) {
                std::snprintf(plain, sizeof(plain), "WIFI SAVE ERROR error=%s (0x%x)",
                              esp_err_to_name(event.error), static_cast<unsigned>(event.error));
                std::snprintf(pretty, sizeof(pretty), "Credential save failed: %s (0x%x)",
                              esp_err_to_name(event.error), static_cast<unsigned>(event.error));
                print_tagged_line(ConsoleTone::kError, "WIFI", plain, pretty);
            }
            break;
        case NetworkWifiEventType::kDisconnected:
            if (format_console_wifi_disconnected_message(
                    ConsoleStyle::kPlain, event.ssid, event.reason,
                    network_wifi_state_name(event.state), plain, sizeof(plain)) &&
                format_console_wifi_disconnected_message(
                    ConsoleStyle::kPretty, event.ssid, event.reason,
                    network_wifi_state_name(event.state), pretty, sizeof(pretty))) {
                print_tagged_line(ConsoleTone::kWarning, "WIFI", plain, pretty);
            } else {
                ESP_LOGE(kTag, "Could not format disconnected Wi-Fi event");
            }
            break;
        case NetworkWifiEventType::kScanResult:
            std::snprintf(plain, sizeof(plain), "WIFI AP ssid=%s rssi=%d channel=%u auth=%u",
                          event.ssid, event.rssi, event.channel, event.auth_mode);
            std::snprintf(pretty, sizeof(pretty), "AP %-32s %4d dBm | channel %u | auth %u",
                          event.ssid, event.rssi, event.channel, event.auth_mode);
            print_tagged_line(ConsoleTone::kInfo, "WIFI", plain, pretty);
            break;
        case NetworkWifiEventType::kScanCompleted:
            std::snprintf(plain, sizeof(plain), "WIFI SCAN COMPLETE count=%ld",
                          static_cast<long>(event.reason));
            std::snprintf(pretty, sizeof(pretty), "Scan complete | %ld network(s)",
                          static_cast<long>(event.reason));
            print_tagged_line(ConsoleTone::kSuccess, "WIFI", plain, pretty);
            break;
        case NetworkWifiEventType::kError:
            std::snprintf(plain, sizeof(plain), "WIFI ERROR state=%s error=%s (0x%x)",
                          network_wifi_state_name(event.state), esp_err_to_name(event.error),
                          static_cast<unsigned>(event.error));
            std::snprintf(pretty, sizeof(pretty), "%s failed: %s (0x%x)",
                          wifi_state_display(event.state), esp_err_to_name(event.error),
                          static_cast<unsigned>(event.error));
            print_tagged_line(ConsoleTone::kError, "WIFI", plain, pretty);
            break;
        case NetworkWifiEventType::kStateChanged:
            std::snprintf(plain, sizeof(plain), "WIFI STATE state=%s",
                          network_wifi_state_name(event.state));
            std::snprintf(pretty, sizeof(pretty), "State: %s", wifi_state_display(event.state));
            print_tagged_line(wifi_state_tone(event.state), "WIFI", plain, pretty);
            break;
    }
    std::fflush(stdout);
}

bool enqueue_network_event(const NetworkWifiEvent &event, void *)
{
    if (s_network_event_queue == nullptr || xQueueSend(s_network_event_queue, &event, 0) != pdTRUE) {
        return false;
    }
    if (s_event_worker_task != nullptr) {
        xTaskNotifyGive(s_event_worker_task);
    }
    return true;
}

void process_ota_event(const ConsoleOtaEvent &queued_event)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    const OtaUpdateEvent &event = queued_event.event;
    char plain[kSystemEventLineSize]{};
    char pretty[kSystemEventLineSize]{};
    switch (event.type) {
        case OtaUpdateEventType::kServerStarted: {
            s_ota_progress_series_active = false;
            NetworkWifiStatus wifi{};
            OtaUpdateStatus ota{};
            char ip[16]{};
            if (get_network_wifi_status(&wifi) == ESP_OK && get_ota_update_status(&ota) == ESP_OK) {
                format_ipv4(wifi.ip, ip, sizeof(ip));
                std::snprintf(plain, sizeof(plain), "OTA READY url=http://%s:%u/api/v1/ota", ip,
                              ota.port);
                std::snprintf(pretty, sizeof(pretty), "Ready: http://%s:%u/api/v1/ota", ip,
                              ota.port);
                print_tagged_line(ConsoleTone::kSuccess, "OTA", plain, pretty);
            }
            break;
        }
        case OtaUpdateEventType::kServerStopped:
            s_ota_progress_series_active = false;
            print_tagged_line(ConsoleTone::kMuted, "OTA", "OTA SERVER STOPPED", "Server stopped");
            break;
        case OtaUpdateEventType::kProgress:
            if (format_ota_progress_line(current_console_style(), event.bytes_received,
                                         event.content_length, pretty, sizeof(pretty))) {
                const bool continuing_series =
                    s_ota_progress_series_active &&
                    s_rendered_ota_series_generation == queued_event.series_generation;
                std::printf(continuing_series ? "%s\n" : "\n%s\n", pretty);
                s_ota_progress_series_active = true;
                s_rendered_ota_series_generation = queued_event.series_generation;
            }
            break;
        case OtaUpdateEventType::kCompleted:
            s_ota_progress_series_active = false;
            std::snprintf(plain, sizeof(plain), "OTA COMPLETE version=%s bytes=%lu rebooting=1",
                          event.candidate_version,
                          static_cast<unsigned long>(event.bytes_received));
            std::snprintf(pretty, sizeof(pretty), "Complete | version %s | %lu bytes | rebooting",
                          event.candidate_version,
                          static_cast<unsigned long>(event.bytes_received));
            print_tagged_line(ConsoleTone::kSuccess, "OTA", plain, pretty);
            break;
        case OtaUpdateEventType::kFailed:
            s_ota_progress_series_active = false;
            std::snprintf(plain, sizeof(plain),
                          "OTA ERROR error=%s (0x%x) maintenance_error=%s (0x%x) bytes=%lu total=%lu",
                          esp_err_to_name(event.error), static_cast<unsigned>(event.error),
                          esp_err_to_name(event.maintenance_error),
                          static_cast<unsigned>(event.maintenance_error),
                          static_cast<unsigned long>(event.bytes_received),
                          static_cast<unsigned long>(event.content_length));
            std::snprintf(pretty, sizeof(pretty), "Failed: %s (0x%x) | maintenance %s | %lu/%lu bytes",
                          esp_err_to_name(event.error), static_cast<unsigned>(event.error),
                          esp_err_to_name(event.maintenance_error),
                          static_cast<unsigned long>(event.bytes_received),
                          static_cast<unsigned long>(event.content_length));
            print_tagged_line(ConsoleTone::kError, "OTA", plain, pretty);
            break;
    }
    std::fflush(stdout);
}

bool enqueue_ota_event(const OtaUpdateEvent &event, void *)
{
    ConsoleOtaEvent queued_event{};
    queued_event.event = event;
    // Advance at every boundary even if its display event is dropped, so a later upload starts fresh.
    queued_event.series_generation = event.type == OtaUpdateEventType::kProgress
                                         ? s_ota_series_generation.load(std::memory_order_acquire)
                                         : s_ota_series_generation.fetch_add(1, std::memory_order_acq_rel);
    if (s_ota_event_queue == nullptr ||
        xQueueSend(s_ota_event_queue, &queued_event, 0) != pdTRUE) {
        return false;
    }
    if (s_event_worker_task != nullptr) {
        xTaskNotifyGive(s_event_worker_task);
    }
    return true;
}

void process_bridge_event(const ConsoleSystemEvent &event)
{
    char plain[192]{};
    char pretty[192]{};
    ConsoleTone tone = ConsoleTone::kInfo;
    const char *tag = "EVENT";
    switch (event.type) {
        case BridgeEventType::kLearnArmed:
            std::snprintf(plain, sizeof(plain), "LEARN ARMED name=%s timeout=30s", event.name);
            std::snprintf(pretty, sizeof(pretty), "Armed for %s | timeout 30 s", event.name);
            tag = "LEARN";
            break;
        case BridgeEventType::kLearnReplaced:
            std::snprintf(plain, sizeof(plain), "LEARN REPLACED old=%s new=%s", event.name,
                          event.target);
            std::snprintf(pretty, sizeof(pretty), "Replaced pending %s with %s", event.name,
                          event.target);
            tag = "LEARN";
            tone = ConsoleTone::kWarning;
            break;
        case BridgeEventType::kLearnCompleted:
            std::snprintf(plain, sizeof(plain), "LEARNED name=%s", event.name);
            std::snprintf(pretty, sizeof(pretty), "Saved %s", event.name);
            tag = "LEARN";
            tone = ConsoleTone::kSuccess;
            break;
        case BridgeEventType::kLearnCancelled:
            std::snprintf(plain, sizeof(plain), "LEARN CANCELLED name=%s", event.name);
            std::snprintf(pretty, sizeof(pretty), "Cancelled learning for %s", event.name);
            tag = "LEARN";
            tone = ConsoleTone::kWarning;
            break;
        case BridgeEventType::kLearnTimeout:
            std::snprintf(plain, sizeof(plain), "LEARN TIMEOUT name=%s", event.name);
            std::snprintf(pretty, sizeof(pretty), "Timed out waiting for %s", event.name);
            tag = "LEARN";
            tone = ConsoleTone::kWarning;
            break;
        case BridgeEventType::kLearnFailed:
            std::snprintf(plain, sizeof(plain), "LEARN ERROR name=%s error=%s (0x%x)",
                          event.name, esp_err_to_name(event.result),
                          static_cast<unsigned>(event.result));
            std::snprintf(pretty, sizeof(pretty), "Could not learn %s: %s", event.name,
                          esp_err_to_name(event.result));
            tag = "LEARN";
            tone = ConsoleTone::kError;
            break;
        case BridgeEventType::kTxStarted:
            std::snprintf(plain, sizeof(plain), "TX START source=%u name=%s repeats=%u",
                          static_cast<unsigned>(event.source), event.name[0] == '\0' ? "-" : event.name,
                          event.repeats);
            std::snprintf(pretty, sizeof(pretty), "Started %s | repeats %u",
                          event.name[0] == '\0' ? "transmission" : event.name, event.repeats);
            tag = "RF TX";
            tone = ConsoleTone::kAction;
            break;
        case BridgeEventType::kTxCompleted:
            std::snprintf(plain, sizeof(plain), "TX COMPLETE source=%u name=%s repeats=%u result=%s",
                          static_cast<unsigned>(event.source), event.name[0] == '\0' ? "-" : event.name,
                          event.repeats, esp_err_to_name(event.result));
            std::snprintf(pretty, sizeof(pretty), "%s | %s",
                          event.name[0] == '\0' ? "Transmission" : event.name,
                          event.result == ESP_OK ? "complete" : esp_err_to_name(event.result));
            tag = "RF TX";
            tone = event.result == ESP_OK ? ConsoleTone::kSuccess : ConsoleTone::kError;
            break;
        case BridgeEventType::kWebStarted:
            std::snprintf(plain, sizeof(plain), "WEB STARTED port=%lu",
                          static_cast<unsigned long>(event.value));
            std::snprintf(pretty, sizeof(pretty), "Web UI started on port %lu",
                          static_cast<unsigned long>(event.value));
            tag = "WEB";
            tone = ConsoleTone::kSuccess;
            break;
        case BridgeEventType::kWebStopped:
            std::snprintf(plain, sizeof(plain), "WEB STOPPED");
            std::snprintf(pretty, sizeof(pretty), "Web UI stopped");
            tag = "WEB";
            tone = ConsoleTone::kMuted;
            break;
        default:
            return;
    }
    print_tagged_line(tone, tag, plain, pretty);
    std::fflush(stdout);
}

bool enqueue_bridge_event(const BridgeEvent &event, void *)
{
    switch (event.type) {
        case BridgeEventType::kRx: {
            ConsoleEvent console_event{};
            console_event.occurred_us = event.occurred_us;
            console_event.frame = event.payload.rf.frame;
            console_event.learned = event.payload.rf.learned;
            if (s_event_queue == nullptr || xQueueSend(s_event_queue, &console_event, 0) != pdTRUE) {
                s_frame_queue_drops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            break;
        }
        case BridgeEventType::kAutomation:
            return enqueue_automation_log_event(event.payload.automation, nullptr);
        case BridgeEventType::kNetwork:
            return enqueue_network_event(event.payload.network, nullptr);
        case BridgeEventType::kOta:
            return enqueue_ota_event(event.payload.ota, nullptr);
        case BridgeEventType::kLearnArmed:
        case BridgeEventType::kLearnReplaced:
        case BridgeEventType::kLearnCompleted:
        case BridgeEventType::kLearnCancelled:
        case BridgeEventType::kLearnTimeout:
        case BridgeEventType::kLearnFailed:
        case BridgeEventType::kTxStarted:
        case BridgeEventType::kTxCompleted:
        case BridgeEventType::kWebStarted:
        case BridgeEventType::kWebStopped: {
            ConsoleSystemEvent system_event{};
            system_event.type = event.type;
            system_event.source = event.source;
            system_event.result = event.result;
            system_event.value = event.value;
            system_event.repeats = event.repeats;
            std::memcpy(system_event.name, event.payload.rf.name, sizeof(system_event.name));
            std::memcpy(system_event.target, event.payload.rf.target, sizeof(system_event.target));
            if (s_bridge_event_queue == nullptr ||
                xQueueSend(s_bridge_event_queue, &system_event, 0) != pdTRUE) {
                return false;
            }
            break;
        }
        case BridgeEventType::kOperationCompleted:
            return true;
    }
    if (s_event_worker_task != nullptr) {
        xTaskNotifyGive(s_event_worker_task);
    }
    return true;
}


void event_worker_task(void *)
{
    uint8_t frame_budget = kFrameEventBurst;
    uint8_t system_cursor = 0;
    while (true) {
        ConsoleEvent frame_event{};
        if (frame_budget > 0 && xQueueReceive(s_event_queue, &frame_event, 0) == pdTRUE) {
            process_console_event(frame_event);
            --frame_budget;
            continue;
        }

        bool processed = false;
        for (uint8_t offset = 0; offset < 4U && !processed; ++offset) {
            const uint8_t queue_index = static_cast<uint8_t>((system_cursor + offset) % 4U);
            if (queue_index == 0) {
                NetworkWifiEvent event{};
                if (xQueueReceive(s_network_event_queue, &event, 0) == pdTRUE) {
                    process_network_event(event);
                    processed = true;
                }
            } else if (queue_index == 1) {
                ConsoleOtaEvent event{};
                if (xQueueReceive(s_ota_event_queue, &event, 0) == pdTRUE) {
                    process_ota_event(event);
                    processed = true;
                }
            } else if (queue_index == 2) {
                RfAutomationEvent event{};
                if (xQueueReceive(s_automation_log_queue, &event, 0) == pdTRUE) {
                    process_automation_log_event(event);
                    processed = true;
                }
            } else {
                ConsoleSystemEvent event{};
                if (xQueueReceive(s_bridge_event_queue, &event, 0) == pdTRUE) {
                    process_bridge_event(event);
                    processed = true;
                }
            }
            if (processed) {
                system_cursor = static_cast<uint8_t>((queue_index + 1U) % 4U);
                frame_budget = kFrameEventBurst;
            }
        }
        if (processed) {
            continue;
        }

        frame_budget = kFrameEventBurst;
        if (xQueueReceive(s_event_queue, &frame_event, 0) == pdTRUE) {
            process_console_event(frame_event);
            --frame_budget;
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}


bool parse_bounded(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value)
{
    return parse_unsigned_value(text, maximum, value) && *value >= minimum;
}

int print_result(const char *operation, esp_err_t error)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    char plain[192]{};
    char pretty[192]{};
    char output[256]{};
    if (error == ESP_OK) {
        std::snprintf(plain, sizeof(plain), "OK %s", operation);
        std::snprintf(pretty, sizeof(pretty), "%s completed", operation);
    } else {
        std::snprintf(plain, sizeof(plain), "ERROR %s: %s (0x%x)", operation,
                      esp_err_to_name(error), static_cast<unsigned>(error));
        std::snprintf(pretty, sizeof(pretty), "%s failed: %s (0x%x)", operation,
                      esp_err_to_name(error), static_cast<unsigned>(error));
    }
    if (format_console_tagged_line(current_console_style(),
                                   error == ESP_OK ? ConsoleTone::kSuccess : ConsoleTone::kError,
                                   error == ESP_OK ? " OK " : "FAIL", plain, pretty, output,
                                   sizeof(output))) {
        std::printf("%s\n", output);
    }
    return error == ESP_OK ? 0 : 1;
}

int print_usage(const char *usage)
{
    print_tagged_line(ConsoleTone::kMuted, "HELP", usage, usage, false);
    return 1;
}

int print_validation_error(const char *plain, const char *pretty)
{
    print_tagged_line(ConsoleTone::kError, "FAIL", plain, pretty, false);
    return 1;
}

const char *wifi_state_display(NetworkWifiState state)
{
    switch (state) {
        case NetworkWifiState::kOff:
            return "OFF";
        case NetworkWifiState::kStarting:
            return "STARTING";
        case NetworkWifiState::kConnecting:
            return "CONNECTING";
        case NetworkWifiState::kWaitingDhcp:
            return "WAITING DHCP";
        case NetworkWifiState::kOnline:
            return "ONLINE";
        case NetworkWifiState::kRetryWait:
            return "RETRY WAIT";
        case NetworkWifiState::kStopping:
            return "STOPPING";
        case NetworkWifiState::kFault:
            return "FAULT";
    }
    return "INVALID";
}

const char *ota_state_display(OtaUpdateState state)
{
    switch (state) {
        case OtaUpdateState::kUnavailable:
            return "UNAVAILABLE";
        case OtaUpdateState::kIdle:
            return "IDLE";
        case OtaUpdateState::kReceiving:
            return "RECEIVING";
        case OtaUpdateState::kValidating:
            return "VALIDATING";
        case OtaUpdateState::kPendingReboot:
            return "PENDING REBOOT";
        case OtaUpdateState::kFailed:
            return "FAILED";
    }
    return "INVALID";
}

void format_half_dbm(int16_t value_x2, char *output, std::size_t capacity)
{
    const bool negative = value_x2 < 0;
    const uint16_t magnitude = static_cast<uint16_t>(negative ? -value_x2 : value_x2);
    std::snprintf(output, capacity, "%s%u.%u dBm", negative ? "-" : "", magnitude / 2U,
                  (magnitude % 2U) * 5U);
}

void print_plain_radio_status(const RfRadioStatus &status, bool include_summary)
{
    if (include_summary) {
        std::printf("STATUS running=%u rx_enabled=%u rx_active=%u tx=%u maintenance=%u has_last=%u accepted=%lu duplicates=%lu rx_drops=%lu truncated=%lu command_timeouts=%lu console_drops=%lu\n",
                    status.running, status.receive_enabled, status.receive_active, status.transmitting,
                    status.maintenance_active, status.has_last_frame,
                    static_cast<unsigned long>(status.accepted_frames),
                    static_cast<unsigned long>(status.suppressed_duplicates),
                    static_cast<unsigned long>(status.rx_queue_drops),
                    static_cast<unsigned long>(status.truncated_captures),
                    static_cast<unsigned long>(status.command_timeouts),
                    static_cast<unsigned long>(s_frame_queue_drops.load(std::memory_order_relaxed)));
    }
    if (status.cc1101_info_valid) {
        std::printf("RADIO part=0x%02X version=0x%02X state=0x%02X rssi_x2=%d cs=%u cca=%u resets=%lu recoveries=%lu ready_timeouts=%lu state_timeouts=%lu frequency_hz=%d power_dbm=%d\n",
                    status.cc1101.part_number, status.cc1101.version, status.cc1101.marc_state,
                    status.cc1101.rssi_dbm_x2, status.cc1101.carrier_sense,
                    status.cc1101.clear_channel,
                    static_cast<unsigned long>(status.cc1101.reset_count),
                    static_cast<unsigned long>(status.cc1101.recovery_count),
                    static_cast<unsigned long>(status.cc1101.ready_timeout_count),
                    static_cast<unsigned long>(status.cc1101.state_timeout_count),
                    CONFIG_CC1101_FREQUENCY_HZ, CONFIG_CC1101_TX_POWER_DBM);
    } else {
        std::printf("RADIO unavailable error=%s (0x%x) frequency_hz=%d power_dbm=%d\n",
                    esp_err_to_name(status.cc1101_error),
                    static_cast<unsigned>(status.cc1101_error), CONFIG_CC1101_FREQUENCY_HZ,
                    CONFIG_CC1101_TX_POWER_DBM);
    }
}

void print_pretty_radio_status(const RfRadioStatus &status, bool include_summary)
{
    char left[64]{};
    char right[64]{};
    if (include_summary) {
        print_dashboard_header("System / Console");
        print_dashboard_row("Style", console_style_name(current_console_style()), ConsoleTone::kInfo,
                            "RF service", status.running ? "RUNNING" : "STOPPED",
                            status.running ? ConsoleTone::kSuccess : ConsoleTone::kError);
        std::snprintf(left, sizeof(left), "%lu", static_cast<unsigned long>(
                                                   s_frame_queue_drops.load(std::memory_order_relaxed)));
        print_dashboard_row("Last frame", status.has_last_frame ? "available" : "none",
                            status.has_last_frame ? ConsoleTone::kInfo : ConsoleTone::kMuted,
                            "Console drops", left,
                            std::strcmp(left, "0") == 0 ? ConsoleTone::kMuted : ConsoleTone::kError);
        print_dashboard_footer();
    }

    print_dashboard_header("RF / CC1101");
    const char *receive = status.receive_active
                              ? "ACTIVE"
                              : (status.receive_enabled ? "ENABLED" : "OFF");
    print_dashboard_row("Service", status.running ? "RUNNING" : "STOPPED",
                        status.running ? ConsoleTone::kSuccess : ConsoleTone::kError,
                        "Receive", receive,
                        status.receive_active ? ConsoleTone::kSuccess : ConsoleTone::kWarning);
    print_dashboard_row("Transmit", status.transmitting ? "ACTIVE" : "idle",
                        status.transmitting ? ConsoleTone::kAction : ConsoleTone::kMuted,
                        "Maintenance", status.maintenance_active ? "ACTIVE" : "no",
                        status.maintenance_active ? ConsoleTone::kWarning : ConsoleTone::kMuted);
    std::snprintf(left, sizeof(left), "%d.%03d MHz", CONFIG_CC1101_FREQUENCY_HZ / 1000000,
                  (CONFIG_CC1101_FREQUENCY_HZ % 1000000) / 1000);
    std::snprintf(right, sizeof(right), "%+d dBm", CONFIG_CC1101_TX_POWER_DBM);
    print_dashboard_row("Frequency", left, ConsoleTone::kInfo, "TX power", right,
                        ConsoleTone::kAction);
    if (status.cc1101_info_valid) {
        std::snprintf(left, sizeof(left), "part 0x%02X / ver 0x%02X", status.cc1101.part_number,
                      status.cc1101.version);
        std::snprintf(right, sizeof(right), "0x%02X", status.cc1101.marc_state);
        print_dashboard_row("Chip", left, ConsoleTone::kInfo, "Radio state", right,
                            ConsoleTone::kInfo);
        format_half_dbm(status.cc1101.rssi_dbm_x2, left, sizeof(left));
        std::snprintf(right, sizeof(right), "CS %u / CCA %u", status.cc1101.carrier_sense,
                      status.cc1101.clear_channel);
        print_dashboard_row("Signal", left, ConsoleTone::kInfo, "Channel", right,
                            ConsoleTone::kInfo);
        std::snprintf(left, sizeof(left), "reset %lu / recovery %lu",
                      static_cast<unsigned long>(status.cc1101.reset_count),
                      static_cast<unsigned long>(status.cc1101.recovery_count));
        std::snprintf(right, sizeof(right), "ready %lu / state %lu",
                      static_cast<unsigned long>(status.cc1101.ready_timeout_count),
                      static_cast<unsigned long>(status.cc1101.state_timeout_count));
        print_dashboard_value("Lifecycle", left, ConsoleTone::kMuted);
        print_dashboard_value("Timeouts", right,
                              (status.cc1101.ready_timeout_count == 0 &&
                               status.cc1101.state_timeout_count == 0)
                                  ? ConsoleTone::kMuted
                                  : ConsoleTone::kError);
    } else {
        std::snprintf(left, sizeof(left), "%s (0x%x)", esp_err_to_name(status.cc1101_error),
                      static_cast<unsigned>(status.cc1101_error));
        print_dashboard_value("Hardware", left, ConsoleTone::kError);
    }
    std::snprintf(left, sizeof(left), "accepted %lu / duplicate %lu",
                  static_cast<unsigned long>(status.accepted_frames),
                  static_cast<unsigned long>(status.suppressed_duplicates));
    std::snprintf(right, sizeof(right), "queue %lu / truncated %lu",
                  static_cast<unsigned long>(status.rx_queue_drops),
                  static_cast<unsigned long>(status.truncated_captures));
    print_dashboard_value("Frames", left, ConsoleTone::kInfo);
    print_dashboard_value("RX faults", right,
                          (status.rx_queue_drops == 0 && status.truncated_captures == 0)
                              ? ConsoleTone::kMuted
                              : ConsoleTone::kWarning);
    print_dashboard_footer();
}

int render_radio_status(bool include_summary)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    RfRadioStatus status{};
    const esp_err_t error = get_rf_radio_status(&status);
    if (error != ESP_OK) {
        return print_result("status", error);
    }
    if (current_console_style() == ConsoleStyle::kPlain) {
        print_plain_radio_status(status, include_summary);
    } else {
        print_pretty_radio_status(status, include_summary);
    }
    return 0;
}

void print_plain_automation_status(const RfAutomationStatus &automation)
{
    if (!automation.available) {
        std::printf("AUTOMATION available=0 enabled_known=%u enabled=%u rules_known=%u rules=%u log_mode_known=%u log_mode=%s error=%s (0x%x) queue_drops=%lu log_events=%lu log_drops=%lu recovery=reboot_after_rule_changes\n",
                    automation.enabled_known, automation.enabled, automation.rule_count_known,
                    automation.rule_count, automation.log_mode_known,
                    automation.log_mode_known ? rf_automation_log_mode_name(automation.log_mode)
                                              : "unknown",
                    esp_err_to_name(automation.initialization_error),
                    static_cast<unsigned>(automation.initialization_error),
                    static_cast<unsigned long>(automation.queue_drops),
                    static_cast<unsigned long>(automation.log_events),
                    static_cast<unsigned long>(automation.log_drops));
        return;
    }
    std::printf("AUTOMATION available=1 enabled=%u paused=%u rules=%u log_mode=%s frames=%lu stale=%lu ambiguous=%lu matches=%lu actions=%lu cooldown_suppressed=%lu queue_drops=%lu tx_errors=%lu log_events=%lu log_drops=%lu last_error=%s last_trigger=%s last_target=%s\n",
                automation.enabled, automation.runtime_paused, automation.rule_count,
                rf_automation_log_mode_name(automation.log_mode),
                static_cast<unsigned long>(automation.frames_seen),
                static_cast<unsigned long>(automation.stale_frames),
                static_cast<unsigned long>(automation.ambiguous_frames),
                static_cast<unsigned long>(automation.matches),
                static_cast<unsigned long>(automation.actions_succeeded),
                static_cast<unsigned long>(automation.cooldown_suppressed),
                static_cast<unsigned long>(automation.queue_drops),
                static_cast<unsigned long>(automation.tx_errors),
                static_cast<unsigned long>(automation.log_events),
                static_cast<unsigned long>(automation.log_drops),
                esp_err_to_name(automation.last_error),
                automation.last_trigger[0] == '\0' ? "-" : automation.last_trigger,
                automation.last_target[0] == '\0' ? "-" : automation.last_target);
}

int render_automation_status()
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    RfAutomationStatus automation{};
    const esp_err_t error = rf_automation_get_status(&automation);
    if (current_console_style() == ConsoleStyle::kPlain) {
        if (error != ESP_OK) {
            std::printf("AUTOMATION unavailable error=%s (0x%x)\n", esp_err_to_name(error),
                        static_cast<unsigned>(error));
        } else {
            print_plain_automation_status(automation);
        }
        return error == ESP_OK ? 0 : 1;
    }

    print_dashboard_header("Automation");
    if (error != ESP_OK || !automation.available) {
        const esp_err_t shown_error = error == ESP_OK ? automation.initialization_error : error;
        char value[64]{};
        std::snprintf(value, sizeof(value), "%s (0x%x)", esp_err_to_name(shown_error),
                      static_cast<unsigned>(shown_error));
        print_dashboard_value("State", "UNAVAILABLE", ConsoleTone::kError);
        print_dashboard_value("Error", value, ConsoleTone::kError);
        print_dashboard_footer();
        return 1;
    }
    char left[64]{};
    char right[64]{};
    std::snprintf(left, sizeof(left), "%u", automation.rule_count);
    print_dashboard_row("Enabled", automation.enabled ? "yes" : "no",
                        automation.enabled ? ConsoleTone::kSuccess : ConsoleTone::kWarning,
                        "Rules", left, ConsoleTone::kInfo);
    print_dashboard_row("Runtime", automation.runtime_paused ? "PAUSED" : "ACTIVE",
                        automation.runtime_paused ? ConsoleTone::kWarning : ConsoleTone::kSuccess,
                        "Log mode", rf_automation_log_mode_name(automation.log_mode),
                        ConsoleTone::kInfo);
    std::snprintf(left, sizeof(left), "frames %lu / matches %lu",
                  static_cast<unsigned long>(automation.frames_seen),
                  static_cast<unsigned long>(automation.matches));
    std::snprintf(right, sizeof(right), "ok %lu / errors %lu",
                  static_cast<unsigned long>(automation.actions_succeeded),
                  static_cast<unsigned long>(automation.tx_errors));
    print_dashboard_value("Activity", left, ConsoleTone::kInfo);
    print_dashboard_value("Actions", right,
                          automation.tx_errors == 0 ? ConsoleTone::kSuccess : ConsoleTone::kError);
    std::snprintf(left, sizeof(left), "stale %lu / ambiguous %lu",
                  static_cast<unsigned long>(automation.stale_frames),
                  static_cast<unsigned long>(automation.ambiguous_frames));
    std::snprintf(right, sizeof(right), "cooldown %lu / queue %lu",
                  static_cast<unsigned long>(automation.cooldown_suppressed),
                  static_cast<unsigned long>(automation.queue_drops));
    print_dashboard_value("Skipped", left,
                          (automation.stale_frames == 0 && automation.ambiguous_frames == 0)
                              ? ConsoleTone::kMuted
                              : ConsoleTone::kWarning);
    print_dashboard_value("Suppressed", right,
                          automation.queue_drops == 0 ? ConsoleTone::kMuted
                                                      : ConsoleTone::kError);
    std::snprintf(left, sizeof(left), "%s -> %s",
                  automation.last_trigger[0] == '\0' ? "-" : automation.last_trigger,
                  automation.last_target[0] == '\0' ? "-" : automation.last_target);
    print_dashboard_value("Last rule", left, ConsoleTone::kAction);
    print_dashboard_value("Last result", esp_err_to_name(automation.last_error),
                          automation.last_error == ESP_OK ? ConsoleTone::kSuccess
                                                          : ConsoleTone::kError);
    print_dashboard_footer();
    return 0;
}

int render_wifi_status()
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    NetworkWifiStatus wifi{};
    const esp_err_t error = get_network_wifi_status(&wifi);
    if (current_console_style() == ConsoleStyle::kPlain) {
        if (error != ESP_OK) {
            std::printf("WIFI available=0 error=%s (0x%x)\n", esp_err_to_name(error),
                        static_cast<unsigned>(error));
            return 1;
        }
        char ip[16]{};
        char netmask[16]{};
        char gateway[16]{};
        char dns[16]{};
        format_ipv4(wifi.ip, ip, sizeof(ip));
        format_ipv4(wifi.netmask, netmask, sizeof(netmask));
        format_ipv4(wifi.gateway, gateway, sizeof(gateway));
        format_ipv4(wifi.dns, dns, sizeof(dns));
        std::printf("WIFI available=%u state=%s driver=%u started=%u saved_known=%u saved=%u active_saved=%u ota_locked=%u ssid=%s saved_ssid=%s ip=%s netmask=%s gateway=%s dns=%s rssi=%d retries=%lu reason=%ld persistence=%s last_error=%s drops=%lu\n",
                    wifi.available, network_wifi_state_name(wifi.state), wifi.driver_initialized,
                    wifi.driver_started, wifi.saved_known, wifi.saved, wifi.active_saved,
                    wifi.ota_locked, wifi.active_ssid[0] == '\0' ? "-" : wifi.active_ssid,
                    wifi.saved_ssid[0] == '\0' ? "-" : wifi.saved_ssid, ip, netmask, gateway,
                    dns, wifi.rssi, static_cast<unsigned long>(wifi.retry_count),
                    static_cast<long>(wifi.disconnect_reason),
                    esp_err_to_name(wifi.persistence_error), esp_err_to_name(wifi.last_error),
                    static_cast<unsigned long>(wifi.event_drops));
        return 0;
    }

    print_dashboard_header("Wi-Fi");
    if (error != ESP_OK) {
        char value[64]{};
        std::snprintf(value, sizeof(value), "%s (0x%x)", esp_err_to_name(error),
                      static_cast<unsigned>(error));
        print_dashboard_value("State", "UNAVAILABLE", ConsoleTone::kError);
        print_dashboard_value("Error", value, ConsoleTone::kError);
        print_dashboard_footer();
        return 1;
    }
    char left[64]{};
    char right[64]{};
    std::snprintf(left, sizeof(left), "%d dBm", wifi.rssi);
    print_dashboard_row("State", wifi_state_display(wifi.state), wifi_state_tone(wifi.state),
                        "Signal", wifi.state == NetworkWifiState::kOnline ? left : "-",
                        wifi.state == NetworkWifiState::kOnline ? ConsoleTone::kInfo
                                                               : ConsoleTone::kMuted);
    print_dashboard_value("Network", wifi.active_ssid[0] == '\0' ? "-" : wifi.active_ssid,
                          wifi.state == NetworkWifiState::kOnline ? ConsoleTone::kSuccess
                                                                 : ConsoleTone::kMuted);
    print_dashboard_value("Saved SSID", wifi.saved_ssid[0] == '\0' ? "-" : wifi.saved_ssid,
                          wifi.saved ? ConsoleTone::kInfo : ConsoleTone::kMuted);
    format_ipv4(wifi.ip, left, sizeof(left));
    format_ipv4(wifi.netmask, right, sizeof(right));
    print_dashboard_row("Address", left, ConsoleTone::kInfo, "Netmask", right,
                        ConsoleTone::kMuted);
    format_ipv4(wifi.gateway, left, sizeof(left));
    format_ipv4(wifi.dns, right, sizeof(right));
    print_dashboard_row("Gateway", left, ConsoleTone::kMuted, "DNS", right,
                        ConsoleTone::kMuted);
    print_dashboard_row("Saved", wifi.saved ? "yes" : "no",
                        wifi.saved ? ConsoleTone::kSuccess : ConsoleTone::kWarning,
                        "OTA lock", wifi.ota_locked ? "ACTIVE" : "no",
                        wifi.ota_locked ? ConsoleTone::kWarning : ConsoleTone::kMuted);
    std::snprintf(left, sizeof(left), "%lu", static_cast<unsigned long>(wifi.retry_count));
    std::snprintf(right, sizeof(right), "%lu", static_cast<unsigned long>(wifi.event_drops));
    print_dashboard_row("Retries", left,
                        wifi.retry_count == 0 ? ConsoleTone::kMuted : ConsoleTone::kWarning,
                        "Event drops", right,
                        wifi.event_drops == 0 ? ConsoleTone::kMuted : ConsoleTone::kError);
    print_dashboard_value("Persistence", esp_err_to_name(wifi.persistence_error),
                          wifi.persistence_error == ESP_OK ? ConsoleTone::kSuccess
                                                           : ConsoleTone::kError);
    print_dashboard_footer();
    return 0;
}

int render_ota_status()
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    OtaUpdateStatus ota{};
    const esp_err_t error = get_ota_update_status(&ota);
    if (current_console_style() == ConsoleStyle::kPlain) {
        if (error != ESP_OK) {
            std::printf("OTA available=0 error=%s (0x%x)\n", esp_err_to_name(error),
                        static_cast<unsigned>(error));
            return 1;
        }
        std::printf("OTA available=%u state=%s server=%u upload=%u port=%u running=%s update=%s version=%s candidate=%s bytes=%lu total=%lu pending_verify=%u rollback=%u last_error=%s maintenance_error=%s\n",
                    ota.available, ota_update_state_name(ota.state), ota.server_running,
                    ota.upload_active, ota.port, ota.running_partition, ota.update_partition,
                    ota.running_version,
                    ota.candidate_version[0] == '\0' ? "-" : ota.candidate_version,
                    static_cast<unsigned long>(ota.bytes_received),
                    static_cast<unsigned long>(ota.content_length), ota.pending_verification,
                    ota.rollback_possible, esp_err_to_name(ota.last_error),
                    esp_err_to_name(ota.maintenance_error));
        return 0;
    }

    print_dashboard_header("LAN OTA");
    if (error != ESP_OK) {
        char value[64]{};
        std::snprintf(value, sizeof(value), "%s (0x%x)", esp_err_to_name(error),
                      static_cast<unsigned>(error));
        print_dashboard_value("State", "UNAVAILABLE", ConsoleTone::kError);
        print_dashboard_value("Error", value, ConsoleTone::kError);
        print_dashboard_footer();
        return 1;
    }
    char left[64]{};
    std::snprintf(left, sizeof(left), "%s : %u", ota.server_running ? "LISTENING" : "stopped",
                  ota.port);
    print_dashboard_row("State", ota_state_display(ota.state), ota_state_tone(ota.state),
                        "Server", left,
                        ota.server_running ? ConsoleTone::kSuccess : ConsoleTone::kMuted);
    print_dashboard_row("Running", ota.running_partition, ConsoleTone::kSuccess,
                        "Next slot", ota.update_partition, ConsoleTone::kInfo);
    print_dashboard_value("Version", ota.running_version, ConsoleTone::kInfo);
    print_dashboard_value("Candidate",
                          ota.candidate_version[0] == '\0' ? "-" : ota.candidate_version,
                          ota.candidate_version[0] == '\0' ? ConsoleTone::kMuted
                                                           : ConsoleTone::kAction);
    std::snprintf(left, sizeof(left), "%u%% (%lu / %lu bytes)",
                  ota_progress_percent(ota.bytes_received, ota.content_length),
                  static_cast<unsigned long>(ota.bytes_received),
                  static_cast<unsigned long>(ota.content_length));
    print_dashboard_value("Progress", left,
                          ota.upload_active ? ConsoleTone::kWarning : ConsoleTone::kMuted);
    print_dashboard_row("Pending", ota.pending_verification ? "VERIFY" : "no",
                        ota.pending_verification ? ConsoleTone::kWarning : ConsoleTone::kMuted,
                        "Rollback", ota.rollback_possible ? "available" : "no",
                        ota.rollback_possible ? ConsoleTone::kSuccess : ConsoleTone::kMuted);
    print_dashboard_value("Last error", esp_err_to_name(ota.last_error),
                          ota.last_error == ESP_OK ? ConsoleTone::kSuccess
                                                   : ConsoleTone::kError);
    print_dashboard_value("Maintenance", esp_err_to_name(ota.maintenance_error),
                          ota.maintenance_error == ESP_OK ? ConsoleTone::kSuccess
                                                          : ConsoleTone::kError);
    print_dashboard_footer();
    return 0;
}

int status_command(int argc, char **)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    if (argc != 1) {
        return print_usage("usage: status");
    }
    const int result = render_radio_status(true);
    render_automation_status();
    render_wifi_status();
    return result;
}

bool read_wifi_password(WifiCredentials *credentials)
{
    char input[kWifiPasswordCapacity]{};
    errno = 0;
    const int result = rf_linenoiseReadMasked("WiFi password: ", input, sizeof(input));
    if (result < 0) {
        const int input_error = errno;
        std::memset(input, 0, sizeof(input));
        OutputGuard guard;
        if (result == RF_LINENOISE_MASKED_TOO_LONG) {
            std::printf("ERROR wifi password is too long\n");
        } else if (input_error == EAGAIN) {
            std::printf("ERROR wifi password input cancelled\n");
        } else {
            std::printf("ERROR wifi password input failed\n");
        }
        return false;
    }
    const std::size_t length = static_cast<std::size_t>(result);
    std::memcpy(credentials->password, input, length + 1U);
    std::memset(input, 0, sizeof(input));
    return true;
}

int console_command(int argc, char **argv)
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "style") == 0) {
        char plain[48]{};
        char pretty[64]{};
        char output[128]{};
        const ConsoleStyle style = current_console_style();
        std::snprintf(plain, sizeof(plain), "CONSOLE style=%s", console_style_name(style));
        std::snprintf(pretty, sizeof(pretty), "Console style is %s", console_style_name(style));
        if (format_console_tagged_line(style, ConsoleTone::kInfo, "STYLE", plain, pretty, output,
                                       sizeof(output))) {
            std::printf("%s\n", output);
        }
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "style") == 0) {
        ConsoleStyle style{};
        if (!parse_console_style(argv[2], &style)) {
            return print_usage("usage: console style <pretty|plain>");
        }
        s_console_style.store(style, std::memory_order_release);
        char plain[48]{};
        char pretty[64]{};
        char output[128]{};
        std::snprintf(plain, sizeof(plain), "CONSOLE style=%s", console_style_name(style));
        std::snprintf(pretty, sizeof(pretty), "Console style changed to %s",
                      console_style_name(style));
        if (format_console_tagged_line(style, ConsoleTone::kSuccess, "STYLE", plain, pretty,
                                       output, sizeof(output))) {
            std::printf("%s\n", output);
        }
        return 0;
    }
    return print_usage("usage: console style [pretty|plain]");
}

int wifi_command(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
        return render_wifi_status();
    }
    if (argc == 2 && std::strcmp(argv[1], "start") == 0) {
        return print_result("wifi start", start_saved_network_wifi());
    }
    if (argc == 2 && std::strcmp(argv[1], "stop") == 0) {
        return print_result("wifi stop", stop_network_wifi());
    }
    if (argc == 2 && std::strcmp(argv[1], "forget") == 0) {
        return print_result("wifi forget", forget_network_wifi());
    }
    if (argc == 2 && std::strcmp(argv[1], "scan") == 0) {
        return print_result("wifi scan", scan_network_wifi());
    }
    if (argc == 3 && std::strcmp(argv[1], "connect") == 0) {
        const std::size_t ssid_length = std::strlen(argv[2]);
        if (ssid_length == 0 || ssid_length >= kWifiSsidCapacity) {
            return print_validation_error(
                "ERROR wifi SSID must contain 1..32 printable characters",
                "Wi-Fi SSID must contain 1..32 printable characters");
        }
        WifiCredentials credentials{};
        std::memcpy(credentials.ssid, argv[2], ssid_length + 1U);
        if (!read_wifi_password(&credentials) || !wifi_credentials_are_valid(credentials)) {
            std::memset(&credentials, 0, sizeof(credentials));
            return print_validation_error(
                "ERROR wifi password must be empty, 8..63 printable characters, or 64 hex digits",
                "Password must be empty, 8..63 printable characters, or 64 hex digits");
        }
        const esp_err_t error = connect_network_wifi(credentials);
        std::memset(&credentials, 0, sizeof(credentials));
        return print_result("wifi connect", error);
    }
    return print_usage("usage: wifi <status|connect <ssid>|start|stop|forget|scan>");
}

int ota_command(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
        return render_ota_status();
    }
    return print_usage("usage: ota status (updates are uploaded from the PC HTTP client)");
}

int web_command(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "auth") == 0 &&
        std::strcmp(argv[2], "status") == 0) {
        WebAuthStatus status{};
        const esp_err_t error = get_web_auth_status(&status);
        if (error != ESP_OK) {
            return print_result("web auth status", error);
        }
        char plain[128]{};
        char pretty[128]{};
        std::snprintf(plain, sizeof(plain),
                       "WEB_AUTH available=%u provisioned=%u generation=%lu failures=%lu blocked_ms=%lu",
                       status.available, status.provisioned,
                       static_cast<unsigned long>(status.generation),
                      static_cast<unsigned long>(status.failed_attempts),
                      static_cast<unsigned long>(status.blocked_ms));
        std::snprintf(pretty, sizeof(pretty), "Provisioned %s | blocked %lu ms",
                      status.provisioned ? "yes" : "no",
                      static_cast<unsigned long>(status.blocked_ms));
        print_tagged_line(ConsoleTone::kInfo, "WEB", plain, pretty, false);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "auth") == 0 &&
        std::strcmp(argv[2], "rotate") == 0) {
        WebAuthStatus previous{};
        const bool recovering =
            get_web_auth_status(&previous) == ESP_OK && !previous.available;
        char token[kWebAuthTokenLength + 1U]{};
        const esp_err_t error = rotate_web_auth_token(token, sizeof(token));
        if (error != ESP_OK) {
            return print_result("web auth rotate", error);
        }
        OutputGuard guard;
        if (!guard.locked()) {
            std::memset(token, 0, sizeof(token));
            return 1;
        }
        std::printf("\nWEB AUTH TOKEN %s\nStore this token; it will not be shown again.%s\n",
                    token, recovering ? " Reboot to restart Web and OTA services." : "");
        std::fflush(stdout);
        std::memset(token, 0, sizeof(token));
        return 0;
    }
    return print_usage("usage: web auth <status|rotate>");
}

int radio_command(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "info") == 0) {
        return render_radio_status(false);
    }
    if (argc == 2 && std::strcmp(argv[1], "reset") == 0) {
        return print_result("radio reset", reset_rf_radio());
    }
    if (argc == 2 && std::strcmp(argv[1], "start") == 0) {
        return print_result("radio start", bridge_control_start_radio());
    }
    return print_usage("usage: radio <info|reset|start>");
}

int last_command(int argc, char **)
{
    if (argc != 1) {
        return print_usage("usage: last");
    }
    RfFrame frame{};
    LearnedMatch learned{};
    const esp_err_t error = get_last_rf_frame_with_match(&frame, &learned);
    if (error != ESP_OK) {
        return print_result("last", error);
    }
    print_frame("LAST", frame, &learned);
    return 0;
}

int list_learned_names()
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::size_t count = 0;
        esp_err_t error = rf_storage_list(nullptr, 0, &count);
        if (error != ESP_OK) {
            return print_result("learn list", error);
        }
        if (count == 0) {
            if (current_console_style() == ConsoleStyle::kPlain) {
                std::printf("LEARNED_NAMES count=0\n");
            } else {
                print_dashboard_header("Learned signals");
                print_dashboard_value("Count", "0", ConsoleTone::kMuted);
                print_dashboard_footer();
            }
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
        if (current_console_style() == ConsoleStyle::kPlain) {
            std::printf("LEARNED_NAMES count=%zu\n", count);
            for (std::size_t index = 0; index < count; ++index) {
                std::printf("  %s\n", names[index].value);
            }
        } else {
            char count_text[24]{};
            std::snprintf(count_text, sizeof(count_text), "%zu", count);
            print_dashboard_header("Learned signals");
            print_dashboard_value("Count", count_text, ConsoleTone::kInfo);
            for (std::size_t index = 0; index < count; ++index) {
                print_dashboard_value("Signal", names[index].value, ConsoleTone::kInfo);
            }
            print_dashboard_footer();
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
        return print_usage("usage: learn <name>|list (name: letter then up to 14 letters/digits/_/-)");
    }
    const esp_err_t error =
        rf_signals_arm_learning(argv[1], BridgeEventSource::kUart);
    return error == ESP_OK ? 0 : print_result("learn", error);
}


int forget_command(int argc, char **argv)
{
    if (argc != 2 || !rf_storage_name_is_valid(argv[1])) {
        return print_usage("usage: forget <name>");
    }
    const esp_err_t error = bridge_control_forget_signal(argv[1]);
    if (error == ESP_ERR_INVALID_STATE) {
        return print_validation_error(
            "ERROR forget: learned name is referenced by an automation rule",
            "Cannot forget signal while an automation rule references it");
    }
    return print_result("forget", error);
}

int replay_command(int argc, char **argv)
{
    ReplayArguments arguments{};
    if (!parse_replay_arguments(argc, argv, CONFIG_RF_DEFAULT_TX_REPEATS, &arguments)) {
        return print_usage("usage: replay [repeats:1..20] | replay <name> <repeats:1..20>");
    }
    if (arguments.target == ReplayTarget::kRam) {
        return print_result("replay", bridge_control_replay_last(
                                          arguments.repeats, BridgeEventSource::kUart));
    }

    return print_result("replay named", bridge_control_replay_named(
                                              argv[1], arguments.repeats,
                                              BridgeEventSource::kUart));
}


int list_rules()
{
    OutputGuard guard;
    if (!guard.locked()) {
        return 1;
    }
    RfAutomationStatus status{};
    const esp_err_t status_error = rf_automation_get_status(&status);
    if (status_error != ESP_OK) {
        return print_result("rule list", status_error);
    }
    std::size_t count = 0;
    esp_err_t error = rf_automation_list_rule_info(nullptr, 0, &count);
    if (error != ESP_OK) {
        return print_result("rule list", error);
    }
    std::unique_ptr<RfAutomationRuleInfo[]> rules;
    if (count > 0) {
        rules.reset(new (std::nothrow) RfAutomationRuleInfo[count]);
        if (!rules) {
            return print_result("rule list", ESP_ERR_NO_MEM);
        }
        error = rf_automation_list_rule_info(rules.get(), count, &count);
        if (error != ESP_OK) {
            return print_result("rule list", error);
        }
    }
    if (current_console_style() == ConsoleStyle::kPlain) {
        std::printf("RULES automation_available=%u enabled_known=%u enabled=%u log_mode_known=%u log_mode=%s count=%zu%s\n",
                    status.available, status.enabled_known, status.enabled, status.log_mode_known,
                    status.log_mode_known ? rf_automation_log_mode_name(status.log_mode) : "unknown",
                    count, status.available ? "" : " recovery=reboot_after_changes");
        for (std::size_t index = 0; index < count; ++index) {
            if (rules[index].validation_error != ESP_OK) {
                std::printf("  %s INVALID error=%s (0x%x)\n", rules[index].entry.trigger_name,
                            esp_err_to_name(rules[index].validation_error),
                            static_cast<unsigned>(rules[index].validation_error));
                continue;
            }
            std::printf("  %s -> %s repeats=%u cooldown_ms=%lu\n",
                        rules[index].entry.trigger_name, rules[index].entry.rule.target_name,
                        rules[index].entry.rule.repeats,
                        static_cast<unsigned long>(rules[index].entry.rule.cooldown_ms));
        }
        return 0;
    }

    char value[64]{};
    print_dashboard_header("Automation rules");
    if (!status.available) {
        print_dashboard_value("State", "UNAVAILABLE", ConsoleTone::kError);
        std::snprintf(value, sizeof(value), "%s (0x%x)",
                      esp_err_to_name(status.initialization_error),
                      static_cast<unsigned>(status.initialization_error));
        print_dashboard_value("Error", value, ConsoleTone::kError);
    }
    if (status.rule_count_known) {
        std::snprintf(value, sizeof(value), "%u", status.rule_count);
    } else {
        std::snprintf(value, sizeof(value), "unknown (listed %zu)", count);
    }
    print_dashboard_row("Enabled",
                        status.enabled_known ? (status.enabled ? "yes" : "no") : "unknown",
                        !status.enabled_known
                            ? ConsoleTone::kWarning
                            : (status.enabled ? ConsoleTone::kSuccess : ConsoleTone::kWarning),
                        "Rule count", value,
                        status.rule_count_known ? ConsoleTone::kInfo : ConsoleTone::kWarning);
    print_dashboard_value("Log mode",
                          status.log_mode_known ? rf_automation_log_mode_name(status.log_mode)
                                                : "unknown",
                          status.log_mode_known ? ConsoleTone::kInfo : ConsoleTone::kWarning);
    for (std::size_t index = 0; index < count; ++index) {
        if (rules[index].validation_error != ESP_OK) {
            print_dashboard_value("Rule", rules[index].entry.trigger_name, ConsoleTone::kError);
            std::snprintf(value, sizeof(value), "%s (0x%x)",
                          esp_err_to_name(rules[index].validation_error),
                          static_cast<unsigned>(rules[index].validation_error));
            print_dashboard_value("Error", value, ConsoleTone::kError);
            continue;
        }
        std::snprintf(value, sizeof(value), "%s -> %s | x%u | %lums",
                      rules[index].entry.trigger_name, rules[index].entry.rule.target_name,
                      rules[index].entry.rule.repeats,
                      static_cast<unsigned long>(rules[index].entry.rule.cooldown_ms));
        print_dashboard_value("Rule", value, ConsoleTone::kAction);
    }
    if (!status.available) {
        print_dashboard_value("Recovery", "reboot required after rule changes",
                              ConsoleTone::kWarning);
    }
    print_dashboard_footer();
    return 0;
}

int print_rule_mutation_result(const char *operation, esp_err_t error)
{
    if (error != ESP_OK) {
        return print_result(operation, error);
    }
    RfAutomationStatus status{};
    if (rf_automation_get_status(&status) == ESP_OK && !status.available) {
        char plain[160]{};
        char pretty[160]{};
        std::snprintf(plain, sizeof(plain),
                      "OK %s; reboot required to retry automation startup", operation);
        std::snprintf(pretty, sizeof(pretty), "%s saved | reboot required to restart automation",
                      operation);
        print_tagged_line(ConsoleTone::kWarning, "RULE", plain, pretty, false);
        return 0;
    }
    return print_result(operation, ESP_OK);
}


int show_rule_log_mode()
{
    RfAutomationStatus status{};
    const esp_err_t error = rf_automation_get_status(&status);
    if (error != ESP_OK) {
        return print_result("rule log", error);
    }
    char plain[96]{};
    char pretty[96]{};
    if (!status.log_mode_known) {
        std::snprintf(plain, sizeof(plain), "RULE_LOG mode=unknown automation_available=%u",
                      status.available);
        std::snprintf(pretty, sizeof(pretty), "Log mode unknown | automation %s",
                      status.available ? "available" : "unavailable");
        print_tagged_line(ConsoleTone::kWarning, "RULE", plain, pretty, false);
        return 1;
    }
    std::snprintf(plain, sizeof(plain), "RULE_LOG mode=%s automation_available=%u",
                  rf_automation_log_mode_name(status.log_mode), status.available);
    std::snprintf(pretty, sizeof(pretty), "Log mode %s | automation %s",
                  rf_automation_log_mode_name(status.log_mode),
                  status.available ? "available" : "unavailable");
    print_tagged_line(ConsoleTone::kInfo, "RULE", plain, pretty, false);
    return 0;
}

int rule_command(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "list") == 0) {
        return list_rules();
    }
    if (argc == 2 && std::strcmp(argv[1], "enable") == 0) {
        return print_rule_mutation_result("rule enable", rf_automation_set_enabled(true));
    }
    if (argc == 2 && std::strcmp(argv[1], "disable") == 0) {
        return print_rule_mutation_result("rule disable", rf_automation_set_enabled(false));
    }
    if (argc == 2 && std::strcmp(argv[1], "log") == 0) {
        return show_rule_log_mode();
    }
    if (argc == 3 && std::strcmp(argv[1], "log") == 0) {
        RfAutomationLogMode mode{};
        if (!parse_rule_log_mode(argv[2], &mode)) {
            return print_usage("usage: rule log <off|actions|verbose>");
        }
        return print_rule_mutation_result("rule log", rf_automation_set_log_mode(mode));
    }
    if (argc == 3 && std::strcmp(argv[1], "remove") == 0) {
        return print_rule_mutation_result("rule remove", rf_automation_remove_rule(argv[2]));
    }
    uint8_t repeats = 0;
    if ((argc == 4 || argc == 5) && parse_rule_add_arguments(argc, argv, CONFIG_RF_DEFAULT_TX_REPEATS, &repeats)) {
        return print_result("rule add", rf_automation_add_rule(argv[2], argv[3], repeats));
    }
    return print_usage("usage: rule <add <received_name> <transmit_name> [repeats]|list|remove <received_name>|enable|disable|log [off|actions|verbose]>");
}

int send_value_command(int argc, char **argv)
{
    if (argc < 4 || argc > 6) {
        return print_usage("usage: send <code> <bits> <protocol> [pulse_us] [repeats]");
    }
    uint64_t code = 0;
    uint64_t bits = 0;
    uint64_t protocol_number = 0;
    if (!parse_unsigned_value(argv[1], UINT64_MAX, &code) || !parse_bounded(argv[2], 4, 64, &bits) ||
        !parse_bounded(argv[3], 1, kRfProtocolCount, &protocol_number)) {
        char plain[128]{};
        std::snprintf(plain, sizeof(plain),
                      "ERROR code must be decimal/0x; bits 4..64; protocol 1..%zu",
                      kRfProtocolCount);
        return print_validation_error(plain,
                                      "Code must be decimal/0x; bits 4..64; protocol 1..12");
    }
    if (bits < 64 && (code >> bits) != 0) {
        return print_validation_error("ERROR code does not fit in the requested bit count",
                                      "Code does not fit the requested bit count");
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
        return print_validation_error(
            "ERROR pulse_us is outside the selected protocol's transport limits",
            "Pulse width is outside the selected protocol's transport limits");
    }
    if (argc == 6 && !parse_bounded(argv[5], 1, 20, &repeats)) {
        return print_validation_error("ERROR repeats must be 1..20",
                                      "Repeat count must be 1..20");
    }
    DecodedTransmitRequest request{};
    request.code = code;
    request.pulse_us = static_cast<uint16_t>(pulse_us);
    request.bits = static_cast<uint8_t>(bits);
    request.protocol = static_cast<uint8_t>(protocol_number);
    request.repeats = static_cast<uint16_t>(repeats);
    return print_result("send", bridge_control_transmit_decoded(
                                    request, BridgeEventSource::kUart));
}

void print_staged_raw()
{
    OutputGuard guard;
    if (!guard.locked()) {
        return;
    }
    char line[kOutputLineSize]{};
    std::size_t length = 0;
    bool formatted = false;
    if (!s_raw_staging) {
        formatted = append_to_line(line, sizeof(line), &length, "empty");
    } else {
        formatted = append_to_line(line, sizeof(line), &length,
                                   "start=%u count=%u durations=", s_staged_raw.start_level,
                                   s_staged_raw.count);
        for (std::size_t index = 0; formatted && index < s_staged_raw.count; ++index) {
            formatted = append_to_line(line, sizeof(line), &length, "%s%u", index == 0 ? "" : ",",
                                       s_staged_raw.durations_us[index]);
        }
    }
    if (!formatted) {
        ESP_LOGE(kTag, "Could not format staged raw output without truncation");
        return;
    }
    if (current_console_style() == ConsoleStyle::kPretty) {
        print_tagged_line(ConsoleTone::kAction, "RAW", "", line, false);
    } else if (decorate_console_message(ConsoleStyle::kPlain, ConsoleTone::kAction, "RAW",
                                         "RAW_STAGE ", line, sizeof(line))) {
        std::printf("%s\n", line);
    } else {
        ESP_LOGE(kTag, "Could not format staged raw prefix");
    }
}

int raw_command(int argc, char **argv)
{
    if (argc < 2) {
        return print_usage("usage: raw <begin|append|show|send|clear> ...");
    }
    if (std::strcmp(argv[1], "clear") == 0 && argc == 2) {
        s_staged_raw = {};
        s_raw_staging = false;
        return print_result("raw clear", ESP_OK);
    }
    if (std::strcmp(argv[1], "begin") == 0) {
        uint64_t start_level = 0;
        if (argc != 3 || !parse_bounded(argv[2], 0, 1, &start_level)) {
            return print_usage("usage: raw begin <start_level:0|1>");
        }
        s_staged_raw = {};
        s_staged_raw.start_level = static_cast<uint8_t>(start_level);
        s_raw_staging = true;
        return print_result("raw begin", ESP_OK);
    }
    if (std::strcmp(argv[1], "append") == 0) {
        if (!s_raw_staging || argc < 3) {
            return print_usage("usage: raw begin <0|1>, then raw append <duration_us>...");
        }
        const std::size_t additions = static_cast<std::size_t>(argc - 2);
        if (static_cast<std::size_t>(s_staged_raw.count) + additions > kMaxRawPulses) {
            char plain[64]{};
            std::snprintf(plain, sizeof(plain), "ERROR raw pulse limit is %zu", kMaxRawPulses);
            return print_validation_error(plain, "Raw pulse limit is 256");
        }
        uint16_t parsed[kMaxRawPulses]{};
        for (std::size_t index = 0; index < additions; ++index) {
            uint64_t duration = 0;
            if (!parse_bounded(argv[index + 2U], kMinimumRawPulseUs, kMaximumPulseDurationUs, &duration)) {
                return print_validation_error("ERROR every duration must be 100..29000 us",
                                              "Every duration must be 100..29000 us");
            }
            parsed[index] = static_cast<uint16_t>(duration);
        }
        for (std::size_t index = 0; index < additions; ++index) {
            s_staged_raw.durations_us[s_staged_raw.count++] = parsed[index];
        }
        char plain[64]{};
        char pretty[64]{};
        std::snprintf(plain, sizeof(plain), "OK raw append count=%u", s_staged_raw.count);
        std::snprintf(pretty, sizeof(pretty), "raw append completed | %u pulse(s)",
                      s_staged_raw.count);
        print_tagged_line(ConsoleTone::kSuccess, " OK ", plain, pretty, false);
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
            return print_usage("usage: raw send [repeats]");
        }
        if (!raw_signal_is_valid(s_staged_raw)) {
            char plain[112]{};
            std::snprintf(plain, sizeof(plain),
                          "ERROR raw frame needs 8..%zu alternating pulses and an even count",
                          kMaxRawPulses);
            return print_validation_error(
                plain, "Raw frame needs 8..256 alternating pulses and an even count");
        }
        RawTransmitRequest request{};
        request.signal = s_staged_raw;
        request.repeats = static_cast<uint16_t>(repeats);
        return print_result("raw send", bridge_control_transmit_raw(
                                            request, BridgeEventSource::kUart));
    }
    return print_usage("usage: raw <begin|append|show|send|clear> ...");
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
    {"status", "Show the complete system dashboard", nullptr, status_command},
    {"console", "Select colored or machine-readable output", "style [pretty|plain]", console_command},
    {"wifi", "Control optional DHCP Wi-Fi", "<status|connect <ssid>|start|stop|forget|scan>", wifi_command},
    {"ota", "Show LAN OTA service diagnostics", "<status>", ota_command},
    {"web", "Inspect or rotate Web UI authentication", "auth <status|rotate>", web_command},
    {"radio", "Start, show, or reset the CC1101", "<start|info|reset>", radio_command},
    {"last", "Print the latest RAM frame", nullptr, last_command},
    {"learn", "Learn the next accepted frame or list names", "<name>|list", learn_command},
    {"forget", "Delete one learned NVS frame", "<name>", forget_command},
    {"rule", "Configure persistent receive-to-replay automation",
     "<add <rx> <tx> [repeats]|list|remove <rx>|enable|disable|"
     "log [off|actions|verbose]>", rule_command},
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

char *console_hint(const char *line, int *color, int *bold)
{
    return const_cast<char *>(esp_console_get_hint(line, color, bold));
}

esp_err_t initialize_uart_console()
{
    std::fflush(stdout);
    fsync(fileno(stdout));

    const esp_console_dev_uart_config_t device_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    const uart_port_t uart_port = static_cast<uart_port_t>(device_config.channel);
    if (uart_vfs_dev_port_set_rx_line_endings(device_config.channel, ESP_LINE_ENDINGS_CR) != 0 ||
        uart_vfs_dev_port_set_tx_line_endings(device_config.channel, ESP_LINE_ENDINGS_CRLF) != 0) {
        return ESP_FAIL;
    }

    uart_sclk_t clock_source = UART_SCLK_DEFAULT;
#if SOC_UART_SUPPORT_REF_TICK
    clock_source = UART_SCLK_REF_TICK;
#elif SOC_UART_SUPPORT_XTAL_CLK
    clock_source = UART_SCLK_XTAL;
#endif
    uart_config_t uart_config{};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = clock_source;

    esp_err_t error = uart_param_config(uart_port, &uart_config);
    if (error != ESP_OK) {
        return error;
    }
    error = uart_set_pin(uart_port, device_config.tx_gpio_num,
                         device_config.rx_gpio_num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error != ESP_OK) {
        return error;
    }
    error = uart_driver_install(uart_port, 256, 0, 0, nullptr, 0);
    if (error != ESP_OK) {
        return error;
    }
    s_uart_driver_installed = true;
    uart_vfs_dev_use_driver(device_config.channel);
    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);
    setvbuf(stdin, nullptr, _IONBF, 0);

    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    console_config.max_cmdline_length = 2048;
    console_config.max_cmdline_args = kMaxRawPulses + 4U;
#if CONFIG_LOG_COLORS
    console_config.hint_color = 36;
#else
    console_config.hint_color = -1;
#endif
    error = esp_console_init(&console_config);
    if (error != ESP_OK) {
        return error;
    }
    s_console_initialized = true;
    error = esp_console_register_help_command();
    if (error != ESP_OK) {
        return error;
    }
    s_help_registered = true;

    rf_linenoiseSetSyncCallbacks(editor_lock, editor_unlock, nullptr);
    rf_linenoiseSetMultiLine(1);
    rf_linenoiseSetCompletionCallback(&esp_console_get_completion);
    rf_linenoiseSetHintsCallback(&console_hint);
    rf_linenoiseSetFreeHintsCallback(nullptr);
    if (rf_linenoiseSetMaxLineLen(2048) != 0 || rf_linenoiseHistorySetMaxLen(32) != 1) {
        return ESP_ERR_NO_MEM;
    }

    std::snprintf(s_repl_prompt, sizeof(s_repl_prompt), LOG_COLOR_I "rf> " LOG_RESET_COLOR);
    rf_linenoiseSetDumbMode(0);
    if (rf_linenoiseProbe() != 0) {
        rf_linenoiseSetDumbMode(1);
        std::snprintf(s_repl_prompt, sizeof(s_repl_prompt), "rf> ");
    }
    return ESP_OK;
}

bool is_help_command(const char *line)
{
    while (*line != '\0' && std::isspace(static_cast<unsigned char>(*line))) {
        ++line;
    }
    return std::strncmp(line, "help", 4) == 0 &&
           (line[4] == '\0' || std::isspace(static_cast<unsigned char>(line[4])));
}

void repl_task(void *)
{
    {
        OutputGuard guard;
        std::printf("\r\n"
                    "Type 'help' to get the list of commands.\r\n"
                    "Use UP/DOWN arrows to navigate through command history.\r\n"
                    "Press TAB when typing command name to auto-complete.\r\n");
        if (rf_linenoiseIsDumbMode()) {
            std::printf("\r\n"
                        "Your terminal application does not support escape sequences.\n\n"
                        "Line editing and history features are disabled.\r\n");
        }
    }

    while (true) {
        char *line = rf_linenoise(s_repl_prompt);
        if (line == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        rf_linenoiseHistoryAdd(line);

        int command_result = 0;
        esp_err_t error = ESP_OK;
        if (is_help_command(line)) {
            OutputGuard guard;
            error = esp_console_run(line, &command_result);
        } else {
            error = esp_console_run(line, &command_result);
        }
        if (error != ESP_ERR_INVALID_ARG &&
            (error != ESP_OK || command_result != ESP_OK)) {
            OutputGuard guard;
            if (error == ESP_ERR_NOT_FOUND) {
                std::printf("Unrecognized command\n");
            } else if (error == ESP_OK) {
                std::printf("Command returned non-zero error code: 0x%x (%s)\n",
                            command_result, esp_err_to_name(command_result));
            } else {
                std::printf("Internal error: %s\n", esp_err_to_name(error));
            }
        }
        rf_linenoiseFree(line);
    }
}

esp_err_t start_uart_console_task()
{
    if (xTaskCreatePinnedToCore(repl_task, "console_repl", 8192, nullptr, 2,
                                &s_repl_task, tskNO_AFFINITY) != pdTRUE) {
        s_repl_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void deinitialize_uart_console()
{
    if (s_repl_task != nullptr) {
        vTaskDelete(s_repl_task);
        s_repl_task = nullptr;
    }
    rf_linenoiseSetSyncCallbacks(nullptr, nullptr, nullptr);
    rf_linenoiseSetCompletionCallback(nullptr);
    rf_linenoiseSetHintsCallback(nullptr);
    rf_linenoiseHistoryFree();
    if (s_help_registered) {
        esp_console_deregister_help_command();
        s_help_registered = false;
    }
    if (s_console_initialized) {
        esp_console_deinit();
        s_console_initialized = false;
    }
    if (s_uart_driver_installed) {
        const esp_console_dev_uart_config_t device_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
        uart_vfs_dev_use_nonblocking(device_config.channel);
        uart_driver_delete(static_cast<uart_port_t>(device_config.channel));
        s_uart_driver_installed = false;
    }
    s_repl_prompt[0] = '\0';
}

void cleanup_failed_start(std::size_t registered_count)
{
    esp_err_t sink_error = ESP_OK;
    if (s_bridge_sink_registered) {
        sink_error = bridge_events_remove_sink(s_bridge_sink_id);
        if (sink_error == ESP_OK) {
            s_bridge_sink_registered = false;
        }
    }
    const bool safe_to_delete_events = sink_error == ESP_OK;
    if (sink_error != ESP_OK) {
        ESP_LOGE(kTag, "Could not detach shared event sink: %s; retaining event queues",
                 esp_err_to_name(sink_error));
    }

    if (safe_to_delete_events && s_event_worker_task != nullptr) {
        vTaskDelete(s_event_worker_task);
        s_event_worker_task = nullptr;
    }
    if (safe_to_delete_events && s_event_queue != nullptr) {
        vQueueDelete(s_event_queue);
        s_event_queue = nullptr;
    }
    if (safe_to_delete_events && s_automation_log_queue != nullptr) {
        vQueueDelete(s_automation_log_queue);
        s_automation_log_queue = nullptr;
    }
    if (safe_to_delete_events && s_network_event_queue != nullptr) {
        vQueueDelete(s_network_event_queue);
        s_network_event_queue = nullptr;
    }
    if (safe_to_delete_events && s_ota_event_queue != nullptr) {
        vQueueDelete(s_ota_event_queue);
        s_ota_event_queue = nullptr;
    }
    if (safe_to_delete_events && s_bridge_event_queue != nullptr) {
        vQueueDelete(s_bridge_event_queue);
        s_bridge_event_queue = nullptr;
    }

    if (s_repl_task != nullptr) {
        vTaskDelete(s_repl_task);
        s_repl_task = nullptr;
    }
    deregister_commands(registered_count);
    if (s_log_hook_installed) {
        esp_log_set_vprintf(s_previous_log_vprintf);
        s_previous_log_vprintf = nullptr;
        s_log_hook_installed = false;
    }
    s_log_output_owner.store(nullptr, std::memory_order_release);
    s_log_output_fragments = 0;
    deinitialize_uart_console();
    if (safe_to_delete_events && s_output_mutex != nullptr) {
        s_output_depth = 0;
        vSemaphoreDelete(s_output_mutex);
        s_output_mutex = nullptr;
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
    if (s_started.load(std::memory_order_acquire) || s_repl_task != nullptr ||
        s_uart_driver_installed || s_console_initialized || s_help_registered ||
        s_log_hook_installed || s_event_queue != nullptr ||
        s_automation_log_queue != nullptr || s_network_event_queue != nullptr ||
        s_ota_event_queue != nullptr || s_bridge_event_queue != nullptr ||
        s_output_mutex != nullptr || s_event_worker_task != nullptr) {
        s_starting.clear(std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }

    s_output_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_output_mutex == nullptr) {
        return fail_start(ESP_ERR_NO_MEM, 0);
    }

    esp_err_t error = initialize_uart_console();
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not initialize UART console: %s", esp_err_to_name(error));
        return fail_start(error, 0);
    }

    std::size_t registered_count = 0;
    error = register_commands(&registered_count);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not register commands: %s", esp_err_to_name(error));
        return fail_start(error, registered_count);
    }

    s_event_queue = xQueueCreate(kFrameEventQueueDepth, sizeof(ConsoleEvent));
    s_automation_log_queue = xQueueCreate(kAutomationLogQueueDepth, sizeof(RfAutomationEvent));
    s_network_event_queue = xQueueCreate(kNetworkEventQueueDepth, sizeof(NetworkWifiEvent));
    s_ota_event_queue = xQueueCreate(kOtaEventQueueDepth, sizeof(ConsoleOtaEvent));
    s_bridge_event_queue = xQueueCreate(kBridgeEventQueueDepth, sizeof(ConsoleSystemEvent));
    if (s_event_queue == nullptr || s_automation_log_queue == nullptr ||
        s_network_event_queue == nullptr || s_ota_event_queue == nullptr ||
        s_bridge_event_queue == nullptr) {
        return fail_start(ESP_ERR_NO_MEM, registered_count);
    }
    s_frame_queue_drops.store(0, std::memory_order_relaxed);
    s_ota_series_generation.store(0, std::memory_order_relaxed);
    s_ota_progress_series_active = false;
    s_rendered_ota_series_generation = 0;
    if (xTaskCreate(event_worker_task, "rf_events", kEventWorkerTaskStackSize, nullptr, 3,
                    &s_event_worker_task) != pdPASS) {
        return fail_start(ESP_ERR_NO_MEM, registered_count);
    }
    error = bridge_events_add_sink(enqueue_bridge_event, nullptr, &s_bridge_sink_id);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not register shared event sink: %s", esp_err_to_name(error));
        return fail_start(error, registered_count);
    }
    s_bridge_sink_registered = true;
    s_previous_log_vprintf = esp_log_set_vprintf(coordinated_log_vprintf);
    s_log_hook_installed = true;

    {
        OutputGuard output_guard;
        if (current_console_style() == ConsoleStyle::kPretty) {
            std::printf("\n");
            print_dashboard_header("ESP32 + CC1101 RF Bridge");
            print_dashboard_row("Console", "READY", ConsoleTone::kSuccess, "Style", "pretty",
                                ConsoleTone::kInfo);
            print_dashboard_value("Hint", "Type help to list commands", ConsoleTone::kMuted);
            print_dashboard_footer();
        } else {
            std::printf("\nNative ESP32 + CC1101 RF console ready. Type 'help'.\n");
        }
    }
    error = start_uart_console_task();
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not start UART REPL: %s", esp_err_to_name(error));
        return fail_start(error, registered_count);
    }

    s_started.store(true, std::memory_order_release);
    s_starting.clear(std::memory_order_release);
    return ESP_OK;
}

ConsoleStyle get_rf_console_style()
{
    return current_console_style();
}

esp_err_t set_rf_console_style(ConsoleStyle style)
{
    if (style != ConsoleStyle::kPretty && style != ConsoleStyle::kPlain) {
        return ESP_ERR_INVALID_ARG;
    }
    s_console_style.store(style, std::memory_order_release);
    return ESP_OK;
}

void rf_console_on_frame(const RfFrame &frame, void *context)
{
    rf_signals_on_frame(frame, context);
}


}  // namespace rfbridge
