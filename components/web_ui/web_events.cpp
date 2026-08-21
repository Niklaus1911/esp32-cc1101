#include "web_events.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bridge_events.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "rf_automation_event.hpp"
#include "rf_ook.hpp"
#include "sdkconfig.h"
#include "web_events_format.hpp"

#ifndef CONFIG_WEB_SSE_ENABLE
#define CONFIG_WEB_SSE_ENABLE 0
#endif

namespace rfbridge {
namespace {

constexpr char kTag[] = "web_events";
constexpr UBaseType_t kEventQueueDepth = 8;
constexpr uint32_t kWorkerStackSize = 6144;
constexpr UBaseType_t kWorkerPriority = 4;
constexpr TickType_t kHeartbeatInterval = pdMS_TO_TICKS(15000);
constexpr TickType_t kQueueWait = pdMS_TO_TICKS(1000);
constexpr int kStopWaitAttempts = 80;
constexpr TickType_t kStopWaitDelay = pdMS_TO_TICKS(25);
constexpr std::size_t kNameCapacity = kRfStorageNameCapacity;
constexpr std::size_t kSseBufferCapacity = 1536;

struct WebSseEvent {
    uint64_t sequence = 0;
    int64_t occurred_us = 0;
    BridgeEventType type = BridgeEventType::kRx;
    esp_err_t result = ESP_OK;
    uint32_t operation_id = 0;
    uint32_t value = 0;
    uint32_t total = 0;
    uint16_t repeats = 0;
    RfEncoding encoding = RfEncoding::kDecoded;
    uint64_t decoded_code = 0;
    uint16_t decoded_bits = 0;
    uint16_t decoded_protocol = 0;
    uint16_t decoded_pulse_us = 0;
    uint16_t raw_pulses = 0;
    uint8_t raw_start_level = 0;
    uint32_t fingerprint = 0;
    LearnedMatchKind match_kind = LearnedMatchKind::kNone;
    uint16_t match_count = 0;
    RfAutomationEventType automation_type = RfAutomationEventType::kTriggered;
    RfAutomationConfigChange config_change = RfAutomationConfigChange::kRuleAdded;
    uint32_t action_id = 0;
    uint32_t elapsed_ms = 0;
    uint32_t configuration_revision = 0;
    bool enabled = false;
    RfAutomationLogMode log_mode = RfAutomationLogMode::kActions;
    char name[kNameCapacity]{};
    char target[kNameCapacity]{};
    char match_name[kNameCapacity]{};
};

QueueHandle_t s_event_queue = nullptr;
httpd_handle_t s_server = nullptr;
std::atomic<bool> s_started{false};
std::atomic<bool> s_stopping{false};
std::atomic<bool> s_client_active{false};
std::atomic<bool> s_sink_registered{false};
std::atomic<bool> s_resync_pending{false};
std::atomic<int> s_client_socket{-1};
uint8_t s_sink_id = 0;
uint32_t s_last_queue_drops = 0;
uint32_t s_last_sink_drops = 0;

void copy_text(char *destination, const char *source)
{
    if (destination == nullptr) {
        return;
    }
    std::strncpy(destination, source == nullptr ? "" : source, kNameCapacity - 1U);
    destination[kNameCapacity - 1U] = '\0';
}

WebSseEvent make_event(const BridgeEvent &event)
{
    WebSseEvent result{};
    result.sequence = event.sequence;
    result.occurred_us = event.occurred_us;
    result.type = event.type;
    result.result = event.result;
    result.operation_id = event.operation_id;
    result.value = event.value;
    result.total = event.total;
    result.repeats = event.repeats;
    switch (event.type) {
        case BridgeEventType::kRx: {
            const RfFrame &frame = event.payload.rf.frame;
            result.encoding = frame.encoding;
            result.repeats = frame.observed_repeats;
            result.fingerprint = frame.fingerprint;
            if (frame.encoding == RfEncoding::kDecoded) {
                result.decoded_code = frame.decoded.code;
                result.decoded_bits = frame.decoded.bits;
                result.decoded_protocol = frame.decoded.protocol;
                result.decoded_pulse_us = frame.decoded.pulse_us;
            } else {
                result.raw_pulses = frame.raw.count;
                result.raw_start_level = frame.raw.start_level;
            }
            result.match_kind = event.payload.rf.learned.kind;
            result.match_count = event.payload.rf.learned.count;
            copy_text(result.match_name, event.payload.rf.learned.name);
            break;
        }
        case BridgeEventType::kAutomation:
            result.automation_type = event.payload.automation.type;
            result.action_id = event.payload.automation.action_id;
            result.elapsed_ms = event.payload.automation.elapsed_ms;
            result.value = event.payload.automation.value;
            result.repeats = event.payload.automation.repeats;
            result.result = static_cast<esp_err_t>(event.payload.automation.result);
            copy_text(result.name, event.payload.automation.trigger_name);
            copy_text(result.target, event.payload.automation.target_name);
            break;
        case BridgeEventType::kAutomationConfig:
            result.config_change = event.payload.automation_config.change;
            result.configuration_revision = event.payload.automation_config.configuration_revision;
            result.enabled = event.payload.automation_config.enabled;
            result.log_mode = event.payload.automation_config.log_mode;
            copy_text(result.name, event.payload.automation_config.trigger_name);
            copy_text(result.target, event.payload.automation_config.target_name);
            break;
        case BridgeEventType::kSignalCatalogChanged:
        case BridgeEventType::kLearnArmed:
        case BridgeEventType::kLearnReplaced:
        case BridgeEventType::kLearnCompleted:
        case BridgeEventType::kLearnCancelled:
        case BridgeEventType::kLearnTimeout:
        case BridgeEventType::kLearnFailed:
        case BridgeEventType::kTxStarted:
        case BridgeEventType::kTxCompleted:
            copy_text(result.name, event.payload.rf.name);
            copy_text(result.target, event.payload.rf.target);
            break;
        default:
            break;
    }
    return result;
}

bool web_event_sink(const BridgeEvent &event, void *)
{
#if CONFIG_WEB_SSE_ENABLE
    if (!s_started.load(std::memory_order_acquire) ||
        !s_client_active.load(std::memory_order_acquire) || s_event_queue == nullptr) {
        return true;
    }
    const WebSseEvent bounded = make_event(event);
    if (xQueueSend(s_event_queue, &bounded, 0) != pdTRUE) {
        s_resync_pending.store(true, std::memory_order_release);
        return false;
    }
    return true;
#else
    (void)event;
    return true;
#endif
}

const char *event_name(BridgeEventType type)
{
    switch (type) {
        case BridgeEventType::kRx: return "rx";
        case BridgeEventType::kAutomation: return "automation";
        case BridgeEventType::kAutomationConfig: return "automation_config";
        case BridgeEventType::kSignalCatalogChanged: return "catalog";
        case BridgeEventType::kLearnArmed:
        case BridgeEventType::kLearnReplaced:
        case BridgeEventType::kLearnCompleted:
        case BridgeEventType::kLearnCancelled:
        case BridgeEventType::kLearnTimeout:
        case BridgeEventType::kLearnFailed: return "learning";
        case BridgeEventType::kNetwork: return "network";
        case BridgeEventType::kTxStarted:
        case BridgeEventType::kTxCompleted: return "tx";
        case BridgeEventType::kHardwareSwitch: return "hardware";
        case BridgeEventType::kOta: return "ota";
        default: return "system";
    }
}

const char *automation_event_name(RfAutomationEventType type)
{
    switch (type) {
        case RfAutomationEventType::kTriggered: return "triggered";
        case RfAutomationEventType::kActionCompleted: return "completed";
        case RfAutomationEventType::kCooldownSuppressed: return "cooldown";
        case RfAutomationEventType::kAmbiguousFrame: return "ambiguous";
        case RfAutomationEventType::kStaleFrame: return "stale";
        case RfAutomationEventType::kQueueDrop: return "queue_drop";
    }
    return "unknown";
}

const char *config_change_name(RfAutomationConfigChange change)
{
    switch (change) {
        case RfAutomationConfigChange::kRuleAdded: return "rule_added";
        case RfAutomationConfigChange::kRuleRemoved: return "rule_removed";
        case RfAutomationConfigChange::kEnabled: return "enabled";
        case RfAutomationConfigChange::kLogMode: return "log_mode";
    }
    return "unknown";
}

const char *log_mode_name(RfAutomationLogMode mode)
{
    switch (mode) {
        case RfAutomationLogMode::kOff: return "off";
        case RfAutomationLogMode::kActions: return "actions";
        case RfAutomationLogMode::kVerbose: return "verbose";
    }
    return "unknown";
}

const char *match_name(LearnedMatchKind kind)
{
    switch (kind) {
        case LearnedMatchKind::kNone: return "none";
        case LearnedMatchKind::kUnique: return "unique";
        case LearnedMatchKind::kAmbiguous: return "ambiguous";
        case LearnedMatchKind::kUnavailable: return "unavailable";
    }
    return "unknown";
}

bool append_format(char *output, std::size_t capacity, std::size_t *length, const char *format, ...)
{
    if (output == nullptr || length == nullptr || *length >= capacity) {
        return false;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(output + *length, capacity - *length, format, arguments);
    va_end(arguments);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity - *length) {
        return false;
    }
    *length += static_cast<std::size_t>(written);
    return true;
}

bool append_json_string(char *output, std::size_t capacity, std::size_t *length, const char *value)
{
    if (!append_format(output, capacity, length, "\"")) {
        return false;
    }
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(value == nullptr ? "" : value);
         *cursor != '\0'; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') {
            if (!append_format(output, capacity, length, "\\%c", *cursor)) {
                return false;
            }
        } else if (*cursor == '\n' || *cursor == '\r' || *cursor == '\t') {
            const char escaped = *cursor == '\n' ? 'n' : (*cursor == '\r' ? 'r' : 't');
            if (!append_format(output, capacity, length, "\\%c", escaped)) {
                return false;
            }
        } else if (*cursor < 0x20U) {
            if (!append_format(output, capacity, length, "\\u%04x", static_cast<unsigned>(*cursor))) {
                return false;
            }
        } else if (!append_format(output, capacity, length, "%c", *cursor)) {
            return false;
        }
    }
    return append_format(output, capacity, length, "\"");
}

