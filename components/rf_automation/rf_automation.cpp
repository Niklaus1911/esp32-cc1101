#include "rf_automation.hpp"
#include "rf_automation_engine.hpp"

#include "rf_storage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace rfbridge {

using RfStorageRuleValidator = esp_err_t (*)(const RfStoredSignal &, const RfStoredSignal &, void *);
esp_err_t rf_storage_rule_create_validated(const char *, const RfStoredRule &, RfStorageRuleValidator, void *,
                                           RfStoredSignal *, RfStoredSignal *);
esp_err_t rf_storage_rule_remove(const char *);
esp_err_t rf_storage_rule_remove_recovery(const char *);
esp_err_t rf_storage_rule_list(RfStorageRuleEntry *, std::size_t, std::size_t *);
esp_err_t rf_storage_rule_name_list(RfStorageName *, std::size_t, std::size_t *);
esp_err_t rf_storage_rule_load(const char *, RfStoredRule *);
esp_err_t rf_storage_rule_enabled_get(bool *);
esp_err_t rf_storage_rule_enabled_set(bool);

namespace {

constexpr TickType_t kMutexTimeout = pdMS_TO_TICKS(7000);
constexpr uint32_t kTaskStackSize = 8192;
constexpr UBaseType_t kTaskPriority = 4;
constexpr std::size_t kFrameQueueDepth = 4;

struct RuntimeRule {
    RfStorageRuleEntry entry{};
    RfStoredSignal trigger_signal{};
    RfStoredSignal target_signal{};
    int64_t last_fired_us = 0;
};

struct FrameEvent {
    RfFrame frame{};
    uint32_t generation = 0;
};

struct AddValidationContext {
    const char *trigger_name = nullptr;
    const char *target_name = nullptr;
};

static_assert(std::is_trivially_copyable_v<RuntimeRule>);

std::array<RuntimeRule, CONFIG_RF_MAX_AUTOMATION_RULES> s_rules{};
std::array<RfStorageRuleEntry, CONFIG_RF_MAX_AUTOMATION_RULES> s_graph_scratch{};
std::size_t s_rule_count = 0;
SemaphoreHandle_t s_mutex = nullptr;
QueueHandle_t s_frame_queue = nullptr;
TaskHandle_t s_task = nullptr;
std::atomic<bool> s_initialization_started{false};
std::atomic<bool> s_available{false};
std::atomic<bool> s_enabled{false};
std::atomic<uint32_t> s_generation{0};
std::atomic<int64_t> s_generation_changed_us{0};
std::atomic<uint32_t> s_queue_drops{0};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};
RfAutomationStatus s_status{};

