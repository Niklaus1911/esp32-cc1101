#include "network_mqtt.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "bridge_control.hpp"
#include "bridge_events.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mqtt_client.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_discovery.hpp"
#include "mqtt_telemetry.hpp"
#include "network_mqtt_storage.hpp"
#include "nvs.h"
#include "rf_signals.hpp"
#include "rf_automation.hpp"
#include "rf_storage.hpp"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

constexpr char kTag[] = "network_mqtt";
constexpr char kHomeAssistantStatusTopic[] = "homeassistant/status";
constexpr char kOnlinePayload[] = "online";
constexpr uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kMqttTaskStackSize = 6144;
constexpr uint32_t kWorkerTaskStackSize = 5632;
constexpr UBaseType_t kWorkerTaskPriority = 3;
constexpr UBaseType_t kCommandQueueDepth = 8;
constexpr UBaseType_t kTelemetryQueueDepth = 8;
constexpr TickType_t kPublishWait = pdMS_TO_TICKS(35000);
constexpr TickType_t kSubscribeWait = pdMS_TO_TICKS(10000);
constexpr TickType_t kDisconnectWait = pdMS_TO_TICKS(10000);
constexpr TickType_t kAuditInterval = pdMS_TO_TICKS(60000);
constexpr TickType_t kShortWait = pdMS_TO_TICKS(100);

constexpr uint32_t kWakeConnection = 1U << 0U;
constexpr uint32_t kWakePublish = 1U << 1U;
constexpr uint32_t kWakeSubscription = 1U << 2U;
constexpr uint32_t kWakeCommand = 1U << 3U;
constexpr uint32_t kWakeCatalog = 1U << 4U;
constexpr uint32_t kWakeBirth = 1U << 5U;
constexpr uint32_t kWakeRetirement = 1U << 6U;
constexpr uint32_t kWakeTelemetry = 1U << 7U;
constexpr uint32_t kWakeNetwork = 1U << 8U;
constexpr TickType_t kTelemetryStateInterval = pdMS_TO_TICKS(1000);

static_assert(CONFIG_RF_MAX_LEARNED_SIGNALS + CONFIG_RF_MAX_AUTOMATION_RULES <=
              kMqttMaximumAdvertisedSignals);
static_assert(static_cast<uint8_t>(RfAutomationEventType::kTriggered) == 0);
static_assert(static_cast<uint8_t>(RfAutomationEventType::kActionCompleted) == 1);
static_assert(static_cast<uint8_t>(RfAutomationEventType::kCooldownSuppressed) == 2);
static_assert(static_cast<uint8_t>(RfAutomationEventType::kAmbiguousFrame) == 3);
static_assert(static_cast<uint8_t>(RfAutomationEventType::kStaleFrame) == 4);
static_assert(static_cast<uint8_t>(RfAutomationEventType::kQueueDrop) == 5);

enum class MqttCommandType : uint8_t {
    kReplay,
    kSetEnabled,
    kSetLogMode,
};

struct MqttCommand {
    MqttCommandType type = MqttCommandType::kReplay;
    bool enabled = false;
    uint8_t log_mode = 0;
    RfStorageName signal{};
};

enum class MqttTelemetryType : uint8_t {
    kRx,
    kAutomation,
};

struct MqttTelemetryMessage {
    MqttTelemetryType type = MqttTelemetryType::kRx;
    MqttRxTelemetry rx{};
    MqttAutomationTelemetry automation{};
};

struct MqttRuleSnapshot {
    MqttRuleTelemetry telemetry{};
};

struct RuntimeContext {
    MqttDeviceIdentity identity{};
    esp_mqtt_client_handle_t client = nullptr;
    QueueHandle_t command_queue = nullptr;
    QueueHandle_t telemetry_queue = nullptr;
    TaskHandle_t worker_task = nullptr;
    uint32_t broker_ipv4 = 0;
    uint32_t boot_generation = 0;
    uint16_t port = 0;
    uint8_t bridge_sink_id = 0;
    bool bridge_sink_registered = false;
    std::atomic<bool> client_started{false};
    std::atomic<bool> network_online{false};
    std::atomic<bool> activated{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> subscribed{false};
    std::atomic<bool> retirement_requested{false};
    std::atomic<bool> birth_pending{false};
    std::atomic<bool> catalog_pending{false};
    std::atomic<bool> telemetry_pending{false};
    std::atomic<bool> state_pending{false};
    std::atomic<uint32_t> automation_revision{0};
    std::atomic<int> last_published_id{0};
    std::atomic<int> last_deleted_id{0};
    std::atomic<int> last_subscribed_id{0};
    std::atomic<bool> last_subscription_ok{false};
    std::atomic<uint32_t> mqtt_stack_minimum_free{UINT32_MAX};
    MqttAdvertisedLedger current{};
    MqttAdvertisedLedger ledger{};
    MqttAdvertisedLedger merged{};
    bool ledger_present = false;
    std::array<MqttRuleSnapshot, CONFIG_RF_MAX_AUTOMATION_RULES> current_rules{};
    uint8_t current_rule_count = 0;
    portMUX_TYPE telemetry_lock = portMUX_INITIALIZER_UNLOCKED;
    MqttRxTelemetry pending_last_rx{};
    MqttAutomationTelemetry pending_last_automation{};
    bool has_pending_last_rx = false;
    bool has_pending_last_automation = false;
    void *mqtt_stack_reserve = nullptr;
    void *mqtt_tcb_reserve = nullptr;
    char command_filter[kMqttTopicCapacity]{};
    char enabled_command_topic[kMqttTopicCapacity]{};
    char log_command_topic[kMqttTopicCapacity]{};
    char availability_topic[kMqttTopicCapacity]{};
    char topic[kMqttTopicCapacity]{};
    char payload[kMqttDiscoveryPayloadCapacity]{};
};

esp_err_t publish_automation_state(RuntimeContext *context);

portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
NetworkMqttStatus s_status{};
RuntimeContext *s_context = nullptr;
std::atomic<bool> s_profile_initialized{false};

void copy_text(char *destination, std::size_t capacity, const char *source)
{
    if (capacity == 0) {
        return;
    }
    std::strncpy(destination, source == nullptr ? "" : source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

uint32_t next_generation(uint32_t generation)
{
    ++generation;
    return generation == 0 ? 1 : generation;
}

void set_discovery_state(NetworkMqttDiscoveryState state, esp_err_t error = ESP_OK)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.discovery_state = state;
    s_status.reconciliation_error = error;
    taskEXIT_CRITICAL(&s_status_lock);
}

void set_profile_status(const MqttServiceConfig *config, esp_err_t profile_error)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.profile_initialized = true;
    s_status.profile_error = profile_error;
    s_status.requested_profile = NetworkServiceProfile::kWeb;
    s_status.configured = false;
    s_status.retirement_pending = false;
    s_status.broker_ipv4 = 0;
    s_status.port = 0;
    s_status.persisted_generation = 0;
    s_status.username[0] = '\0';
    if (config != nullptr) {
        s_status.configured = mqtt_service_has_credentials(*config);
        s_status.requested_profile =
            config->state == MqttServiceState::kMqtt ||
                    config->state == MqttServiceState::kRetiring
                ? NetworkServiceProfile::kMqtt
                : NetworkServiceProfile::kWeb;
        s_status.retirement_pending = config->state == MqttServiceState::kRetiring ||
                                      config->state == MqttServiceState::kRetired;
        s_status.broker_ipv4 = config->broker_ipv4;
        s_status.port = config->port;
        s_status.persisted_generation = config->generation;
        copy_text(s_status.username, sizeof(s_status.username), config->username);
    }
    s_status.reboot_required =
        s_status.requested_profile != s_status.effective_profile ||
        (s_status.effective_profile == NetworkServiceProfile::kMqtt &&
         s_status.boot_generation != 0 &&
         s_status.persisted_generation != s_status.boot_generation);
    taskEXIT_CRITICAL(&s_status_lock);
}

void update_ledger_status(esp_err_t error, uint8_t signal_count, uint8_t rule_count = 0)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.ledger_known = error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND;
    s_status.advertised_count = error == ESP_OK ? signal_count : 0;
    s_status.advertised_rule_count = error == ESP_OK ? rule_count : 0;
    if (error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND) {
        s_status.reconciliation_error = ESP_OK;
    } else {
        s_status.reconciliation_error = error;
    }
    taskEXIT_CRITICAL(&s_status_lock);
}

bool ledgers_have_same_names(const MqttAdvertisedLedger &left,
                             const MqttAdvertisedLedger &right)
{
    if (left.count != right.count || left.rule_count != right.rule_count) {
        return false;
    }
    for (std::size_t index = 0; index < mqtt_advertised_ledger_total_count(left); ++index) {
        if (std::strcmp(left.names[index].value, right.names[index].value) != 0) {
            return false;
        }
    }
    return true;
}

bool ledger_contains(const MqttAdvertisedLedger &ledger, const char *name)
{
    std::size_t first = 0;
    std::size_t last = ledger.count;
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2U;
        const int comparison = std::strcmp(ledger.names[middle].value, name);
        if (comparison == 0) {
            return true;
        }
        if (comparison < 0) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    return false;
}

