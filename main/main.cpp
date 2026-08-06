#include "bridge_events.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_wifi.hpp"
#include "network_mdns.hpp"
#include "network_mqtt.hpp"
#include "ota_update.hpp"
#include "platform_board.hpp"
#include "platform_nvs.hpp"
#include "rf_automation.hpp"
#include "rf_console.hpp"
#include "rf_ook.hpp"
#include "rf_signals.hpp"
#include "rf_storage.hpp"
#include "web_ui.hpp"

namespace {

constexpr char kTag[] = "app";

void network_online_changed(bool online, void *)
{
    const esp_err_t error = rfbridge::set_web_ui_network_online(online);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not update Web UI network state: %s", esp_err_to_name(error));
    }
}

void initialize_web_profile(esp_err_t *mdns_error, esp_err_t *ota_error)
{
    *mdns_error = rfbridge::initialize_network_mdns();
    if (*mdns_error != ESP_OK) {
        ESP_LOGE(kTag, "Optional mDNS unavailable: %s", esp_err_to_name(*mdns_error));
    }
    *ota_error = rfbridge::initialize_ota_update();
    if (*ota_error != ESP_OK) {
        ESP_LOGE(kTag, "LAN OTA unavailable: %s", esp_err_to_name(*ota_error));
    }
}

}  // namespace

