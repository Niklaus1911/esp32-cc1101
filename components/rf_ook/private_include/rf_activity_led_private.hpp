#pragma once

#include "esp_err.h"

namespace rfbridge {

struct BoardGpioMap;

esp_err_t initialize_rf_activity_led(const BoardGpioMap &radio_gpios);
void notify_rf_activity_led();

}  // namespace rfbridge
