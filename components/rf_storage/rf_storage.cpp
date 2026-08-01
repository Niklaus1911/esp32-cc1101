#include "rf_storage.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace rfbridge {
namespace {

constexpr char kNamespace[] = "rf_codes";
constexpr TickType_t kMutexTimeout = pdMS_TO_TICKS(1000);

SemaphoreHandle_t s_mutex = nullptr;
std::atomic<bool> s_initialization_started{false};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};

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

esp_err_t validate_operation(const char *name)
{
    const esp_err_t ready = storage_ready_error();
    if (ready != ESP_OK) {
        return ready;
    }
    return rf_storage_name_is_valid(name) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t open_storage(nvs_open_mode_t mode, NvsHandle *handle)
{
    return nvs_open(kNamespace, mode, handle->output());
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
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = nvs_flash_init();
    if (error == ESP_OK) {
        NvsHandle handle;
        error = open_storage(NVS_READWRITE, &handle);
    }
    s_initialization_error.store(error, std::memory_order_release);
    return error;
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
    RfStoredSignal loaded{};
    error = map_format_result(decode_rf_storage_record(record, record_size, &loaded));
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
    NvsHandle handle;
    ESP_RETURN_ON_ERROR(open_storage(NVS_READWRITE, &handle), "rf_storage", "open NVS");
    const esp_err_t erase_error = nvs_erase_key(handle.get(), name);
    if (erase_error != ESP_OK) {
        return map_not_found(erase_error);
    }
    return nvs_commit(handle.get());
}

}  // namespace rfbridge