bool serialize_event(const WebSseEvent &event, char *output, std::size_t capacity)
{
    char *framed_output = output;
    const std::size_t framed_capacity = capacity;
    char payload[kSseBufferCapacity - 128U]{};
    output = payload;
    capacity = sizeof(payload);
    std::size_t length = 0;
    if (!append_format(output, capacity, &length, "{\"sequence\":%llu",
                       static_cast<unsigned long long>(event.sequence))) {
        return false;
    }
    switch (event.type) {
        case BridgeEventType::kRx:
            if (!append_format(output, capacity, &length, ",\"encoding\":\"%s\"",
                               event.encoding == RfEncoding::kDecoded ? "decoded" : "raw")) {
                return false;
            }
            if (event.encoding == RfEncoding::kDecoded &&
                !append_format(output, capacity, &length,
                               ",\"code\":\"0x%llX\",\"bits\":%u,\"protocol\":%u,\"pulse_us\":%u",
                               static_cast<unsigned long long>(event.decoded_code), event.decoded_bits,
                               event.decoded_protocol, event.decoded_pulse_us)) {
                return false;
            }
            if (event.encoding == RfEncoding::kRaw &&
                !append_format(output, capacity, &length,
                               ",\"pulses\":%u,\"start_level\":%u,\"fingerprint\":\"0x%08lX\"",
                               event.raw_pulses, event.raw_start_level,
                               static_cast<unsigned long>(event.fingerprint))) {
                return false;
            }
            if (!append_format(output, capacity, &length, ",\"repeats\":%u,\"match\":\"%s\",\"match_count\":%u,\"match_name\":",
                               event.repeats, match_name(event.match_kind), event.match_count) ||
                !append_json_string(output, capacity, &length, event.match_name)) {
                return false;
            }
            break;
        case BridgeEventType::kAutomation:
            if (!append_format(output, capacity, &length,
                               ",\"kind\":\"%s\",\"trigger\":",
                               automation_event_name(event.automation_type)) ||
                !append_json_string(output, capacity, &length, event.name) ||
                !append_format(output, capacity, &length, ",\"target\":") ||
                !append_json_string(output, capacity, &length, event.target) ||
                !append_format(output, capacity, &length,
                               ",\"action_id\":%lu,\"result\":%d,\"elapsed_ms\":%lu,\"value\":%lu",
                               static_cast<unsigned long>(event.action_id), static_cast<int>(event.result),
                               static_cast<unsigned long>(event.elapsed_ms), static_cast<unsigned long>(event.value))) {
                return false;
            }
            break;
        case BridgeEventType::kAutomationConfig:
            if (!append_format(output, capacity, &length,
                               ",\"change\":\"%s\",\"configuration_revision\":%lu,\"enabled\":%s,\"log_mode\":\"%s\",\"trigger\":",
                               config_change_name(event.config_change),
                               static_cast<unsigned long>(event.configuration_revision),
                               event.enabled ? "true" : "false", log_mode_name(event.log_mode)) ||
                !append_json_string(output, capacity, &length, event.name) ||
                !append_format(output, capacity, &length, ",\"target\":") ||
                !append_json_string(output, capacity, &length, event.target)) {
                return false;
            }
            break;
        case BridgeEventType::kSignalCatalogChanged:
            if (!append_format(output, capacity, &length, ",\"name\":") ||
                !append_json_string(output, capacity, &length, event.name)) {
                return false;
            }
            break;
        case BridgeEventType::kLearnArmed:
        case BridgeEventType::kLearnReplaced:
        case BridgeEventType::kLearnCompleted:
        case BridgeEventType::kLearnCancelled:
        case BridgeEventType::kLearnTimeout:
        case BridgeEventType::kLearnFailed:
            if (!append_format(output, capacity, &length, ",\"name\":") ||
                !append_json_string(output, capacity, &length, event.name) ||
                !append_format(output, capacity, &length, ",\"result\":%d", static_cast<int>(event.result))) {
                return false;
            }
            break;
        case BridgeEventType::kTxStarted:
        case BridgeEventType::kTxCompleted:
            if (!append_format(output, capacity, &length, ",\"name\":") ||
                !append_json_string(output, capacity, &length, event.name) ||
                !append_format(output, capacity, &length, ",\"repeats\":%u,\"result\":%d",
                               event.repeats, static_cast<int>(event.result))) {
                return false;
            }
            break;
        default:
            if (!append_format(output, capacity, &length, ",\"result\":%d,\"value\":%lu",
                               static_cast<int>(event.result), static_cast<unsigned long>(event.value))) {
                return false;
            }
            break;
    }
    if (!append_format(output, capacity, &length, "}")) {
        return false;
    }
    return format_web_sse_event(event.sequence, event_name(event.type), payload, framed_output,
                                framed_capacity, nullptr);
}

