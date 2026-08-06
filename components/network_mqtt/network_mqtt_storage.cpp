#include "network_mqtt_storage.hpp"

#include "esp_heap_caps.h"
#include "nvs.h"
#include "platform_board.hpp"
#include "platform_nvs.hpp"

namespace rfbridge {
namespace {

constexpr char kNamespace[] = "mqtt_cfg";
constexpr char kServiceKey[] = "service";
constexpr char kAdvertisedKey[] = "advertised";

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

enum class RecordMemory : uint8_t {
    kInternal,
    kCold,
};

template <std::size_t Capacity, RecordMemory Memory>
class SecureRecordBuffer {
public:
    SecureRecordBuffer()
    {
        uint32_t capabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        if (Memory == RecordMemory::kCold && current_board_info().psram_mib != 0) {
            capabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        }
        data_ = static_cast<uint8_t *>(heap_caps_calloc(1, Capacity, capabilities));
    }

    ~SecureRecordBuffer()
    {
        if (data_ != nullptr) {
            volatile uint8_t *cursor = data_;
            for (std::size_t index = 0; index < Capacity; ++index) {
                cursor[index] = 0;
            }
            heap_caps_free(data_);
        }
    }

    SecureRecordBuffer(const SecureRecordBuffer &) = delete;
    SecureRecordBuffer &operator=(const SecureRecordBuffer &) = delete;

    explicit operator bool() const { return data_ != nullptr; }
    uint8_t *get() { return data_; }

private:
    uint8_t *data_ = nullptr;
};

esp_err_t format_error(MqttConfigFormatResult result)
{
    switch (result) {
        case MqttConfigFormatResult::kOk: return ESP_OK;
        case MqttConfigFormatResult::kInvalidVersion: return ESP_ERR_INVALID_VERSION;
        case MqttConfigFormatResult::kInvalidCrc: return ESP_ERR_INVALID_CRC;
        case MqttConfigFormatResult::kBufferTooSmall: return ESP_ERR_INVALID_SIZE;
        case MqttConfigFormatResult::kInvalidArgument: return ESP_ERR_INVALID_ARG;
        case MqttConfigFormatResult::kInvalidRecord: return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t open_namespace(nvs_open_mode_t mode, NvsHandle *handle)
{
    if (!platform_nvs_is_available()) {
        return platform_nvs_initialization_error();
    }
    return nvs_open(kNamespace, mode, handle->output());
}

template <std::size_t Capacity, RecordMemory Memory, typename Value, typename Decoder>
esp_err_t load_record(const char *key, Value *value, Decoder decode)
{
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    NvsHandle handle;
    esp_err_t error = open_namespace(NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    std::size_t size = 0;
    error = nvs_get_blob(handle.get(), key, nullptr, &size);
    if (error != ESP_OK) {
        return error;
    }
    if (size == 0 || size > Capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    SecureRecordBuffer<Capacity, Memory> record;
    if (!record) {
        return ESP_ERR_NO_MEM;
    }
    error = nvs_get_blob(handle.get(), key, record.get(), &size);
    if (error != ESP_OK) {
        return error;
    }
    return format_error(decode(record.get(), size, value));
}

template <std::size_t Capacity, RecordMemory Memory, typename Value, typename Encoder>
esp_err_t save_record(const char *key, const Value &value, Encoder encode)
{
    SecureRecordBuffer<Capacity, Memory> record;
    if (!record) {
        return ESP_ERR_NO_MEM;
    }
    std::size_t size = 0;
    const esp_err_t encoding_error =
        format_error(encode(value, record.get(), Capacity, &size));
    if (encoding_error != ESP_OK) {
        return encoding_error;
    }
    NvsHandle handle;
    esp_err_t error = open_namespace(NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle.get(), key, record.get(), size);
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) {
        error = nvs_erase_key(handle.get(), key);
        if (error == ESP_OK) {
            error = nvs_set_blob(handle.get(), key, record.get(), size);
        }
    }
    return error == ESP_OK ? nvs_commit(handle.get()) : error;
}

esp_err_t erase_record(const char *key)
{
    NvsHandle handle;
    esp_err_t error = open_namespace(NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_key(handle.get(), key);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return error == ESP_OK ? nvs_commit(handle.get()) : error;
}

}  // namespace

esp_err_t load_mqtt_service_config(MqttServiceConfig *config)
{
    return load_record<kMqttServiceMaxRecordSize, RecordMemory::kInternal>(
        kServiceKey, config, decode_mqtt_service_record);
}

esp_err_t save_mqtt_service_config(const MqttServiceConfig &config)
{
    return save_record<kMqttServiceMaxRecordSize, RecordMemory::kInternal>(
        kServiceKey, config, encode_mqtt_service_record);
}

esp_err_t erase_mqtt_service_config()
{
    return erase_record(kServiceKey);
}

esp_err_t load_mqtt_advertised_ledger(MqttAdvertisedLedger *ledger)
{
    return load_record<kMqttAdvertisedMaxRecordSize, RecordMemory::kCold>(
        kAdvertisedKey, ledger, decode_mqtt_advertised_record);
}

esp_err_t save_mqtt_advertised_ledger(const MqttAdvertisedLedger &ledger)
{
    return save_record<kMqttAdvertisedMaxRecordSize, RecordMemory::kCold>(
        kAdvertisedKey, ledger, encode_mqtt_advertised_record);
}

esp_err_t erase_mqtt_advertised_ledger()
{
    return erase_record(kAdvertisedKey);
}

}  // namespace rfbridge
