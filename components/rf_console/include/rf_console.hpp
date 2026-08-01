#pragma once

#include "esp_err.h"
#include "rf_ook.hpp"

namespace rfbridge {

esp_err_t start_rf_console();
void rf_console_on_frame(const RfFrame &frame, void *context);

}  // namespace rfbridge