bool ledger_contains_rule(const MqttAdvertisedLedger &ledger, const char *name)
{
    for (std::size_t index = 0; index < ledger.rule_count; ++index) {
        if (std::strcmp(ledger.names[ledger.count + index].value, name) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t load_current_signals(RuntimeContext *context)
{
    std::size_t count = 0;
    esp_err_t error = rf_storage_list(nullptr, 0, &count);
    if (error != ESP_OK) {
        return error;
    }
    if (count > CONFIG_RF_MAX_LEARNED_SIGNALS ||
        count > kMqttMaximumAdvertisedSignals) {
        return ESP_ERR_INVALID_SIZE;
    }
    context->current = {};
    context->current_rules = {};
    context->current_rule_count = 0;
    context->current.broker_ipv4 = context->broker_ipv4;
    context->current.port = context->port;
    if (count > 0) {
        std::size_t loaded = count;
        error = rf_storage_list(context->current.names.data(), context->current.names.size(),
                                &loaded);
        if (error != ESP_OK) {
            return error;
        }
        if (loaded != count) {
            return ESP_ERR_INVALID_SIZE;
        }
        std::sort(context->current.names.begin(), context->current.names.begin() + count,
                  [](const RfStorageName &left, const RfStorageName &right) {
                      return std::strcmp(left.value, right.value) < 0;
                  });
    }
    context->current.count = static_cast<uint8_t>(count);
    RfAutomationStatus automation{};
    const esp_err_t automation_error = rf_automation_get_status(&automation);
    if (automation_error != ESP_OK) {
        return automation_error;
    }
    context->automation_revision.store(automation.configuration_revision,
                                       std::memory_order_release);
    std::size_t rule_count = 0;
    error = rf_automation_list_rule_info(nullptr, 0, &rule_count);
    if (error != ESP_OK || rule_count > CONFIG_RF_MAX_AUTOMATION_RULES ||
        rule_count + count > context->current.names.size()) {
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }
    if (rule_count > 0) {
        std::unique_ptr<RfAutomationRuleInfo[]> rules(
            new (std::nothrow) RfAutomationRuleInfo[rule_count]);
        if (!rules) {
            return ESP_ERR_NO_MEM;
        }
        error = rf_automation_list_rule_info(rules.get(), rule_count, &rule_count);
        if (error != ESP_OK) {
            return error;
        }
        for (std::size_t index = 0; index < rule_count; ++index) {
            MqttRuleSnapshot &snapshot = context->current_rules[index];
            snapshot.telemetry.valid = rules[index].validation_error == ESP_OK;
            snapshot.telemetry.error = rules[index].validation_error;
            snapshot.telemetry.repeats = rules[index].entry.rule.repeats;
            snapshot.telemetry.cooldown_ms = rules[index].entry.rule.cooldown_ms;
            copy_text(snapshot.telemetry.trigger_name, sizeof(snapshot.telemetry.trigger_name),
                      rules[index].entry.trigger_name);
            copy_text(snapshot.telemetry.target_name, sizeof(snapshot.telemetry.target_name),
                      rules[index].entry.rule.target_name);
        }
        context->current_rule_count = static_cast<uint8_t>(rule_count);
        std::sort(context->current_rules.begin(),
                  context->current_rules.begin() + context->current_rule_count,
                  [](const MqttRuleSnapshot &left, const MqttRuleSnapshot &right) {
                      return std::strcmp(left.telemetry.trigger_name,
                                         right.telemetry.trigger_name) < 0;
                  });
        for (std::size_t index = 0; index < rule_count; ++index) {
            std::memcpy(context->current.names[count + index].value,
                        context->current_rules[index].telemetry.trigger_name,
                        kRfStorageNameCapacity);
        }
    }
    context->current.rule_count = static_cast<uint8_t>(rule_count);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.current_count = context->current.count;
    s_status.current_rule_count = context->current.rule_count;
    taskEXIT_CRITICAL(&s_status_lock);
    return mqtt_advertised_ledger_is_valid(context->current) ? ESP_OK
                                                             : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t load_ledger(RuntimeContext *context)
{
    context->ledger = {};
    context->ledger_present = false;
    const esp_err_t error = load_mqtt_advertised_ledger(&context->ledger);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        context->ledger.broker_ipv4 = context->broker_ipv4;
        context->ledger.port = context->port;
        update_ledger_status(error, 0);
        return ESP_OK;
    }
    update_ledger_status(error, context->ledger.count, context->ledger.rule_count);
    if (error != ESP_OK) {
        return error;
    }
    context->ledger_present = true;
    return mqtt_advertised_ledger_matches_endpoint(context->ledger, context->broker_ipv4,
                                                   context->port)
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

esp_err_t build_merged_ledger(RuntimeContext *context)
{
    const MqttConfigFormatResult result = merge_mqtt_advertised_ledgers(
        context->ledger, context->current, &context->merged);
    switch (result) {
        case MqttConfigFormatResult::kOk: return ESP_OK;
        case MqttConfigFormatResult::kBufferTooSmall: return ESP_ERR_INVALID_SIZE;
        case MqttConfigFormatResult::kInvalidArgument: return ESP_ERR_INVALID_ARG;
        case MqttConfigFormatResult::kInvalidRecord: return ESP_ERR_INVALID_RESPONSE;
        default: return ESP_ERR_INVALID_RESPONSE;
    }
}

void notify_worker(RuntimeContext *context, uint32_t bits)
{
    if (context != nullptr && context->worker_task != nullptr) {
        xTaskNotify(context->worker_task, bits, eSetBits);
    }
}

void sample_mqtt_stack(RuntimeContext *context)
{
    const uint32_t margin = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    uint32_t current = context->mqtt_stack_minimum_free.load(std::memory_order_relaxed);
    while (margin < current &&
           !context->mqtt_stack_minimum_free.compare_exchange_weak(
               current, margin, std::memory_order_relaxed)) {
    }
}

void note_telemetry_drop(RuntimeContext *context)
{
    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.telemetry_drops;
    taskEXIT_CRITICAL(&s_status_lock);
    context->state_pending.store(true, std::memory_order_release);
}

void remember_last_rx(RuntimeContext *context, const MqttRxTelemetry &telemetry)
{
    taskENTER_CRITICAL(&context->telemetry_lock);
    context->pending_last_rx = telemetry;
    context->has_pending_last_rx = true;
    taskEXIT_CRITICAL(&context->telemetry_lock);
    context->state_pending.store(true, std::memory_order_release);
}

void remember_last_automation(RuntimeContext *context,
                              const MqttAutomationTelemetry &telemetry)
{
    taskENTER_CRITICAL(&context->telemetry_lock);
    context->pending_last_automation = telemetry;
    context->has_pending_last_automation = true;
    taskEXIT_CRITICAL(&context->telemetry_lock);
    context->state_pending.store(true, std::memory_order_release);
}

void restore_last_rx_if_unset(RuntimeContext *context, const MqttRxTelemetry &telemetry)
{
    taskENTER_CRITICAL(&context->telemetry_lock);
    if (!context->has_pending_last_rx) {
        context->pending_last_rx = telemetry;
        context->has_pending_last_rx = true;
    }
    taskEXIT_CRITICAL(&context->telemetry_lock);
    context->state_pending.store(true, std::memory_order_release);
}

void restore_last_automation_if_unset(RuntimeContext *context,
                                      const MqttAutomationTelemetry &telemetry)
{
    taskENTER_CRITICAL(&context->telemetry_lock);
    if (!context->has_pending_last_automation) {
        context->pending_last_automation = telemetry;
        context->has_pending_last_automation = true;
    }
    taskEXIT_CRITICAL(&context->telemetry_lock);
    context->state_pending.store(true, std::memory_order_release);
}

bool enqueue_telemetry(RuntimeContext *context, const MqttTelemetryMessage &message)
{
    if (context->telemetry_queue == nullptr ||
        xQueueSend(context->telemetry_queue, &message, 0) != pdTRUE) {
        note_telemetry_drop(context);
        return false;
    }
    context->telemetry_pending.store(true, std::memory_order_release);
    notify_worker(context, kWakeTelemetry);
    return true;
}

MqttTelemetryMatch mqtt_match_kind(LearnedMatchKind kind)
{
    switch (kind) {
        case LearnedMatchKind::kUnique: return MqttTelemetryMatch::kUnique;
        case LearnedMatchKind::kAmbiguous: return MqttTelemetryMatch::kAmbiguous;
        case LearnedMatchKind::kUnavailable: return MqttTelemetryMatch::kUnavailable;
        case LearnedMatchKind::kNone: default: return MqttTelemetryMatch::kNone;
    }
}

MqttRxTelemetry make_rx_telemetry(const BridgeEvent &event)
{
    MqttRxTelemetry telemetry{};
    telemetry.sequence = event.sequence;
    telemetry.encoding = event.payload.rf.frame.encoding == RfEncoding::kRaw
                             ? MqttTelemetryEncoding::kRaw
                             : MqttTelemetryEncoding::kDecoded;
    telemetry.match = mqtt_match_kind(event.payload.rf.learned.kind);
    telemetry.fingerprint = event.payload.rf.frame.fingerprint;
    telemetry.observed_repeats = event.payload.rf.frame.observed_repeats;
    telemetry.learned_count = event.payload.rf.learned.count;
    copy_text(telemetry.learned_name, sizeof(telemetry.learned_name),
              event.payload.rf.learned.name);
    if (telemetry.encoding == MqttTelemetryEncoding::kDecoded) {
        telemetry.code = event.payload.rf.frame.decoded.code;
        telemetry.pulse_us = event.payload.rf.frame.decoded.pulse_us;
        telemetry.bits = event.payload.rf.frame.decoded.bits;
        telemetry.protocol = event.payload.rf.frame.decoded.protocol;
        telemetry.inverted = event.payload.rf.frame.decoded.inverted;
    } else {
        telemetry.pulses = event.payload.rf.frame.raw.count;
        telemetry.start_level = event.payload.rf.frame.raw.start_level;
    }
    return telemetry;
}

MqttAutomationTelemetry make_automation_telemetry(const BridgeEvent &event)
{
    MqttAutomationTelemetry telemetry{};
    telemetry.sequence = event.sequence;
    telemetry.type = static_cast<uint8_t>(event.payload.automation.type);
    telemetry.result = event.payload.automation.result;
    telemetry.action_id = event.payload.automation.action_id;
    telemetry.elapsed_ms = event.payload.automation.elapsed_ms;
    telemetry.value = event.payload.automation.value;
    telemetry.repeats = event.payload.automation.repeats;
    copy_text(telemetry.trigger_name, sizeof(telemetry.trigger_name),
              event.payload.automation.trigger_name);
    copy_text(telemetry.target_name, sizeof(telemetry.target_name),
              event.payload.automation.target_name);
    return telemetry;
}

void mqtt_event_handler(void *handler_context, esp_event_base_t, int32_t event_id,
                        void *event_data)
{
    auto *context = static_cast<RuntimeContext *>(handler_context);
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (context == nullptr || event == nullptr) {
        return;
    }
    sample_mqtt_stack(context);
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            context->connected.store(true, std::memory_order_release);
            context->subscribed.store(false, std::memory_order_release);
            taskENTER_CRITICAL(&s_status_lock);
            s_status.connected = true;
            s_status.subscribed = false;
            s_status.runtime_error = ESP_OK;
            ++s_status.connections;
            taskEXIT_CRITICAL(&s_status_lock);
            notify_worker(context, kWakeConnection);
            break;
        case MQTT_EVENT_DISCONNECTED:
            context->connected.store(false, std::memory_order_release);
            context->subscribed.store(false, std::memory_order_release);
            taskENTER_CRITICAL(&s_status_lock);
            s_status.connected = false;
            s_status.subscribed = false;
            ++s_status.disconnects;
            taskEXIT_CRITICAL(&s_status_lock);
            notify_worker(context, kWakeConnection);
            break;
        case MQTT_EVENT_SUBSCRIBED: {
            bool accepted = event->data != nullptr && event->data_len > 0;
            for (int index = 0; accepted && index < event->data_len; ++index) {
                accepted = static_cast<uint8_t>(event->data[index]) != 0x80U;
            }
            context->last_subscription_ok.store(accepted, std::memory_order_release);
            context->last_subscribed_id.store(event->msg_id, std::memory_order_release);
            notify_worker(context, kWakeSubscription);
            break;
        }
        case MQTT_EVENT_PUBLISHED:
            context->last_published_id.store(event->msg_id, std::memory_order_release);
            notify_worker(context, kWakePublish);
            break;
        case MQTT_EVENT_DELETED:
            context->last_deleted_id.store(event->msg_id, std::memory_order_release);
            taskENTER_CRITICAL(&s_status_lock);
            ++s_status.outbox_deleted;
            taskEXIT_CRITICAL(&s_status_lock);
            notify_worker(context, kWakePublish);
            break;
        case MQTT_EVENT_DATA: {
            if (event->topic_len < 0 || event->data_len < 0 || event->total_data_len < 0 ||
                event->current_data_offset < 0) {
                taskENTER_CRITICAL(&s_status_lock);
                ++s_status.commands_rejected;
                taskEXIT_CRITICAL(&s_status_lock);
                break;
            }
            MqttIncomingMessage message{};
            message.topic = event->topic;
            message.topic_length = static_cast<std::size_t>(event->topic_len);
            message.data = event->data;
            message.data_length = static_cast<std::size_t>(event->data_len);
            message.total_data_length = static_cast<std::size_t>(event->total_data_len);
            message.current_data_offset = static_cast<std::size_t>(event->current_data_offset);
            message.qos = event->qos;
            message.retain = event->retain;
            message.duplicate = event->dup;
            if (mqtt_message_is_home_assistant_birth(message)) {
                context->birth_pending.store(true, std::memory_order_release);
                notify_worker(context, kWakeBirth);
                break;
            }
            if (context->retirement_requested.load(std::memory_order_acquire)) {
                taskENTER_CRITICAL(&s_status_lock);
                ++s_status.commands_rejected;
                taskEXIT_CRITICAL(&s_status_lock);
                break;
            }
            MqttCommand command{};
            MqttAutomationCommandKind automation_kind{};
            bool enabled = false;
            uint8_t log_mode = 0;
            if (parse_mqtt_automation_command(context->identity, message, &automation_kind,
                                              &enabled, &log_mode)) {
                command.type = automation_kind == MqttAutomationCommandKind::kEnabled
                                   ? MqttCommandType::kSetEnabled
                                   : MqttCommandType::kSetLogMode;
                command.enabled = enabled;
                command.log_mode = log_mode;
            } else if (parse_mqtt_button_command(context->identity, message,
                                                 command.signal.value)) {
                command.type = MqttCommandType::kReplay;
            } else {
                taskENTER_CRITICAL(&s_status_lock);
                ++s_status.commands_rejected;
                taskEXIT_CRITICAL(&s_status_lock);
                break;
            }
            if (context->command_queue == nullptr ||
                xQueueSend(context->command_queue, &command, 0) != pdTRUE) {
                taskENTER_CRITICAL(&s_status_lock);
                ++s_status.command_queue_drops;
                taskEXIT_CRITICAL(&s_status_lock);
                break;
            }
            taskENTER_CRITICAL(&s_status_lock);
            ++s_status.commands_accepted;
            taskEXIT_CRITICAL(&s_status_lock);
            notify_worker(context, kWakeCommand);
            break;
        }
        case MQTT_EVENT_ERROR:
            taskENTER_CRITICAL(&s_status_lock);
            s_status.runtime_error = ESP_FAIL;
            taskEXIT_CRITICAL(&s_status_lock);
            break;
        default: break;
    }
}

void service_commands(RuntimeContext *context)
{
    MqttCommand command{};
    while (context->command_queue != nullptr &&
           xQueueReceive(context->command_queue, &command, 0) == pdTRUE) {
        if (context->retirement_requested.load(std::memory_order_acquire)) {
            continue;
        }
        esp_err_t error = ESP_OK;
        if (command.type == MqttCommandType::kReplay) {
            error = bridge_control_replay_named(
                command.signal.value, CONFIG_RF_DEFAULT_TX_REPEATS, BridgeEventSource::kMqtt);
        } else if (command.type == MqttCommandType::kSetEnabled) {
            error = rf_automation_set_enabled(command.enabled);
        } else if (command.type == MqttCommandType::kSetLogMode) {
            error = rf_automation_set_log_mode(
                static_cast<RfAutomationLogMode>(command.log_mode));
        }
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "MQTT command failed: %s", esp_err_to_name(error));
        } else if (command.type != MqttCommandType::kReplay) {
            context->state_pending.store(true, std::memory_order_release);
            notify_worker(context, kWakePublish);
        }
    }
}

void wait_for_worker(RuntimeContext *context, TickType_t ticks)
{
    service_commands(context);
    uint32_t notification = 0;
    (void)xTaskNotifyWait(0, UINT32_MAX, &notification, ticks);
    service_commands(context);
}

esp_err_t wait_for_publish(RuntimeContext *context, const char *topic, const char *payload,
                           int payload_length)
{
    if (!context->connected.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    context->last_published_id.store(0, std::memory_order_release);
    context->last_deleted_id.store(0, std::memory_order_release);
    const int message_id = esp_mqtt_client_publish(context->client, topic, payload,
                                                   payload_length, 1, 1);
    if (message_id < 0) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.publish_failures;
        taskEXIT_CRITICAL(&s_status_lock);
        return message_id == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    const TickType_t started = xTaskGetTickCount();
    while (xTaskGetTickCount() - started < kPublishWait) {
        if (!context->connected.load(std::memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (context->last_published_id.load(std::memory_order_acquire) == message_id) {
            return ESP_OK;
        }
        if (context->last_deleted_id.load(std::memory_order_acquire) == message_id) {
            return ESP_ERR_TIMEOUT;
        }
        wait_for_worker(context, kShortWait);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t publish_ephemeral(RuntimeContext *context, const char *topic, const char *payload)
{
    if (!context->connected.load(std::memory_order_acquire) ||
        !context->subscribed.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int message_id = esp_mqtt_client_publish(
        context->client, topic, payload, static_cast<int>(std::strlen(payload)), 0, 0);
    if (message_id < 0) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.publish_failures;
        taskEXIT_CRITICAL(&s_status_lock);
        return message_id == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.telemetry_published;
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

uint32_t telemetry_drop_count()
{
    taskENTER_CRITICAL(&s_status_lock);
    const uint32_t drops = s_status.telemetry_drops;
    taskEXIT_CRITICAL(&s_status_lock);
    return drops;
}

esp_err_t publish_pending_states(RuntimeContext *context)
{
    context->state_pending.store(false, std::memory_order_release);
    MqttRxTelemetry rx{};
    MqttAutomationTelemetry automation{};
    bool has_rx = false;
    bool has_automation = false;
    taskENTER_CRITICAL(&context->telemetry_lock);
    if (context->has_pending_last_rx) {
        rx = context->pending_last_rx;
        context->has_pending_last_rx = false;
        has_rx = true;
    }
    if (context->has_pending_last_automation) {
        automation = context->pending_last_automation;
        context->has_pending_last_automation = false;
        has_automation = true;
    }
    taskEXIT_CRITICAL(&context->telemetry_lock);

    char topic[kMqttTopicCapacity]{};
    if (has_rx) {
        if (!format_mqtt_state_topic(context->identity, MqttStateTopicKind::kLastRx, nullptr,
                                     topic, sizeof(topic)) ||
            !format_mqtt_rx_event_payload(rx, context->payload, sizeof(context->payload))) {
            restore_last_rx_if_unset(context, rx);
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t error = wait_for_publish(context, topic, context->payload,
                                           static_cast<int>(std::strlen(context->payload)));
        if (error != ESP_OK) {
            restore_last_rx_if_unset(context, rx);
            return error;
        }
    }
    if (has_automation) {
        if (!format_mqtt_state_topic(context->identity, MqttStateTopicKind::kLastAutomation,
                                     nullptr, topic, sizeof(topic)) ||
            !format_mqtt_automation_event_payload(automation, context->payload,
                                                  sizeof(context->payload))) {
            restore_last_automation_if_unset(context, automation);
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t error = wait_for_publish(context, topic, context->payload,
                                           static_cast<int>(std::strlen(context->payload)));
        if (error != ESP_OK) {
            restore_last_automation_if_unset(context, automation);
            return error;
        }
    }
    const esp_err_t state_error = publish_automation_state(context);
    if (state_error != ESP_OK) {
        if (has_rx) {
            restore_last_rx_if_unset(context, rx);
        }
        if (has_automation) {
            restore_last_automation_if_unset(context, automation);
        }
        context->state_pending.store(true, std::memory_order_release);
        return state_error;
    }
    return ESP_OK;
}

void discard_telemetry(RuntimeContext *context)
{
    if (context->telemetry_queue == nullptr) {
        return;
    }
    MqttTelemetryMessage message{};
    while (xQueueReceive(context->telemetry_queue, &message, 0) == pdTRUE) {
        note_telemetry_drop(context);
    }
}

void service_telemetry(RuntimeContext *context)
{
    if (!context->connected.load(std::memory_order_acquire) ||
        !context->subscribed.load(std::memory_order_acquire)) {
        discard_telemetry(context);
        return;
    }
    MqttTelemetryMessage message{};
    while (context->telemetry_queue != nullptr &&
           xQueueReceive(context->telemetry_queue, &message, 0) == pdTRUE) {
        char topic[kMqttTopicCapacity]{};
        bool formatted = false;
        if (message.type == MqttTelemetryType::kRx) {
            formatted = format_mqtt_event_topic(context->identity, MqttEventTopicKind::kRx,
                                                topic, sizeof(topic)) &&
                        format_mqtt_rx_event_payload(message.rx, context->payload,
                                                      sizeof(context->payload));
        } else {
            formatted = format_mqtt_event_topic(context->identity,
                                                MqttEventTopicKind::kAutomation, topic,
                                                sizeof(topic)) &&
                        format_mqtt_automation_event_payload(message.automation, context->payload,
                                                              sizeof(context->payload));
        }
        if (!formatted || publish_ephemeral(context, topic, context->payload) != ESP_OK) {
            note_telemetry_drop(context);
        }
    }
    context->telemetry_pending.store(false, std::memory_order_release);
}

esp_err_t wait_for_clean_disconnect(RuntimeContext *context)
{
    if (context->client == nullptr ||
        !context->connected.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = esp_mqtt_client_disconnect(context->client);
    if (error != ESP_OK) {
        return error;
    }
    const TickType_t started = xTaskGetTickCount();
    while (xTaskGetTickCount() - started < kDisconnectWait) {
        if (!context->connected.load(std::memory_order_acquire)) {
            return ESP_OK;
        }
        wait_for_worker(context, kShortWait);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t subscribe_topics(RuntimeContext *context)
{
    context->last_subscribed_id.store(0, std::memory_order_release);
    context->last_subscription_ok.store(false, std::memory_order_release);
    const esp_mqtt_topic_t topics[] = {
        {context->command_filter, 0},
        {kHomeAssistantStatusTopic, 0},
        {context->enabled_command_topic, 0},
        {context->log_command_topic, 0},
    };
    const int message_id = esp_mqtt_client_subscribe_multiple(context->client, topics,
                                                               sizeof(topics) / sizeof(topics[0]));
    if (message_id < 0) {
        return message_id == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    set_discovery_state(NetworkMqttDiscoveryState::kSubscribing);
    const TickType_t started = xTaskGetTickCount();
    while (xTaskGetTickCount() - started < kSubscribeWait) {
        if (!context->connected.load(std::memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (context->last_subscribed_id.load(std::memory_order_acquire) == message_id) {
            if (!context->last_subscription_ok.load(std::memory_order_acquire)) {
                return ESP_ERR_NOT_ALLOWED;
            }
            context->subscribed.store(true, std::memory_order_release);
            taskENTER_CRITICAL(&s_status_lock);
            s_status.subscribed = true;
            taskEXIT_CRITICAL(&s_status_lock);
            return ESP_OK;
        }
        wait_for_worker(context, kShortWait);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t publish_discovery_config(RuntimeContext *context, const char *name)
{
    if (!format_mqtt_discovery_topic(context->identity, name, context->topic,
                                     sizeof(context->topic)) ||
        !format_mqtt_discovery_payload(context->identity, name,
                                       esp_app_get_description()->version,
                                       context->payload, sizeof(context->payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return wait_for_publish(context, context->topic, context->payload,
                            static_cast<int>(std::strlen(context->payload)));
}

esp_err_t publish_discovery_tombstone(RuntimeContext *context, const char *name)
{
    if (!format_mqtt_discovery_topic(context->identity, name, context->topic,
                                     sizeof(context->topic))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return wait_for_publish(context, context->topic, "", 0);
}

esp_err_t publish_entity_discovery_config(RuntimeContext *context, MqttDiscoveryEntityKind kind,
                                          const char *rule_name)
{
    if (!format_mqtt_entity_discovery_topic(context->identity, kind, rule_name, context->topic,
                                            sizeof(context->topic)) ||
        !format_mqtt_entity_discovery_payload(context->identity, kind, rule_name,
                                              esp_app_get_description()->version,
                                              context->payload, sizeof(context->payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return wait_for_publish(context, context->topic, context->payload,
                            static_cast<int>(std::strlen(context->payload)));
}

esp_err_t publish_entity_discovery_tombstone(RuntimeContext *context,
                                             MqttDiscoveryEntityKind kind,
                                             const char *rule_name)
{
    if (!format_mqtt_entity_discovery_topic(context->identity, kind, rule_name, context->topic,
                                            sizeof(context->topic))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return wait_for_publish(context, context->topic, "", 0);
}

esp_err_t publish_rule_tombstones(RuntimeContext *context, const char *rule_name)
{
    esp_err_t error = publish_entity_discovery_tombstone(
        context, MqttDiscoveryEntityKind::kRuleSensor, rule_name);
    char topic[kMqttTopicCapacity]{};
    if (error == ESP_OK &&
        !format_mqtt_state_topic(context->identity, MqttStateTopicKind::kRule, rule_name,
                                 topic, sizeof(topic))) {
        error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK) {
        error = wait_for_publish(context, topic, "", 0);
    }
    return error;
}

bool ledger_entry_is_rule(const MqttAdvertisedLedger &ledger, std::size_t index)
{
    return index >= ledger.count && index < mqtt_advertised_ledger_total_count(ledger);
}

void remove_ledger_entry(MqttAdvertisedLedger *ledger, std::size_t index)
{
    if (ledger == nullptr || index >= mqtt_advertised_ledger_total_count(*ledger)) {
        return;
    }
    const std::size_t total = mqtt_advertised_ledger_total_count(*ledger);
    if (index < ledger->count) {
        for (std::size_t cursor = index + 1U; cursor < ledger->count; ++cursor) {
            ledger->names[cursor - 1U] = ledger->names[cursor];
        }
        for (std::size_t cursor = ledger->count; cursor < total; ++cursor) {
            ledger->names[cursor - 1U] = ledger->names[cursor];
        }
        --ledger->count;
    } else {
        for (std::size_t cursor = index + 1U; cursor < total; ++cursor) {
            ledger->names[cursor - 1U] = ledger->names[cursor];
        }
        --ledger->rule_count;
    }
    ledger->names[mqtt_advertised_ledger_total_count(*ledger)] = {};
}

constexpr MqttDiscoveryEntityKind kFixedEntityKinds[] = {
    MqttDiscoveryEntityKind::kRxEvent,
    MqttDiscoveryEntityKind::kAutomationEvent,
    MqttDiscoveryEntityKind::kAutomationSwitch,
    MqttDiscoveryEntityKind::kAutomationLogSelect,
    MqttDiscoveryEntityKind::kRuleCountSensor,
    MqttDiscoveryEntityKind::kEventDropsSensor,
};

esp_err_t publish_fixed_discovery(RuntimeContext *context)
{
    for (const MqttDiscoveryEntityKind kind : kFixedEntityKinds) {
        const esp_err_t error = publish_entity_discovery_config(context, kind, nullptr);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t publish_automation_state(RuntimeContext *context)
{
    RfAutomationStatus automation{};
    const esp_err_t automation_error = rf_automation_get_status(&automation);
    if (automation_error != ESP_OK) {
        return automation_error;
    }
    MqttAutomationStateTelemetry state{};
    state.enabled = automation.enabled;
    state.enabled_known = automation.enabled_known;
    state.log_mode_known = automation.log_mode_known;
    state.log_mode = static_cast<uint8_t>(automation.log_mode);
    state.rules = automation.rule_count;
    state.frames = automation.frames_seen;
    state.matches = automation.matches;
    state.actions = automation.actions_succeeded;
    state.tx_errors = automation.tx_errors;
    state.cooldown_suppressed = automation.cooldown_suppressed;
    state.queue_drops = automation.queue_drops;
    state.event_drops = telemetry_drop_count();
    state.log_drops = automation.log_drops;
    state.last_error = automation.last_error;
    copy_text(state.last_trigger, sizeof(state.last_trigger), automation.last_trigger);
    copy_text(state.last_target, sizeof(state.last_target), automation.last_target);
    char topic[kMqttTopicCapacity]{};
    if (!format_mqtt_state_topic(context->identity, MqttStateTopicKind::kAutomation, nullptr,
                                 topic, sizeof(topic)) ||
        !format_mqtt_automation_state_payload(state, context->payload, sizeof(context->payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t error = wait_for_publish(context, topic, context->payload,
                                             static_cast<int>(std::strlen(context->payload)));
    if (error != ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.state_publish_failures;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    return error;
}

esp_err_t publish_rule_states(RuntimeContext *context)
{
    for (std::size_t index = 0; index < context->current_rule_count; ++index) {
        const MqttRuleTelemetry &rule = context->current_rules[index].telemetry;
        char topic[kMqttTopicCapacity]{};
        if (!format_mqtt_state_topic(context->identity, MqttStateTopicKind::kRule,
                                     rule.trigger_name, topic, sizeof(topic)) ||
            !format_mqtt_rule_state_payload(rule, context->payload, sizeof(context->payload))) {
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t error = wait_for_publish(context, topic, context->payload,
                                           static_cast<int>(std::strlen(context->payload)));
        if (error != ESP_OK) {
            taskENTER_CRITICAL(&s_status_lock);
            ++s_status.state_publish_failures;
            taskEXIT_CRITICAL(&s_status_lock);
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t reconcile_discovery(RuntimeContext *context, bool force_configs)
{
    set_discovery_state(NetworkMqttDiscoveryState::kReconciling);
    esp_err_t error = load_current_signals(context);
    if (error == ESP_OK) {
        error = load_ledger(context);
    }
    if (error == ESP_OK) {
        error = build_merged_ledger(context);
    }
    if (error == ESP_OK && !force_configs && context->ledger_present &&
        context->ledger.format_version == kMqttAdvertisedFormatVersion &&
        ledgers_have_same_names(context->ledger, context->current)) {
        error = publish_automation_state(context);
        if (error == ESP_OK) {
            set_discovery_state(NetworkMqttDiscoveryState::kReady);
        }
        return error;
    }
    while (error == ESP_ERR_INVALID_SIZE) {
        std::size_t stale_index = mqtt_advertised_ledger_total_count(context->ledger);
        for (std::size_t index = 0;
             index < mqtt_advertised_ledger_total_count(context->ledger); ++index) {
            const bool present = ledger_entry_is_rule(context->ledger, index)
                                     ? ledger_contains_rule(context->current,
                                                            context->ledger.names[index].value)
                                     : ledger_contains(context->current,
                                                       context->ledger.names[index].value);
            if (!present) {
                stale_index = index;
                break;
            }
        }
        if (stale_index >= mqtt_advertised_ledger_total_count(context->ledger)) {
            break;
        }
        const bool stale_rule = ledger_entry_is_rule(context->ledger, stale_index);
        error = stale_rule
                    ? publish_rule_tombstones(context,
                                              context->ledger.names[stale_index].value)
                    : publish_discovery_tombstone(context,
                                                  context->ledger.names[stale_index].value);
        MqttAdvertisedLedger reduced = context->ledger;
        remove_ledger_entry(&reduced, stale_index);
        if (error == ESP_OK) {
            error = save_mqtt_advertised_ledger(reduced);
        }
        if (error == ESP_OK) {
            context->ledger = reduced;
            context->ledger_present = true;
            update_ledger_status(ESP_OK, reduced.count, reduced.rule_count);
            error = build_merged_ledger(context);
        }
    }
    if (error == ESP_OK &&
        (!context->ledger_present ||
         context->ledger.format_version != kMqttAdvertisedFormatVersion ||
         !ledgers_have_same_names(context->merged, context->ledger))) {
        error = save_mqtt_advertised_ledger(context->merged);
        if (error == ESP_OK) {
            context->ledger = context->merged;
            context->ledger_present = true;
            update_ledger_status(ESP_OK, context->ledger.count, context->ledger.rule_count);
        }
    }
    for (std::size_t index = 0;
         error == ESP_OK && index < mqtt_advertised_ledger_total_count(context->ledger);
         ++index) {
        const bool present = ledger_entry_is_rule(context->ledger, index)
                                 ? ledger_contains_rule(context->current,
                                                        context->ledger.names[index].value)
                                 : ledger_contains(context->current,
                                                   context->ledger.names[index].value);
        if (!present) {
            error = ledger_entry_is_rule(context->ledger, index)
                        ? publish_rule_tombstones(context,
                                                  context->ledger.names[index].value)
                        : publish_discovery_tombstone(context, context->ledger.names[index].value);
        }
    }
    for (std::size_t index = 0; error == ESP_OK && index < context->current.count; ++index) {
        error = publish_discovery_config(context, context->current.names[index].value);
    }
    for (std::size_t index = 0;
         error == ESP_OK && index < context->current_rule_count; ++index) {
        error = publish_entity_discovery_config(context, MqttDiscoveryEntityKind::kRuleSensor,
                                                context->current_rules[index].telemetry.trigger_name);
    }
    if (error == ESP_OK && (force_configs || !context->ledger_present ||
                            context->ledger.format_version != kMqttAdvertisedFormatVersion ||
                            !ledgers_have_same_names(context->ledger, context->current))) {
        error = publish_fixed_discovery(context);
    }
    if (error == ESP_OK &&
        (!context->ledger_present ||
         context->ledger.format_version != kMqttAdvertisedFormatVersion ||
         !ledgers_have_same_names(context->ledger, context->current))) {
        error = save_mqtt_advertised_ledger(context->current);
    }
    if (error == ESP_OK) {
        context->ledger = context->current;
        context->ledger_present = true;
        update_ledger_status(ESP_OK, context->current.count, context->current.rule_count);
        error = publish_automation_state(context);
    }
    if (error == ESP_OK) {
        error = publish_rule_states(context);
    }
    if (error == ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.reconciliations;
        taskEXIT_CRITICAL(&s_status_lock);
        set_discovery_state(NetworkMqttDiscoveryState::kReady);
    } else {
        set_discovery_state(NetworkMqttDiscoveryState::kFaulted, error);
    }
    return error;
}

esp_err_t finish_retired_cleanup()
{
    esp_err_t error = erase_mqtt_advertised_ledger();
    if (error == ESP_OK) {
        error = erase_mqtt_service_config();
    }
    return error;
}

esp_err_t retire_discovery(RuntimeContext *context)
{
    set_discovery_state(NetworkMqttDiscoveryState::kRetiring);
    MqttServiceConfig config{};
    esp_err_t error = load_mqtt_service_config(&config);
    if (error != ESP_OK) {
        return error;
    }
    if (config.state == MqttServiceState::kRetired) {
        std::memset(&config, 0, sizeof(config));
        return finish_retired_cleanup();
    }
    if (config.state != MqttServiceState::kRetiring ||
        config.broker_ipv4 != context->broker_ipv4 || config.port != context->port) {
        std::memset(&config, 0, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }
    error = load_ledger(context);
    for (std::size_t index = 0;
         error == ESP_OK && index < mqtt_advertised_ledger_total_count(context->ledger);
         ++index) {
        error = ledger_entry_is_rule(context->ledger, index)
                    ? publish_rule_tombstones(context,
                                              context->ledger.names[index].value)
                    : publish_discovery_tombstone(context, context->ledger.names[index].value);
    }
    for (const MqttDiscoveryEntityKind kind : kFixedEntityKinds) {
        if (error != ESP_OK) {
            break;
        }
        error = publish_entity_discovery_tombstone(context, kind, nullptr);
    }
    if (error == ESP_OK) {
        error = wait_for_publish(context, context->availability_topic, "", 0);
    }
    if (error == ESP_OK) {
        char topic[kMqttTopicCapacity]{};
        const MqttStateTopicKind state_kinds[] = {
            MqttStateTopicKind::kAutomation,
            MqttStateTopicKind::kLastRx,
            MqttStateTopicKind::kLastAutomation,
        };
        for (const MqttStateTopicKind kind : state_kinds) {
            if (!format_mqtt_state_topic(context->identity, kind, nullptr, topic, sizeof(topic)) ||
                wait_for_publish(context, topic, "", 0) != ESP_OK) {
                error = ESP_FAIL;
                break;
            }
        }
    }
    if (error == ESP_OK) {
        error = wait_for_clean_disconnect(context);
    }
    if (error == ESP_OK) {
        config.state = MqttServiceState::kRetired;
        config.generation = next_generation(config.generation);
        error = save_mqtt_service_config(config);
    }
    std::memset(&config, 0, sizeof(config));
    if (error == ESP_OK) {
        error = finish_retired_cleanup();
    }
    if (error == ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);
        s_status.configured = false;
        s_status.requested_profile = NetworkServiceProfile::kWeb;
        s_status.retirement_pending = false;
        s_status.persisted_generation = 0;
        s_status.reboot_required = true;
        s_status.advertised_count = 0;
        s_status.advertised_rule_count = 0;
        s_status.ledger_known = true;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    return error;
}

void stop_client_from_worker(RuntimeContext *context)
{
    if (context->client != nullptr) {
        if (context->client_started.load(std::memory_order_acquire)) {
            (void)esp_mqtt_client_disconnect(context->client);
            vTaskDelay(pdMS_TO_TICKS(50));
            (void)esp_mqtt_client_stop(context->client);
            context->client_started.store(false, std::memory_order_release);
        }
        (void)esp_mqtt_client_destroy(context->client);
        context->client = nullptr;
    }
    context->connected.store(false, std::memory_order_release);
    context->subscribed.store(false, std::memory_order_release);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.connected = false;
    s_status.subscribed = false;
    s_status.runtime_available = false;
    s_status.runtime_error = ESP_OK;
    s_status.discovery_state = NetworkMqttDiscoveryState::kStopped;
    s_status.client_start_pending = false;
    taskEXIT_CRITICAL(&s_status_lock);
}

esp_err_t start_client_when_network_ready(RuntimeContext *context)
{
    if (context->client == nullptr || context->client_started.load(std::memory_order_acquire) ||
        !context->network_online.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (context->mqtt_stack_reserve != nullptr) {
        heap_caps_free(context->mqtt_stack_reserve);
        context->mqtt_stack_reserve = nullptr;
    }
    if (context->mqtt_tcb_reserve != nullptr) {
        heap_caps_free(context->mqtt_tcb_reserve);
        context->mqtt_tcb_reserve = nullptr;
    }
    const esp_err_t error = esp_mqtt_client_start(context->client);
    if (error == ESP_OK) {
        context->client_started.store(true, std::memory_order_release);
        taskENTER_CRITICAL(&s_status_lock);
        s_status.client_start_pending = false;
        s_status.runtime_error = ESP_OK;
        taskEXIT_CRITICAL(&s_status_lock);
    } else {
        taskENTER_CRITICAL(&s_status_lock);
        s_status.client_start_pending = false;
        s_status.runtime_error = error;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    return error;
}

void worker_task(void *argument)
{
    auto *context = static_cast<RuntimeContext *>(argument);
    TickType_t last_audit = xTaskGetTickCount();
    bool connection_ready = false;
    while (true) {
        service_commands(context);
        if (context->network_online.load(std::memory_order_acquire) &&
            !context->client_started.load(std::memory_order_acquire)) {
            const esp_err_t start_error = start_client_when_network_ready(context);
            if (start_error != ESP_OK && start_error != ESP_ERR_INVALID_STATE) {
                set_discovery_state(NetworkMqttDiscoveryState::kFaulted, start_error);
                wait_for_worker(context, pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (!context->client_started.load(std::memory_order_acquire)) {
            set_discovery_state(NetworkMqttDiscoveryState::kDisconnected);
            wait_for_worker(context, pdMS_TO_TICKS(1000));
            continue;
        }
        if (!context->connected.load(std::memory_order_acquire)) {
            connection_ready = false;
            set_discovery_state(NetworkMqttDiscoveryState::kDisconnected);
            wait_for_worker(context, pdMS_TO_TICKS(1000));
            continue;
        }
        if (context->retirement_requested.load(std::memory_order_acquire)) {
            const esp_err_t error = retire_discovery(context);
            if (error == ESP_OK) {
                stop_client_from_worker(context);
                vTaskSuspend(nullptr);
            }
            set_discovery_state(NetworkMqttDiscoveryState::kFaulted, error);
            wait_for_worker(context, pdMS_TO_TICKS(1000));
            continue;
        }
        if (!context->activated.load(std::memory_order_acquire)) {
            wait_for_worker(context, pdMS_TO_TICKS(100));
            continue;
        }
        if (!connection_ready) {
            esp_err_t error = subscribe_topics(context);
            if (error == ESP_OK) {
                set_discovery_state(NetworkMqttDiscoveryState::kPublishingAvailability);
                error = wait_for_publish(context, context->availability_topic, kOnlinePayload,
                                         sizeof(kOnlinePayload) - 1U);
            }
            if (error == ESP_OK) {
                error = reconcile_discovery(context, true);
            }
            if (error != ESP_OK) {
                set_discovery_state(NetworkMqttDiscoveryState::kFaulted, error);
                wait_for_worker(context, pdMS_TO_TICKS(1000));
                continue;
            }
            connection_ready = true;
            last_audit = xTaskGetTickCount();
        }

        bool force = false;
        const bool catalog_changed =
            context->catalog_pending.exchange(false, std::memory_order_acq_rel);
        bool reconcile = catalog_changed;
        force = catalog_changed;
        if (context->birth_pending.exchange(false, std::memory_order_acq_rel)) {
            const TickType_t jitter = pdMS_TO_TICKS(esp_random() % 2001U);
            const TickType_t started = xTaskGetTickCount();
            while (xTaskGetTickCount() - started < jitter &&
                   context->connected.load(std::memory_order_acquire)) {
                wait_for_worker(context, std::min<TickType_t>(kShortWait,
                    jitter - (xTaskGetTickCount() - started)));
            }
            reconcile = true;
            force = true;
        }
        const TickType_t now = xTaskGetTickCount();
        RfAutomationStatus automation{};
        if (rf_automation_get_status(&automation) == ESP_OK &&
            automation.configuration_revision !=
                context->automation_revision.load(std::memory_order_acquire)) {
            reconcile = true;
            force = true;
        }
        if (now - last_audit >= kAuditInterval) {
            reconcile = true;
            last_audit = now;
        }
        if (reconcile && context->connected.load(std::memory_order_acquire)) {
            (void)reconcile_discovery(context, force);
        }
        service_telemetry(context);
        if (context->state_pending.load(std::memory_order_acquire)) {
            const TickType_t now_state = xTaskGetTickCount();
            static TickType_t last_state_publish = 0;
            if (now_state - last_state_publish >= kTelemetryStateInterval) {
                if (publish_pending_states(context) == ESP_OK) {
                    last_state_publish = now_state;
                }
            }
        }
        wait_for_worker(context, pdMS_TO_TICKS(1000));
    }
}

bool mqtt_bridge_sink(const BridgeEvent &event, void *context_pointer)
{
    auto *context = static_cast<RuntimeContext *>(context_pointer);
    if (event.type == BridgeEventType::kSignalCatalogChanged) {
        context->catalog_pending.store(true, std::memory_order_release);
        notify_worker(context, kWakeCatalog);
    } else if (event.type == BridgeEventType::kRx) {
        MqttTelemetryMessage message{};
        message.type = MqttTelemetryType::kRx;
        message.rx = make_rx_telemetry(event);
        remember_last_rx(context, message.rx);
        (void)enqueue_telemetry(context, message);
    } else if (event.type == BridgeEventType::kAutomation) {
        MqttTelemetryMessage message{};
        message.type = MqttTelemetryType::kAutomation;
        message.automation = make_automation_telemetry(event);
        remember_last_automation(context, message.automation);
        (void)enqueue_telemetry(context, message);
    } else if (event.type == BridgeEventType::kNetwork) {
        const bool online = event.payload.network.type == NetworkWifiEventType::kConnected &&
                            event.payload.network.state == NetworkWifiState::kOnline;
        const bool offline = event.payload.network.type == NetworkWifiEventType::kDisconnected;
        if (online) {
            context->network_online.store(true, std::memory_order_release);
            taskENTER_CRITICAL(&s_status_lock);
            s_status.network_ready = true;
            s_status.client_start_pending = !context->client_started.load(std::memory_order_relaxed);
            taskEXIT_CRITICAL(&s_status_lock);
            notify_worker(context, kWakeNetwork);
        } else if (offline) {
            context->network_online.store(false, std::memory_order_release);
            taskENTER_CRITICAL(&s_status_lock);
            s_status.network_ready = false;
            taskEXIT_CRITICAL(&s_status_lock);
        }
    }
    return true;
}

void destroy_partial_runtime(RuntimeContext *context)
{
    if (context == nullptr) {
        return;
    }
    if (context->bridge_sink_registered) {
        (void)bridge_events_remove_sink(context->bridge_sink_id);
        context->bridge_sink_registered = false;
    }
    if (context->worker_task != nullptr) {
        vTaskDelete(context->worker_task);
        context->worker_task = nullptr;
    }
    if (context->client != nullptr) {
        if (context->client_started.load(std::memory_order_acquire)) {
            (void)esp_mqtt_client_stop(context->client);
        }
        (void)esp_mqtt_client_destroy(context->client);
        context->client = nullptr;
    }
    if (context->command_queue != nullptr) {
        vQueueDelete(context->command_queue);
        context->command_queue = nullptr;
    }
    if (context->telemetry_queue != nullptr) {
        vQueueDelete(context->telemetry_queue);
        context->telemetry_queue = nullptr;
    }
    if (context->mqtt_stack_reserve != nullptr) {
        heap_caps_free(context->mqtt_stack_reserve);
        context->mqtt_stack_reserve = nullptr;
    }
    if (context->mqtt_tcb_reserve != nullptr) {
        heap_caps_free(context->mqtt_tcb_reserve);
        context->mqtt_tcb_reserve = nullptr;
    }
    delete context;
}

void refresh_persisted_status(const MqttServiceConfig *config, esp_err_t error)
{
    set_profile_status(config, error);
    MqttAdvertisedLedger ledger{};
    const esp_err_t ledger_error = load_mqtt_advertised_ledger(&ledger);
    update_ledger_status(ledger_error, ledger.count, ledger.rule_count);
}

}  // namespace

const char *network_service_profile_name(NetworkServiceProfile profile)
{
    return profile == NetworkServiceProfile::kMqtt ? "mqtt" : "web";
}

const char *network_mqtt_discovery_state_name(NetworkMqttDiscoveryState state)
{
    switch (state) {
        case NetworkMqttDiscoveryState::kStopped: return "stopped";
        case NetworkMqttDiscoveryState::kDisconnected: return "disconnected";
        case NetworkMqttDiscoveryState::kSubscribing: return "subscribing";
        case NetworkMqttDiscoveryState::kPublishingAvailability: return "availability";
        case NetworkMqttDiscoveryState::kReconciling: return "reconciling";
        case NetworkMqttDiscoveryState::kReady: return "ready";
        case NetworkMqttDiscoveryState::kRetiring: return "retiring";
        case NetworkMqttDiscoveryState::kFaulted: return "faulted";
    }
    return "invalid";
}

esp_err_t initialize_network_service_profile()
{
    bool expected = false;
    if (!s_profile_initialized.compare_exchange_strong(expected, true,
                                                        std::memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_status_lock);
    s_status = {};
    s_status.profile_initialized = true;
    s_status.effective_profile = NetworkServiceProfile::kWeb;
    s_status.runtime_error = ESP_OK;
    taskEXIT_CRITICAL(&s_status_lock);

    MqttServiceConfig config{};
    esp_err_t error = load_mqtt_service_config(&config);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        refresh_persisted_status(nullptr, ESP_OK);
        return ESP_OK;
    }
    if (error != ESP_OK) {
        refresh_persisted_status(nullptr, error);
        return error;
    }
    if (config.state == MqttServiceState::kRetired) {
        error = finish_retired_cleanup();
        std::memset(&config, 0, sizeof(config));
        refresh_persisted_status(nullptr, error);
        return error;
    }
    refresh_persisted_status(&config, ESP_OK);
    std::memset(&config, 0, sizeof(config));
    return ESP_OK;
}

NetworkServiceProfile requested_network_service_profile()
{
    taskENTER_CRITICAL(&s_status_lock);
    const NetworkServiceProfile profile = s_status.requested_profile;
    taskEXIT_CRITICAL(&s_status_lock);
    return profile;
}

esp_err_t prepare_network_mqtt()
{
    if (!s_profile_initialized.load(std::memory_order_acquire) || s_context != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    MqttServiceConfig service{};
    esp_err_t error = load_mqtt_service_config(&service);
    if (error != ESP_OK ||
        (service.state != MqttServiceState::kMqtt &&
         service.state != MqttServiceState::kRetiring)) {
        std::memset(&service, 0, sizeof(service));
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }

    auto *context = new (std::nothrow) RuntimeContext{};
    if (context == nullptr) {
        std::memset(&service, 0, sizeof(service));
        return ESP_ERR_NO_MEM;
    }
    context->broker_ipv4 = service.broker_ipv4;
    context->port = service.port;
    context->boot_generation = service.generation;
    context->retirement_requested.store(service.state == MqttServiceState::kRetiring,
                                         std::memory_order_release);
    uint8_t station_mac[6]{};
    error = esp_read_mac(station_mac, ESP_MAC_WIFI_STA);
    if (error != ESP_OK ||
        !derive_mqtt_device_identity(station_mac, &context->identity) ||
        !format_mqtt_command_filter(context->identity, context->command_filter,
                                    sizeof(context->command_filter)) ||
        !format_mqtt_automation_command_topic(context->identity,
                                              MqttAutomationCommandKind::kEnabled,
                                              context->enabled_command_topic,
                                              sizeof(context->enabled_command_topic)) ||
        !format_mqtt_automation_command_topic(context->identity,
                                              MqttAutomationCommandKind::kLogMode,
                                              context->log_command_topic,
                                              sizeof(context->log_command_topic)) ||
        !format_mqtt_availability_topic(context->identity, context->availability_topic,
                                        sizeof(context->availability_topic))) {
        std::memset(&service, 0, sizeof(service));
        destroy_partial_runtime(context);
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }
    context->command_queue = xQueueCreate(kCommandQueueDepth, sizeof(MqttCommand));
    context->telemetry_queue = xQueueCreate(kTelemetryQueueDepth, sizeof(MqttTelemetryMessage));
    if (context->command_queue == nullptr || context->telemetry_queue == nullptr) {
        std::memset(&service, 0, sizeof(service));
        destroy_partial_runtime(context);
        return ESP_ERR_NO_MEM;
    }

    char broker[16]{};
    if (!format_mqtt_broker_ipv4(service.broker_ipv4, broker, sizeof(broker))) {
        std::memset(&service, 0, sizeof(service));
        destroy_partial_runtime(context);
        return ESP_ERR_INVALID_ARG;
    }
    esp_mqtt_client_config_t config{};
    config.broker.address.hostname = broker;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    config.broker.address.port = service.port;
    config.credentials.client_id = context->identity.client_id;
    config.credentials.username = service.username;
    config.credentials.authentication.password = service.password;
    config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    config.session.disable_clean_session = false;
    config.session.last_will.topic = context->availability_topic;
    config.session.last_will.msg = "offline";
    config.session.last_will.msg_len = 7;
    config.session.last_will.qos = 1;
    config.session.last_will.retain = 1;
    config.network.reconnect_timeout_ms = 10000;
    config.network.timeout_ms = 5000;
    config.network.disable_auto_reconnect = false;
    config.task.priority = 5;
    config.task.stack_size = kMqttTaskStackSize;
    config.buffer.size = 1024;
    config.buffer.out_size = 1024;
    config.outbox.limit = 2048;

    context->client = esp_mqtt_client_init(&config);
    std::memset(&service, 0, sizeof(service));
    std::memset(&config, 0, sizeof(config));
    if (context->client == nullptr) {
        destroy_partial_runtime(context);
        return ESP_ERR_NO_MEM;
    }
    error = esp_mqtt_client_register_event(context->client,
                                           static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                           mqtt_event_handler, context);
    if (error == ESP_OK) {
        context->mqtt_stack_reserve = heap_caps_malloc(kMqttTaskStackSize, kInternalHeapCaps);
        context->mqtt_tcb_reserve = heap_caps_malloc(sizeof(StaticTask_t), kInternalHeapCaps);
        if (context->mqtt_stack_reserve == nullptr || context->mqtt_tcb_reserve == nullptr) {
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error == ESP_OK &&
        xTaskCreate(worker_task, "mqtt_worker", kWorkerTaskStackSize, context,
                    kWorkerTaskPriority, &context->worker_task) != pdPASS) {
        error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        destroy_partial_runtime(context);
        return error;
    }
    s_context = context;
    taskENTER_CRITICAL(&s_status_lock);
    s_status.runtime_available = true;
    s_status.runtime_error = ESP_OK;
    s_status.effective_profile = NetworkServiceProfile::kMqtt;
    s_status.boot_generation = context->boot_generation;
    s_status.reboot_required = s_status.persisted_generation != s_status.boot_generation;
    s_status.discovery_state = NetworkMqttDiscoveryState::kDisconnected;
    s_status.network_ready = network_wifi_is_online();
    s_status.client_start_pending = true;
    taskEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(kTag,
             "Runtime prepared: context=%u heap free=%u minimum=%u largest=%u",
             static_cast<unsigned>(sizeof(RuntimeContext)),
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));
    return ESP_OK;
}

esp_err_t activate_network_mqtt()
{
    RuntimeContext *context = s_context;
    if (context == nullptr || context->bridge_sink_registered) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bridge_events_add_sink(mqtt_bridge_sink, context,
                                                   &context->bridge_sink_id);
    if (error != ESP_OK) {
        return error;
    }
    context->bridge_sink_registered = true;
    const bool network_online = network_wifi_is_online();
    context->network_online.store(network_online, std::memory_order_release);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.network_ready = network_online;
    taskEXIT_CRITICAL(&s_status_lock);
    context->activated.store(true, std::memory_order_release);
    notify_worker(context, kWakeCatalog | kWakeNetwork);
    return ESP_OK;
}

esp_err_t stop_network_mqtt_for_web_fallback()
{
    RuntimeContext *context = s_context;
    if (context == nullptr) {
        return ESP_OK;
    }
    s_context = nullptr;
    destroy_partial_runtime(context);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.runtime_available = false;
    s_status.connected = false;
    s_status.subscribed = false;
    s_status.discovery_state = NetworkMqttDiscoveryState::kStopped;
    s_status.client_start_pending = false;
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

void mark_network_service_web_fallback(esp_err_t error)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.effective_profile = NetworkServiceProfile::kWeb;
    s_status.current_boot_fallback = true;
    s_status.runtime_error = error;
    s_status.reboot_required = s_status.requested_profile != s_status.effective_profile;
    s_status.discovery_state = NetworkMqttDiscoveryState::kStopped;
    s_status.client_start_pending = false;
    taskEXIT_CRITICAL(&s_status_lock);
}

esp_err_t set_network_service_profile(NetworkServiceProfile profile)
{
    if (!s_profile_initialized.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    MqttServiceConfig config{};
    esp_err_t error = load_mqtt_service_config(&config);
    if (error == ESP_ERR_NVS_NOT_FOUND && profile == NetworkServiceProfile::kWeb) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (config.state == MqttServiceState::kRetiring ||
        config.state == MqttServiceState::kRetired) {
        std::memset(&config, 0, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }
    if (profile == NetworkServiceProfile::kMqtt && !mqtt_service_has_credentials(config)) {
        std::memset(&config, 0, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }
    const MqttServiceState requested = profile == NetworkServiceProfile::kMqtt
                                           ? MqttServiceState::kMqtt
                                           : MqttServiceState::kWeb;
    if (config.state != requested) {
        config.state = requested;
        config.generation = next_generation(config.generation);
        error = save_mqtt_service_config(config);
    }
    if (error == ESP_OK) {
        refresh_persisted_status(&config, ESP_OK);
    }
    std::memset(&config, 0, sizeof(config));
    return error;
}

esp_err_t configure_network_mqtt(uint32_t broker_ipv4, uint16_t port, const char *username,
                                 const char *password)
{
    if (!s_profile_initialized.load(std::memory_order_acquire) || username == nullptr ||
        password == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const std::size_t username_length = std::strlen(username);
    const std::size_t password_length = std::strlen(password);
    if (username_length == 0 || username_length >= kMqttUsernameCapacity ||
        password_length == 0 || password_length >= kMqttPasswordCapacity) {
        return ESP_ERR_INVALID_ARG;
    }
    MqttAdvertisedLedger ledger{};
    esp_err_t ledger_error = load_mqtt_advertised_ledger(&ledger);
    if (ledger_error != ESP_OK && ledger_error != ESP_ERR_NVS_NOT_FOUND) {
        return ledger_error;
    }
    if (ledger_error == ESP_OK &&
        (ledger.broker_ipv4 != broker_ipv4 || ledger.port != port)) {
        return ESP_ERR_INVALID_STATE;
    }

    MqttServiceConfig previous{};
    esp_err_t load_error = load_mqtt_service_config(&previous);
    if (load_error == ESP_OK &&
        (previous.state == MqttServiceState::kRetiring ||
         previous.state == MqttServiceState::kRetired)) {
        std::memset(&previous, 0, sizeof(previous));
        return ESP_ERR_INVALID_STATE;
    }
    MqttServiceConfig candidate{};
    candidate.state = load_error == ESP_OK ? previous.state : MqttServiceState::kWeb;
    candidate.broker_ipv4 = broker_ipv4;
    candidate.port = port;
    candidate.generation = next_generation(load_error == ESP_OK ? previous.generation : 0);
    copy_text(candidate.username, sizeof(candidate.username), username);
    copy_text(candidate.password, sizeof(candidate.password), password);
    std::memset(&previous, 0, sizeof(previous));
    if (!mqtt_service_config_is_valid(candidate)) {
        std::memset(&candidate, 0, sizeof(candidate));
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = save_mqtt_service_config(candidate);
    if (error == ESP_OK) {
        refresh_persisted_status(&candidate, ESP_OK);
    }
    std::memset(&candidate, 0, sizeof(candidate));
    return error;
}

esp_err_t forget_network_mqtt()
{
    if (!s_profile_initialized.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    MqttServiceConfig config{};
    esp_err_t error = load_mqtt_service_config(&config);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK || config.state == MqttServiceState::kRetired) {
        std::memset(&config, 0, sizeof(config));
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    MqttAdvertisedLedger ledger{};
    const esp_err_t ledger_error = load_mqtt_advertised_ledger(&ledger);
    if (ledger_error != ESP_OK && ledger_error != ESP_ERR_NVS_NOT_FOUND) {
        std::memset(&config, 0, sizeof(config));
        return ledger_error;
    }
    const bool has_ledger = ledger_error == ESP_OK;
    if (has_ledger && !mqtt_advertised_ledger_matches_endpoint(
                          ledger, config.broker_ipv4, config.port)) {
        std::memset(&config, 0, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }
    RuntimeContext *context = s_context;
    if (!has_ledger && context == nullptr) {
        error = erase_mqtt_advertised_ledger();
        if (error == ESP_OK) {
            error = erase_mqtt_service_config();
        }
        if (error == ESP_OK) {
            refresh_persisted_status(nullptr, ESP_OK);
        }
        std::memset(&config, 0, sizeof(config));
        return error;
    }
    if (context == nullptr) {
        std::memset(&config, 0, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }
    if (!context->connected.load(std::memory_order_acquire) ||
        config.generation != context->boot_generation ||
        config.broker_ipv4 != context->broker_ipv4 || config.port != context->port ||
        config.state != MqttServiceState::kMqtt) {
        std::memset(&config, 0, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }
    config.state = MqttServiceState::kRetiring;
    config.generation = next_generation(config.generation);
    error = save_mqtt_service_config(config);
    if (error == ESP_OK) {
        refresh_persisted_status(&config, ESP_OK);
        context->retirement_requested.store(true, std::memory_order_release);
        notify_worker(context, kWakeRetirement);
    }
    std::memset(&config, 0, sizeof(config));
    return error;
}

esp_err_t get_network_mqtt_status(NetworkMqttStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    status->heap_free = static_cast<uint32_t>(heap_caps_get_free_size(kInternalHeapCaps));
    status->heap_minimum =
        static_cast<uint32_t>(heap_caps_get_minimum_free_size(kInternalHeapCaps));
    status->heap_largest =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(kInternalHeapCaps));
    RuntimeContext *context = s_context;
    if (context != nullptr) {
        const uint32_t mqtt_margin =
            context->mqtt_stack_minimum_free.load(std::memory_order_relaxed);
        status->mqtt_stack_minimum_free = mqtt_margin == UINT32_MAX ? 0 : mqtt_margin;
        if (context->worker_task != nullptr) {
            status->worker_stack_minimum_free =
                static_cast<uint32_t>(uxTaskGetStackHighWaterMark(context->worker_task));
        }
        if (context->client != nullptr) {
            const int outbox = esp_mqtt_client_get_outbox_size(context->client);
            status->outbox_bytes = outbox < 0 ? 0 : static_cast<uint32_t>(outbox);
        }
    }
    return ESP_OK;
}

}  // namespace rfbridge
