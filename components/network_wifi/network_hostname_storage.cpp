#include "network_wifi_storage_private.hpp"

#include "platform_nvs.hpp"

#include "nvs.h"

namespace rfbridge {
namespace {

constexpr char kNamespace[] = "net_cfg";
constexpr char kHostnameKey[] = "hostname";

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

esp_err_t map_format_result(NetworkHostnameFormatResult result)
{
    switch (result) {
        case NetworkHostnameFormatResult::kOk: return ESP_OK;
        case NetworkHostnameFormatResult::kInvalidArgument: return ESP_ERR_INVALID_ARG;
        case NetworkHostnameFormatResult::kInvalidVersion: return ESP_ERR_INVALID_VERSION;
        case NetworkHostnameFormatResult::kInvalidCrc: return ESP_ERR_INVALID_CRC;
        case NetworkHostnameFormatResult::kBufferTooSmall: return ESP_ERR_INVALID_SIZE;
        case NetworkHostnameFormatResult::kInvalidRecord: return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

}  // namespace

esp_err_t load_saved_network_hostname(char *hostname, std::size_t capacity)
{
    if (hostname == nullptr || capacity < kNetworkHostnameCapacity) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ready = initialize_platform_nvs();
    if (ready != ESP_OK) {
        return ready;
    }
    NvsHandle handle;
    esp_err_t error = nvs_open(kNamespace, NVS_READONLY, handle.output());
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error != ESP_OK) {
        return error;
    }
    std::size_t size = 0;
    error = nvs_get_blob(handle.get(), kHostnameKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error == ESP_ERR_NVS_TYPE_MISMATCH || size > kNetworkHostnameRecordMaxSize) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) {
        return error;
    }
    uint8_t record[kNetworkHostnameRecordMaxSize]{};
    error = nvs_get_blob(handle.get(), kHostnameKey, record, &size);
    return error == ESP_OK
               ? map_format_result(decode_network_hostname_record(record, size, hostname, capacity))
               : error;
}

esp_err_t save_network_hostname(const char *hostname)
{
    const esp_err_t ready = initialize_platform_nvs();
    if (ready != ESP_OK) {
        return ready;
    }
    uint8_t record[kNetworkHostnameRecordMaxSize]{};
    std::size_t size = 0;
    const esp_err_t format_error = map_format_result(
        encode_network_hostname_record(hostname, record, sizeof(record), &size));
    if (format_error != ESP_OK) {
        return format_error;
    }
    NvsHandle handle;
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, handle.output());
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle.get(), kHostnameKey, record, size);
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) {
        error = nvs_erase_key(handle.get(), kHostnameKey);
        if (error == ESP_OK) {
            error = nvs_set_blob(handle.get(), kHostnameKey, record, size);
        }
    }
    return error == ESP_OK ? nvs_commit(handle.get()) : error;
}

esp_err_t forget_network_hostname()
{
    const esp_err_t ready = initialize_platform_nvs();
    if (ready != ESP_OK) {
        return ready;
    }
    NvsHandle handle;
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, handle.output());
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_key(handle.get(), kHostnameKey);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return error == ESP_OK ? nvs_commit(handle.get()) : error;
}

}  // namespace rfbridge
