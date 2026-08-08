#include "web_ui.hpp"

#include <atomic>
#include <cstdint>

#include "bridge_events.hpp"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_mdns.hpp"
#include "ota_update.hpp"
#include "web_api.hpp"

namespace rfbridge {
namespace {

constexpr char kTag[] = "web_ui";
constexpr uint16_t kHttpPort = 80;
constexpr uint32_t kHttpTaskStackSize = 8192;
constexpr uint32_t kServiceTaskStackSize = 4608;
constexpr uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr UBaseType_t kServiceTaskPriority = 3;
constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(5000);
constexpr TickType_t kMdnsPollDelay = pdMS_TO_TICKS(1000);

static_assert(kHttpTaskStackSize >= 8192U);
static_assert(kServiceTaskStackSize >= 4096U);

TaskHandle_t s_service_task = nullptr;
httpd_handle_t s_server = nullptr;
std::atomic<bool> s_initialization_started{false};
std::atomic<bool> s_available{false};
std::atomic<bool> s_network_online{false};
std::atomic<bool> s_server_ready{false};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};
portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
WebUiStatus s_status{};

uint32_t service_stack_minimum_free() {
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
}

void update_status(esp_err_t error, bool running, bool count_start, bool count_stop,
                   bool count_failure) {
    taskENTER_CRITICAL(&s_status_lock);
    s_status.server_running = running;
    s_status.last_error = error;
    if (count_start) {
        ++s_status.starts;
    }
    if (count_stop) {
        ++s_status.stops;
    }
    if (count_failure) {
        ++s_status.start_failures;
    }
    taskEXIT_CRITICAL(&s_status_lock);
}

esp_err_t accept_ready_session(httpd_handle_t, int) {
    return s_server_ready.load(std::memory_order_acquire) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

[[gnu::noinline]] void publish_web_lifecycle_event(BridgeEventType type) {
    BridgeEvent event{};
    event.type = type;
    if (type == BridgeEventType::kWebStarted) {
        event.value = kHttpPort;
    }
    bridge_events_publish(event);
}

esp_err_t rollback_server_start(esp_err_t error) {
    s_server_ready.store(false, std::memory_order_release);
    if (s_server != nullptr) {
        const esp_err_t stop_error = httpd_stop(s_server);
        if (stop_error != ESP_OK) {
            ESP_LOGE(kTag, "Could not roll back partial HTTP server: %s",
                     esp_err_to_name(stop_error));
            update_status(stop_error, true, false, false, false);
            return stop_error;
        }
        s_server = nullptr;
    }
    set_ota_http_server_running(false, error);
    update_status(error, false, false, false, false);
    return error;
}

esp_err_t start_server() {
    if (!s_network_online.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server != nullptr) {
        return s_server_ready.load(std::memory_order_acquire) ? ESP_OK
                                                              : rollback_server_start(ESP_ERR_INVALID_STATE);
    }

    ESP_LOGI(kTag, "Starting HTTP server; service stack min free=%u; heap free=%u largest=%u",
             static_cast<unsigned>(service_stack_minimum_free()),
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kHttpPort;
    config.stack_size = kHttpTaskStackSize;
    config.max_uri_handlers = 22;
    config.max_open_sockets = 2;
    config.open_fn = accept_ready_session;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    esp_err_t error = httpd_start(&s_server, &config);
    if (error != ESP_OK) {
        update_status(error, false, false, false, false);
        return error;
    }
    ESP_LOGI(kTag, "HTTP server allocated; service stack min free=%u; heap free=%u largest=%u",
             static_cast<unsigned>(service_stack_minimum_free()),
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));

    if (!s_network_online.load(std::memory_order_acquire)) {
        return rollback_server_start(ESP_ERR_INVALID_STATE);
    }
    error = register_web_handlers(s_server);
    if (error == ESP_OK) {
        error = register_ota_http_handlers(s_server, authorize_web_ota_request, nullptr);
    }
    if (error == ESP_OK && !s_network_online.load(std::memory_order_acquire)) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error != ESP_OK) {
        return rollback_server_start(error);
    }

    s_server_ready.store(true, std::memory_order_release);
    set_ota_http_server_running(true);
    update_status(ESP_OK, true, true, false, false);
    publish_web_lifecycle_event(BridgeEventType::kWebStarted);
    ESP_LOGI(kTag,
             "Web UI ready at http://<device-ip>:%u/; service stack min free=%u; heap free=%u "
             "largest=%u",
             static_cast<unsigned>(kHttpPort), static_cast<unsigned>(service_stack_minimum_free()),
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));
    ESP_LOGI(kTag, "Starting mDNS after HTTP readiness; service stack min free=%u; heap free=%u largest=%u",
             static_cast<unsigned>(service_stack_minimum_free()),
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));
    const esp_err_t mdns_error = start_network_mdns(kHttpPort);
    if (mdns_error != ESP_OK) {
        ESP_LOGE(kTag, "mDNS startup failed without stopping HTTP: %s",
                 esp_err_to_name(mdns_error));
    }
    ESP_LOGI(kTag, "mDNS startup returned; service stack min free=%u; heap free=%u largest=%u",
             static_cast<unsigned>(service_stack_minimum_free()),
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));
    return ESP_OK;
}

