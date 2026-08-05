#include "web_auth.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "nvs.h"

namespace rfbridge {
namespace {

constexpr char kNamespace[] = "web_cfg";
constexpr char kAuthKey[] = "auth";
constexpr uint32_t kRecordMagic = 0x48545541U;
constexpr uint8_t kRecordVersion = 1;
constexpr TickType_t kMutexWait = pdMS_TO_TICKS(1000);
constexpr uint32_t kFailureLimit = 5;
constexpr int64_t kLockoutUs = 30000000;

struct AuthRecord {
    uint32_t magic = kRecordMagic;
    uint8_t version = kRecordVersion;
    uint8_t reserved[3]{};
    uint8_t verifier[32]{};
    uint32_t crc = 0;
};

SemaphoreHandle_t s_mutex = nullptr;
std::array<uint8_t, 32> s_verifier{};
std::atomic<bool> s_initialization_started{false};
std::atomic<bool> s_available{false};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};
std::atomic<uint32_t> s_generation{0};
uint32_t s_failed_attempts = 0;
int64_t s_blocked_until_us = 0;

class AuthLock {
public:
    AuthLock() : locked_(s_mutex != nullptr && xSemaphoreTake(s_mutex, kMutexWait) == pdTRUE) {}
    ~AuthLock()
    {
        if (locked_) {
            xSemaphoreGive(s_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};

uint32_t crc32(const uint8_t *data, std::size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

bool hash_token(const char *token, uint8_t output[32])
{
    std::size_t output_length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256,
                            reinterpret_cast<const uint8_t *>(token), std::strlen(token), output,
                            32, &output_length) == PSA_SUCCESS &&
           output_length == 32;
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right, std::size_t size)
{
    uint8_t difference = 0;
    for (std::size_t index = 0; index < size; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

void generate_token(char output[kWebAuthTokenLength + 1U])
{
    std::array<uint8_t, kWebAuthTokenLength / 2U> random{};
    esp_fill_random(random.data(), random.size());
    constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < random.size(); ++index) {
        output[index * 2U] = kHex[random[index] >> 4U];
        output[index * 2U + 1U] = kHex[random[index] & 0x0fU];
    }
    output[kWebAuthTokenLength] = '\0';
}

esp_err_t save_verifier(const uint8_t verifier[32])
{
    AuthRecord record{};
    std::memcpy(record.verifier, verifier, sizeof(record.verifier));
    record.crc = crc32(reinterpret_cast<const uint8_t *>(&record), offsetof(AuthRecord, crc));
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, kAuthKey, &record, sizeof(record));
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return error;
}

esp_err_t load_verifier(bool *found)
{
    *found = false;
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    AuthRecord record{};
    std::size_t size = sizeof(record);
    error = nvs_get_blob(handle, kAuthKey, &record, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (size != sizeof(record) || record.magic != kRecordMagic ||
        record.version != kRecordVersion ||
        record.crc != crc32(reinterpret_cast<const uint8_t *>(&record), offsetof(AuthRecord, crc))) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    std::memcpy(s_verifier.data(), record.verifier, s_verifier.size());
    *found = true;
    return ESP_OK;
}

}  // namespace

esp_err_t initialize_web_auth()
{
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        s_initialization_error.store(ESP_FAIL, std::memory_order_release);
        return ESP_FAIL;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    bool found = false;
    esp_err_t error = load_verifier(&found);
    if (error == ESP_OK && !found) {
        error = ESP_ERR_NOT_FOUND;
    }
    if (error != ESP_OK) {
        s_initialization_error.store(error, std::memory_order_release);
        return error;
    }
    s_generation.store(1, std::memory_order_release);
    s_available.store(true, std::memory_order_release);
    s_initialization_error.store(ESP_OK, std::memory_order_release);
    return ESP_OK;
}

esp_err_t verify_web_auth_token(const char *token, uint32_t *generation)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    if (token == nullptr || std::strlen(token) != kWebAuthTokenLength) {
        return ESP_ERR_INVALID_ARG;
    }
    AuthLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    const int64_t now_us = esp_timer_get_time();
    std::array<uint8_t, 32> candidate{};
    if (!hash_token(token, candidate.data())) {
        return ESP_FAIL;
    }
    const bool matches = constant_time_equal(candidate.data(), s_verifier.data(), candidate.size());
    candidate.fill(0);
    if (matches) {
        s_failed_attempts = 0;
        s_blocked_until_us = 0;
        if (generation != nullptr) {
            *generation = s_generation.load(std::memory_order_relaxed);
        }
        return ESP_OK;
    }
    if (now_us < s_blocked_until_us) {
        return ESP_ERR_INVALID_STATE;
    }
    ++s_failed_attempts;
    if (s_failed_attempts >= kFailureLimit) {
        s_failed_attempts = 0;
        s_blocked_until_us = now_us + kLockoutUs;
    }
    return ESP_ERR_INVALID_CRC;
}

esp_err_t rotate_web_auth_token(char *token, std::size_t capacity)
{
    if (token == nullptr || capacity < kWebAuthTokenLength + 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t initialization_error =
        s_initialization_error.load(std::memory_order_acquire);
    if (!s_available.load(std::memory_order_acquire) &&
        initialization_error == ESP_ERR_INVALID_STATE) {
        initialization_error = initialize_web_auth();
    }
    const bool recovering_record =
        !s_available.load(std::memory_order_acquire) &&
        (initialization_error == ESP_ERR_NOT_FOUND ||
         initialization_error == ESP_ERR_INVALID_RESPONSE);
    if (!s_available.load(std::memory_order_acquire) && !recovering_record) {
        return initialization_error;
    }
    AuthLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    generate_token(token);
    std::array<uint8_t, 32> verifier{};
    const esp_err_t error = hash_token(token, verifier.data()) ? save_verifier(verifier.data())
                                                               : ESP_FAIL;
    if (error == ESP_OK) {
        s_verifier = verifier;
        s_failed_attempts = 0;
        s_blocked_until_us = 0;
        if (recovering_record) {
            s_generation.store(1, std::memory_order_release);
            s_initialization_error.store(ESP_OK, std::memory_order_release);
            s_available.store(true, std::memory_order_release);
        } else {
            s_generation.fetch_add(1, std::memory_order_acq_rel);
        }
    } else {
        std::memset(token, 0, capacity);
    }
    return error;
}

esp_err_t get_web_auth_status(WebAuthStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    WebAuthStatus result{};
    result.available = s_available.load(std::memory_order_acquire);
    result.provisioned = result.available;
    result.generation = s_generation.load(std::memory_order_relaxed);
    result.initialization_error = s_initialization_error.load(std::memory_order_relaxed);
    if (s_mutex == nullptr) {
        *status = result;
        return ESP_OK;
    }
    AuthLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    result.failed_attempts = s_failed_attempts;
    const int64_t remaining_us = s_blocked_until_us - esp_timer_get_time();
    result.blocked_ms = remaining_us > 0
                            ? static_cast<uint32_t>((remaining_us + 999U) / 1000U)
                            : 0;
    *status = result;
    return ESP_OK;
}

uint32_t get_web_auth_generation()
{
    return s_generation.load(std::memory_order_acquire);
}

}  // namespace rfbridge
