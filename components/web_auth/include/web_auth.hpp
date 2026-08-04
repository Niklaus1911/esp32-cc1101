#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace rfbridge {

constexpr std::size_t kWebAuthTokenLength = 64;

struct WebAuthStatus {
    bool available = false;
    bool provisioned = false;
    uint32_t generation = 0;
    uint32_t failed_attempts = 0;
    uint32_t blocked_ms = 0;
    esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
};

esp_err_t initialize_web_auth();
esp_err_t verify_web_auth_token(const char *token, uint32_t *generation = nullptr);
esp_err_t rotate_web_auth_token(char *token, std::size_t capacity);
esp_err_t get_web_auth_status(WebAuthStatus *status);
uint32_t get_web_auth_generation();

}  // namespace rfbridge
