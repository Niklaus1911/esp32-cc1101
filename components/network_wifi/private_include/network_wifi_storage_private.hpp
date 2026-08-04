#pragma once

#include "esp_err.h"
#include "network_hostname_config.hpp"
#include "network_wifi_config.hpp"

namespace rfbridge {

esp_err_t load_saved_wifi_credentials(WifiCredentials *credentials);
esp_err_t save_wifi_credentials(const WifiCredentials &credentials);
esp_err_t forget_wifi_credentials();
esp_err_t load_saved_network_hostname(char *hostname, std::size_t capacity);
esp_err_t save_network_hostname(const char *hostname);
esp_err_t forget_network_hostname();

}  // namespace rfbridge
