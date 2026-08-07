#include "app_maintenance.hpp"

#include <atomic>

#include "network_wifi.hpp"
#include "network_mqtt.hpp"
#include "rf_automation.hpp"
#include "rf_ook.hpp"

namespace rfbridge {
namespace {

std::atomic<bool> s_active{false};
std::atomic<bool> s_network_acquired{false};
std::atomic<bool> s_mqtt_acquired{false};
std::atomic<bool> s_automation_acquired{false};
std::atomic<bool> s_rf_acquired{false};

}  // namespace

esp_err_t begin_ota_maintenance()
{
    bool expected = false;
    if (!s_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = begin_network_mqtt_maintenance();
    s_mqtt_acquired.store(error == ESP_OK, std::memory_order_release);
    if (error == ESP_OK) {
        error = set_network_wifi_ota_lock(true);
    }
    s_network_acquired.store(error == ESP_OK, std::memory_order_release);
    if (error == ESP_OK) {
        error = rf_automation_set_runtime_paused(
            RfAutomationPauseReason::kOtaMaintenance, true);
        s_automation_acquired.store(error == ESP_OK, std::memory_order_release);
    }
    if (error == ESP_OK) {
        error = begin_rf_maintenance();
        s_rf_acquired.store(error == ESP_OK, std::memory_order_release);
    }
    if (error != ESP_OK) {
        end_ota_maintenance();
    }
    return error;
}

esp_err_t end_ota_maintenance()
{
    if (!s_active.load(std::memory_order_acquire)) {
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;
    if (s_rf_acquired.load(std::memory_order_acquire)) {
        const esp_err_t error = end_rf_maintenance();
        if (error == ESP_OK) {
            s_rf_acquired.store(false, std::memory_order_release);
        } else {
            first_error = error;
        }
    }
    if (s_automation_acquired.load(std::memory_order_acquire)) {
        const esp_err_t error = rf_automation_set_runtime_paused(
            RfAutomationPauseReason::kOtaMaintenance, false);
        if (error == ESP_OK) {
            s_automation_acquired.store(false, std::memory_order_release);
        } else if (first_error == ESP_OK) {
            first_error = error;
        }
    }
    if (s_mqtt_acquired.load(std::memory_order_acquire)) {
        const esp_err_t error = end_network_mqtt_maintenance();
        if (error == ESP_OK) {
            s_mqtt_acquired.store(false, std::memory_order_release);
        } else if (first_error == ESP_OK) {
            first_error = error;
        }
    }
    if (s_network_acquired.load(std::memory_order_acquire)) {
        const esp_err_t error = set_network_wifi_ota_lock(false);
        if (error == ESP_OK) {
            s_network_acquired.store(false, std::memory_order_release);
        } else if (first_error == ESP_OK) {
            first_error = error;
        }
    }
    if (!s_rf_acquired.load(std::memory_order_acquire) &&
        !s_automation_acquired.load(std::memory_order_acquire) &&
        !s_mqtt_acquired.load(std::memory_order_acquire) &&
        !s_network_acquired.load(std::memory_order_acquire)) {
        s_active.store(false, std::memory_order_release);
    }
    return first_error;
}

bool ota_maintenance_is_active()
{
    return s_active.load(std::memory_order_acquire);
}

}  // namespace rfbridge
