#include "rf_storage.hpp"
#include "rf_storage_rule_backend.hpp"

#include "platform_nvs.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

namespace rfbridge {
namespace {

constexpr char kCodeNamespace[] = "rf_codes";
constexpr char kRuleNamespace[] = "rf_rules";
constexpr char kRuleMetaNamespace[] = "rf_rule_meta";
constexpr char kHardwareNamespace[] = "rf_hw";
constexpr char kRecentNamespace[] = "rf_recent";
constexpr char kHardwareKey[] = "selection";
constexpr char kRecentKey[] = "history";
constexpr char kRuleEnabledKey[] = "enabled";
constexpr char kRuleLogModeKey[] = "log_mode";
constexpr TickType_t kMutexTimeout = pdMS_TO_TICKS(1000);

SemaphoreHandle_t s_mutex = nullptr;
std::atomic<bool> s_initialization_started{false};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};
std::atomic<esp_err_t> s_rule_initialization_error{ESP_ERR_INVALID_STATE};
std::atomic<esp_err_t> s_rule_meta_initialization_error{ESP_ERR_INVALID_STATE};
std::atomic<esp_err_t> s_hardware_initialization_error{ESP_ERR_INVALID_STATE};
std::atomic<esp_err_t> s_recent_initialization_error{ESP_ERR_INVALID_STATE};
std::atomic<bool> s_rule_namespace_known_absent{false};

class StorageLock {
public:
    StorageLock() : locked_(s_mutex != nullptr && xSemaphoreTake(s_mutex, kMutexTimeout) == pdTRUE) {}
    ~StorageLock()
    {
        if (locked_) {
            xSemaphoreGive(s_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};

class NvsHandle {
public:
    ~NvsHandle()
    {
        if (handle_ != 0) {
            nvs_close(handle_);
        }
    }
    nvs_handle_t *output() { return &handle_; }
    nvs_handle_t get() const { return handle_; }

private:
    nvs_handle_t handle_ = 0;
};

esp_err_t storage_ready_error()
{
    return s_initialization_error.load(std::memory_order_acquire);
}

esp_err_t rule_storage_ready_error()
{
    return s_rule_initialization_error.load(std::memory_order_acquire);
}

esp_err_t rule_meta_ready_error()
{
    return s_rule_meta_initialization_error.load(std::memory_order_acquire);
}

esp_err_t map_not_found(esp_err_t error)
{
    return error == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : error;
}

esp_err_t map_load_error(esp_err_t error)
{
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return error == ESP_ERR_NVS_TYPE_MISMATCH ? ESP_ERR_INVALID_RESPONSE : error;
}

esp_err_t map_format_result(RfStorageFormatResult result)
{
    switch (result) {
        case RfStorageFormatResult::kOk:
            return ESP_OK;
        case RfStorageFormatResult::kInvalidArgument:
            return ESP_ERR_INVALID_ARG;
        case RfStorageFormatResult::kInvalidVersion:
            return ESP_ERR_INVALID_VERSION;
        case RfStorageFormatResult::kInvalidCrc:
            return ESP_ERR_INVALID_CRC;
        case RfStorageFormatResult::kBufferTooSmall:
            return ESP_ERR_INVALID_SIZE;
        case RfStorageFormatResult::kInvalidRecord:
            return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t validate_name(const char *name)
{
    return rf_storage_name_is_valid(name) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t validate_operation(const char *name)
{
    const esp_err_t ready = storage_ready_error();
    return ready == ESP_OK ? validate_name(name) : ready;
}

bool recovery_rule_key_is_valid(const char *name)
{
    if (name == nullptr) {
        return false;
    }
    std::size_t length = 0;
    while (length < kRfStorageNameCapacity && name[length] != '\0') {
        ++length;
    }
    return length > 0 && length < kRfStorageNameCapacity;
}

esp_err_t open_namespace(const char *namespace_name, nvs_open_mode_t mode, NvsHandle *handle)
{
    return nvs_open(namespace_name, mode, handle->output());
}

esp_err_t open_storage(nvs_open_mode_t mode, NvsHandle *handle)
{
    return open_namespace(kCodeNamespace, mode, handle);
}

esp_err_t initialize_namespace(const char *namespace_name, bool *known_absent)
{
    if (known_absent == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *known_absent = false;
    NvsHandle read_handle;
    esp_err_t error = open_namespace(namespace_name, NVS_READONLY, &read_handle);
    if (error == ESP_OK) {
        return ESP_OK;
    }
    if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    }

    *known_absent = true;
    NvsHandle write_handle;
    error = open_namespace(namespace_name, NVS_READWRITE, &write_handle);
    if (error == ESP_OK) {
        *known_absent = false;
    }
    return error;
}

esp_err_t map_rule_format_result(RfRuleFormatResult result)
{
    switch (result) {
        case RfRuleFormatResult::kOk:
            return ESP_OK;
        case RfRuleFormatResult::kInvalidArgument:
            return ESP_ERR_INVALID_ARG;
        case RfRuleFormatResult::kInvalidVersion:
            return ESP_ERR_INVALID_VERSION;
        case RfRuleFormatResult::kInvalidCrc:
            return ESP_ERR_INVALID_CRC;
        case RfRuleFormatResult::kBufferTooSmall:
            return ESP_ERR_INVALID_SIZE;
        case RfRuleFormatResult::kInvalidRecord:
            return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t map_hardware_format_result(RfHardwareFormatResult result)
{
    switch (result) {
        case RfHardwareFormatResult::kOk: return ESP_OK;
        case RfHardwareFormatResult::kInvalidArgument: return ESP_ERR_INVALID_ARG;
        case RfHardwareFormatResult::kInvalidVersion: return ESP_ERR_INVALID_VERSION;
        case RfHardwareFormatResult::kInvalidCrc: return ESP_ERR_INVALID_CRC;
        case RfHardwareFormatResult::kBufferTooSmall: return ESP_ERR_INVALID_SIZE;
        case RfHardwareFormatResult::kInvalidRecord: return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t map_recent_format_result(RfRecentFormatResult result)
{
    switch (result) {
        case RfRecentFormatResult::kOk: return ESP_OK;
        case RfRecentFormatResult::kInvalidArgument: return ESP_ERR_INVALID_ARG;
        case RfRecentFormatResult::kInvalidVersion: return ESP_ERR_INVALID_VERSION;
        case RfRecentFormatResult::kInvalidCrc: return ESP_ERR_INVALID_CRC;
        case RfRecentFormatResult::kBufferTooSmall: return ESP_ERR_INVALID_SIZE;
        case RfRecentFormatResult::kInvalidRecord: return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t load_signal_owned(const char *name, RfStoredSignal *signal)
{
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_storage(NVS_READONLY, &handle), "rf_storage", "open NVS");
    std::size_t record_size = 0;
    esp_err_t error = nvs_get_blob(handle.get(), name, nullptr, &record_size);
    if (error != ESP_OK) {
        return map_load_error(error);
    }
    if (record_size > kRfStorageMaxRecordSize) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t record[kRfStorageMaxRecordSize]{};
    error = nvs_get_blob(handle.get(), name, record, &record_size);
    if (error != ESP_OK) {
        return map_load_error(error);
    }
    return map_format_result(decode_rf_storage_record(record, record_size, signal));
}

esp_err_t load_recent_history_owned(RfRecentHistory *history)
{
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRecentNamespace, NVS_READONLY, &handle), "rf_storage",
                        "open recent history");
    std::size_t record_size = 0;
    esp_err_t error = nvs_get_blob(handle.get(), kRecentKey, nullptr, &record_size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        *history = {};
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return map_load_error(error);
    }
    if (record_size > kRfRecentMaxRecordSize) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t record[kRfRecentMaxRecordSize]{};
    error = nvs_get_blob(handle.get(), kRecentKey, record, &record_size);
    if (error != ESP_OK) {
        return map_load_error(error);
    }
    return map_recent_format_result(decode_rf_recent_record(record, record_size, history));
}

esp_err_t load_rule_owned(nvs_handle_t handle, const char *trigger_name, RfStoredRule *rule)
{
    std::size_t record_size = 0;
    esp_err_t error = nvs_get_blob(handle, trigger_name, nullptr, &record_size);
    if (error != ESP_OK) {
        return map_load_error(error);
    }
    if (record_size > kRfStorageMaxRuleRecordSize) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t record[kRfStorageMaxRuleRecordSize]{};
    error = nvs_get_blob(handle, trigger_name, record, &record_size);
    if (error != ESP_OK) {
        return map_load_error(error);
    }
    return map_rule_format_result(decode_rf_rule_record(record, record_size, rule));
}

esp_err_t name_referenced_owned(const char *name, bool *referenced)
{
    *referenced = false;
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleNamespace, NVS_READONLY, &handle), "rf_storage", "open rules");
    nvs_iterator_t iterator = nullptr;
    esp_err_t error = nvs_entry_find_in_handle(handle.get(), NVS_TYPE_ANY, &iterator);
    while (error == ESP_OK) {
        nvs_entry_info_t info{};
        const esp_err_t info_error = nvs_entry_info(iterator, &info);
        if (info_error != ESP_OK) {
            nvs_release_iterator(iterator);
            return info_error;
        }
        if (info.type != NVS_TYPE_BLOB || !rf_storage_name_is_valid(info.key)) {
            nvs_release_iterator(iterator);
            return ESP_ERR_INVALID_RESPONSE;
        }
        RfStoredRule rule{};
        const esp_err_t load_error = load_rule_owned(handle.get(), info.key, &rule);
        if (load_error != ESP_OK) {
            nvs_release_iterator(iterator);
            return load_error;
        }
        if (std::strcmp(info.key, name) == 0 || std::strcmp(rule.target_name, name) == 0) {
            *referenced = true;
            nvs_release_iterator(iterator);
            return ESP_OK;
        }
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
}

}  // namespace

esp_err_t initialize_rf_storage()
{
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return storage_ready_error();
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        s_rule_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        s_rule_meta_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        s_hardware_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        s_recent_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t nvs_error = initialize_platform_nvs();
    esp_err_t code_error = nvs_error;
    esp_err_t rule_error = nvs_error;
    esp_err_t meta_error = nvs_error;
    esp_err_t hardware_error = nvs_error;
    esp_err_t recent_error = nvs_error;
    if (nvs_error == ESP_OK) {
        bool code_absent = false;
        code_error = initialize_namespace(kCodeNamespace, &code_absent);
        bool rule_absent = false;
        rule_error = initialize_namespace(kRuleNamespace, &rule_absent);
        s_rule_namespace_known_absent.store(rule_absent, std::memory_order_release);
        bool meta_absent = false;
        meta_error = initialize_namespace(kRuleMetaNamespace, &meta_absent);
        bool hardware_absent = false;
        hardware_error = initialize_namespace(kHardwareNamespace, &hardware_absent);
        bool recent_absent = false;
        recent_error = initialize_namespace(kRecentNamespace, &recent_absent);
    }

    s_rule_initialization_error.store(rule_error, std::memory_order_release);
    s_rule_meta_initialization_error.store(meta_error, std::memory_order_release);
    s_hardware_initialization_error.store(hardware_error, std::memory_order_release);
    s_recent_initialization_error.store(recent_error, std::memory_order_release);
    s_initialization_error.store(code_error, std::memory_order_release);
    return code_error;
}

bool rf_storage_is_available()
{
    return storage_ready_error() == ESP_OK;
}

esp_err_t rf_storage_initialization_error()
{
    return storage_ready_error();
}

esp_err_t rf_storage_exists(const char *name, bool *exists)
{
    if (exists == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t validation = validate_operation(name);
    if (validation != ESP_OK) {
        return validation;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_storage(NVS_READONLY, &handle), "rf_storage", "open NVS");
    const esp_err_t error = nvs_find_key(handle.get(), name, nullptr);
    if (error == ESP_OK) {
        *exists = true;
        return ESP_OK;
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        *exists = false;
        return ESP_OK;
    }
    return error;
}

esp_err_t rf_storage_create(const char *name, const RfStoredSignal &signal)
{
    const esp_err_t validation = validate_operation(name);
    if (validation != ESP_OK) {
        return validation;
    }
    uint8_t record[kRfStorageMaxRecordSize]{};
    std::size_t record_size = 0;
    const esp_err_t encode_error =
        map_format_result(encode_rf_storage_record(signal, record, sizeof(record), &record_size));
    if (encode_error != ESP_OK) {
        return encode_error;
    }

    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_storage(NVS_READWRITE, &handle), "rf_storage", "open NVS");
    const esp_err_t find_error = nvs_find_key(handle.get(), name, nullptr);
    if (find_error == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (find_error != ESP_ERR_NVS_NOT_FOUND) {
        return find_error;
    }
    ESP_RETURN_ON_ERROR(nvs_set_blob(handle.get(), name, record, record_size), "rf_storage", "set RF record");
    return nvs_commit(handle.get());
}

esp_err_t rf_storage_load(const char *name, RfStoredSignal *signal)
{
    if (signal == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t validation = validate_operation(name);
    if (validation != ESP_OK) {
        return validation;
    }

    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    RfStoredSignal loaded{};
    const esp_err_t error = load_signal_owned(name, &loaded);
    if (error == ESP_OK) {
        *signal = loaded;
    }
    return error;
}

esp_err_t rf_storage_list(RfStorageName *names, std::size_t capacity, std::size_t *count)
{
    if (count == nullptr || (names == nullptr && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = storage_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }

    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_storage(NVS_READONLY, &handle), "rf_storage", "open NVS");

    std::size_t required = 0;
    nvs_iterator_t iterator = nullptr;
    esp_err_t error = nvs_entry_find_in_handle(handle.get(), NVS_TYPE_BLOB, &iterator);
    while (error == ESP_OK) {
        ++required;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    }
    *count = required;
    if (required == 0 || (names == nullptr && capacity == 0)) {
        return ESP_OK;
    }
    if (capacity < required) {
        return ESP_ERR_INVALID_SIZE;
    }

    std::size_t index = 0;
    iterator = nullptr;
    error = nvs_entry_find_in_handle(handle.get(), NVS_TYPE_BLOB, &iterator);
    while (error == ESP_OK && index < required) {
        nvs_entry_info_t info{};
        const esp_err_t info_error = nvs_entry_info(iterator, &info);
        if (info_error != ESP_OK) {
            nvs_release_iterator(iterator);
            return info_error;
        }
        std::memcpy(names[index].value, info.key, sizeof(names[index].value));
        names[index].value[sizeof(names[index].value) - 1U] = '\0';
        ++index;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    if (error != ESP_ERR_NVS_NOT_FOUND || index != required) {
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_FAIL : error;
    }
    std::sort(names, names + required, [](const RfStorageName &left, const RfStorageName &right) {
        return std::strcmp(left.value, right.value) < 0;
    });
    return ESP_OK;
}

esp_err_t rf_storage_forget(const char *name)
{
    const esp_err_t validation = validate_operation(name);
    if (validation != ESP_OK) {
        return validation;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    bool referenced = false;
    const esp_err_t rule_ready = rule_storage_ready_error();
    if (rule_ready == ESP_OK) {
        ESP_RETURN_ON_ERROR(name_referenced_owned(name, &referenced), "rf_storage", "check rule references");
    } else if (!s_rule_namespace_known_absent.load(std::memory_order_acquire)) {
        return rule_ready;
    }
    if (referenced) {
        return ESP_ERR_INVALID_STATE;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_storage(NVS_READWRITE, &handle), "rf_storage", "open NVS");
    const esp_err_t erase_error = nvs_erase_key(handle.get(), name);
    if (erase_error != ESP_OK) {
        return map_not_found(erase_error);
    }
    return nvs_commit(handle.get());
}

esp_err_t rf_storage_hardware_get(RfHardware *hardware, bool *persisted)
{
    if (hardware == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *hardware = RfHardware::kCc1101;
    if (persisted != nullptr) {
        *persisted = false;
    }
    const esp_err_t ready = s_hardware_initialization_error.load(std::memory_order_acquire);
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kHardwareNamespace, NVS_READONLY, &handle), "rf_storage", "open hardware");
    std::size_t size = 0;
    esp_err_t error = nvs_get_blob(handle.get(), kHardwareKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK || size > 32U) {
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    uint8_t record[32]{};
    error = nvs_get_blob(handle.get(), kHardwareKey, record, &size);
    if (error != ESP_OK) {
        return error;
    }
    const esp_err_t decode_error = map_hardware_format_result(
        decode_rf_hardware_record(record, size, hardware));
    if (decode_error == ESP_OK && persisted != nullptr) {
        *persisted = true;
    }
    return decode_error;
}

esp_err_t rf_storage_hardware_set(RfHardware hardware)
{
    const esp_err_t ready = s_hardware_initialization_error.load(std::memory_order_acquire);
    if (ready != ESP_OK) {
        return ready;
    }
    uint8_t record[32]{};
    std::size_t size = 0;
    ESP_RETURN_ON_ERROR(map_hardware_format_result(
                            encode_rf_hardware_record(hardware, record, sizeof(record), &size)),
                        "rf_storage", "encode hardware");
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kHardwareNamespace, NVS_READWRITE, &handle), "rf_storage", "open hardware");
    bool matches = false;
    std::size_t existing_size = 0;
    esp_err_t existing_error = nvs_get_blob(handle.get(), kHardwareKey, nullptr, &existing_size);
    if (existing_error == ESP_OK && existing_size <= sizeof(record)) {
        uint8_t existing_record[32]{};
        existing_error = nvs_get_blob(handle.get(), kHardwareKey, existing_record, &existing_size);
        if (existing_error == ESP_OK) {
            RfHardware existing_hardware = RfHardware::kCc1101;
            matches = decode_rf_hardware_record(existing_record, existing_size, &existing_hardware) ==
                          RfHardwareFormatResult::kOk && existing_hardware == hardware;
        }
    }
    if (existing_error != ESP_OK && existing_error != ESP_ERR_NVS_NOT_FOUND &&
        existing_error != ESP_ERR_NVS_TYPE_MISMATCH) {
        return existing_error;
    }
    if (matches) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(nvs_set_blob(handle.get(), kHardwareKey, record, size), "rf_storage", "set hardware");
    return nvs_commit(handle.get());
}

esp_err_t rf_storage_recent_initialization_error()
{
    return s_recent_initialization_error.load(std::memory_order_acquire);
}

esp_err_t rf_storage_recent_append(const DecodedSignal &decoded, RfRecentSignal *appended)
{
    const esp_err_t ready = rf_storage_recent_initialization_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    RfRecentHistory history{};
    ESP_RETURN_ON_ERROR(load_recent_history_owned(&history), "rf_storage", "load recent history");
    RfRecentSignal candidate{};
    ESP_RETURN_ON_ERROR(map_recent_format_result(
                            append_rf_recent_history(&history, decoded, &candidate)),
                        "rf_storage", "append recent history");
    uint8_t record[kRfRecentMaxRecordSize]{};
    std::size_t record_size = 0;
    ESP_RETURN_ON_ERROR(map_recent_format_result(
                            encode_rf_recent_record(history, record, sizeof(record), &record_size)),
                        "rf_storage", "encode recent history");
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRecentNamespace, NVS_READWRITE, &handle), "rf_storage",
                        "open recent history");
    ESP_RETURN_ON_ERROR(nvs_set_blob(handle.get(), kRecentKey, record, record_size), "rf_storage",
                        "set recent history");
    ESP_RETURN_ON_ERROR(nvs_commit(handle.get()), "rf_storage", "commit recent history");
    if (appended != nullptr) {
        *appended = candidate;
    }
    return ESP_OK;
}

esp_err_t rf_storage_recent_list(RfRecentSignal *entries, std::size_t capacity,
                                 std::size_t *count)
{
    if (count == nullptr || (entries == nullptr && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = rf_storage_recent_initialization_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    RfRecentHistory history{};
    ESP_RETURN_ON_ERROR(load_recent_history_owned(&history), "rf_storage", "load recent history");
    *count = history.count;
    if (entries == nullptr && capacity == 0) {
        return ESP_OK;
    }
    if (capacity < history.count) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::copy(history.entries, history.entries + history.count, entries);
    return ESP_OK;
}

esp_err_t rf_storage_recent_load(uint64_t id, RfRecentSignal *entry)
{
    if (id == 0 || entry == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = rf_storage_recent_initialization_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    RfRecentHistory history{};
    ESP_RETURN_ON_ERROR(load_recent_history_owned(&history), "rf_storage", "load recent history");
    for (std::size_t index = 0; index < history.count; ++index) {
        if (history.entries[index].id == id) {
            *entry = history.entries[index];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t rf_storage_recent_clear()
{
    const esp_err_t ready = rf_storage_recent_initialization_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    RfRecentHistory history{};
    const esp_err_t load_error = load_recent_history_owned(&history);
    if (load_error == ESP_ERR_INVALID_RESPONSE || load_error == ESP_ERR_INVALID_VERSION ||
        load_error == ESP_ERR_INVALID_CRC) {
        history = {};
    } else if (load_error != ESP_OK) {
        return load_error;
    }
    history.count = 0;
    std::fill(history.entries, history.entries + kRfRecentSignalCapacity, RfRecentSignal{});
    uint8_t record[kRfRecentMaxRecordSize]{};
    std::size_t record_size = 0;
    ESP_RETURN_ON_ERROR(map_recent_format_result(
                            encode_rf_recent_record(history, record, sizeof(record), &record_size)),
                        "rf_storage", "encode empty recent history");
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRecentNamespace, NVS_READWRITE, &handle), "rf_storage",
                        "open recent history");
    ESP_RETURN_ON_ERROR(nvs_set_blob(handle.get(), kRecentKey, record, record_size), "rf_storage",
                        "clear recent history");
    return nvs_commit(handle.get());
}


esp_err_t rf_storage_rule_create_validated(const char *trigger_name, const RfStoredRule &rule,
                                                   RfStorageRuleValidator validator, void *context,
                                                   RfStoredSignal *trigger_snapshot,
                                                   RfStoredSignal *target_snapshot)
{
    const esp_err_t validation = validate_operation(trigger_name);
    if (validation != ESP_OK || validator == nullptr || !rf_stored_rule_is_valid(rule) ||
        std::strcmp(trigger_name, rule.target_name) == 0) {
        return validation != ESP_OK ? validation : ESP_ERR_INVALID_ARG;
    }
    const esp_err_t rule_ready = rule_storage_ready_error();
    if (rule_ready != ESP_OK) {
        return rule_ready;
    }
    uint8_t record[kRfStorageMaxRuleRecordSize]{};
    std::size_t record_size = 0;
    const esp_err_t encode_error =
        map_rule_format_result(encode_rf_rule_record(rule, record, sizeof(record), &record_size));
    if (encode_error != ESP_OK) {
        return encode_error;
    }

    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleNamespace, NVS_READWRITE, &handle), "rf_storage", "open rules");
    const esp_err_t find_error = nvs_find_key(handle.get(), trigger_name, nullptr);
    if (find_error == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (find_error != ESP_ERR_NVS_NOT_FOUND) {
        return find_error;
    }

    RfStoredSignal trigger_signal{};
    RfStoredSignal target_signal{};
    ESP_RETURN_ON_ERROR(load_signal_owned(trigger_name, &trigger_signal), "rf_storage", "load rule trigger");
    ESP_RETURN_ON_ERROR(load_signal_owned(rule.target_name, &target_signal), "rf_storage", "load rule target");
    ESP_RETURN_ON_ERROR(validator(trigger_signal, target_signal, context), "rf_storage", "validate rule");

    ESP_RETURN_ON_ERROR(nvs_set_blob(handle.get(), trigger_name, record, record_size), "rf_storage",
                        "set rule record");
    ESP_RETURN_ON_ERROR(nvs_commit(handle.get()), "rf_storage", "commit rule record");
    if (trigger_snapshot != nullptr) {
        *trigger_snapshot = trigger_signal;
    }
    if (target_snapshot != nullptr) {
        *target_snapshot = target_signal;
    }
    return ESP_OK;
}

esp_err_t rf_storage_rule_remove(const char *trigger_name)
{
    const esp_err_t validation = validate_name(trigger_name);
    if (validation != ESP_OK) {
        return validation;
    }
    const esp_err_t rule_ready = rule_storage_ready_error();
    if (rule_ready != ESP_OK) {
        return rule_ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleNamespace, NVS_READWRITE, &handle), "rf_storage", "open rules");
    const esp_err_t erase_error = nvs_erase_key(handle.get(), trigger_name);
    if (erase_error != ESP_OK) {
        return map_not_found(erase_error);
    }
    return nvs_commit(handle.get());
}


esp_err_t rf_storage_rule_remove_recovery(const char *trigger_key)
{
    if (!recovery_rule_key_is_valid(trigger_key)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t rule_ready = rule_storage_ready_error();
    if (rule_ready != ESP_OK) {
        return rule_ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleNamespace, NVS_READWRITE, &handle), "rf_storage", "open rules");
    const esp_err_t erase_error = nvs_erase_key(handle.get(), trigger_key);
    if (erase_error != ESP_OK) {
        return map_not_found(erase_error);
    }
    return nvs_commit(handle.get());
}

esp_err_t rf_storage_rule_list(RfStorageRuleEntry *rules, std::size_t capacity, std::size_t *count)
{
    if (count == nullptr || (rules == nullptr && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = rule_storage_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleNamespace, NVS_READONLY, &handle), "rf_storage", "open rules");

    std::size_t required = 0;
    nvs_iterator_t iterator = nullptr;
    esp_err_t error = nvs_entry_find_in_handle(handle.get(), NVS_TYPE_ANY, &iterator);
    while (error == ESP_OK) {
        ++required;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    }
    *count = required;
    if (required == 0 || (rules == nullptr && capacity == 0)) {
        return ESP_OK;
    }
    if (capacity < required) {
        return ESP_ERR_INVALID_SIZE;
    }

    std::size_t index = 0;
    iterator = nullptr;
    error = nvs_entry_find_in_handle(handle.get(), NVS_TYPE_ANY, &iterator);
    while (error == ESP_OK && index < required) {
        nvs_entry_info_t info{};
        const esp_err_t info_error = nvs_entry_info(iterator, &info);
        if (info_error != ESP_OK) {
            nvs_release_iterator(iterator);
            return info_error;
        }
        if (info.type != NVS_TYPE_BLOB || !rf_storage_name_is_valid(info.key)) {
            nvs_release_iterator(iterator);
            return ESP_ERR_INVALID_RESPONSE;
        }
        std::memcpy(rules[index].trigger_name, info.key, sizeof(rules[index].trigger_name));
        rules[index].trigger_name[sizeof(rules[index].trigger_name) - 1U] = '\0';
        const esp_err_t load_error = load_rule_owned(handle.get(), info.key, &rules[index].rule);
        if (load_error != ESP_OK) {
            nvs_release_iterator(iterator);
            return load_error;
        }
        ++index;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    if (error != ESP_ERR_NVS_NOT_FOUND || index != required) {
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_FAIL : error;
    }
    std::sort(rules, rules + required, [](const RfStorageRuleEntry &left, const RfStorageRuleEntry &right) {
        return std::strcmp(left.trigger_name, right.trigger_name) < 0;
    });
    return ESP_OK;
}


esp_err_t rf_storage_rule_name_list(RfStorageName *names, std::size_t capacity, std::size_t *count)
{
    if (count == nullptr || (names == nullptr && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = rule_storage_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleNamespace, NVS_READONLY, &handle), "rf_storage", "open rules");

    std::size_t required = 0;
    nvs_iterator_t iterator = nullptr;
    esp_err_t error = nvs_entry_find_in_handle(handle.get(), NVS_TYPE_ANY, &iterator);
    while (error == ESP_OK) {
        ++required;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    }
    *count = required;
    if (required == 0 || (names == nullptr && capacity == 0)) {
        return ESP_OK;
    }
    if (capacity < required) {
        return ESP_ERR_INVALID_SIZE;
    }

    std::size_t index = 0;
    iterator = nullptr;
    error = nvs_entry_find_in_handle(handle.get(), NVS_TYPE_ANY, &iterator);
    while (error == ESP_OK && index < required) {
        nvs_entry_info_t info{};
        const esp_err_t info_error = nvs_entry_info(iterator, &info);
        if (info_error != ESP_OK) {
            nvs_release_iterator(iterator);
            return info_error;
        }
        std::memcpy(names[index].value, info.key, sizeof(names[index].value));
        names[index].value[sizeof(names[index].value) - 1U] = '\0';
        ++index;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    if (error != ESP_ERR_NVS_NOT_FOUND || index != required) {
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_FAIL : error;
    }
    std::sort(names, names + required, [](const RfStorageName &left, const RfStorageName &right) {
        return std::strcmp(left.value, right.value) < 0;
    });
    return ESP_OK;
}

esp_err_t rf_storage_rule_load(const char *trigger_name, RfStoredRule *rule)
{
    if (rule == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t validation = validate_name(trigger_name);
    if (validation != ESP_OK) {
        return validation;
    }
    const esp_err_t ready = rule_storage_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleNamespace, NVS_READONLY, &handle), "rf_storage", "open rules");
    return load_rule_owned(handle.get(), trigger_name, rule);
}

esp_err_t rf_storage_rule_enabled_get(bool *enabled)
{
    if (enabled == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = rule_meta_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleMetaNamespace, NVS_READONLY, &handle), "rf_storage",
                        "open rule metadata");
    uint8_t value = 0;
    const esp_err_t error = nvs_get_u8(handle.get(), kRuleEnabledKey, &value);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        *enabled = true;
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error == ESP_ERR_NVS_TYPE_MISMATCH ? ESP_ERR_INVALID_RESPONSE : error;
    }
    if (value > 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *enabled = value != 0;
    return ESP_OK;
}

esp_err_t rf_storage_rule_enabled_set(bool enabled)
{
    const esp_err_t ready = rule_meta_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleMetaNamespace, NVS_READWRITE, &handle), "rf_storage",
                        "open rule metadata");
    esp_err_t set_error = nvs_set_u8(handle.get(), kRuleEnabledKey, enabled ? 1 : 0);
    if (set_error == ESP_ERR_NVS_TYPE_MISMATCH) {
        ESP_RETURN_ON_ERROR(nvs_erase_key(handle.get(), kRuleEnabledKey), "rf_storage",
                            "erase malformed enabled state");
        set_error = nvs_set_u8(handle.get(), kRuleEnabledKey, enabled ? 1 : 0);
    }
    ESP_RETURN_ON_ERROR(set_error, "rf_storage", "set rule enabled state");
    return nvs_commit(handle.get());
}


esp_err_t rf_storage_rule_log_mode_get(uint8_t *mode)
{
    if (mode == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = rule_meta_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleMetaNamespace, NVS_READONLY, &handle), "rf_storage",
                        "open rule metadata");
    uint8_t value = 0;
    const esp_err_t error = nvs_get_u8(handle.get(), kRuleLogModeKey, &value);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        *mode = 1;
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error == ESP_ERR_NVS_TYPE_MISMATCH ? ESP_ERR_INVALID_RESPONSE : error;
    }
    if (value > 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *mode = value;
    return ESP_OK;
}

esp_err_t rf_storage_rule_log_mode_set(uint8_t mode)
{
    if (mode > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = rule_meta_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    StorageLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_namespace(kRuleMetaNamespace, NVS_READWRITE, &handle), "rf_storage",
                        "open rule metadata");
    esp_err_t set_error = nvs_set_u8(handle.get(), kRuleLogModeKey, mode);
    if (set_error == ESP_ERR_NVS_TYPE_MISMATCH) {
        ESP_RETURN_ON_ERROR(nvs_erase_key(handle.get(), kRuleLogModeKey), "rf_storage",
                            "erase malformed log mode");
        set_error = nvs_set_u8(handle.get(), kRuleLogModeKey, mode);
    }
    ESP_RETURN_ON_ERROR(set_error, "rf_storage", "set rule log mode");
    return nvs_commit(handle.get());
}

}  // namespace rfbridge
