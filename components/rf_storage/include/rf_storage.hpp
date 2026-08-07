#pragma once

#include <cstddef>

#include "esp_err.h"
#include "rf_storage_format.hpp"

namespace rfbridge {

esp_err_t initialize_rf_storage();
bool rf_storage_is_available();
esp_err_t rf_storage_initialization_error();
esp_err_t rf_storage_exists(const char *name, bool *exists);
esp_err_t rf_storage_create(const char *name, const RfStoredSignal &signal);
esp_err_t rf_storage_load(const char *name, RfStoredSignal *signal);
esp_err_t rf_storage_list(RfStorageName *names, std::size_t capacity, std::size_t *count);
esp_err_t rf_storage_forget(const char *name);
esp_err_t rf_storage_hardware_get(RfHardware *hardware, bool *persisted = nullptr);
esp_err_t rf_storage_hardware_set(RfHardware hardware);

}  // namespace rfbridge
