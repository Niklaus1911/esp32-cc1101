#include "rf_signals.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rf_automation.hpp"
#include "rf_storage_format.hpp"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

constexpr int64_t kLearnTimeoutUs = 30000000;
constexpr TickType_t kMutexWait = pdMS_TO_TICKS(1000);
constexpr TickType_t kEnqueueWait = pdMS_TO_TICKS(100);
constexpr std::size_t kQueueDepth = 8;
constexpr uint32_t kTaskStackSize = 10240;
constexpr UBaseType_t kTaskPriority = 4;

static_assert(CONFIG_RF_MAX_LEARNED_SIGNALS >= 1);

enum class MessageType : uint8_t {
    kFrame,
    kArm,
    kCancel,
};

struct Message {
    MessageType type = MessageType::kFrame;
    BridgeEventSource source = BridgeEventSource::kSystem;
    uint32_t operation_id = 0;
    int64_t occurred_us = 0;
    RfFrame frame{};
    char name[kRfStorageNameCapacity]{};
};

struct PendingLearn {
    bool active = false;
    int64_t armed_us = 0;
    int64_t deadline_us = 0;
    uint32_t drops_at_arm = 0;
    BridgeEventSource source = BridgeEventSource::kSystem;
    uint32_t operation_id = 0;
    char name[kRfStorageNameCapacity]{};
};

SemaphoreHandle_t s_mutex = nullptr;
SemaphoreHandle_t s_catalog_mutex = nullptr;
SemaphoreHandle_t s_recent_mutex = nullptr;
QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
std::atomic_flag s_initializing = ATOMIC_FLAG_INIT;
std::array<LearnedSignalEntry, CONFIG_RF_MAX_LEARNED_SIGNALS> s_catalog{};
std::size_t s_catalog_count = 0;
bool s_catalog_available = false;
RfSignalsStatus s_status{};
std::atomic<bool> s_available{false};
std::atomic<uint32_t> s_queue_drops{0};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};

class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex)
        : mutex_(mutex), locked_(mutex != nullptr && xSemaphoreTake(mutex, kMutexWait) == pdTRUE)
    {
    }
    ~Lock()
    {
        if (locked_) {
            xSemaphoreGive(mutex_);
        }
    }
    bool locked() const { return locked_; }

private:
    SemaphoreHandle_t mutex_;
    bool locked_;
};

