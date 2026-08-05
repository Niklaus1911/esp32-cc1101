#include "bridge_events.hpp"

#include <array>
#include <atomic>
#include <type_traits>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_wifi.hpp"
#include "ota_update.hpp"
#include "rf_automation.hpp"

namespace rfbridge {
namespace {

constexpr TickType_t kMutexWait = pdMS_TO_TICKS(1000);
constexpr TickType_t kSinkQuiesceWait = pdMS_TO_TICKS(1000);
constexpr std::size_t kMaximumSinks = 2;
constexpr std::size_t kEventQueueDepth = 14;
constexpr uint32_t kDispatcherStackSize = 6144;
constexpr UBaseType_t kDispatcherPriority = 4;
static_assert(std::is_trivially_copyable_v<BridgeEvent>);
static_assert(sizeof(BridgeEvent) <= 672U);
static_assert(kEventQueueDepth * sizeof(BridgeEvent) <= 9U * 1024U);

struct SinkSlot {
    BridgeEventSink sink = nullptr;
    void *context = nullptr;
    uint32_t generation = 0;
    std::atomic<uint32_t> callbacks_in_flight{0};
};

SemaphoreHandle_t s_mutex = nullptr;
QueueHandle_t s_event_queue = nullptr;
TaskHandle_t s_dispatcher_task = nullptr;
std::array<SinkSlot, kMaximumSinks> s_sinks{};
std::atomic_flag s_initializing = ATOMIC_FLAG_INIT;
std::atomic<bool> s_available{false};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};
std::atomic<uint64_t> s_next_sequence{0};
std::atomic<uint64_t> s_published{0};
std::atomic<uint32_t> s_queue_drops{0};
std::atomic<uint32_t> s_sink_drops{0};

class BrokerLock {
public:
    BrokerLock() : locked_(s_mutex != nullptr && xSemaphoreTake(s_mutex, kMutexWait) == pdTRUE) {}
    ~BrokerLock()
    {
        if (locked_) {
            xSemaphoreGive(s_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};

bool automation_sink(const RfAutomationEvent &event, void *)
{
    BridgeEvent bridge{};
    bridge.type = BridgeEventType::kAutomation;
    bridge.source = BridgeEventSource::kAutomation;
    bridge.occurred_us = event.occurred_us;
    bridge.payload.set_automation(event);
    return bridge_events_publish(bridge);
}

bool network_sink(const NetworkWifiEvent &event, void *)
{
    BridgeEvent bridge{};
    bridge.type = BridgeEventType::kNetwork;
    bridge.payload.set_network(event);
    return bridge_events_publish(bridge);
}

bool ota_sink(const OtaUpdateEvent &event, void *)
{
    BridgeEvent bridge{};
    bridge.type = BridgeEventType::kOta;
    bridge.payload.set_ota(event);
    return bridge_events_publish(bridge);
}

void dispatch_event(BridgeEvent event)
{
    event.sequence = s_next_sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    s_published.fetch_add(1, std::memory_order_relaxed);
    for (SinkSlot &slot : s_sinks) {
        BridgeEventSink sink = nullptr;
        void *context = nullptr;
        {
            BrokerLock lock;
            if (!lock.locked()) {
                s_sink_drops.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            sink = slot.sink;
            context = slot.context;
            if (sink != nullptr) {
                slot.callbacks_in_flight.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        if (sink == nullptr) {
            continue;
        }
        if (!sink(event, context)) {
            s_sink_drops.fetch_add(1, std::memory_order_relaxed);
        }
        slot.callbacks_in_flight.fetch_sub(1, std::memory_order_release);
    }
}

void dispatcher_task(void *)
{
    while (true) {
        BridgeEvent event{};
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            dispatch_event(event);
        }
    }
}

}  // namespace

esp_err_t initialize_bridge_events()
{
    if (s_available.load(std::memory_order_acquire) ||
        s_initializing.test_and_set(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mutex = xSemaphoreCreateMutex();
    s_event_queue = xQueueCreate(kEventQueueDepth, sizeof(BridgeEvent));
    if (s_mutex == nullptr || s_event_queue == nullptr ||
        xTaskCreate(dispatcher_task, "bridge_events", kDispatcherStackSize, nullptr,
                    kDispatcherPriority, &s_dispatcher_task) != pdPASS) {
        if (s_event_queue != nullptr) {
            vQueueDelete(s_event_queue);
            s_event_queue = nullptr;
        }
        if (s_mutex != nullptr) {
            vSemaphoreDelete(s_mutex);
            s_mutex = nullptr;
        }
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        s_initializing.clear(std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    s_initialization_error.store(ESP_OK, std::memory_order_release);
    s_available.store(true, std::memory_order_release);
    s_initializing.clear(std::memory_order_release);
    (void)bridge_events_bind_available_sources();
    return ESP_OK;
}

esp_err_t bridge_events_bind_available_sources()
{
    if (!s_available.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    // Optional owners can initialize after the broker during MQTT-to-Web recovery.
    esp_err_t first_error = ESP_OK;
    const esp_err_t automation_error = rf_automation_set_event_sink(automation_sink, nullptr);
    if (automation_error != ESP_OK) {
        first_error = automation_error;
    }
    const esp_err_t network_error = set_network_wifi_event_sink(network_sink, nullptr);
    if (first_error == ESP_OK && network_error != ESP_OK) {
        first_error = network_error;
    }
    const esp_err_t ota_error = set_ota_update_event_sink(ota_sink, nullptr);
    if (first_error == ESP_OK && ota_error != ESP_OK) {
        first_error = ota_error;
    }
    return first_error;
}

esp_err_t bridge_events_add_sink(BridgeEventSink sink, void *context, uint8_t *sink_id)
{
    if (sink == nullptr || sink_id == nullptr || !s_available.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_ARG;
    }
    BrokerLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    for (std::size_t index = 0; index < s_sinks.size(); ++index) {
        SinkSlot &slot = s_sinks[index];
        if (slot.sink == nullptr && slot.callbacks_in_flight.load(std::memory_order_acquire) == 0) {
            ++slot.generation;
            slot.context = context;
            slot.sink = sink;
            *sink_id = static_cast<uint8_t>(index);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t bridge_events_remove_sink(uint8_t sink_id)
{
    if (sink_id >= s_sinks.size() || !s_available.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_ARG;
    }
    {
        BrokerLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        s_sinks[sink_id].sink = nullptr;
        s_sinks[sink_id].context = nullptr;
        ++s_sinks[sink_id].generation;
    }
    const TickType_t started = xTaskGetTickCount();
    while (s_sinks[sink_id].callbacks_in_flight.load(std::memory_order_acquire) != 0 &&
           xTaskGetTickCount() - started < kSinkQuiesceWait) {
        vTaskDelay(1);
    }
    return s_sinks[sink_id].callbacks_in_flight.load(std::memory_order_acquire) == 0
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

bool bridge_events_publish(BridgeEvent event)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return false;
    }
    if (event.occurred_us == 0) {
        event.occurred_us = esp_timer_get_time();
    }
    if (s_event_queue == nullptr || xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        s_queue_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

esp_err_t bridge_events_get_status(BridgeEventBrokerStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    BridgeEventBrokerStatus result{};
    result.available = s_available.load(std::memory_order_acquire);
    result.published = s_published.load(std::memory_order_relaxed);
    result.queue_drops = s_queue_drops.load(std::memory_order_relaxed);
    result.sink_drops = s_sink_drops.load(std::memory_order_relaxed);
    if (!result.available) {
        *status = result;
        return s_initialization_error.load(std::memory_order_acquire);
    }
    BrokerLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    for (const SinkSlot &slot : s_sinks) {
        if (slot.sink != nullptr) {
            ++result.sink_count;
        }
    }
    *status = result;
    return ESP_OK;
}

}  // namespace rfbridge