bool send_chunk(httpd_req_t *request, const char *chunk)
{
    return httpd_resp_send_chunk(request, chunk, HTTPD_RESP_USE_STRLEN) == ESP_OK;
}

bool send_resync(httpd_req_t *request, uint64_t sequence, const char *reason)
{
    char buffer[320]{};
    const int length = std::snprintf(buffer, sizeof(buffer),
                                     "id: %llu\nevent: resync\ndata:{\"sequence\":%llu,\"reason\":\"%s\"}\n\n",
                                     static_cast<unsigned long long>(sequence),
                                     static_cast<unsigned long long>(sequence), reason);
    return length > 0 && static_cast<std::size_t>(length) < sizeof(buffer) && send_chunk(request, buffer);
}

void stream_task(void *context)
{
#if CONFIG_WEB_SSE_ENABLE
    auto *request = static_cast<httpd_req_t *>(context);
    bool ok = true;
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "text/event-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(request, "Connection", "keep-alive");
    httpd_resp_set_hdr(request, "X-Accel-Buffering", "no");
    ok = send_chunk(request, ": connected\n\n");

    BridgeEventBrokerStatus broker{};
    (void)bridge_events_get_status(&broker);
    s_last_queue_drops = broker.queue_drops;
    s_last_sink_drops = broker.sink_drops;
    if (ok) {
        char hello[256]{};
        const int length = std::snprintf(hello, sizeof(hello),
                                         "id: %llu\nevent: hello\ndata:{\"sequence\":%llu,\"snapshot_required\":true}\n\n",
                                         static_cast<unsigned long long>(broker.published),
                                         static_cast<unsigned long long>(broker.published));
        ok = length > 0 && static_cast<std::size_t>(length) < sizeof(hello) && send_chunk(request, hello);
    }

    uint64_t last_sequence = broker.published;
    TickType_t last_heartbeat = xTaskGetTickCount();
    while (ok && s_started.load(std::memory_order_acquire) &&
           !s_stopping.load(std::memory_order_acquire)) {
        BridgeEventBrokerStatus status{};
        if (bridge_events_get_status(&status) == ESP_OK &&
            (status.queue_drops != s_last_queue_drops || status.sink_drops != s_last_sink_drops)) {
            s_last_queue_drops = status.queue_drops;
            s_last_sink_drops = status.sink_drops;
            s_resync_pending.store(true, std::memory_order_release);
        }
        if (s_resync_pending.exchange(false, std::memory_order_acq_rel)) {
            xQueueReset(s_event_queue);
            ok = send_resync(request, status.published, "queue_overflow");
            last_sequence = status.published;
            last_heartbeat = xTaskGetTickCount();
            continue;
        }
        WebSseEvent event{};
        if (xQueueReceive(s_event_queue, &event, kQueueWait) == pdTRUE) {
            if (event.sequence <= last_sequence) {
                continue;
            }
            if (event.sequence > last_sequence + 1U) {
                ok = send_resync(request, event.sequence, "sequence_gap");
                last_sequence = event.sequence;
                continue;
            }
            char serialized[kSseBufferCapacity]{};
            ok = serialize_event(event, serialized, sizeof(serialized)) && send_chunk(request, serialized);
            last_sequence = event.sequence;
            last_heartbeat = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - last_heartbeat >= kHeartbeatInterval) {
            ok = send_chunk(request, ": heartbeat\n\n");
            last_heartbeat = xTaskGetTickCount();
        }
        char pending_byte = 0;
        const int received = recv(httpd_req_to_sockfd(request), &pending_byte, sizeof(pending_byte),
                                  MSG_PEEK | MSG_DONTWAIT);
        if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            ok = false;
        }
    }
    (void)httpd_resp_send_chunk(request, nullptr, 0);
    (void)httpd_req_async_handler_complete(request);
    s_client_socket.store(-1, std::memory_order_release);
    s_client_active.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
