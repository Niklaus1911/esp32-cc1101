#pragma once

#include "esp_err.h"
#include "rf_console_format.hpp"
#include "rf_ook.hpp"

namespace rfbridge {

esp_err_t start_rf_console();
ConsoleStyle get_rf_console_style();
esp_err_t set_rf_console_style(ConsoleStyle style);
void rf_console_on_frame(const RfFrame &frame, void *context);

}  // namespace rfbridge
