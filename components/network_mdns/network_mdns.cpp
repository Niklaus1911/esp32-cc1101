#include "network_mdns.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "network_wifi.hpp"

namespace rfbridge {
namespace {

constexpr char kTag[] = "network_mdns";
constexpr uint32_t kStalledAfterMs = 5000;
constexpr char kHttpService[] = "_http";
constexpr char kRfbridgeService[] = "_rfbridge";
constexpr char kTcpProtocol[] = "_tcp";
constexpr char kInstancePrefix[] = "ESP32 CC1101 RF Bridge ";

std::atomic<bool> s_initialization_started{false};
std::atomic<bool> s_available{false};
std::atomic<bool> s_active{false};
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_owner_task = nullptr;
NetworkMdnsStatus s_status{};
int64_t s_owner_heartbeat_us = 0;
uint32_t s_failed_generation = 0;
bool s_failure_latched = false;
bool s_lifecycle_failure_latched = false;

void copy_text(char *destination, std::size_t capacity, const char *source)
{
    std::strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

bool owner_is_current()
{
    bool matches = false;
    taskENTER_CRITICAL(&s_lock);
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (s_owner_task == nullptr) {
        s_owner_task = current;
    }
    matches = s_owner_task == current;
    taskEXIT_CRITICAL(&s_lock);
    return matches;
}

void heartbeat()
{
    taskENTER_CRITICAL(&s_lock);
    s_owner_heartbeat_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);
}

void set_fault(esp_err_t error, bool initialization, uint32_t failed_generation,
               bool lifecycle_failure = false)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.state = NetworkMdnsState::kFaulted;
    s_status.last_error = error;
    if (initialization) {
        s_status.initialization_error = error;
    }
    s_failed_generation = failed_generation;
    s_failure_latched = true;
    s_lifecycle_failure_latched = lifecycle_failure;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t read_hostname_status(NetworkHostnameStatus *hostname)
{
    const esp_err_t error = get_network_hostname_status(hostname);
    if (error != ESP_OK) {
        set_fault(error, false, 0);
        return error;
    }
    taskENTER_CRITICAL(&s_lock);
    copy_text(s_status.configured_hostname, sizeof(s_status.configured_hostname),
              hostname->configured_hostname);
    s_status.hostname_custom = hostname->custom;
    s_status.configured_generation = hostname->configured_generation;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t apply_desired_hostname(const NetworkHostnameStatus &hostname, bool lifecycle_retry)
{
    uint32_t applied_generation = 0;
    bool failure_latched = false;
    uint32_t failed_generation = 0;
    taskENTER_CRITICAL(&s_lock);
    applied_generation = s_status.applied_generation;
    failure_latched = s_failure_latched;
    failed_generation = s_failed_generation;
    taskEXIT_CRITICAL(&s_lock);
    if (!lifecycle_retry && failure_latched &&
        failed_generation == hostname.configured_generation) {
        taskENTER_CRITICAL(&s_lock);
        const esp_err_t error = s_status.last_error;
        taskEXIT_CRITICAL(&s_lock);
        return error;
    }
    if (applied_generation == hostname.configured_generation) {
        return ESP_OK;
    }
    if (!network_mdns_hostname_apply_needed(applied_generation,
                                            hostname.configured_generation,
                                            failure_latched, failed_generation,
                                            lifecycle_retry)) {
        taskENTER_CRITICAL(&s_lock);
        const esp_err_t error = s_status.last_error;
        taskEXIT_CRITICAL(&s_lock);
        return error;
    }
    taskENTER_CRITICAL(&s_lock);
    s_status.state = NetworkMdnsState::kStarting;
    taskEXIT_CRITICAL(&s_lock);
    heartbeat();
    const esp_err_t error = mdns_hostname_set(hostname.configured_hostname);
    if (error != ESP_OK) {
        set_fault(error, false, hostname.configured_generation);
        return error;
    }
    taskENTER_CRITICAL(&s_lock);
    s_status.applied_generation = hostname.configured_generation;
    s_status.last_error = ESP_OK;
    s_failure_latched = false;
    s_failed_generation = 0;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(kTag, "mDNS hostname applied: %s generation=%lu",
             hostname.configured_hostname,
             static_cast<unsigned long>(hostname.configured_generation));
    return ESP_OK;
}

esp_err_t cache_effective_hostname(uint32_t configured_generation)
{
    char effective[MDNS_NAME_BUF_LEN]{};
    heartbeat();
    const esp_err_t error = mdns_hostname_get(effective);
    if (error != ESP_OK) {
        set_fault(error, false, configured_generation);
        return error;
    }
    taskENTER_CRITICAL(&s_lock);
    copy_text(s_status.effective_hostname, sizeof(s_status.effective_hostname), effective);
    s_status.effective_known = true;
    s_status.conflict_renamed =
        std::strcmp(s_status.effective_hostname, s_status.configured_hostname) != 0;
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t add_services(uint16_t http_port, const NetworkHostnameStatus &hostname)
{
    bool http_registered = false;
    bool rfbridge_registered = false;
    taskENTER_CRITICAL(&s_lock);
    http_registered = s_status.http_service_registered;
    rfbridge_registered = s_status.rfbridge_service_registered;
    taskEXIT_CRITICAL(&s_lock);

    char instance[48]{};
    char upper_suffix[kNetworkHostnameMacSuffixCapacity]{};
    for (std::size_t index = 0; index + 1U < sizeof(upper_suffix) &&
                                hostname.mac_suffix[index] != '\0'; ++index) {
        upper_suffix[index] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(hostname.mac_suffix[index])));
    }
    const int instance_length = std::snprintf(instance, sizeof(instance), "%s%s",
                                              kInstancePrefix, upper_suffix);
    if (instance_length <= 0 || static_cast<std::size_t>(instance_length) >= sizeof(instance)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!http_registered) {
        mdns_txt_item_t txt[] = {{"path", "/"}};
        heartbeat();
        const esp_err_t error = mdns_service_add(instance, kHttpService, kTcpProtocol,
                                                 http_port, txt, 1);
        if (error != ESP_OK) {
            set_fault(error, false, hostname.configured_generation, true);
            return error;
        }
        taskENTER_CRITICAL(&s_lock);
        s_status.http_service_registered = true;
        taskEXIT_CRITICAL(&s_lock);
    }

    if (!rfbridge_registered) {
        const esp_app_desc_t *description = esp_app_get_description();
        const char *version = description == nullptr ? "unknown" : description->version;
        mdns_txt_item_t txt[] = {
            {"txtvers", "1"},
            {"path", "/"},
            {"api", "/api/live"},
            {"ota", "/api/v1/ota"},
            {"project", "esp32-cc1101"},
            {"version", version},
            {"features", "rx,tx,learn,replay,rules,ota"},
            {"auth", "none"},
        };
        heartbeat();
        const esp_err_t error = mdns_service_add(instance, kRfbridgeService, kTcpProtocol,
                                                 http_port, txt, sizeof(txt) / sizeof(txt[0]));
        if (error != ESP_OK) {
            set_fault(error, false, hostname.configured_generation, true);
            return error;
        }
        taskENTER_CRITICAL(&s_lock);
        s_status.rfbridge_service_registered = true;
        taskEXIT_CRITICAL(&s_lock);
    }
    return ESP_OK;
}

void mark_ready()
{
    heartbeat();
    taskENTER_CRITICAL(&s_lock);
    if (network_mdns_registration_is_complete(s_status.initialized,
                                               s_status.http_service_registered,
                                               s_status.rfbridge_service_registered)) {
        s_status.state = NetworkMdnsState::kReady;
        s_status.last_error = ESP_OK;
        s_failure_latched = false;
        s_lifecycle_failure_latched = false;
        s_failed_generation = 0;
    }
    taskEXIT_CRITICAL(&s_lock);
}

}  // namespace

const char *network_mdns_state_name(NetworkMdnsState state)
{
    switch (state) {
        case NetworkMdnsState::kNotStarted: return "not_started";
        case NetworkMdnsState::kStarting: return "starting";
        case NetworkMdnsState::kReady: return "ready";
        case NetworkMdnsState::kFaulted: return "faulted";
        case NetworkMdnsState::kStalled: return "stalled";
    }
    return "invalid";
}

esp_err_t initialize_network_mdns()
{
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel)) {
        taskENTER_CRITICAL(&s_lock);
        const esp_err_t error = s_status.initialization_error;
        taskEXIT_CRITICAL(&s_lock);
        return error;
    }
    NetworkHostnameStatus hostname{};
    const esp_err_t error = get_network_hostname_status(&hostname);
    taskENTER_CRITICAL(&s_lock);
    s_status = {};
    s_status.available = error == ESP_OK;
    s_status.initialization_error = error;
    if (error == ESP_OK) {
        copy_text(s_status.configured_hostname, sizeof(s_status.configured_hostname),
                  hostname.configured_hostname);
        s_status.hostname_custom = hostname.custom;
        s_status.configured_generation = hostname.configured_generation;
    } else {
        s_status.state = NetworkMdnsState::kFaulted;
        s_status.last_error = error;
    }
    taskEXIT_CRITICAL(&s_lock);
    s_available.store(error == ESP_OK, std::memory_order_release);
    return error;
}

