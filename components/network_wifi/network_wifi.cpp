#include "network_wifi.hpp"

#include "network_wifi_storage_private.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace rfbridge {
namespace {

constexpr char kTag[] = "network_wifi";
constexpr TickType_t kApiWait = pdMS_TO_TICKS(100);
constexpr TickType_t kMutexWait = pdMS_TO_TICKS(1000);
constexpr uint32_t kTaskStackSize = 6144;
constexpr UBaseType_t kTaskPriority = 3;
constexpr UBaseType_t kMessageQueueDepth = 12;
constexpr UBaseType_t kDriverEventQueueDepth = 16;
constexpr uint32_t kInitialRetryLimit = 5;
constexpr std::size_t kMaxScanResults = 12;
constexpr std::size_t kCriticalEventCapacity = 4;

SemaphoreHandle_t s_mutex = nullptr;
SemaphoreHandle_t s_admission_mutex = nullptr;
SemaphoreHandle_t s_hostname_mutex = nullptr;

class StatusLock {
public:
    explicit StatusLock(TickType_t wait = kMutexWait)
        : locked_(s_mutex != nullptr && xSemaphoreTake(s_mutex, wait) == pdTRUE)
    {
    }
    ~StatusLock()
    {
        if (locked_) {
            xSemaphoreGive(s_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};

class HostnameLock {
public:
    HostnameLock()
        : locked_(s_hostname_mutex != nullptr &&
                  xSemaphoreTake(s_hostname_mutex, kMutexWait) == pdTRUE)
    {
    }
    ~HostnameLock()
    {
        if (locked_) {
            xSemaphoreGive(s_hostname_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};

enum class MessageType : uint8_t {
    kStartSaved,
    kConnect,
    kStop,
    kForget,
    kScan,
    kStaStarted,
    kStaStopped,
    kStaConnected,
    kStaDisconnected,
    kGotIp,
    kLostIp,
    kScanDone,
    kOtaLock,
    kOtaUnlock,
};

struct Message {
    MessageType type = MessageType::kStartSaved;
    WifiCredentials credentials{};
    char ssid[kWifiSsidCapacity]{};
    uint32_t ip = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    int32_t reason = 0;
};

QueueHandle_t s_message_queue = nullptr;
QueueHandle_t s_driver_event_queue = nullptr;
QueueHandle_t s_ota_reply_queue = nullptr;
TaskHandle_t s_task = nullptr;
esp_netif_t *s_sta_netif = nullptr;
esp_event_handler_instance_t s_wifi_handler = nullptr;
esp_event_handler_instance_t s_ip_handler = nullptr;
std::atomic<bool> s_initialization_started{false};
std::atomic<bool> s_available{false};
std::atomic<bool> s_ota_locked{false};
bool s_ota_ps_needs_restore = false;
std::atomic<uint32_t> s_event_drops{0};
std::atomic<bool> s_reconcile_required{false};
std::atomic<uint32_t> s_sink_callbacks_in_flight{0};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};
NetworkWifiStatus s_status{};
NetworkHostnameStatus s_hostname_status{};
NetworkWifiEventSink s_event_sink = nullptr;
void *s_event_sink_context = nullptr;
NetworkWifiOnlineSink s_online_sink = nullptr;
void *s_online_sink_context = nullptr;

WifiCredentials s_saved_credentials{};
WifiCredentials s_active_credentials{};
bool s_driver_initialized = false;
bool s_wifi_library_initialized = false;
bool s_netif_attached = false;
std::atomic<bool> s_driver_started{false};
bool s_start_in_progress = false;
bool s_operator_stopped = false;
bool s_active_candidate = false;
bool s_network_stack_initialized = false;
bool s_had_ip = false;
bool s_associated = false;
bool s_connection_pending = false;
WifiCredentials s_pending_credentials{};
bool s_pending_candidate = false;
bool s_retry_pending = false;
int64_t s_retry_due_us = 0;
bool s_scan_pending = false;
bool s_scan_then_stop = false;
bool s_scan_cancelled = false;
std::array<NetworkWifiEvent, kCriticalEventCapacity> s_critical_events{};
std::size_t s_critical_event_count = 0;
uint32_t s_hostname_attempted_generation = 0;

bool credentials_equal(const WifiCredentials &left, const WifiCredentials &right)
{
    return std::strcmp(left.ssid, right.ssid) == 0 &&
           std::strcmp(left.password, right.password) == 0;
}

void copy_text(char *destination, std::size_t capacity, const char *source)
{
    std::strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

void cleanup_failed_network_initialization()
{
    if (s_ota_reply_queue != nullptr) {
        vQueueDelete(s_ota_reply_queue);
        s_ota_reply_queue = nullptr;
    }
    if (s_driver_event_queue != nullptr) {
        vQueueDelete(s_driver_event_queue);
        s_driver_event_queue = nullptr;
    }
    if (s_message_queue != nullptr) {
        vQueueDelete(s_message_queue);
        s_message_queue = nullptr;
    }
    if (s_hostname_mutex != nullptr) {
        vSemaphoreDelete(s_hostname_mutex);
        s_hostname_mutex = nullptr;
    }
    if (s_admission_mutex != nullptr) {
        vSemaphoreDelete(s_admission_mutex);
        s_admission_mutex = nullptr;
    }
    if (s_mutex != nullptr) {
        vSemaphoreDelete(s_mutex);
        s_mutex = nullptr;
    }
}

esp_err_t apply_hostname_to_netif(bool force)
{
    if (s_sta_netif == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    char hostname[kNetworkHostnameCapacity]{};
    uint32_t generation = 0;
    {
        StatusLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        generation = s_hostname_status.configured_generation;
        if (!force && generation == s_hostname_attempted_generation) {
            return s_hostname_status.last_apply_error;
        }
        copy_text(hostname, sizeof(hostname), s_hostname_status.configured_hostname);
    }
    s_hostname_attempted_generation = generation;
    const esp_err_t error = esp_netif_set_hostname(s_sta_netif, hostname);
    {
        StatusLock lock(portMAX_DELAY);
        if (lock.locked()) {
            s_hostname_status.last_apply_error = error;
            if (error == ESP_OK) {
                s_hostname_status.netif_applied_generation = generation;
            }
        }
    }
    if (error == ESP_OK) {
        ESP_LOGI(kTag, "STA hostname applied: %s generation=%lu", hostname,
                 static_cast<unsigned long>(generation));
    } else {
        ESP_LOGE(kTag, "Could not apply STA hostname %s: %s", hostname,
                 esp_err_to_name(error));
    }
    return error;
}

bool deliver_event(const NetworkWifiEvent &event)
{
    NetworkWifiEventSink sink = nullptr;
    void *context = nullptr;
    {
        StatusLock lock;
        if (!lock.locked()) {
            return false;
        }
        sink = s_event_sink;
        context = s_event_sink_context;
        if (sink != nullptr) {
            s_sink_callbacks_in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (sink == nullptr) {
        return true;
    }
    const bool accepted = sink(event, context);
    s_sink_callbacks_in_flight.fetch_sub(1, std::memory_order_release);
    return accepted;
}

void queue_critical_event(const NetworkWifiEvent &event)
{
    if (deliver_event(event)) {
        return;
    }
    if (s_critical_event_count < s_critical_events.size()) {
        s_critical_events[s_critical_event_count++] = event;
    } else {
        s_event_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

void retry_critical_event()
{
    if (s_critical_event_count == 0 || !deliver_event(s_critical_events[0])) {
        return;
    }
    for (std::size_t index = 1; index < s_critical_event_count; ++index) {
        s_critical_events[index - 1U] = s_critical_events[index];
    }
    --s_critical_event_count;
}

void notify_online_sink(bool online)
{
    NetworkWifiOnlineSink sink = nullptr;
    void *context = nullptr;
    {
        StatusLock lock;
        if (!lock.locked()) {
            return;
        }
        sink = s_online_sink;
        context = s_online_sink_context;
        if (sink != nullptr) {
            s_sink_callbacks_in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (sink != nullptr) {
        sink(online, context);
        s_sink_callbacks_in_flight.fetch_sub(1, std::memory_order_release);
    }
}

void emit_state_event(NetworkWifiEventType type, esp_err_t error = ESP_OK, int32_t reason = 0)
{
    NetworkWifiEvent event{};
    event.type = type;
    event.error = error;
    event.reason = reason;
    {
        StatusLock lock;
        if (!lock.locked()) {
            return;
        }
        event.state = s_status.state;
        copy_text(event.ssid, sizeof(event.ssid), s_status.active_ssid);
    }
    deliver_event(event);
}

void set_state(NetworkWifiState state, esp_err_t error = ESP_OK)
{
    {
        StatusLock lock;
        if (!lock.locked()) {
            return;
        }
        s_status.state = state;
        s_status.last_error = error;
        s_status.driver_initialized = s_driver_initialized;
        s_status.driver_started = s_driver_started;
    }
    emit_state_event(error == ESP_OK ? NetworkWifiEventType::kStateChanged
                                    : NetworkWifiEventType::kError,
                     error);
}

void set_ip_status(uint32_t ip, uint32_t netmask, uint32_t gateway)
{
    StatusLock lock;
    if (!lock.locked()) {
        return;
    }
    s_status.ip = ip;
    s_status.netmask = netmask;
    s_status.gateway = gateway;
    s_status.dns = 0;
    if (s_sta_netif != nullptr) {
        esp_netif_dns_info_t dns{};
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            s_status.dns = dns.ip.u_addr.ip4.addr;
        }
    }
}

void clear_ip_status()
{
    StatusLock lock;
    if (!lock.locked()) {
        return;
    }
    s_status.ip = 0;
    s_status.netmask = 0;
    s_status.gateway = 0;
    s_status.dns = 0;
    s_status.rssi = 0;
}

void post_driver_event(const Message &message)
{
    if (s_driver_event_queue == nullptr || xQueueSend(s_driver_event_queue, &message, 0) != pdTRUE) {
        s_event_drops.fetch_add(1, std::memory_order_relaxed);
        s_reconcile_required.store(true, std::memory_order_release);
    }
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void wifi_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    Message message{};
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            message.type = MessageType::kStaStarted;
            break;
        case WIFI_EVENT_STA_STOP:
            message.type = MessageType::kStaStopped;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            message.type = MessageType::kStaConnected;
            if (event_data != nullptr) {
                const auto *event = static_cast<wifi_event_sta_connected_t *>(event_data);
                const std::size_t length = std::min<std::size_t>(event->ssid_len, sizeof(message.ssid) - 1U);
                std::memcpy(message.ssid, event->ssid, length);
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            message.type = MessageType::kStaDisconnected;
            if (event_data != nullptr) {
                message.reason = static_cast<wifi_event_sta_disconnected_t *>(event_data)->reason;
            }
            break;
        case WIFI_EVENT_SCAN_DONE:
            message.type = MessageType::kScanDone;
            break;
        default:
            return;
    }
    post_driver_event(message);
}

void ip_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    Message message{};
    if (event_id == IP_EVENT_STA_GOT_IP && event_data != nullptr) {
        const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        message.type = MessageType::kGotIp;
        message.ip = event->ip_info.ip.addr;
        message.netmask = event->ip_info.netmask.addr;
        message.gateway = event->ip_info.gw.addr;
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        message.type = MessageType::kLostIp;
    } else {
        return;
    }
    post_driver_event(message);
}

void cleanup_failed_driver_initialization()
{
    if (s_wifi_handler != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = nullptr;
    }
    if (s_ip_handler != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler);
        s_ip_handler = nullptr;
    }
    if (s_sta_netif != nullptr && s_netif_attached) {
        esp_wifi_clear_default_wifi_driver_and_handlers(s_sta_netif);
        s_netif_attached = false;
    }
    if (s_wifi_library_initialized) {
        (void)esp_wifi_deinit();
        s_wifi_library_initialized = false;
    }
    if (s_sta_netif != nullptr) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = nullptr;
    }
    s_hostname_attempted_generation = 0;
    StatusLock lock;
    if (lock.locked()) {
        s_hostname_status.netif_applied_generation = 0;
    }
}

esp_err_t ensure_network_stack_initialized()
{
    if (xSemaphoreTake(s_admission_mutex, kMutexWait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = ESP_OK;
    if (!s_network_stack_initialized) {
        error = esp_netif_init();
        if (error == ESP_OK) {
            s_network_stack_initialized = true;
        }
    }
    xSemaphoreGive(s_admission_mutex);
    return error;
}

esp_err_t ensure_driver_initialized()
{
    if (s_driver_initialized) {
        return ESP_OK;
    }
    esp_err_t error = ensure_network_stack_initialized();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&netif_config);
    if (s_sta_netif == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    error = esp_netif_attach_wifi_station(s_sta_netif);
    if (error == ESP_OK) {
        s_netif_attached = true;
        error = esp_wifi_set_default_wifi_sta_handlers();
    }
    if (error == ESP_OK) {
        error = apply_hostname_to_netif(true);
    }
    if (error != ESP_OK) {
        cleanup_failed_driver_initialization();
        return error;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0;
    constexpr uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag, "Wi-Fi init heap: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));
    error = esp_wifi_init(&config);
    if (error == ESP_OK) {
        s_wifi_library_initialized = true;
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    } else {
        ESP_LOGE(kTag, "Wi-Fi init failed: %s; heap free=%u largest=%u",
                 esp_err_to_name(error),
                 static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                                    nullptr, &s_wifi_handler);
    }
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler,
                                                    nullptr, &s_ip_handler);
    }
    if (error != ESP_OK) {
        cleanup_failed_driver_initialization();
        return error;
    }

    s_driver_initialized = true;
    ESP_LOGI(kTag, "Wi-Fi initialized; heap free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)));
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status.driver_initialized = true;
        }
    }
    return ESP_OK;
}

esp_err_t configure_station(const WifiCredentials &credentials)
{
    wifi_config_t config{};
    const std::size_t ssid_length = std::strlen(credentials.ssid);
    const std::size_t password_length = std::strlen(credentials.password);
    std::memcpy(config.sta.ssid, credentials.ssid, ssid_length);
    std::memcpy(config.sta.password, credentials.password, password_length);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

bool scan_is_running()
{
    StatusLock lock;
    return lock.locked() && s_status.scan_running;
}

void cancel_running_scan()
{
    if (!s_scan_pending && !scan_is_running()) {
        return;
    }
    s_scan_pending = false;
    s_scan_then_stop = false;
    s_scan_cancelled = true;
    const esp_err_t error = esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
    if (error != ESP_OK) {
        s_scan_cancelled = false;
        StatusLock lock;
        if (lock.locked()) {
            s_status.scan_running = false;
        }
    }
}

void start_connection_now(const WifiCredentials &credentials, bool candidate)
{
    s_active_credentials = credentials;
    s_active_candidate = candidate;
    s_operator_stopped = false;
    s_had_ip = false;
    s_associated = false;
    s_retry_pending = false;
    s_scan_pending = false;
    {
        StatusLock lock;
        if (lock.locked()) {
            copy_text(s_status.active_ssid, sizeof(s_status.active_ssid), credentials.ssid);
            s_status.active_saved = !candidate;
            s_status.retry_count = 0;
            s_status.disconnect_reason = 0;
            s_status.persistence_error = ESP_OK;
        }
    }

    esp_err_t error = configure_station(credentials);
    if (error == ESP_OK) {
        error = apply_hostname_to_netif(true);
    }
    if (error == ESP_OK) {
        set_state(NetworkWifiState::kStarting);
        s_start_in_progress = true;
        error = esp_wifi_start();
    }
    if (error != ESP_OK) {
        s_start_in_progress = false;
        set_state(NetworkWifiState::kFault, error);
    }
}

void begin_connection(const WifiCredentials &credentials, bool candidate)
{
    const esp_err_t init_error = ensure_driver_initialized();
    if (init_error != ESP_OK) {
        set_state(NetworkWifiState::kFault, init_error);
        return;
    }
    if (s_driver_started.load(std::memory_order_acquire) || s_start_in_progress) {
        s_pending_credentials = credentials;
        s_pending_candidate = candidate;
        s_connection_pending = true;
        s_operator_stopped = false;
        s_retry_pending = false;
        cancel_running_scan();
        set_state(NetworkWifiState::kStopping);
        const esp_err_t error = esp_wifi_stop();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) {
            s_connection_pending = false;
            set_state(NetworkWifiState::kFault, error);
        } else if (error == ESP_ERR_WIFI_NOT_STARTED) {
            s_driver_started.store(false, std::memory_order_release);
            s_start_in_progress = false;
            s_connection_pending = false;
            start_connection_now(credentials, candidate);
        }
        return;
    }
    start_connection_now(credentials, candidate);
}

void schedule_retry(int32_t reason)
{
    uint32_t retry_count = 0;
    {
        StatusLock lock;
        if (!lock.locked()) {
            return;
        }
        s_status.disconnect_reason = reason;
        retry_count = ++s_status.retry_count;
    }
    if (!s_had_ip && retry_count > kInitialRetryLimit) {
        s_retry_pending = false;
        set_state(NetworkWifiState::kFault, ESP_ERR_WIFI_CONN);
        return;
    }
    const uint32_t delay_ms = network_wifi_retry_delay_ms(retry_count);
    s_retry_due_us = esp_timer_get_time() + static_cast<int64_t>(delay_ms) * 1000;
    s_retry_pending = true;
    set_state(NetworkWifiState::kRetryWait);
}

void converge_driver_stopped(NetworkWifiState state, esp_err_t error)
{
    s_start_in_progress = false;
    s_driver_started.store(false, std::memory_order_release);
    s_associated = false;
    clear_ip_status();
    notify_online_sink(false);
    set_state(state, error);
}

void process_scan_done()
{
    if (s_scan_cancelled) {
        s_scan_cancelled = false;
        s_scan_pending = false;
        s_scan_then_stop = false;
        esp_wifi_clear_ap_list();
        StatusLock lock;
        if (lock.locked()) {
            s_status.scan_running = false;
        }
        return;
    }
    uint16_t count = kMaxScanResults;
    std::array<wifi_ap_record_t, kMaxScanResults> records{};
    const esp_err_t error = esp_wifi_scan_get_ap_records(&count, records.data());
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status.scan_running = false;
            s_status.last_error = error;
        }
    }
    if (error == ESP_OK) {
        std::sort(records.begin(), records.begin() + count,
                  [](const wifi_ap_record_t &left, const wifi_ap_record_t &right) {
                      return left.rssi > right.rssi;
                  });
        for (uint16_t index = 0; index < count; ++index) {
            NetworkWifiEvent event{};
            event.type = NetworkWifiEventType::kScanResult;
            event.state = s_status.state;
            copy_text(event.ssid, sizeof(event.ssid), reinterpret_cast<const char *>(records[index].ssid));
            event.rssi = records[index].rssi;
            event.channel = records[index].primary;
            event.auth_mode = static_cast<uint8_t>(records[index].authmode);
            deliver_event(event);
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    NetworkWifiEvent complete{};
    complete.type = error == ESP_OK ? NetworkWifiEventType::kScanCompleted : NetworkWifiEventType::kError;
    complete.error = error;
    complete.reason = count;
    queue_critical_event(complete);
    if (s_scan_then_stop) {
        s_scan_then_stop = false;
        s_operator_stopped = true;
        set_state(NetworkWifiState::kStopping);
        const esp_err_t stop_error = esp_wifi_stop();
        if (stop_error != ESP_OK && stop_error != ESP_ERR_WIFI_NOT_STARTED) {
            set_state(NetworkWifiState::kFault, stop_error);
        } else if (stop_error == ESP_ERR_WIFI_NOT_STARTED) {
            converge_driver_stopped(NetworkWifiState::kOff, ESP_OK);
        }
    } else if (network_wifi_is_online()) {
        set_state(NetworkWifiState::kOnline);
    }
}

void start_scan()
{
    s_scan_cancelled = false;
    const esp_err_t error = esp_wifi_scan_start(nullptr, false);
    s_scan_pending = false;
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status.scan_running = error == ESP_OK;
        }
    }
    if (error != ESP_OK) {
        set_state(NetworkWifiState::kFault, error);
    }
}

void process_message(const Message &message)
{
    switch (message.type) {
        case MessageType::kStartSaved:
            begin_connection(message.credentials, false);
            break;
        case MessageType::kConnect:
            begin_connection(message.credentials, true);
            break;
        case MessageType::kStop:
            s_operator_stopped = true;
            s_connection_pending = false;
            s_retry_pending = false;
            s_associated = false;
            cancel_running_scan();
            if (s_driver_started.load(std::memory_order_acquire) || s_start_in_progress) {
                set_state(NetworkWifiState::kStopping);
                const esp_err_t error = esp_wifi_stop();
                if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) {
                    set_state(NetworkWifiState::kFault, error);
                } else if (error == ESP_ERR_WIFI_NOT_STARTED) {
                    converge_driver_stopped(NetworkWifiState::kOff, ESP_OK);
                }
            } else {
                converge_driver_stopped(NetworkWifiState::kOff, ESP_OK);
            }
            break;
        case MessageType::kForget: {
            const esp_err_t error = forget_wifi_credentials();
            {
                StatusLock lock;
                if (lock.locked()) {
                    s_status.persistence_error = error;
                    if (error == ESP_OK) {
                        s_saved_credentials = {};
                        s_active_credentials = {};
                        s_active_candidate = false;
                        s_status.saved_known = true;
                        s_status.saved = false;
                        s_status.active_saved = false;
                        s_status.saved_ssid[0] = '\0';
                    }
                }
            }
            s_operator_stopped = true;
            s_connection_pending = false;
            s_retry_pending = false;
            s_associated = false;
            cancel_running_scan();
            if (s_driver_started.load(std::memory_order_acquire) || s_start_in_progress) {
                set_state(NetworkWifiState::kStopping, error);
                const esp_err_t stop_error = esp_wifi_stop();
                if (stop_error != ESP_OK && stop_error != ESP_ERR_WIFI_NOT_STARTED) {
                    set_state(NetworkWifiState::kFault, stop_error);
                } else if (stop_error == ESP_ERR_WIFI_NOT_STARTED) {
                    converge_driver_stopped(error == ESP_OK ? NetworkWifiState::kOff
                                                             : NetworkWifiState::kFault,
                                             error);
                }
            } else {
                converge_driver_stopped(error == ESP_OK ? NetworkWifiState::kOff
                                                         : NetworkWifiState::kFault,
                                         error);
            }
            break;
        }
        case MessageType::kScan: {
            NetworkWifiStatus status{};
            if (get_network_wifi_status(&status) != ESP_OK || status.scan_running ||
                (status.state != NetworkWifiState::kOff && status.state != NetworkWifiState::kOnline)) {
                emit_state_event(NetworkWifiEventType::kError, ESP_ERR_INVALID_STATE);
                break;
            }
            const esp_err_t init_error = ensure_driver_initialized();
            if (init_error != ESP_OK) {
                set_state(NetworkWifiState::kFault, init_error);
                break;
            }
            s_scan_pending = true;
            s_scan_then_stop = !s_driver_started.load(std::memory_order_acquire);
            if (s_driver_started.load(std::memory_order_acquire)) {
                start_scan();
            } else {
                set_state(NetworkWifiState::kStarting);
                s_start_in_progress = true;
                esp_err_t start_error = apply_hostname_to_netif(true);
                if (start_error == ESP_OK) {
                    start_error = esp_wifi_start();
                }
                if (start_error != ESP_OK) {
                    s_start_in_progress = false;
                    set_state(NetworkWifiState::kFault, start_error);
                }
            }
            break;
        }
        case MessageType::kStaStarted:
            s_start_in_progress = false;
            s_driver_started.store(true, std::memory_order_release);
            {
                StatusLock lock;
                if (lock.locked()) {
                    s_status.driver_started = true;
                }
            }
            if (s_operator_stopped && !s_scan_pending) {
                set_state(NetworkWifiState::kStopping);
                const esp_err_t error = esp_wifi_stop();
                if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) {
                    set_state(NetworkWifiState::kFault, error);
                } else if (error == ESP_ERR_WIFI_NOT_STARTED) {
                    converge_driver_stopped(NetworkWifiState::kOff, ESP_OK);
                }
            } else if (s_scan_pending) {
                start_scan();
            } else if (wifi_credentials_are_valid(s_active_credentials)) {
                const esp_err_t error = esp_wifi_connect();
                set_state(error == ESP_OK ? NetworkWifiState::kConnecting : NetworkWifiState::kFault,
                          error);
            } else {
                set_state(NetworkWifiState::kOff);
            }
            break;
        case MessageType::kStaStopped: {
            s_start_in_progress = false;
            s_driver_started.store(false, std::memory_order_release);
            s_associated = false;
            clear_ip_status();
            {
                StatusLock lock;
                if (lock.locked()) {
                    s_status.driver_started = false;
                }
            }
            notify_online_sink(false);
            set_state(NetworkWifiState::kOff);
            if (s_connection_pending) {
                const WifiCredentials credentials = s_pending_credentials;
                const bool candidate = s_pending_candidate;
                s_connection_pending = false;
                start_connection_now(credentials, candidate);
            } else {
                StatusLock lock;
                if (lock.locked()) {
                    s_status.active_ssid[0] = '\0';
                }
            }
            break;
        }
        case MessageType::kStaConnected:
            if (!s_connection_pending && !s_operator_stopped &&
                std::strcmp(message.ssid, s_active_credentials.ssid) == 0) {
                s_associated = true;
                set_state(NetworkWifiState::kWaitingDhcp);
            }
            break;
        case MessageType::kStaDisconnected:
            clear_ip_status();
            notify_online_sink(false);
            emit_state_event(NetworkWifiEventType::kDisconnected, ESP_OK, message.reason);
            s_associated = false;
            if (!s_operator_stopped && !s_connection_pending) {
                schedule_retry(message.reason);
            }
            break;
        case MessageType::kGotIp: {
            wifi_ap_record_t ap{};
            const esp_err_t ap_error = esp_wifi_sta_get_ap_info(&ap);
            if (!s_associated || s_connection_pending || s_operator_stopped || ap_error != ESP_OK ||
                std::strcmp(reinterpret_cast<const char *>(ap.ssid), s_active_credentials.ssid) != 0) {
                break;
            }
            s_had_ip = true;
            s_retry_pending = false;
            set_ip_status(message.ip, message.netmask, message.gateway);
            const bool save_candidate = s_active_candidate;
            const esp_err_t persistence_error = save_candidate
                                                    ? save_wifi_credentials(s_active_credentials)
                                                    : ESP_OK;
            {
                StatusLock lock;
                if (lock.locked()) {
                    if (save_candidate && persistence_error == ESP_OK) {
                        s_saved_credentials = s_active_credentials;
                        s_active_candidate = false;
                        s_status.saved_known = true;
                        s_status.saved = true;
                        copy_text(s_status.saved_ssid, sizeof(s_status.saved_ssid),
                                  s_saved_credentials.ssid);
                    }
                    s_status.state = NetworkWifiState::kOnline;
                    s_status.last_error = ESP_OK;
                    s_status.retry_count = 0;
                    s_status.rssi = ap.rssi;
                    s_status.persistence_error = persistence_error;
                    s_status.active_saved = credentials_equal(s_active_credentials,
                                                               s_saved_credentials);
                }
            }
            NetworkWifiEvent event{};
            event.type = NetworkWifiEventType::kConnected;
            event.state = NetworkWifiState::kOnline;
            copy_text(event.ssid, sizeof(event.ssid), s_active_credentials.ssid);
            event.ip = message.ip;
            event.netmask = message.netmask;
            event.gateway = message.gateway;
            event.rssi = ap.rssi;
            event.error = persistence_error;
            queue_critical_event(event);
            notify_online_sink(true);
            break;
        }
        case MessageType::kLostIp:
            clear_ip_status();
            notify_online_sink(false);
            if (!s_operator_stopped) {
                set_state(NetworkWifiState::kConnecting);
            }
            break;
        case MessageType::kScanDone:
            process_scan_done();
            break;
        case MessageType::kOtaLock: {
            const bool stable = network_wifi_is_online() &&
                                s_driver_started.load(std::memory_order_acquire) &&
                                !s_scan_pending && !scan_is_running();
            const esp_err_t error = stable ? esp_wifi_set_ps(WIFI_PS_NONE)
                                           : ESP_ERR_INVALID_STATE;
            if (error == ESP_OK) {
                s_ota_ps_needs_restore = true;
            } else {
                s_ota_locked.store(false, std::memory_order_release);
            }
            xQueueSend(s_ota_reply_queue, &error, 0);
            break;
        }
        case MessageType::kOtaUnlock: {
            const esp_err_t error = s_ota_ps_needs_restore &&
                                            s_driver_started.load(std::memory_order_acquire)
                                        ? esp_wifi_set_ps(WIFI_PS_MIN_MODEM)
                                        : ESP_OK;
            if (error == ESP_OK) {
                s_ota_ps_needs_restore = false;
                s_ota_locked.store(false, std::memory_order_release);
            }
            xQueueSend(s_ota_reply_queue, &error, 0);
            break;
        }
    }
}

void reconcile_driver_state()
{
    if (!s_reconcile_required.exchange(false, std::memory_order_acq_rel) || s_sta_netif == nullptr) {
        return;
    }
    if (scan_is_running()) {
        Message message{};
        message.type = MessageType::kScanDone;
        process_message(message);
        return;
    }
    esp_netif_ip_info_t ip_info{};
    wifi_ap_record_t ap{};
    const bool has_ip = esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0;
    const bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (has_ip && associated &&
        std::strcmp(reinterpret_cast<const char *>(ap.ssid), s_active_credentials.ssid) == 0) {
        s_associated = true;
        Message message{};
        message.type = MessageType::kGotIp;
        message.ip = ip_info.ip.addr;
        message.netmask = ip_info.netmask.addr;
        message.gateway = ip_info.gw.addr;
        process_message(message);
    } else if (associated) {
        s_associated = true;
        clear_ip_status();
        notify_online_sink(false);
        set_state(NetworkWifiState::kWaitingDhcp);
    } else if (s_operator_stopped || s_connection_pending) {
        Message message{};
        message.type = MessageType::kStaStopped;
        process_message(message);
    } else if (s_start_in_progress) {
        Message message{};
        message.type = MessageType::kStaStarted;
        process_message(message);
    } else if (s_driver_started.load(std::memory_order_acquire)) {
        Message message{};
        message.type = MessageType::kStaDisconnected;
        message.reason = WIFI_REASON_UNSPECIFIED;
        process_message(message);
    }
}

TickType_t retry_wait_ticks()
{
    if (s_critical_event_count != 0) {
        return pdMS_TO_TICKS(10);
    }
    if (!s_retry_pending) {
        return portMAX_DELAY;
    }
    const int64_t remaining_us = s_retry_due_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    const uint64_t ticks = (static_cast<uint64_t>(remaining_us) * configTICK_RATE_HZ + 999999U) / 1000000U;
    return static_cast<TickType_t>(ticks == 0 ? 1 : ticks);
}

void network_task(void *)
{
    while (true) {
        retry_critical_event();
        NetworkHostnameStatus hostname{};
        if (get_network_hostname_status(&hostname) == ESP_OK && s_sta_netif != nullptr &&
            hostname.configured_generation != s_hostname_attempted_generation) {
            (void)apply_hostname_to_netif(false);
            continue;
        }
        Message message{};
        if (xQueueReceive(s_driver_event_queue, &message, 0) == pdTRUE) {
            process_message(message);
            continue;
        }
        if (s_reconcile_required.load(std::memory_order_acquire)) {
            reconcile_driver_state();
            continue;
        }
        if (xQueueReceive(s_message_queue, &message, 0) == pdTRUE) {
            process_message(message);
            continue;
        }
        if (s_retry_pending && esp_timer_get_time() >= s_retry_due_us) {
            s_retry_pending = false;
            const esp_err_t error = esp_wifi_connect();
            set_state(error == ESP_OK ? NetworkWifiState::kConnecting : NetworkWifiState::kFault,
                      error);
            continue;
        }
        ulTaskNotifyTake(pdTRUE, retry_wait_ticks());
    }
}

esp_err_t enqueue_command(Message message)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    if (xSemaphoreTake(s_admission_mutex, kMutexWait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = ESP_OK;
    if (s_ota_locked.load(std::memory_order_acquire)) {
        error = ESP_ERR_INVALID_STATE;
    } else if (xQueueSend(s_message_queue, &message, kApiWait) != pdTRUE) {
        error = ESP_ERR_TIMEOUT;
    } else {
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_admission_mutex);
    return error;
}

}  // namespace

esp_err_t initialize_network_wifi()
{
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    s_status = {};
    s_mutex = xSemaphoreCreateMutex();
    s_admission_mutex = xSemaphoreCreateMutex();
    s_hostname_mutex = xSemaphoreCreateMutex();
    s_message_queue = xQueueCreate(kMessageQueueDepth, sizeof(Message));
    s_driver_event_queue = xQueueCreate(kDriverEventQueueDepth, sizeof(Message));
    s_ota_reply_queue = xQueueCreate(1, sizeof(esp_err_t));
    if (s_mutex == nullptr || s_admission_mutex == nullptr || s_hostname_mutex == nullptr ||
        s_message_queue == nullptr || s_driver_event_queue == nullptr ||
        s_ota_reply_queue == nullptr) {
        cleanup_failed_network_initialization();
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    uint8_t station_mac[6]{};
    esp_err_t mac_error = esp_read_mac(station_mac, ESP_MAC_WIFI_STA);
    char default_hostname[kNetworkHostnameCapacity]{};
    char mac_suffix[kNetworkHostnameMacSuffixCapacity]{};
    if (mac_error == ESP_OK &&
        !derive_default_network_hostname(station_mac, default_hostname, sizeof(default_hostname),
                                         mac_suffix, sizeof(mac_suffix))) {
        mac_error = ESP_ERR_INVALID_SIZE;
    }
    if (mac_error != ESP_OK) {
        cleanup_failed_network_initialization();
        s_initialization_error.store(mac_error, std::memory_order_release);
        return mac_error;
    }
    char saved_hostname[kNetworkHostnameCapacity]{};
    const esp_err_t hostname_error =
        load_saved_network_hostname(saved_hostname, sizeof(saved_hostname));
    {
        StatusLock lock;
        if (!lock.locked()) {
            cleanup_failed_network_initialization();
            s_initialization_error.store(ESP_ERR_TIMEOUT, std::memory_order_release);
            return ESP_ERR_TIMEOUT;
        }
        copy_text(s_hostname_status.default_hostname,
                  sizeof(s_hostname_status.default_hostname), default_hostname);
        copy_text(s_hostname_status.configured_hostname,
                  sizeof(s_hostname_status.configured_hostname),
                  hostname_error == ESP_OK ? saved_hostname : default_hostname);
        copy_text(s_hostname_status.mac_suffix, sizeof(s_hostname_status.mac_suffix), mac_suffix);
        s_hostname_status.custom = hostname_error == ESP_OK;
        s_hostname_status.configured_generation = 1;
        s_hostname_status.persistence_error =
            hostname_error == ESP_ERR_NOT_FOUND ? ESP_OK : hostname_error;
    }

    WifiCredentials saved{};
    const esp_err_t saved_error = load_saved_wifi_credentials(&saved);
    if (saved_error == ESP_OK) {
        s_saved_credentials = saved;
    }
    if (xTaskCreate(network_task, "wifi_mgr", kTaskStackSize, nullptr, kTaskPriority, &s_task) != pdPASS) {
        StatusLock lock;
        if (lock.locked()) {
            s_status = {};
            s_status.initialization_error = ESP_ERR_NO_MEM;
        }
        cleanup_failed_network_initialization();
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status = {};
            s_status.available = true;
            s_status.initialization_error = ESP_OK;
            s_status.saved_known = saved_error == ESP_OK || saved_error == ESP_ERR_NOT_FOUND;
            s_status.saved = saved_error == ESP_OK;
            s_status.persistence_error = saved_error == ESP_ERR_NOT_FOUND ? ESP_OK : saved_error;
            if (saved_error == ESP_OK) {
                copy_text(s_status.saved_ssid, sizeof(s_status.saved_ssid), saved.ssid);
            }
        }
    }
    s_available.store(true, std::memory_order_release);
    s_initialization_error.store(ESP_OK, std::memory_order_release);
    return ESP_OK;
}

esp_err_t prepare_network_wifi_stack()
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    return ensure_network_stack_initialized();
}

esp_err_t start_saved_network_wifi()
{
    Message message{};
    message.type = MessageType::kStartSaved;
    {
        StatusLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        if (!s_status.saved) {
            return ESP_ERR_NOT_FOUND;
        }
        message.credentials = s_saved_credentials;
    }
    return enqueue_command(message);
}

esp_err_t connect_network_wifi(const WifiCredentials &credentials)
{
    if (!wifi_credentials_are_valid(credentials)) {
        return ESP_ERR_INVALID_ARG;
    }
    Message message{};
    message.type = MessageType::kConnect;
    message.credentials = credentials;
    return enqueue_command(message);
}

esp_err_t stop_network_wifi()
{
    Message message{};
    message.type = MessageType::kStop;
    return enqueue_command(message);
}

esp_err_t forget_network_wifi()
{
    Message message{};
    message.type = MessageType::kForget;
    return enqueue_command(message);
}

esp_err_t scan_network_wifi()
{
    Message message{};
    message.type = MessageType::kScan;
    return enqueue_command(message);
}

esp_err_t get_network_wifi_status(NetworkWifiStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available.load(std::memory_order_acquire)) {
        *status = {};
        status->event_drops = s_event_drops.load(std::memory_order_relaxed);
        status->ota_locked = s_ota_locked.load(std::memory_order_relaxed);
        status->initialization_error = s_initialization_error.load(std::memory_order_acquire);
        return ESP_OK;
    }
    StatusLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_status;
    status->event_drops = s_event_drops.load(std::memory_order_relaxed);
    status->ota_locked = s_ota_locked.load(std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t get_network_hostname_status(NetworkHostnameStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    StatusLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_hostname_status;
    return ESP_OK;
}

esp_err_t set_network_hostname(const char *hostname)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    char canonical[kNetworkHostnameCapacity]{};
    if (!canonicalize_network_hostname(hostname, canonical, sizeof(canonical))) {
        return ESP_ERR_INVALID_ARG;
    }
    char default_hostname[kNetworkHostnameCapacity]{};
    {
        StatusLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        copy_text(default_hostname, sizeof(default_hostname),
                  s_hostname_status.default_hostname);
    }
    if (std::strcmp(canonical, default_hostname) == 0) {
        return reset_network_hostname();
    }
    HostnameLock hostname_lock;
    if (!hostname_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t persistence_error = save_network_hostname(canonical);
    if (persistence_error != ESP_OK) {
        StatusLock lock;
        if (lock.locked()) {
            s_hostname_status.persistence_error = persistence_error;
        }
        return persistence_error;
    }
    {
        StatusLock lock(portMAX_DELAY);
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        const bool changed = !s_hostname_status.custom ||
                             std::strcmp(s_hostname_status.configured_hostname, canonical) != 0;
        copy_text(s_hostname_status.configured_hostname,
                  sizeof(s_hostname_status.configured_hostname), canonical);
        s_hostname_status.custom = true;
        s_hostname_status.persistence_error = ESP_OK;
        if (changed && ++s_hostname_status.configured_generation == 0) {
            s_hostname_status.configured_generation = 1;
        }
    }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t reset_network_hostname()
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    HostnameLock hostname_lock;
    if (!hostname_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t persistence_error = forget_network_hostname();
    if (persistence_error != ESP_OK) {
        StatusLock lock;
        if (lock.locked()) {
            s_hostname_status.persistence_error = persistence_error;
        }
        return persistence_error;
    }
    {
        StatusLock lock(portMAX_DELAY);
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        const bool changed = s_hostname_status.custom ||
                             std::strcmp(s_hostname_status.configured_hostname,
                                         s_hostname_status.default_hostname) != 0;
        copy_text(s_hostname_status.configured_hostname,
                  sizeof(s_hostname_status.configured_hostname),
                  s_hostname_status.default_hostname);
        s_hostname_status.custom = false;
        s_hostname_status.persistence_error = ESP_OK;
        if (changed && ++s_hostname_status.configured_generation == 0) {
            s_hostname_status.configured_generation = 1;
        }
    }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t set_network_wifi_event_sink(NetworkWifiEventSink sink, void *context)
{
    {
        StatusLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        s_event_sink_context = context;
        s_event_sink = sink;
    }
    if (sink == nullptr) {
        const TickType_t started = xTaskGetTickCount();
        while (s_sink_callbacks_in_flight.load(std::memory_order_acquire) != 0 &&
               xTaskGetTickCount() - started < pdMS_TO_TICKS(1000)) {
            vTaskDelay(1);
        }
        if (s_sink_callbacks_in_flight.load(std::memory_order_acquire) != 0) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t set_network_wifi_online_sink(NetworkWifiOnlineSink sink, void *context)
{
    {
        StatusLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        s_online_sink_context = context;
        s_online_sink = sink;
    }
    if (sink == nullptr) {
        const TickType_t started = xTaskGetTickCount();
        while (s_sink_callbacks_in_flight.load(std::memory_order_acquire) != 0 &&
               xTaskGetTickCount() - started < pdMS_TO_TICKS(1000)) {
            vTaskDelay(1);
        }
        if (s_sink_callbacks_in_flight.load(std::memory_order_acquire) != 0) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t set_network_wifi_ota_lock(bool enabled)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    if (xSemaphoreTake(s_admission_mutex, kMutexWait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool current = s_ota_locked.load(std::memory_order_acquire);
    if (current == enabled) {
        xSemaphoreGive(s_admission_mutex);
        return ESP_OK;
    }

    xQueueReset(s_ota_reply_queue);
    if (enabled) {
        s_ota_locked.store(true, std::memory_order_release);
    }
    Message message{};
    message.type = enabled ? MessageType::kOtaLock : MessageType::kOtaUnlock;
    esp_err_t error = ESP_ERR_TIMEOUT;
    if (xQueueSend(s_message_queue, &message, kApiWait) == pdTRUE) {
        xTaskNotifyGive(s_task);
        xQueueReceive(s_ota_reply_queue, &error, portMAX_DELAY);
    } else if (enabled) {
        s_ota_locked.store(false, std::memory_order_release);
    }
    xSemaphoreGive(s_admission_mutex);
    return error;
}

bool network_wifi_is_online()
{
    StatusLock lock;
    return lock.locked() && s_status.state == NetworkWifiState::kOnline && s_status.ip != 0;
}

}  // namespace rfbridge