esp_err_t stop_server() {
    if (s_server == nullptr) {
        return ESP_OK;
    }
    const bool was_ready = s_server_ready.exchange(false, std::memory_order_acq_rel);
    set_ota_http_server_running(false);
    const esp_err_t error = httpd_stop(s_server);
    if (error == ESP_OK) {
        s_server = nullptr;
    } else {
        s_server_ready.store(was_ready, std::memory_order_release);
        if (was_ready) {
            set_ota_http_server_running(true, error);
        }
    }
    update_status(error, s_server_ready.load(std::memory_order_relaxed), false,
                  error == ESP_OK && was_ready, false);
    if (error == ESP_OK && was_ready) {
        publish_web_lifecycle_event(BridgeEventType::kWebStopped);
    }
    return error;
}

void service_task(void *) {
    uint32_t consecutive_start_failures = 0;
    esp_err_t last_start_error = ESP_OK;
    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (s_network_online.load(std::memory_order_acquire)) {
            const esp_err_t error = start_server();
            if (error != ESP_OK) {
                if (error != last_start_error) {
                    consecutive_start_failures = 0;
                    last_start_error = error;
                }
                ++consecutive_start_failures;
                if (consecutive_start_failures == 1U || consecutive_start_failures % 12U == 0U) {
                    ESP_LOGE(kTag,
                             "Start failed: %s; service stack min free=%u; heap free=%u largest=%u; "
                             "retrying",
                             esp_err_to_name(error),
                             static_cast<unsigned>(service_stack_minimum_free()),
                             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
                             static_cast<unsigned>(
                                 heap_caps_get_largest_free_block(kInternalHeapCaps)));
                }
                update_status(error, false, false, false, true);
                wait = kRetryDelay;
            } else {
                consecutive_start_failures = 0;
                last_start_error = ESP_OK;
            }
        } else {
            consecutive_start_failures = 0;
            last_start_error = ESP_OK;
            const esp_err_t error = stop_server();
            if (error != ESP_OK) {
                wait = kRetryDelay;
            }
        }
        if (network_mdns_is_active()) {
            (void)reconcile_network_mdns();
            if (wait == portMAX_DELAY || wait > kMdnsPollDelay) {
                wait = kMdnsPollDelay;
            }
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

}  // namespace

esp_err_t initialize_web_ui() {
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status = {};
    s_status.port = kHttpPort;
    taskEXIT_CRITICAL(&s_status_lock);

    if (xTaskCreate(service_task, "web_service", kServiceTaskStackSize, nullptr,
                    kServiceTaskPriority, &s_service_task) != pdPASS) {
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status.available = true;
    s_status.initialization_error = ESP_OK;
    taskEXIT_CRITICAL(&s_status_lock);
    s_available.store(true, std::memory_order_release);
    s_initialization_error.store(ESP_OK, std::memory_order_release);
    return ESP_OK;
}

esp_err_t set_web_ui_network_online(bool online) {
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    s_network_online.store(online, std::memory_order_release);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.network_online = online;
    taskEXIT_CRITICAL(&s_status_lock);
    xTaskNotifyGive(s_service_task);
    return ESP_OK;
}

esp_err_t get_web_ui_status(WebUiStatus *status) {
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    status->available = s_available.load(std::memory_order_acquire);
    status->network_online = s_network_online.load(std::memory_order_relaxed);
    status->server_running = s_server_ready.load(std::memory_order_relaxed);
    status->initialization_error = s_initialization_error.load(std::memory_order_relaxed);
    return ESP_OK;
}

}  // namespace rfbridge
