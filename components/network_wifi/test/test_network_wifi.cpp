#include <cstddef>
#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../private_include/network_wifi_storage_private.hpp"
#include "network_wifi_config.hpp"
#include "network_wifi.hpp"
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

TEST_CASE("network hostname storage retains corruption and repairs only its key",
          "[network_wifi][hostname][storage][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_platform_nvs());

    char previous[rfbridge::kNetworkHostnameCapacity]{};
    const esp_err_t previous_error =
        rfbridge::load_saved_network_hostname(previous, sizeof(previous));
    TEST_ASSERT_TRUE(previous_error == ESP_OK || previous_error == ESP_ERR_NOT_FOUND);
    const bool previous_present = previous_error == ESP_OK;

    constexpr char temporary[] = "unity-mdns-host";
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::save_network_hostname(temporary));
    char loaded[rfbridge::kNetworkHostnameCapacity]{};
    TEST_ASSERT_EQUAL(ESP_OK,
                      rfbridge::load_saved_network_hostname(loaded, sizeof(loaded)));
    TEST_ASSERT_EQUAL_STRING(temporary, loaded);

    nvs_handle_t handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("net_cfg", NVS_READWRITE, &handle));
    uint8_t record[rfbridge::kNetworkHostnameRecordMaxSize]{};
    std::size_t size = sizeof(record);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(handle, "hostname", record, &size));
    record[8] ^= 1U;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(handle, "hostname", record, size));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      rfbridge::load_saved_network_hostname(loaded, sizeof(loaded)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      rfbridge::load_saved_network_hostname(loaded, sizeof(loaded)));

    handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("net_cfg", NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_key(handle, "hostname"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(handle, "hostname", 7));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      rfbridge::load_saved_network_hostname(loaded, sizeof(loaded)));

    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::save_network_hostname(temporary));
    TEST_ASSERT_EQUAL(ESP_OK,
                      rfbridge::load_saved_network_hostname(loaded, sizeof(loaded)));
    TEST_ASSERT_EQUAL_STRING(temporary, loaded);
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::forget_network_hostname());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      rfbridge::load_saved_network_hostname(loaded, sizeof(loaded)));

    const esp_err_t restore_error = previous_present
                                        ? rfbridge::save_network_hostname(previous)
                                        : rfbridge::forget_network_hostname();
    TEST_ASSERT_EQUAL(ESP_OK, restore_error);
}

TEST_CASE("network hostname updates persist before STA netif application",
          "[network_wifi][hostname][runtime][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_network_wifi());

    rfbridge::NetworkHostnameStatus original{};
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::get_network_hostname_status(&original));
    const char *temporary = std::strcmp(original.configured_hostname, "unity-mdns-a") == 0
                                ? "unity-mdns-b"
                                : "unity-mdns-a";

    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::set_network_hostname(temporary));
    char persisted[rfbridge::kNetworkHostnameCapacity]{};
    TEST_ASSERT_EQUAL(ESP_OK,
                      rfbridge::load_saved_network_hostname(persisted, sizeof(persisted)));
    TEST_ASSERT_EQUAL_STRING(temporary, persisted);

    rfbridge::NetworkHostnameStatus changed{};
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::get_network_hostname_status(&changed));
    TEST_ASSERT_TRUE(changed.custom);
    TEST_ASSERT_EQUAL_STRING(temporary, changed.configured_hostname);
    TEST_ASSERT_NOT_EQUAL(original.configured_generation, changed.configured_generation);

    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::scan_network_wifi());
    bool applied = false;
    for (std::size_t attempt = 0; attempt < 500; ++attempt) {
        TEST_ASSERT_EQUAL(ESP_OK, rfbridge::get_network_hostname_status(&changed));
        if (changed.netif_applied_generation == changed.configured_generation &&
            changed.last_apply_error == ESP_OK) {
            applied = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    TEST_ASSERT_TRUE(applied);

    const esp_err_t restore_error = original.custom
                                        ? rfbridge::set_network_hostname(
                                              original.configured_hostname)
                                        : rfbridge::reset_network_hostname();
    TEST_ASSERT_EQUAL(ESP_OK, restore_error);

    rfbridge::NetworkHostnameStatus restored{};
    bool restore_applied = false;
    for (std::size_t attempt = 0; attempt < 500; ++attempt) {
        TEST_ASSERT_EQUAL(ESP_OK, rfbridge::get_network_hostname_status(&restored));
        if (restored.netif_applied_generation == restored.configured_generation &&
            restored.last_apply_error == ESP_OK) {
            restore_applied = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    TEST_ASSERT_TRUE(restore_applied);
    TEST_ASSERT_EQUAL(original.custom, restored.custom);
    TEST_ASSERT_EQUAL_STRING(original.configured_hostname, restored.configured_hostname);
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::stop_network_wifi());
}