esp_err_t start_network_mdns(uint16_t http_port)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (http_port == 0 || !owner_is_current()) {
        return ESP_ERR_INVALID_ARG;
    }
    s_active.store(true, std::memory_order_release);
    taskENTER_CRITICAL(&s_lock);
    s_status.state = NetworkMdnsState::kStarting;
    s_failure_latched = false;
    s_lifecycle_failure_latched = false;
    s_failed_generation = 0;
    taskEXIT_CRITICAL(&s_lock);
    heartbeat();

    bool initialized = false;
    taskENTER_CRITICAL(&s_lock);
    initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_lock);
    if (!initialized) {
        const esp_err_t error = mdns_init();
        if (error != ESP_OK) {
            set_fault(error, true, 0, true);
            return error;
        }
        taskENTER_CRITICAL(&s_lock);
        s_status.initialized = true;
        s_status.initialization_error = ESP_OK;
        taskEXIT_CRITICAL(&s_lock);
    }

    NetworkHostnameStatus hostname{};
    esp_err_t error = read_hostname_status(&hostname);
    if (error == ESP_OK) {
        error = apply_desired_hostname(hostname, true);
    }
    if (error == ESP_OK) {
        error = add_services(http_port, hostname);
    }
    if (error == ESP_OK) {
        error = cache_effective_hostname(hostname.configured_generation);
    }
    if (error == ESP_OK) {
        mark_ready();
        ESP_LOGI(kTag, "mDNS services registered for http://%s.local:%u/",
                 hostname.configured_hostname, static_cast<unsigned>(http_port));
    }
    return error;
}

