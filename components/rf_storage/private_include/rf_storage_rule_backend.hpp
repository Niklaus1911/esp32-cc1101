#pragma once

#include <cstddef>

#include "esp_err.h"
#include "rf_storage_rule_format.hpp"

namespace rfbridge {

using RfStorageRuleValidator = esp_err_t (*)(const RfStoredSignal &trigger_signal, const RfStoredSignal &target_signal,
                                             void *context);

esp_err_t rf_storage_rule_create_validated(const char *trigger_name, const RfStoredRule &rule,
                                           RfStorageRuleValidator validator, void *context,
                                           RfStoredSignal *trigger_snapshot, RfStoredSignal *target_snapshot);
esp_err_t rf_storage_rule_remove(const char *trigger_name);
esp_err_t rf_storage_rule_remove_recovery(const char *trigger_key);
esp_err_t rf_storage_rule_list(RfStorageRuleEntry *rules, std::size_t capacity, std::size_t *count);
esp_err_t rf_storage_rule_name_list(RfStorageName *names, std::size_t capacity, std::size_t *count);
esp_err_t rf_storage_rule_load(const char *trigger_name, RfStoredRule *rule);
esp_err_t rf_storage_rule_enabled_get(bool *enabled);
esp_err_t rf_storage_rule_enabled_set(bool enabled);

} // namespace rfbridge
