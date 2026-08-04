#include "network_mqtt.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
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
#include "network_mqtt_storage.hpp"
#include "nvs.h"
#include "rf_signals.hpp"
#include "rf_storage.hpp"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

constexpr char kTag[] = "network_mqtt";
constexpr char kHomeAssistantStatusTopic[] = "homeassistant/status";
constexpr char kOnlinePayload[] = "online";
constexpr uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kMqttTaskStackSize = 6144;
constexpr uint32_t kWorkerTaskStackSize = 4096;
constexpr UBaseType_t kWorkerTaskPriority = 3;
constexpr UBaseType_t kCommandQueueDepth = 8;
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

static_assert(CONFIG_RF_MAX_LEARNED_SIGNALS <= kMqttMaximumAdvertisedSignals);

struct RuntimeContext {
    MqttDeviceIdentity identity{};
    esp_mqtt_client_handle_t client = nullptr;
    QueueHandle_t command_queue = nullptr;
    TaskHandle_t worker_task = nullptr;
    uint32_t broker_ipv4 = 0;
    uint32_t boot_generation = 0;
    uint16_t port = 0;
    uint8_t bridge_sink_id = 0;
    bool bridge_sink_registered = false;
    std::atomic<bool> activated{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> subscribed{false};
    std::atomic<bool> retirement_requested{false};
    std::atomic<bool> birth_pending{false};
    std::atomic<bool> catalog_pending{false};
    std::atomic<int> last_published_id{0};
    std::atomic<int> last_deleted_id{0};
    std::atomic<int> last_subscribed_id{0};
    std::atomic<bool> last_subscription_ok{false};
    std::atomic<uint32_t> mqtt_stack_minimum_free{UINT32_MAX};
    MqttAdvertisedLedger current{};
    MqttAdvertisedLedger ledger{};
    MqttAdvertisedLedger merged{};
    char command_filter[kMqttTopicCapacity]{};
    char availability_topic[kMqttTopicCapacity]{};
    char topic[kMqttTopicCapacity]{};
    char payload[kMqttDiscoveryPayloadCapacity]{};
};

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

void update_ledger_status(esp_err_t error, uint8_t count)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.ledger_known = error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND;
    s_status.advertised_count = error == ESP_OK ? count : 0;
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
    if (left.count != right.count) {
        return false;
    }
    for (std::size_t index = 0; index < left.count; ++index) {
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
    taskENTER_CRITICAL(&s_status_lock);
    s_status.current_count = context->current.count;
    taskEXIT_CRITICAL(&s_status_lock);
    return mqtt_advertised_ledger_is_valid(context->current) ? ESP_OK
                                                             : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t load_ledger(RuntimeContext *context)
{
    context->ledger = {};
    const esp_err_t error = load_mqtt_advertised_ledger(&context->ledger);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        context->ledger.broker_ipv4 = context->broker_ipv4;
        context->ledger.port = context->port;
        update_ledger_status(error, 0);
        return ESP_OK;
    }
    update_ledger_status(error, context->ledger.count);
    if (error != ESP_OK) {
        return error;
    }
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
            RfStorageName command{};
            if (!parse_mqtt_button_command(context->identity, message, command.value)) {
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
    RfStorageName command{};
    while (context->command_queue != nullptr &&
           xQueueReceive(context->command_queue, &command, 0) == pdTRUE) {
        if (context->retirement_requested.load(std::memory_order_acquire)) {
            continue;
        }
        const esp_err_t error = bridge_control_replay_named(
            command.value, CONFIG_RF_DEFAULT_TX_REPEATS, BridgeEventSource::kMqtt);
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "MQTT replay of %s failed: %s", command.value,
                     esp_err_to_name(error));
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
    };
    const int message_id = esp_mqtt_client_subscribe_multiple(context->client, topics, 2);
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

esp_err_t reconcile_discovery(RuntimeContext *context, bool force_configs)
{
    set_discovery_state(NetworkMqttDiscoveryState::kReconciling);
    esp_err_t error = load_current_signals(context);
    if (error == ESP_OK) {
        error = load_ledger(context);
    }
    if (error == ESP_OK && !force_configs &&
        ledgers_have_same_names(context->ledger, context->current)) {
        set_discovery_state(NetworkMqttDiscoveryState::kReady);
        ESP_LOGI(kTag,
                 "Discovery ready: signals=%u heap free=%u minimum=%u largest=%u "
                 "worker stack min=%u",
                 context->current.count,
                 static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(kInternalHeapCaps)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        return ESP_OK;
    }
    if (error == ESP_OK) {
        error = build_merged_ledger(context);
    }
    while (error == ESP_ERR_INVALID_SIZE) {
        std::size_t stale_index = context->ledger.count;
        for (std::size_t index = 0; index < context->ledger.count; ++index) {
            if (!ledger_contains(context->current, context->ledger.names[index].value)) {
                stale_index = index;
                break;
            }
        }
        if (stale_index >= context->ledger.count) {
            break;
        }
        error = publish_discovery_tombstone(context,
                                            context->ledger.names[stale_index].value);
        MqttAdvertisedLedger reduced = context->ledger;
        for (std::size_t index = stale_index + 1U; index < reduced.count; ++index) {
            reduced.names[index - 1U] = reduced.names[index];
        }
        if (reduced.count > 0) {
            --reduced.count;
            reduced.names[reduced.count] = {};
        }
        if (error == ESP_OK) {
            error = reduced.count == 0 ? erase_mqtt_advertised_ledger()
                                       : save_mqtt_advertised_ledger(reduced);
        }
        if (error == ESP_OK) {
            context->ledger = reduced;
            update_ledger_status(reduced.count == 0 ? ESP_ERR_NVS_NOT_FOUND : ESP_OK,
                                 reduced.count);
            error = build_merged_ledger(context);
        }
    }
    if (error == ESP_OK &&
        !ledgers_have_same_names(context->merged, context->ledger)) {
        error = save_mqtt_advertised_ledger(context->merged);
        if (error == ESP_OK) {
            context->ledger = context->merged;
            update_ledger_status(ESP_OK, context->ledger.count);
        }
    }
    for (std::size_t index = 0; error == ESP_OK && index < context->ledger.count; ++index) {
        if (!ledger_contains(context->current, context->ledger.names[index].value)) {
            error = publish_discovery_tombstone(context, context->ledger.names[index].value);
        }
    }
    for (std::size_t index = 0; error == ESP_OK && index < context->current.count; ++index) {
        error = publish_discovery_config(context, context->current.names[index].value);
    }
    if (error == ESP_OK && !ledgers_have_same_names(context->ledger, context->current)) {
        error = context->current.count == 0
                    ? erase_mqtt_advertised_ledger()
                    : save_mqtt_advertised_ledger(context->current);
    }
    if (error == ESP_OK) {
        context->ledger = context->current;
        update_ledger_status(context->current.count == 0 ? ESP_ERR_NVS_NOT_FOUND : ESP_OK,
                             context->current.count);
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
    for (std::size_t index = 0; error == ESP_OK && index < context->ledger.count; ++index) {
        error = publish_discovery_tombstone(context, context->ledger.names[index].value);
    }
    if (error == ESP_OK) {
        error = wait_for_publish(context, context->availability_topic, "", 0);
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
        s_status.ledger_known = true;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    return error;
}

void stop_client_from_worker(RuntimeContext *context)
{
    if (context->client != nullptr) {
        (void)esp_mqtt_client_disconnect(context->client);
        vTaskDelay(pdMS_TO_TICKS(50));
        (void)esp_mqtt_client_stop(context->client);
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
    taskEXIT_CRITICAL(&s_status_lock);
}

void worker_task(void *argument)
{
    auto *context = static_cast<RuntimeContext *>(argument);
    TickType_t last_audit = xTaskGetTickCount();
    bool connection_ready = false;
    while (true) {
        service_commands(context);
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
        bool reconcile = context->catalog_pending.exchange(false, std::memory_order_acq_rel);
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
        if (now - last_audit >= kAuditInterval) {
            reconcile = true;
            last_audit = now;
        }
        if (reconcile && context->connected.load(std::memory_order_acquire)) {
            (void)reconcile_discovery(context, force);
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
        (void)esp_mqtt_client_stop(context->client);
        (void)esp_mqtt_client_destroy(context->client);
        context->client = nullptr;
    }
    if (context->command_queue != nullptr) {
        vQueueDelete(context->command_queue);
        context->command_queue = nullptr;
    }
    delete context;
}

void refresh_persisted_status(const MqttServiceConfig *config, esp_err_t error)
{
    set_profile_status(config, error);
    MqttAdvertisedLedger ledger{};
    const esp_err_t ledger_error = load_mqtt_advertised_ledger(&ledger);
    update_ledger_status(ledger_error, ledger.count);
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
        !format_mqtt_availability_topic(context->identity, context->availability_topic,
                                        sizeof(context->availability_topic))) {
        std::memset(&service, 0, sizeof(service));
        destroy_partial_runtime(context);
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }
    context->command_queue = xQueueCreate(kCommandQueueDepth, sizeof(RfStorageName));
    if (context->command_queue == nullptr) {
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
        error = esp_mqtt_client_start(context->client);
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
    context->activated.store(true, std::memory_order_release);
    notify_worker(context, kWakeCatalog);
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
    if (ledger_error == ESP_OK && ledger.count > 0 &&
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
    const bool has_ledger = ledger_error == ESP_OK && ledger.count > 0;
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