esp_err_t reconcile_network_mdns()
{
    if (!s_active.load(std::memory_order_acquire) || !owner_is_current()) {
        return ESP_ERR_INVALID_STATE;
    }
    heartbeat();
    bool initialized = false;
    bool lifecycle_failure_latched = false;
    esp_err_t latched_error = ESP_OK;
    taskENTER_CRITICAL(&s_lock);
    initialized = s_status.initialized;
    lifecycle_failure_latched = s_lifecycle_failure_latched;
    latched_error = s_status.last_error;
    taskEXIT_CRITICAL(&s_lock);
    if (!initialized || lifecycle_failure_latched) {
        return latched_error == ESP_OK ? ESP_ERR_INVALID_STATE : latched_error;
    }
    NetworkHostnameStatus hostname{};
    esp_err_t error = read_hostname_status(&hostname);
    if (error == ESP_OK) {
        error = apply_desired_hostname(hostname, false);
    }
    if (error == ESP_OK) {
        error = cache_effective_hostname(hostname.configured_generation);
    }
    if (error == ESP_OK) {
        mark_ready();
    }
    return error;
}

esp_err_t set_network_mdns_hostname(const char *hostname)
{
    const esp_err_t error = set_network_hostname(hostname);
    if (error == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        const TaskHandle_t owner = s_owner_task;
        taskEXIT_CRITICAL(&s_lock);
        if (owner != nullptr) {
            xTaskNotifyGive(owner);
        }
    }
    return error;
}

esp_err_t reset_network_mdns_hostname()
{
    const esp_err_t error = reset_network_hostname();
    if (error == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        const TaskHandle_t owner = s_owner_task;
        taskEXIT_CRITICAL(&s_lock);
        if (owner != nullptr) {
            xTaskNotifyGive(owner);
        }
    }
    return error;
}

esp_err_t get_network_mdns_status(NetworkMdnsStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    NetworkHostnameStatus hostname{};
    const esp_err_t hostname_error = get_network_hostname_status(&hostname);
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    *status = s_status;
    if (hostname_error == ESP_OK) {
        copy_text(status->configured_hostname, sizeof(status->configured_hostname),
                  hostname.configured_hostname);
        status->hostname_custom = hostname.custom;
        status->configured_generation = hostname.configured_generation;
    }
    const bool heartbeat_known = s_owner_heartbeat_us != 0;
    const int64_t age_us = heartbeat_known ? now - s_owner_heartbeat_us : 0;
    status->owner_heartbeat_age_ms = static_cast<uint32_t>(
        age_us <= 0 ? 0 : age_us / 1000 > UINT32_MAX ? UINT32_MAX : age_us / 1000);
    status->state = network_mdns_reported_state(status->state, heartbeat_known,
                                                status->owner_heartbeat_age_ms,
                                                kStalledAfterMs);
    taskEXIT_CRITICAL(&s_lock);
    status->available = s_available.load(std::memory_order_acquire);
    return hostname_error;
}

bool network_mdns_is_active()
{
    return s_active.load(std::memory_order_acquire);
}

}  // namespace rfbridge
