#pragma once

#include <cstdint>

#include "esp_err.h"

namespace rfbridge {

struct WebUiStatus {
    bool available = false;
    bool network_online = false;
    bool server_running = false;
    uint16_t port = 0;
    uint32_t starts = 0;
    uint32_t stops = 0;
    uint32_t start_failures = 0;
    esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
    esp_err_t last_error = ESP_OK;
};

esp_err_t initialize_web_ui();
esp_err_t set_web_ui_network_online(bool online);
esp_err_t get_web_ui_status(WebUiStatus *status);

}  // namespace rfbridge