class AutomationLock {
public:
    AutomationLock() : locked_(s_mutex != nullptr && xSemaphoreTake(s_mutex, kMutexTimeout) == pdTRUE) {}
    ~AutomationLock()
    {
        if (locked_) {
            xSemaphoreGive(s_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};

RfStoredSignal stored_signal_from_frame(const RfFrame &frame)
{
    RfStoredSignal signal{};
    if (frame.encoding == RfEncoding::kDecoded) {
        signal.encoding = RfStoredEncoding::kDecoded;
        signal.decoded = frame.decoded;
    } else {
        signal.encoding = RfStoredEncoding::kRaw;
        signal.raw = frame.raw;
    }
    return signal;
}

void copy_name(char *destination, const char *source)
{
    std::strncpy(destination, source, kRfStorageNameCapacity - 1U);
    destination[kRfStorageNameCapacity - 1U] = '\0';
}

void clear_runtime_rules()
{
    std::memset(s_rules.data(), 0, sizeof(s_rules));
    s_rule_count = 0;
}

void advance_generation()
{
    s_generation_changed_us.store(esp_timer_get_time(), std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

esp_err_t build_canonical_graph(const RfStoredSignal *candidate_trigger = nullptr,
                                const char *candidate_trigger_name = nullptr)
{
    for (std::size_t index = 0; index < s_rule_count; ++index) {
        s_graph_scratch[index] = s_rules[index].entry;
        std::size_t trigger_matches = 0;
        for (std::size_t candidate = 0; candidate < s_rule_count; ++candidate) {
            if (rf_stored_signals_equivalent(s_rules[index].target_signal, s_rules[candidate].trigger_signal)) {
                ++trigger_matches;
                copy_name(s_graph_scratch[index].rule.target_name, s_rules[candidate].entry.trigger_name);
            }
        }
        if (candidate_trigger != nullptr && candidate_trigger_name != nullptr &&
            rf_stored_signals_equivalent(s_rules[index].target_signal, *candidate_trigger)) {
            ++trigger_matches;
            copy_name(s_graph_scratch[index].rule.target_name, candidate_trigger_name);
        }
        if (trigger_matches > 1) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

esp_err_t validate_new_rule_signals(const RfStoredSignal &trigger_signal, const RfStoredSignal &target_signal,
                                    void *opaque_context)
{
    const auto *context = static_cast<const AddValidationContext *>(opaque_context);
    if (context == nullptr || context->trigger_name == nullptr || context->target_name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rf_stored_signals_equivalent(trigger_signal, target_signal)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (std::size_t index = 0; index < s_rule_count; ++index) {
        if (std::strcmp(s_rules[index].entry.trigger_name, context->trigger_name) == 0 ||
            rf_stored_signals_equivalent(s_rules[index].trigger_signal, trigger_signal)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    ESP_RETURN_ON_ERROR(build_canonical_graph(&trigger_signal, context->trigger_name), "rf_automation",
                        "canonicalize existing graph");

    char canonical_target[kRfStorageNameCapacity]{};
    copy_name(canonical_target, context->target_name);
    std::size_t target_matches = 0;
    for (std::size_t index = 0; index < s_rule_count; ++index) {
        if (rf_stored_signals_equivalent(target_signal, s_rules[index].trigger_signal)) {
            ++target_matches;
            copy_name(canonical_target, s_rules[index].entry.trigger_name);
        }
    }
    if (target_matches > 1 ||
        rf_rule_would_create_cycle(s_graph_scratch.data(), s_rule_count, context->trigger_name, canonical_target)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void process_frame(const FrameEvent &event)
{
    AutomationLock lock;
    if (!lock.locked() || !s_available.load(std::memory_order_acquire) || !s_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const uint32_t generation = s_generation.load(std::memory_order_relaxed);
    const int64_t changed_us = s_generation_changed_us.load(std::memory_order_relaxed);
    if (!rf_automation_event_is_current(event.generation, generation, event.frame.captured_us, changed_us)) {
        ++s_status.stale_frames;
        return;
    }

    ++s_status.frames_seen;
    const RfStoredSignal incoming = stored_signal_from_frame(event.frame);
    std::array<const RfStoredSignal *, CONFIG_RF_MAX_AUTOMATION_RULES> triggers{};
    for (std::size_t index = 0; index < s_rule_count; ++index) {
        triggers[index] = &s_rules[index].trigger_signal;
    }
    std::size_t matched_index = 0;
    const RfAutomationMatchResult match =
        rf_find_unique_stored_signal_match(incoming, triggers.data(), s_rule_count, &matched_index);
    if (match == RfAutomationMatchResult::kAmbiguous) {
        ++s_status.ambiguous_frames;
        return;
    }
    if (match == RfAutomationMatchResult::kNone) {
        return;
    }
    RuntimeRule *matched = &s_rules[matched_index];

    ++s_status.matches;
    const int64_t action_us = esp_timer_get_time();
    if (!rf_automation_cooldown_allows(matched->last_fired_us, action_us, matched->entry.rule.cooldown_ms)) {
        ++s_status.cooldown_suppressed;
        return;
    }
    matched->last_fired_us = action_us;
    copy_name(s_status.last_trigger, matched->entry.trigger_name);
    copy_name(s_status.last_target, matched->entry.rule.target_name);
    s_status.last_error = ESP_OK;

    const esp_err_t error = matched->target_signal.encoding == RfStoredEncoding::kDecoded
                                ? transmit_rf_decoded(matched->target_signal.decoded, matched->entry.rule.repeats)
                                : transmit_rf_raw(matched->target_signal.raw, matched->entry.rule.repeats);
    s_status.last_error = error;
    if (error == ESP_OK) {
        ++s_status.actions_succeeded;
    } else {
        ++s_status.tx_errors;
    }
}

void automation_task(void *)
{
    FrameEvent event{};
    while (true) {
        if (xQueueReceive(s_frame_queue, &event, portMAX_DELAY) == pdTRUE) {
            process_frame(event);
        }
    }
}

esp_err_t load_runtime_rules()
{
    std::size_t count = 0;
    ESP_RETURN_ON_ERROR(rf_storage_rule_list(nullptr, 0, &count), "rf_automation", "count rules");
    if (count > s_rules.size()) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (count > 0) {
        ESP_RETURN_ON_ERROR(rf_storage_rule_list(s_graph_scratch.data(), s_graph_scratch.size(), &count),
                            "rf_automation", "load rules");
    }

    clear_runtime_rules();
    for (std::size_t index = 0; index < count; ++index) {
        RuntimeRule &runtime = s_rules[s_rule_count];
        std::memset(&runtime, 0, sizeof(runtime));
        runtime.entry = s_graph_scratch[index];
        if (std::strcmp(runtime.entry.trigger_name, runtime.entry.rule.target_name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_RETURN_ON_ERROR(rf_storage_load(runtime.entry.trigger_name, &runtime.trigger_signal), "rf_automation",
                            "load trigger signal");
        ESP_RETURN_ON_ERROR(rf_storage_load(runtime.entry.rule.target_name, &runtime.target_signal), "rf_automation",
                            "load target signal");
        if (rf_stored_signals_equivalent(runtime.trigger_signal, runtime.target_signal)) {
            return ESP_ERR_INVALID_STATE;
        }
        for (std::size_t previous = 0; previous < s_rule_count; ++previous) {
            if (rf_stored_signals_equivalent(runtime.trigger_signal, s_rules[previous].trigger_signal)) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        ++s_rule_count;
    }
    ESP_RETURN_ON_ERROR(build_canonical_graph(), "rf_automation", "canonicalize rule graph");
    return rf_rule_graph_has_cycle(s_graph_scratch.data(), s_rule_count) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void destroy_initialization_resources()
{
    if (s_frame_queue != nullptr) {
        vQueueDelete(s_frame_queue);
        s_frame_queue = nullptr;
    }
    if (s_mutex != nullptr) {
        vSemaphoreDelete(s_mutex);
        s_mutex = nullptr;
    }
    clear_runtime_rules();
}

} // namespace

esp_err_t initialize_rf_automation()
{
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    if (!rf_storage_is_available()) {
        const esp_err_t error = rf_storage_initialization_error();
        s_initialization_error.store(error, std::memory_order_release);
        return error;
    }

    s_mutex = xSemaphoreCreateMutex();
    s_frame_queue = xQueueCreate(kFrameQueueDepth, sizeof(FrameEvent));
    if (s_mutex == nullptr || s_frame_queue == nullptr) {
        destroy_initialization_resources();
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    bool enabled = true;
    esp_err_t error = rf_storage_rule_enabled_get(&enabled);
    if (error == ESP_OK) {
        error = load_runtime_rules();
    }
    if (error == ESP_OK) {
        s_status = {};
        s_status.available = true;
        s_status.enabled = enabled;
        s_status.enabled_known = true;
        s_status.rule_count_known = true;
        s_status.rule_count = static_cast<uint16_t>(s_rule_count);
        s_status.initialization_error = ESP_OK;
        s_queue_drops.store(0, std::memory_order_relaxed);
        s_enabled.store(enabled, std::memory_order_release);
        s_generation.store(1, std::memory_order_release);
        s_generation_changed_us.store(esp_timer_get_time(), std::memory_order_release);
        if (xTaskCreate(automation_task, "rf_auto", kTaskStackSize, nullptr, kTaskPriority, &s_task) != pdPASS) {
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error != ESP_OK) {
        s_enabled.store(false, std::memory_order_release);
        destroy_initialization_resources();
        s_initialization_error.store(error, std::memory_order_release);
        return error;
    }

    s_available.store(true, std::memory_order_release);
    s_initialization_error.store(ESP_OK, std::memory_order_release);
    return ESP_OK;
}

esp_err_t rf_automation_add_rule(const char *trigger_name, const char *target_name, uint8_t repeats)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    if (!rf_storage_name_is_valid(trigger_name) || !rf_storage_name_is_valid(target_name) || repeats < 1 ||
        repeats > 20 || std::strcmp(trigger_name, target_name) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    AutomationLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_rule_count >= s_rules.size()) {
        return ESP_ERR_INVALID_SIZE;
    }

    RfStoredRule stored_rule{};
    copy_name(stored_rule.target_name, target_name);
    stored_rule.repeats = repeats;
    stored_rule.cooldown_ms = kRfAutomationCooldownMs;
    AddValidationContext context{trigger_name, target_name};
    RfStoredSignal trigger_signal{};
    RfStoredSignal target_signal{};
    ESP_RETURN_ON_ERROR(rf_storage_rule_create_validated(trigger_name, stored_rule, validate_new_rule_signals, &context,
                                                         &trigger_signal, &target_signal),
                        "rf_automation", "persist validated rule");

    RuntimeRule &runtime = s_rules[s_rule_count++];
    std::memset(&runtime, 0, sizeof(runtime));
    copy_name(runtime.entry.trigger_name, trigger_name);
    runtime.entry.rule = stored_rule;
    runtime.trigger_signal = trigger_signal;
    runtime.target_signal = target_signal;
    s_status.rule_count = static_cast<uint16_t>(s_rule_count);
    advance_generation();
    return ESP_OK;
}

esp_err_t rf_automation_remove_rule(const char *trigger_name)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return rf_storage_rule_remove_recovery(trigger_name);
    }
    if (!rf_storage_name_is_valid(trigger_name)) {
        return ESP_ERR_INVALID_ARG;
    }

    AutomationLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    std::size_t index = 0;
    while (index < s_rule_count && std::strcmp(s_rules[index].entry.trigger_name, trigger_name) != 0) {
        ++index;
    }
    if (index == s_rule_count) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(rf_storage_rule_remove(trigger_name), "rf_automation", "remove rule");
    if (index + 1U < s_rule_count) {
        std::memmove(&s_rules[index], &s_rules[index + 1U], (s_rule_count - index - 1U) * sizeof(RuntimeRule));
    }
    --s_rule_count;
    std::memset(&s_rules[s_rule_count], 0, sizeof(RuntimeRule));
    s_status.rule_count = static_cast<uint16_t>(s_rule_count);
    advance_generation();
    return ESP_OK;
}

esp_err_t rf_automation_list_rules(RfStorageRuleEntry *rules, std::size_t capacity, std::size_t *count)
{
    if (count == nullptr || (rules == nullptr && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available.load(std::memory_order_acquire)) {
        return rf_storage_rule_list(rules, capacity, count);
    }

    AutomationLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    *count = s_rule_count;
    if (rules == nullptr && capacity == 0) {
        return ESP_OK;
    }
    if (capacity < s_rule_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (std::size_t index = 0; index < s_rule_count; ++index) {
        rules[index] = s_rules[index].entry;
    }
    std::sort(rules, rules + s_rule_count, [](const RfStorageRuleEntry &left, const RfStorageRuleEntry &right) {
        return std::strcmp(left.trigger_name, right.trigger_name) < 0;
    });
    return ESP_OK;
}

esp_err_t rf_automation_list_rule_info(RfAutomationRuleInfo *rules, std::size_t capacity, std::size_t *count)
{
    if (count == nullptr || (rules == nullptr && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_available.load(std::memory_order_acquire)) {
        AutomationLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        *count = s_rule_count;
        if (rules == nullptr && capacity == 0) {
            return ESP_OK;
        }
        if (capacity < s_rule_count) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (std::size_t index = 0; index < s_rule_count; ++index) {
            rules[index].entry = s_rules[index].entry;
            rules[index].validation_error = ESP_OK;
        }
    } else {
        std::size_t required = 0;
        ESP_RETURN_ON_ERROR(rf_storage_rule_name_list(nullptr, 0, &required), "rf_automation", "count persisted rules");
        *count = required;
        if (rules == nullptr && capacity == 0) {
            return ESP_OK;
        }
        if (capacity < required) {
            return ESP_ERR_INVALID_SIZE;
        }
        std::unique_ptr<RfStorageName[]> names;
        std::unique_ptr<RfStoredSignal[]> trigger_signals;
        std::unique_ptr<RfStoredSignal[]> target_signals;
        std::unique_ptr<RfStorageRuleEntry[]> graph;
        const bool analyze_graph = required <= CONFIG_RF_MAX_AUTOMATION_RULES;
        if (required > 0) {
            names.reset(new (std::nothrow) RfStorageName[required]);
            if (analyze_graph) {
                trigger_signals.reset(new (std::nothrow) RfStoredSignal[required]);
                target_signals.reset(new (std::nothrow) RfStoredSignal[required]);
                graph.reset(new (std::nothrow) RfStorageRuleEntry[required]);
            }
            if (!names || (analyze_graph && (!trigger_signals || !target_signals || !graph))) {
                return ESP_ERR_NO_MEM;
            }
            ESP_RETURN_ON_ERROR(rf_storage_rule_name_list(names.get(), required, &required), "rf_automation",
                                "list persisted rule names");
        }
        for (std::size_t index = 0; index < required; ++index) {
            rules[index] = {};
            copy_name(rules[index].entry.trigger_name, names[index].value);
            rules[index].validation_error = rf_storage_rule_load(names[index].value, &rules[index].entry.rule);
            if (rules[index].validation_error != ESP_OK) {
                continue;
            }
            if (!analyze_graph) {
                rules[index].validation_error = ESP_ERR_INVALID_SIZE;
                continue;
            }
            rules[index].validation_error = rf_storage_load(names[index].value, &trigger_signals[index]);
            if (rules[index].validation_error == ESP_OK) {
                rules[index].validation_error =
                    rf_storage_load(rules[index].entry.rule.target_name, &target_signals[index]);
            }
            if (rules[index].validation_error == ESP_OK &&
                rf_stored_signals_equivalent(trigger_signals[index], target_signals[index])) {
                rules[index].validation_error = ESP_ERR_INVALID_STATE;
            }
        }
        if (analyze_graph) {
            for (std::size_t left = 0; left < required; ++left) {
                if (rules[left].validation_error != ESP_OK) {
                    continue;
                }
                for (std::size_t right = left + 1U; right < required; ++right) {
                    if (rules[right].validation_error == ESP_OK &&
                        rf_stored_signals_equivalent(trigger_signals[left], trigger_signals[right])) {
                        rules[left].validation_error = ESP_ERR_INVALID_STATE;
                        rules[right].validation_error = ESP_ERR_INVALID_STATE;
                    }
                }
            }

            std::size_t graph_count = 0;
            for (std::size_t index = 0; index < required; ++index) {
                if (rules[index].validation_error != ESP_OK) {
                    continue;
                }
                graph[graph_count] = rules[index].entry;
                std::size_t target_matches = 0;
                for (std::size_t candidate = 0; candidate < required; ++candidate) {
                    if (rules[candidate].validation_error == ESP_OK &&
                        rf_stored_signals_equivalent(target_signals[index], trigger_signals[candidate])) {
                        ++target_matches;
                        copy_name(graph[graph_count].rule.target_name, rules[candidate].entry.trigger_name);
                    }
                }
                if (target_matches > 1) {
                    rules[index].validation_error = ESP_ERR_INVALID_STATE;
                    continue;
                }
                ++graph_count;
            }
            if (rf_rule_graph_has_cycle(graph.get(), graph_count)) {
                for (std::size_t index = 0; index < required; ++index) {
                    if (rules[index].validation_error == ESP_OK) {
                        rules[index].validation_error = ESP_ERR_INVALID_STATE;
                    }
                }
            }
        }
        *count = required;
    }
    if (rules != nullptr) {
        std::sort(rules, rules + *count, [](const RfAutomationRuleInfo &left, const RfAutomationRuleInfo &right) {
            return std::strcmp(left.entry.trigger_name, right.entry.trigger_name) < 0;
        });
    }
    return ESP_OK;
}

esp_err_t rf_automation_set_enabled(bool enabled)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return rf_storage_rule_enabled_set(enabled);
    }

    AutomationLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_RETURN_ON_ERROR(rf_storage_rule_enabled_set(enabled), "rf_automation", "persist enabled state");
    const bool changed = s_enabled.load(std::memory_order_relaxed) != enabled;
    s_enabled.store(enabled, std::memory_order_release);
    s_status.enabled = enabled;
    if (changed) {
        advance_generation();
    }
    return ESP_OK;
}

esp_err_t rf_automation_get_status(RfAutomationStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available.load(std::memory_order_acquire)) {
        *status = {};
        status->initialization_error = s_initialization_error.load(std::memory_order_acquire);
        status->queue_drops = s_queue_drops.load(std::memory_order_relaxed);
        bool persisted_enabled = false;
        if (rf_storage_rule_enabled_get(&persisted_enabled) == ESP_OK) {
            status->enabled = persisted_enabled;
            status->enabled_known = true;
        }
        std::size_t persisted_count = 0;
        if (rf_storage_rule_name_list(nullptr, 0, &persisted_count) == ESP_OK && persisted_count <= UINT16_MAX) {
            status->rule_count = static_cast<uint16_t>(persisted_count);
            status->rule_count_known = true;
        }
        return ESP_OK;
    }
    AutomationLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_status;
    status->available = true;
    status->enabled = s_enabled.load(std::memory_order_relaxed);
    status->enabled_known = true;
    status->rule_count = static_cast<uint16_t>(s_rule_count);
    status->rule_count_known = true;
    status->queue_drops = s_queue_drops.load(std::memory_order_relaxed);
    return ESP_OK;
}

void rf_automation_on_frame(const RfFrame &frame)
{
    if (!s_available.load(std::memory_order_acquire) || !s_enabled.load(std::memory_order_acquire)) {
        return;
    }
    FrameEvent event{};
    event.frame = frame;
    event.generation = s_generation.load(std::memory_order_acquire);
    if (s_frame_queue == nullptr || xQueueSend(s_frame_queue, &event, 0) != pdTRUE) {
        s_queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace rfbridge