#else
    (void)context;
    vTaskDelete(nullptr);
#endif
}

esp_err_t send_rejection(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    char response[160]{};
    const int length = std::snprintf(response, sizeof(response),
                                     "{\"ok\":false,\"error\":\"%s\"}", message);
    return length > 0 && static_cast<std::size_t>(length) < sizeof(response)
               ? httpd_resp_send(request, response, length)
               : ESP_ERR_INVALID_SIZE;
}

}  // namespace

esp_err_t web_events_start(httpd_handle_t server)
{
#if CONFIG_WEB_SSE_ENABLE
    if (server == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!s_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return s_server == server && !s_stopping.load(std::memory_order_acquire)
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }
    s_server = server;
    s_stopping.store(false, std::memory_order_release);
    s_resync_pending.store(false, std::memory_order_release);
    s_event_queue = xQueueCreate(kEventQueueDepth, sizeof(WebSseEvent));
    if (s_event_queue == nullptr) {
        s_server = nullptr;
        s_started.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    uint8_t sink_id = 0;
    const esp_err_t error = bridge_events_add_sink(web_event_sink, nullptr, &sink_id);
    if (error == ESP_OK) {
        s_sink_id = sink_id;
        s_sink_registered.store(true, std::memory_order_release);
    } else {
        ESP_LOGW(kTag, "Bridge event sink unavailable: %s; SSE will resync via snapshots",
                 esp_err_to_name(error));
    }
    return ESP_OK;
#else
    (void)server;
    return ESP_OK;
#endif
}

esp_err_t web_events_stop()
{
#if CONFIG_WEB_SSE_ENABLE
    if (!s_started.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    s_stopping.store(true, std::memory_order_release);
    const int socket = s_client_socket.load(std::memory_order_acquire);
    if (socket >= 0 && s_server != nullptr) {
        (void)httpd_sess_trigger_close(s_server, socket);
    }
    for (int attempt = 0;
         attempt < kStopWaitAttempts && s_client_active.load(std::memory_order_acquire);
         ++attempt) {
        vTaskDelay(kStopWaitDelay);
    }
    if (s_client_active.load(std::memory_order_acquire)) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_sink_registered.load(std::memory_order_acquire)) {
        const esp_err_t sink_error = bridge_events_remove_sink(s_sink_id);
        if (sink_error != ESP_OK) {
            return sink_error;
        }
        s_sink_registered.store(false, std::memory_order_release);
    }
    s_started.store(false, std::memory_order_release);
    if (s_event_queue != nullptr) {
        vQueueDelete(s_event_queue);
        s_event_queue = nullptr;
    }
    s_client_socket.store(-1, std::memory_order_release);
    s_resync_pending.store(false, std::memory_order_release);
    s_server = nullptr;
    s_stopping.store(false, std::memory_order_release);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t web_events_handler(httpd_req_t *request)
{
#if CONFIG_WEB_SSE_ENABLE
    if (request == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started.load(std::memory_order_acquire) ||
        s_stopping.load(std::memory_order_acquire)) {
        return send_rejection(request, "503 Service Unavailable", "events_unavailable");
    }
    bool expected = false;
    if (!s_client_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return send_rejection(request, "409 Conflict", "events_client_limit");
    }
    if (!s_started.load(std::memory_order_acquire) ||
        s_stopping.load(std::memory_order_acquire)) {
        s_client_active.store(false, std::memory_order_release);
        return send_rejection(request, "503 Service Unavailable", "events_unavailable");
    }
    if (!s_sink_registered.load(std::memory_order_acquire)) {
        uint8_t sink_id = 0;
        const esp_err_t sink_error = bridge_events_add_sink(web_event_sink, nullptr, &sink_id);
        if (sink_error == ESP_OK) {
            s_sink_id = sink_id;
            s_sink_registered.store(true, std::memory_order_release);
        } else {
            s_client_active.store(false, std::memory_order_release);
            return send_rejection(request, "503 Service Unavailable", "events_sink_unavailable");
        }
    }
    if (s_event_queue == nullptr) {
        s_client_active.store(false, std::memory_order_release);
        return send_rejection(request, "503 Service Unavailable", "events_unavailable");
    }
    xQueueReset(s_event_queue);
    s_resync_pending.store(false, std::memory_order_release);
    httpd_req_t *copy = nullptr;
    const esp_err_t begin_error = httpd_req_async_handler_begin(request, &copy);
    if (begin_error != ESP_OK) {
        s_client_active.store(false, std::memory_order_release);
        return begin_error;
    }
    s_client_socket.store(httpd_req_to_sockfd(copy), std::memory_order_release);
    if (xTaskCreate(stream_task, "web_sse", kWorkerStackSize, copy, kWorkerPriority,
                    nullptr) != pdPASS) {
        const esp_err_t response_error =
            send_rejection(copy, "503 Service Unavailable", "events_worker_unavailable");
        const esp_err_t complete_error = httpd_req_async_handler_complete(copy);
        s_client_socket.store(-1, std::memory_order_release);
        s_client_active.store(false, std::memory_order_release);
        return response_error != ESP_OK ? response_error : complete_error;
    }
    return ESP_OK;
#else
    return send_rejection(request, "404 Not Found", "events_disabled");
#endif
}

}  // namespace rfbridge
