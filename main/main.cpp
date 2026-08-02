#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf_automation.hpp"
#include "rf_console.hpp"
#include "rf_ook.hpp"
#include "rf_storage.hpp"

namespace {

constexpr char kTag[] = "app";

}  // namespace

extern "C" void app_main(void)
{
    const esp_err_t storage_error = rfbridge::initialize_rf_storage();
    if (storage_error != ESP_OK) {
        ESP_LOGE(kTag, "Persistent RF storage unavailable: %s; NVS was not erased",
                 esp_err_to_name(storage_error));
    }
    const esp_err_t automation_error = rfbridge::initialize_rf_automation();
    if (automation_error != ESP_OK) {
        ESP_LOGE(kTag, "RF automation unavailable: %s; ordinary RF remains enabled",
                 esp_err_to_name(automation_error));
    }
    ESP_ERROR_CHECK(rfbridge::start_rf_console());
    esp_err_t error = ESP_FAIL;
    for (int attempt = 1; attempt <= 3 && error != ESP_OK; ++attempt) {
        error = rfbridge::start_rf_ook(rfbridge::rf_console_on_frame, nullptr);
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
}