void copy_name(char *destination, const char *source)
{
    std::strncpy(destination, source, kRfStorageNameCapacity - 1U);
    destination[kRfStorageNameCapacity - 1U] = '\0';
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

void set_learning_status(RfLearningState state, const char *name, esp_err_t result)
{
    Lock lock(s_mutex);
    if (!lock.locked()) {
        return;
    }
    s_status.learning_state = state;
    s_status.learning_result = result;
    s_status.learning_armed = state == RfLearningState::kArmed;
    s_status.pending_name[0] = '\0';
    s_status.learning_name[0] = '\0';
    if (name != nullptr) {
        copy_name(s_status.learning_name, name);
        if (s_status.learning_armed) {
            copy_name(s_status.pending_name, name);
        }
    }
    ++s_status.learning_revision;
}

void publish_learn(BridgeEventType type, const PendingLearn &pending, esp_err_t result = ESP_OK,
                   const char *replacement = nullptr)
{
    BridgeEvent event{};
    event.type = type;
    event.source = pending.source;
    event.operation_id = pending.operation_id;
    event.result = result;
    copy_name(event.payload.rf.name, pending.name);
    if (replacement != nullptr) {
        copy_name(event.payload.rf.target, replacement);
    }
    bridge_events_publish(event);
}

void publish_catalog_changed(const char *name, BridgeEventSource source)
{
    BridgeEvent event{};
    event.type = BridgeEventType::kSignalCatalogChanged;
    event.source = source;
    copy_name(event.payload.rf.name, name);
    bridge_events_publish(event);
}

esp_err_t save_stored_signal(const char *name, const RfStoredSignal &stored,
                             BridgeEventSource source)
{
    if (!rf_storage_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = rf_storage_create(name, stored);
    if (error != ESP_OK) {
        return error;
    }
    (void)rf_signals_refresh_catalog();
    publish_catalog_changed(name, source);
    return ESP_OK;
}

LearnedMatch match_stored_signal(const RfStoredSignal &stored)
{
    Lock lock(s_catalog_mutex);
    if (!lock.locked()) {
        LearnedMatch unavailable{};
        unavailable.kind = LearnedMatchKind::kUnavailable;
        return unavailable;
    }
    return find_learned_signal_match(stored, s_catalog.data(), s_catalog_count,
                                     s_catalog_available);
}

void publish_frame(const RfFrame &frame, int64_t occurred_us)
{
    BridgeEvent event{};
    event.type = BridgeEventType::kRx;
    event.occurred_us = occurred_us;
    event.payload.rf.frame = frame;
    event.payload.rf.learned = match_stored_signal(stored_signal_from_frame(frame));
    bridge_events_publish(event);
}

void update_recent_status(esp_err_t error, bool changed, std::size_t count)
{
    Lock lock(s_mutex);
    if (!lock.locked()) {
        return;
    }
    s_status.recent_available = rf_storage_recent_initialization_error() == ESP_OK;
    s_status.recent_last_error = error;
    if (error == ESP_OK) {
        s_status.recent_count = static_cast<uint8_t>(
            std::min<std::size_t>(count, kRfRecentSignalCapacity));
        if (changed) {
            ++s_status.recent_revision;
        }
    } else {
        ++s_status.recent_errors;
    }
}

void record_recent_decoded(const RfFrame &frame)
{
    if (frame.encoding != RfEncoding::kDecoded) {
        return;
    }
    Lock recent_lock(s_recent_mutex);
    if (!recent_lock.locked()) {
        update_recent_status(ESP_ERR_TIMEOUT, false, 0);
        return;
    }
    const esp_err_t error = rf_storage_recent_append(frame.decoded);
    std::size_t count = 0;
    if (error == ESP_OK) {
        const esp_err_t list_error = rf_storage_recent_list(nullptr, 0, &count);
        if (list_error != ESP_OK) {
            update_recent_status(list_error, false, 0);
            return;
        }
    }
    update_recent_status(error, error == ESP_OK, count);
}

void clear_pending(PendingLearn *pending, RfLearningState state, esp_err_t result)
{
    char name[kRfStorageNameCapacity]{};
    copy_name(name, pending->name);
    *pending = {};
    set_learning_status(state, name, result);
}

void process_arm(const Message &message, PendingLearn *pending)
{
    RfRadioStatus radio{};
    const esp_err_t radio_error = get_rf_radio_status(&radio);
    if (radio_error != ESP_OK || !radio.running || !radio.receive_enabled) {
        PendingLearn failed{};
        failed.source = message.source;
        failed.operation_id = message.operation_id;
        copy_name(failed.name, message.name);
        const esp_err_t error = radio_error == ESP_OK ? ESP_ERR_INVALID_STATE : radio_error;
        if (!pending->active) {
            set_learning_status(RfLearningState::kFailed, failed.name, error);
        }
        publish_learn(BridgeEventType::kLearnFailed, failed, error);
        return;
    }
    bool exists = false;
    const esp_err_t exists_error = rf_storage_exists(message.name, &exists);
    if (exists_error != ESP_OK || exists) {
        PendingLearn failed{};
        failed.source = message.source;
        failed.operation_id = message.operation_id;
        copy_name(failed.name, message.name);
        const esp_err_t error = exists ? ESP_ERR_INVALID_STATE : exists_error;
        if (!pending->active) {
            set_learning_status(RfLearningState::kFailed, failed.name, error);
        }
        publish_learn(BridgeEventType::kLearnFailed, failed, error);
        return;
    }
    if (pending->active) {
        const PendingLearn replaced = *pending;
        publish_learn(BridgeEventType::kLearnReplaced, replaced, ESP_OK, message.name);
    }
    *pending = {};
    pending->active = true;
    pending->source = message.source;
    pending->operation_id = message.operation_id;
    pending->armed_us = esp_timer_get_time();
    pending->deadline_us = pending->armed_us + kLearnTimeoutUs;
    pending->drops_at_arm = s_queue_drops.load(std::memory_order_acquire);
    copy_name(pending->name, message.name);
    set_learning_status(RfLearningState::kArmed, pending->name, ESP_OK);
    publish_learn(BridgeEventType::kLearnArmed, *pending);
}

void process_frame(const Message &message, PendingLearn *pending)
{
    if (!pending->active) {
        publish_frame(message.frame, message.occurred_us);
        return;
    }
    const RfLearnFrameDisposition disposition = classify_learn_frame_window(
        pending->armed_us, pending->deadline_us, message.occurred_us, message.frame.captured_us);
    if (disposition == RfLearnFrameDisposition::kIgnore) {
        publish_frame(message.frame, message.occurred_us);
        return;
    }
    if (disposition == RfLearnFrameDisposition::kTimeout) {
        const PendingLearn expired = *pending;
        clear_pending(pending, RfLearningState::kTimedOut, ESP_ERR_TIMEOUT);
        publish_learn(BridgeEventType::kLearnTimeout, expired, ESP_ERR_TIMEOUT);
        publish_frame(message.frame, message.occurred_us);
        return;
    }

    const PendingLearn captured = *pending;
    *pending = {};
    const esp_err_t create_error = save_stored_signal(
        captured.name, stored_signal_from_frame(message.frame), captured.source);
    publish_frame(message.frame, message.occurred_us);
    if (create_error == ESP_OK) {
        set_learning_status(RfLearningState::kCompleted, captured.name, ESP_OK);
        publish_learn(BridgeEventType::kLearnCompleted, captured);
    } else {
        set_learning_status(RfLearningState::kFailed, captured.name, create_error);
        publish_learn(BridgeEventType::kLearnFailed, captured, create_error);
    }
}

TickType_t next_wait(const PendingLearn &pending)
{
    if (!pending.active) {
        return portMAX_DELAY;
    }
    const int64_t remaining_us = pending.deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    const uint64_t remaining_ms = (static_cast<uint64_t>(remaining_us) + 999U) / 1000U;
    return std::max<TickType_t>(1, pdMS_TO_TICKS(static_cast<uint32_t>(remaining_ms)));
}

void service_task(void *)
{
    PendingLearn pending{};
    while (true) {
        if (pending.active &&
            s_queue_drops.load(std::memory_order_acquire) != pending.drops_at_arm) {
            const PendingLearn cancelled = pending;
            clear_pending(&pending, RfLearningState::kFailed, ESP_ERR_INVALID_SIZE);
            publish_learn(BridgeEventType::kLearnFailed, cancelled, ESP_ERR_INVALID_SIZE);
        }
        Message message{};
        if (xQueueReceive(s_queue, &message, next_wait(pending)) == pdTRUE) {
            if (message.type == MessageType::kFrame) {
                process_frame(message, &pending);
                record_recent_decoded(message.frame);
            } else if (message.type == MessageType::kArm) {
                process_arm(message, &pending);
            } else if (pending.active) {
                const PendingLearn cancelled = pending;
                clear_pending(&pending, RfLearningState::kCancelled, ESP_OK);
                publish_learn(BridgeEventType::kLearnCancelled, cancelled);
            }
            continue;
        }
        if (pending.active && esp_timer_get_time() >= pending.deadline_us) {
            const PendingLearn expired = pending;
            clear_pending(&pending, RfLearningState::kTimedOut, ESP_ERR_TIMEOUT);
            publish_learn(BridgeEventType::kLearnTimeout, expired, ESP_ERR_TIMEOUT);
        }
    }
}

void destroy_initialization_resources()
{
    if (s_queue != nullptr) {
        vQueueDelete(s_queue);
        s_queue = nullptr;
    }
    if (s_catalog_mutex != nullptr) {
        vSemaphoreDelete(s_catalog_mutex);
        s_catalog_mutex = nullptr;
    }
    if (s_recent_mutex != nullptr) {
        vSemaphoreDelete(s_recent_mutex);
        s_recent_mutex = nullptr;
    }
    if (s_mutex != nullptr) {
        vSemaphoreDelete(s_mutex);
        s_mutex = nullptr;
    }
    s_task = nullptr;
    s_catalog = {};
    s_catalog_count = 0;
    s_catalog_available = false;
}

}  // namespace

esp_err_t initialize_rf_signals()
{
    if (s_available.load(std::memory_order_acquire) ||
        s_initializing.test_and_set(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mutex = xSemaphoreCreateMutex();
    s_catalog_mutex = xSemaphoreCreateMutex();
    s_recent_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(kQueueDepth, sizeof(Message));
    if (s_mutex == nullptr || s_catalog_mutex == nullptr || s_recent_mutex == nullptr ||
        s_queue == nullptr) {
        destroy_initialization_resources();
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        s_initializing.clear(std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    (void)rf_signals_refresh_catalog();
    std::size_t recent_count = 0;
    const esp_err_t recent_error = rf_storage_recent_list(nullptr, 0, &recent_count);
    update_recent_status(recent_error, false, recent_count);
    if (xTaskCreate(service_task, "rf_signals", kTaskStackSize, nullptr, kTaskPriority, &s_task) !=
        pdPASS) {
        destroy_initialization_resources();
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        s_initializing.clear(std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    {
        Lock lock(s_mutex);
        if (lock.locked()) {
            s_status.available = true;
            s_status.initialization_error = ESP_OK;
        }
    }
    s_initialization_error.store(ESP_OK, std::memory_order_release);
    s_available.store(true, std::memory_order_release);
    s_initializing.clear(std::memory_order_release);
    return ESP_OK;
}

void rf_signals_on_frame(const RfFrame &frame, void *)
{
    rf_automation_on_frame(frame);
    if (!s_available.load(std::memory_order_acquire) || s_queue == nullptr) {
        return;
    }
    Message message{};
    message.type = MessageType::kFrame;
    message.occurred_us = esp_timer_get_time();
    message.frame = frame;
    if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        s_queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

esp_err_t rf_signals_arm_learning(const char *name, BridgeEventSource source, uint32_t operation_id)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    if (!rf_storage_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    Message message{};
    message.type = MessageType::kArm;
    message.source = source;
    message.operation_id = operation_id;
    copy_name(message.name, name);
    return xQueueSend(s_queue, &message, kEnqueueWait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t rf_signals_cancel_learning(BridgeEventSource source, uint32_t operation_id)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    Message message{};
    message.type = MessageType::kCancel;
    message.source = source;
    message.operation_id = operation_id;
    return xQueueSend(s_queue, &message, kEnqueueWait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t rf_signals_forget(const char *name)
{
    const esp_err_t error = rf_storage_forget(name);
    if (error != ESP_OK) {
        return error;
    }
    const esp_err_t refresh_error = rf_signals_refresh_catalog();
    publish_catalog_changed(name, BridgeEventSource::kSystem);
    return refresh_error;
}

esp_err_t rf_signals_save_decoded(const char *name, const DecodedSignal &decoded,
                                  BridgeEventSource source)
{
    RfStoredSignal stored{};
    stored.encoding = RfStoredEncoding::kDecoded;
    stored.decoded = decoded;
    return save_stored_signal(name, stored, source);
}

esp_err_t rf_signals_save_recent(uint64_t id, const char *name, BridgeEventSource source)
{
    if (id == 0 || !rf_storage_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    RfRecentSignal recent{};
    ESP_RETURN_ON_ERROR(rf_storage_recent_load(id, &recent), "rf_signals",
                        "load recent signal");
    RfStoredSignal stored{};
    stored.encoding = RfStoredEncoding::kDecoded;
    stored.decoded = recent.decoded;
    return save_stored_signal(name, stored, source);
}

esp_err_t rf_signals_clear_recent()
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    Lock recent_lock(s_recent_mutex);
    if (!recent_lock.locked()) {
        update_recent_status(ESP_ERR_TIMEOUT, false, 0);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t error = rf_storage_recent_clear();
    update_recent_status(error, error == ESP_OK, 0);
    return error;
}

esp_err_t rf_signals_refresh_catalog()
{
    if (s_catalog_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock(s_catalog_mutex);
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    std::size_t count = 0;
    esp_err_t error = rf_storage_list(nullptr, 0, &count);
    if (error != ESP_OK) {
        s_catalog_available = false;
        s_catalog_count = 0;
    } else if (count > s_catalog.size()) {
        s_catalog_available = false;
        s_catalog_count = 0;
        error = ESP_ERR_INVALID_SIZE;
    } else if (count > 0) {
        std::array<RfStorageName, CONFIG_RF_MAX_LEARNED_SIGNALS> names{};
        std::size_t loaded_count = count;
        error = rf_storage_list(names.data(), names.size(), &loaded_count);
        if (error == ESP_OK && loaded_count != count) {
            error = ESP_ERR_INVALID_SIZE;
        }
        for (std::size_t index = 0; error == ESP_OK && index < count; ++index) {
            s_catalog[index].name = names[index];
            error = rf_storage_load(names[index].value, &s_catalog[index].signal);
        }
        s_catalog_available = error == ESP_OK;
        s_catalog_count = error == ESP_OK ? count : 0;
    } else {
        s_catalog_available = true;
        s_catalog_count = 0;
    }
    {
        Lock status_lock(s_mutex);
        if (status_lock.locked()) {
            s_status.catalog_available = s_catalog_available;
            s_status.learned_count = static_cast<uint16_t>(std::min<std::size_t>(count, UINT16_MAX));
            if (error != ESP_OK) {
                ++s_status.catalog_errors;
            }
        }
    }
    return error;
}

LearnedMatch rf_signals_match_frame(const RfFrame &frame)
{
    return match_stored_signal(stored_signal_from_frame(frame));
}

esp_err_t get_last_rf_frame_with_match(RfFrame *frame, LearnedMatch *match)
{
    if (frame == nullptr || match == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = get_last_rf_frame(frame);
    if (error == ESP_OK) {
        *match = rf_signals_match_frame(*frame);
    }
    return error;
}

esp_err_t get_rf_signals_status(RfSignalsStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available.load(std::memory_order_acquire)) {
        *status = {};
        status->queue_drops = s_queue_drops.load(std::memory_order_relaxed);
        status->initialization_error = s_initialization_error.load(std::memory_order_acquire);
        return ESP_OK;
    }
    Lock lock(s_mutex);
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_status;
    status->available = s_available.load(std::memory_order_acquire);
    status->queue_drops = s_queue_drops.load(std::memory_order_relaxed);
    status->initialization_error = s_initialization_error.load(std::memory_order_relaxed);
    return ESP_OK;
}

}  // namespace rfbridge
