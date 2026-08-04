#pragma once

#include "esp_err.h"
#include "mqtt_config_format.hpp"

namespace rfbridge {

esp_err_t load_mqtt_service_config(MqttServiceConfig *config);
esp_err_t save_mqtt_service_config(const MqttServiceConfig &config);
esp_err_t erase_mqtt_service_config();
esp_err_t load_mqtt_advertised_ledger(MqttAdvertisedLedger *ledger);
esp_err_t save_mqtt_advertised_ledger(const MqttAdvertisedLedger &ledger);
esp_err_t erase_mqtt_advertised_ledger();

}  // namespace rfbridge
