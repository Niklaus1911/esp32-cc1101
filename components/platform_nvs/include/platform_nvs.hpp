#pragma once

#include "esp_err.h"

namespace rfbridge {

esp_err_t initialize_platform_nvs();
bool platform_nvs_is_available();
esp_err_t platform_nvs_initialization_error();

}  // namespace rfbridge
