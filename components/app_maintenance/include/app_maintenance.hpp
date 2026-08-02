#pragma once

#include "esp_err.h"

namespace rfbridge {

esp_err_t begin_ota_maintenance();
esp_err_t end_ota_maintenance();
bool ota_maintenance_is_active();

}  // namespace rfbridge