extern "C" void app_main(void)
{
    const rfbridge::BoardInfo &board = rfbridge::current_board_info();
    const rfbridge::RfBoardImageDescriptor &image =
        rfbridge::current_board_image_descriptor();
    const rfbridge::BoardMemorySnapshot boot_memory = rfbridge::board_memory_snapshot();
    ESP_LOGI(kTag,
             "Board profile=%s target=%s flash=%uMB psram=%uMB console=%s both=%u; "
             "image_board=%u layout=%u internal_free=%lu psram_free=%lu",
             board.profile_name, board.target_name, board.flash_mib, board.psram_mib,
             rfbridge::console_transport_name(board.console), board.combined_services,
             image.board_id, image.partition_layout_id,
             static_cast<unsigned long>(boot_memory.internal.free),
             static_cast<unsigned long>(boot_memory.psram.free));
    const esp_err_t nvs_error = rfbridge::initialize_platform_nvs();
    if (nvs_error != ESP_OK) {
        ESP_LOGE(kTag, "Platform NVS unavailable: %s; NVS was not erased", esp_err_to_name(nvs_error));
    }
    const esp_err_t storage_error = rfbridge::initialize_rf_storage();
    if (storage_error != ESP_OK) {
        ESP_LOGE(kTag, "Persistent RF storage unavailable: %s; NVS was not erased",
                 esp_err_to_name(storage_error));
    }
    const esp_err_t network_error = rfbridge::initialize_network_wifi();
    if (network_error != ESP_OK) {
        ESP_LOGE(kTag, "Optional Wi-Fi unavailable: %s", esp_err_to_name(network_error));
    }
    const esp_err_t automation_error = rfbridge::initialize_rf_automation();
    if (automation_error != ESP_OK) {
        ESP_LOGE(kTag, "RF automation unavailable: %s; ordinary RF remains enabled",
                 esp_err_to_name(automation_error));
    }
    const esp_err_t profile_error = rfbridge::initialize_network_service_profile();
    if (profile_error != ESP_OK) {
        ESP_LOGE(kTag, "Network service profile record unavailable: %s; using Web recovery",
                 esp_err_to_name(profile_error));
    }

    const rfbridge::NetworkServiceMask boot_services =
        rfbridge::boot_network_service_mask();
    const bool mqtt_requested = rfbridge::network_service_mask_has(
        boot_services, rfbridge::NetworkServiceMask::kMqtt);
    bool web_requested = rfbridge::network_service_mask_has(
        boot_services, rfbridge::NetworkServiceMask::kWeb);
    bool mqtt_available = network_error == ESP_OK && mqtt_requested;
    if (mqtt_requested && network_error != ESP_OK) {
        rfbridge::mark_network_service_web_fallback(network_error);
        web_requested = true;
    }
    if (mqtt_available) {
        const esp_err_t stack_error = rfbridge::prepare_network_wifi_stack();
        if (stack_error != ESP_OK) {
            ESP_LOGE(kTag, "TCP/IP stack initialization failed for MQTT: %s",
                     esp_err_to_name(stack_error));
            rfbridge::mark_network_service_web_fallback(stack_error);
            mqtt_available = false;
            web_requested = true;
        }
    }
    if (mqtt_available) {
        const esp_err_t mqtt_error = rfbridge::prepare_network_mqtt();
        if (mqtt_error != ESP_OK) {
            ESP_LOGE(kTag, "MQTT runtime allocation failed: %s",
                     esp_err_to_name(mqtt_error));
            rfbridge::mark_network_service_web_fallback(mqtt_error);
            mqtt_available = false;
            web_requested = true;
        }
    }

    esp_err_t mdns_error = ESP_ERR_INVALID_STATE;
    esp_err_t ota_error = ESP_ERR_INVALID_STATE;
    if (web_requested && network_error == ESP_OK) {
        initialize_web_profile(&mdns_error, &ota_error);
    }
    const esp_err_t events_error = rfbridge::initialize_bridge_events();
    if (events_error != ESP_OK) {
        ESP_LOGE(kTag, "Shared event broker unavailable: %s", esp_err_to_name(events_error));
    }
    const esp_err_t signals_error = rfbridge::initialize_rf_signals();
    if (signals_error != ESP_OK) {
        ESP_LOGE(kTag, "Learned-signal service unavailable: %s", esp_err_to_name(signals_error));
    }
    if (mqtt_available) {
        const esp_err_t mqtt_activation_error = rfbridge::activate_network_mqtt();
        if (mqtt_activation_error != ESP_OK) {
            ESP_LOGE(kTag, "MQTT runtime activation failed: %s",
                     esp_err_to_name(mqtt_activation_error));
            (void)rfbridge::stop_network_mqtt_for_web_fallback();
            rfbridge::mark_network_service_web_fallback(mqtt_activation_error);
            mqtt_available = false;
            if (!web_requested && network_error == ESP_OK) {
                web_requested = true;
                initialize_web_profile(&mdns_error, &ota_error);
                (void)rfbridge::bridge_events_bind_available_sources();
            }
        }
    }

    esp_err_t web_error = ESP_ERR_INVALID_STATE;
    if (web_requested) {
        web_error = rfbridge::initialize_web_ui();
        if (web_error != ESP_OK) {
            ESP_LOGE(kTag, "Web UI unavailable: %s", esp_err_to_name(web_error));
        }
        if (network_error == ESP_OK && web_error == ESP_OK) {
            ESP_ERROR_CHECK(
                rfbridge::set_network_wifi_online_sink(network_online_changed, nullptr));
        }
    }
    rfbridge::set_network_service_web_result(web_requested ? web_error
                                                           : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(rfbridge::start_rf_console());
    esp_err_t error = ESP_FAIL;
    for (int attempt = 1; attempt <= 3 && error != ESP_OK; ++attempt) {
        error = rfbridge::start_rf_ook(rfbridge::rf_signals_on_frame, nullptr);
        if (error != ESP_OK) {
            rfbridge::RfRadioStatus status{};
            if (rfbridge::get_rf_radio_status(&status) == ESP_OK && status.running) {
                error = ESP_OK;
                break;
            }
            ESP_LOGW(kTag, "RF startup attempt %d/3 failed: %s", attempt, esp_err_to_name(error));
            if (attempt < 3) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "RF remains stopped; fix wiring and use 'radio start' to retry");
    }
    const esp_err_t confirm_error = rfbridge::confirm_running_ota_image();
    if (confirm_error != ESP_OK) {
        ESP_LOGE(kTag, "Could not confirm the running OTA image: %s",
                 esp_err_to_name(confirm_error));
    }
    if (network_error == ESP_OK) {
        const esp_err_t start_wifi_error = rfbridge::start_saved_network_wifi();
        if (start_wifi_error != ESP_OK && start_wifi_error != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(kTag, "Could not queue saved Wi-Fi connection: %s",
                     esp_err_to_name(start_wifi_error));
        }
    }
}
