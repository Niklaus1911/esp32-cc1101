#include <array>
#include <cstdint>
#include <cstring>

#include "../../../components/network_mqtt/private_include/network_mqtt_storage.hpp"
#include "mqtt_config_format.hpp"
#include "nvs.h"
#include "platform_nvs.hpp"
#include "unity.h"

namespace {

constexpr char kNamespace[] = "mqtt_cfg";
constexpr char kServiceKey[] = "service";
constexpr char kAdvertisedKey[] = "advertised";

rfbridge::MqttServiceConfig service_config()
{
    rfbridge::MqttServiceConfig config{};
    config.state = rfbridge::MqttServiceState::kMqtt;
    config.broker_ipv4 = 0xc0a8010aU;
    config.port = 1883;
    config.generation = 11;
    std::strcpy(config.username, "rfbridge");
    std::strcpy(config.password, "test-password");
    return config;
}

void erase_test_records()
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::erase_mqtt_advertised_ledger());
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::erase_mqtt_service_config());
}

}  // namespace

TEST_CASE("MQTT NVS service record survives missing corrupt and wrong-type states",
          "[network_mqtt][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_platform_nvs());
    erase_test_records();

    rfbridge::MqttServiceConfig loaded{};
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, rfbridge::load_mqtt_service_config(&loaded));

    const rfbridge::MqttServiceConfig source = service_config();
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::save_mqtt_service_config(source));
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::load_mqtt_service_config(&loaded));
    TEST_ASSERT_EQUAL(static_cast<int>(source.state), static_cast<int>(loaded.state));
    TEST_ASSERT_EQUAL_HEX32(source.broker_ipv4, loaded.broker_ipv4);
    TEST_ASSERT_EQUAL_UINT16(source.port, loaded.port);
    TEST_ASSERT_EQUAL_UINT32(source.generation, loaded.generation);
    TEST_ASSERT_EQUAL_STRING(source.username, loaded.username);
    TEST_ASSERT_EQUAL_STRING(source.password, loaded.password);

    std::array<uint8_t, rfbridge::kMqttServiceMaxRecordSize> record{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(
        static_cast<int>(rfbridge::MqttConfigFormatResult::kOk),
        static_cast<int>(rfbridge::encode_mqtt_service_record(
            source, record.data(), record.size(), &size)));
    record[20] ^= 1U;
    nvs_handle_t handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(kNamespace, NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(handle, kServiceKey, record.data(), size));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, rfbridge::load_mqtt_service_config(&loaded));

    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::save_mqtt_service_config(source));
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::load_mqtt_service_config(&loaded));
    TEST_ASSERT_EQUAL_UINT32(source.generation, loaded.generation);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(kNamespace, NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_key(handle, kServiceKey));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(handle, kServiceKey, 0x12345678U));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_TYPE_MISMATCH, rfbridge::load_mqtt_service_config(&loaded));
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::save_mqtt_service_config(source));
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::load_mqtt_service_config(&loaded));

    erase_test_records();
}

TEST_CASE("MQTT advertised ledger remains sorted endpoint-bound and independently repairable",
          "[network_mqtt][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_platform_nvs());
    erase_test_records();

    rfbridge::MqttAdvertisedLedger source{};
    source.broker_ipv4 = 0xc0a8010aU;
    source.port = 1883;
    source.count = 2;
    source.rule_count = 1;
    std::strcpy(source.names[0].value, "gate");
    std::strcpy(source.names[1].value, "porch");
    std::strcpy(source.names[2].value, "gate_rule");
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::save_mqtt_advertised_ledger(source));

    rfbridge::MqttAdvertisedLedger loaded{};
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::load_mqtt_advertised_ledger(&loaded));
    TEST_ASSERT_EQUAL_HEX32(source.broker_ipv4, loaded.broker_ipv4);
    TEST_ASSERT_EQUAL_UINT16(source.port, loaded.port);
    TEST_ASSERT_EQUAL_UINT8(source.count, loaded.count);
    TEST_ASSERT_EQUAL_UINT8(source.rule_count, loaded.rule_count);
    TEST_ASSERT_EQUAL_UINT8(rfbridge::kMqttAdvertisedFormatVersion, loaded.format_version);
    TEST_ASSERT_EQUAL_STRING(source.names[0].value, loaded.names[0].value);
    TEST_ASSERT_EQUAL_STRING(source.names[1].value, loaded.names[1].value);
    TEST_ASSERT_EQUAL_STRING(source.names[2].value, loaded.names[2].value);

    nvs_handle_t handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(kNamespace, NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_key(handle, kAdvertisedKey));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(handle, kAdvertisedKey, 0xabcdef01U));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_TYPE_MISMATCH,
                      rfbridge::load_mqtt_advertised_ledger(&loaded));
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::save_mqtt_advertised_ledger(source));
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::load_mqtt_advertised_ledger(&loaded));

    rfbridge::MqttAdvertisedLedger other_endpoint = source;
    other_endpoint.broker_ipv4 = 0xc0a8010bU;
    TEST_ASSERT_TRUE(rfbridge::mqtt_advertised_ledger_is_valid(other_endpoint));
    TEST_ASSERT_TRUE(rfbridge::mqtt_advertised_ledger_matches_endpoint(
        source, source.broker_ipv4, source.port));
    TEST_ASSERT_FALSE(rfbridge::mqtt_advertised_ledger_matches_endpoint(
        source, other_endpoint.broker_ipv4, other_endpoint.port));

    erase_test_records();
}
