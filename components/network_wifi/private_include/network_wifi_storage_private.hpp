#pragma once

#include "esp_err.h"
#include "network_wifi_config.hpp"

namespace rfbridge {

esp_err_t load_saved_wifi_credentials(WifiCredentials *credentials);
esp_err_t save_wifi_credentials(const WifiCredentials &credentials);
esp_err_t forget_wifi_credentials();

}  // namespace rfbridge
