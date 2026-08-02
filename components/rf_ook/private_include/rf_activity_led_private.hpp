#pragma once

#include "esp_err.h"

namespace rfbridge {

esp_err_t initialize_rf_activity_led();
void notify_rf_activity_led();

}  // namespace rfbridge
