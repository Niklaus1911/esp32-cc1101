#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../private_include/network_wifi_storage_private.hpp"
#include "network_wifi_config.hpp"
#include "nvs.h"
#include "platform_nvs.hpp"
#include "unity.h"

TEST_CASE("Wi-Fi credential records validate and round trip", "[network_wifi][storage]")
{
    rfbridge::WifiCredentials credentials{};
    std::strcpy(credentials.ssid, "Workshop WiFi");
    std::strcpy(credentials.password, "personal-password");
    uint8_t record[rfbridge::kWifiConfigMaxRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_TRUE(rfbridge::wifi_credentials_are_valid(credentials));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::WifiConfigFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_wifi_config_record(
                          credentials, record, sizeof(record), &size)));

    rfbridge::WifiCredentials decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::WifiConfigFormatResult::kOk),
                      static_cast<int>(rfbridge::decode_wifi_config_record(record, size, &decoded)));
    TEST_ASSERT_EQUAL_STRING(credentials.ssid, decoded.ssid);
    TEST_ASSERT_EQUAL_STRING(credentials.password, decoded.password);

    record[8] ^= 1U;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::WifiConfigFormatResult::kInvalidCrc),
                      static_cast<int>(rfbridge::decode_wifi_config_record(record, size, &decoded)));
    std::memset(credentials.password, 0, sizeof(credentials.password));
    TEST_ASSERT_TRUE(rfbridge::wifi_credentials_are_valid(credentials));
    std::strcpy(credentials.password, "short");
    TEST_ASSERT_FALSE(rfbridge::wifi_credentials_are_valid(credentials));
    std::memset(credentials.password, 'a', 64);
    credentials.password[64] = '\0';
    TEST_ASSERT_TRUE(rfbridge::wifi_credentials_are_valid(credentials));
    credentials.password[2] = 'z';
    TEST_ASSERT_FALSE(rfbridge::wifi_credentials_are_valid(credentials));
}

TEST_CASE("Wi-Fi credential storage repairs only its key", "[network_wifi][storage][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_platform_nvs());

    rfbridge::WifiCredentials previous{};
    const esp_err_t previous_error = rfbridge::load_saved_wifi_credentials(&previous);
    TEST_ASSERT_TRUE(previous_error == ESP_OK || previous_error == ESP_ERR_NOT_FOUND);
    const bool previous_present = previous_error == ESP_OK;

    rfbridge::WifiCredentials temporary{};
    std::strcpy(temporary.ssid, "UnityNet");
    std::strcpy(temporary.password, "unity-password");
    const esp_err_t save_error = rfbridge::save_wifi_credentials(temporary);
    rfbridge::WifiCredentials loaded{};
    const esp_err_t load_error = rfbridge::load_saved_wifi_credentials(&loaded);

    nvs_handle_t handle = 0;
    esp_err_t corrupt_error = nvs_open("net_cfg", NVS_READWRITE, &handle);
    uint8_t record[rfbridge::kWifiConfigMaxRecordSize]{};
    std::size_t size = sizeof(record);
    if (corrupt_error == ESP_OK) {
        corrupt_error = nvs_get_blob(handle, "station", record, &size);
    }
    if (corrupt_error == ESP_OK) {
        record[8] ^= 1U;
        corrupt_error = nvs_set_blob(handle, "station", record, size);
    }
    if (corrupt_error == ESP_OK) {
        corrupt_error = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    const esp_err_t corrupt_load = rfbridge::load_saved_wifi_credentials(&loaded);
    const esp_err_t corrupt_load_again = rfbridge::load_saved_wifi_credentials(&loaded);

    handle = 0;
    esp_err_t wrong_type_error = nvs_open("net_cfg", NVS_READWRITE, &handle);
    if (wrong_type_error == ESP_OK) {
        wrong_type_error = nvs_erase_key(handle, "station");
    }
    if (wrong_type_error == ESP_OK) {
        wrong_type_error = nvs_set_u32(handle, "station", 7);
    }
    if (wrong_type_error == ESP_OK) {
        wrong_type_error = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    const esp_err_t wrong_type_load = rfbridge::load_saved_wifi_credentials(&loaded);
    const esp_err_t repair_error = rfbridge::save_wifi_credentials(temporary);
    const esp_err_t repaired_load = rfbridge::load_saved_wifi_credentials(&loaded);
    const esp_err_t forget_error = rfbridge::forget_wifi_credentials();
    const esp_err_t forgotten_load = rfbridge::load_saved_wifi_credentials(&loaded);
    const esp_err_t restore_error = previous_present ? rfbridge::save_wifi_credentials(previous)
                                                     : rfbridge::forget_wifi_credentials();

    TEST_ASSERT_EQUAL(ESP_OK, save_error);
    TEST_ASSERT_EQUAL(ESP_OK, load_error);
    TEST_ASSERT_EQUAL_STRING(temporary.ssid, loaded.ssid);
    TEST_ASSERT_EQUAL(ESP_OK, corrupt_error);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, corrupt_load);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, corrupt_load_again);
    TEST_ASSERT_EQUAL(ESP_OK, wrong_type_error);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, wrong_type_load);
    TEST_ASSERT_EQUAL(ESP_OK, repair_error);
    TEST_ASSERT_EQUAL(ESP_OK, repaired_load);
    TEST_ASSERT_EQUAL(ESP_OK, forget_error);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, forgotten_load);
    TEST_ASSERT_EQUAL(ESP_OK, restore_error);
}
