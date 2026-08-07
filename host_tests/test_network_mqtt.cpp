#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "mqtt_config_format.hpp"
#include "mqtt_discovery.hpp"
#include "network_mqtt_policy.hpp"
#include "mqtt_telemetry.hpp"

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

uint32_t crc32(const uint8_t *data, std::size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

void rewrite_crc(uint8_t *record, std::size_t size)
{
    const uint32_t value = crc32(record, size - 4U);
    for (std::size_t index = 0; index < 4; ++index) {
        record[size - 4U + index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

rfbridge::MqttServiceConfig service_config()
{
    rfbridge::MqttServiceConfig config{};
    config.requested_services = rfbridge::NetworkServiceMask::kMqtt;
    config.broker_ipv4 = 0xc0a8010aU;
    config.port = 1883;
    config.generation = 7;
    std::strcpy(config.username, "rfbridge");
    std::strcpy(config.password, "secret-password");
    return config;
}

rfbridge::MqttAdvertisedLedger advertised_ledger()
{
    rfbridge::MqttAdvertisedLedger ledger{};
    ledger.broker_ipv4 = 0xc0a8010aU;
    ledger.port = 1883;
    ledger.count = 2;
    std::strcpy(ledger.names[0].value, "gate");
    std::strcpy(ledger.names[1].value, "porch");
    return ledger;
}

void test_ipv4_contract()
{
    uint32_t address = 0;
    require(rfbridge::parse_mqtt_broker_ipv4("192.168.1.10", &address) &&
                address == 0xc0a8010aU,
            "numeric broker IPv4 parses in network display order");
    char formatted[16]{};
    require(rfbridge::format_mqtt_broker_ipv4(address, formatted, sizeof(formatted)) &&
                std::strcmp(formatted, "192.168.1.10") == 0,
            "broker IPv4 formatting preserves octet order");
    require(!rfbridge::parse_mqtt_broker_ipv4("192.168.1", &address) &&
                !rfbridge::parse_mqtt_broker_ipv4("192.168.1.256", &address) &&
                !rfbridge::parse_mqtt_broker_ipv4("192.168.01.10x", &address) &&
                !rfbridge::parse_mqtt_broker_ipv4("0.1.2.3", &address) &&
                !rfbridge::parse_mqtt_broker_ipv4("127.0.0.1", &address) &&
                !rfbridge::parse_mqtt_broker_ipv4("224.0.0.1", &address) &&
                !rfbridge::parse_mqtt_broker_ipv4("255.255.255.255", &address),
            "broker parser rejects malformed and non-unicast addresses");
    char short_buffer[15]{};
    require(!rfbridge::format_mqtt_broker_ipv4(0xdfffffffU, short_buffer,
                                                sizeof(short_buffer)) &&
                !rfbridge::format_mqtt_broker_ipv4(0, formatted, sizeof(formatted)),
            "broker formatter rejects invalid addresses and short buffers");
}

void test_network_availability_policy()
{
    using rfbridge::MqttNetworkAvailability;
    using rfbridge::MqttNetworkEventKind;
    require(rfbridge::mqtt_network_availability(MqttNetworkEventKind::kConnected, true) ==
                MqttNetworkAvailability::kOnline,
            "connected online event marks MQTT network ready");
    require(rfbridge::mqtt_network_availability(MqttNetworkEventKind::kStateChanged, false) ==
                MqttNetworkAvailability::kOffline,
            "non-online state change marks MQTT network offline");
    require(rfbridge::mqtt_network_availability(MqttNetworkEventKind::kDisconnected, true) ==
                MqttNetworkAvailability::kOffline,
            "disconnect event is offline even before Wi-Fi state settles");
    require(rfbridge::mqtt_network_availability(MqttNetworkEventKind::kOther, true) ==
                MqttNetworkAvailability::kUnchanged,
            "scan-only events do not change MQTT network readiness");
}

void test_service_mode_policy()
{
    using rfbridge::NetworkServiceMask;
    require(rfbridge::network_service_request_is_supported(NetworkServiceMask::kWeb, false) &&
                rfbridge::network_service_request_is_supported(NetworkServiceMask::kMqtt,
                                                                false) &&
                !rfbridge::network_service_request_is_supported(NetworkServiceMask::kBoth,
                                                                 false) &&
                rfbridge::network_service_request_is_supported(NetworkServiceMask::kBoth, true),
            "classic rejects Both while S3-capable profiles accept it");
    require(rfbridge::network_service_boot_mask(NetworkServiceMask::kBoth, false) ==
                NetworkServiceMask::kWeb &&
                rfbridge::network_service_boot_mask(NetworkServiceMask::kBoth, true) ==
                    NetworkServiceMask::kBoth,
            "transplanted Both records fall back without changing S3 requests");
    require(rfbridge::network_service_recovery_mask(NetworkServiceMask::kMqtt, true) ==
                NetworkServiceMask::kBoth &&
                rfbridge::network_service_recovery_mask(NetworkServiceMask::kBoth, true) ==
                    NetworkServiceMask::kBoth,
            "MQTT startup failure adds Web recovery without removing an existing Web request");
    for (uint8_t failures = 0; failures < 4; ++failures) {
        const bool web_started = (failures & 1U) == 0;
        const bool mqtt_started = (failures & 2U) == 0;
        const NetworkServiceMask effective = rfbridge::network_service_effective_mask(
            NetworkServiceMask::kBoth, web_started, mqtt_started);
        const uint8_t expected = static_cast<uint8_t>(web_started ? NetworkServiceMask::kWeb
                                                                  : NetworkServiceMask::kNone) |
                                 static_cast<uint8_t>(mqtt_started ? NetworkServiceMask::kMqtt
                                                                   : NetworkServiceMask::kNone);
        require(static_cast<uint8_t>(effective) == expected,
                "Both startup preserves each independently successful frontend");
    }
    require(!rfbridge::mqtt_retirement_requires_reboot(NetworkServiceMask::kWeb) &&
                rfbridge::mqtt_retirement_requires_reboot(NetworkServiceMask::kNone),
            "Both retirement leaves Web live while MQTT-only retirement requires reboot");
}

void test_service_record()
{
    rfbridge::MqttServiceConfig source = service_config();
    require(rfbridge::mqtt_service_has_credentials(source) &&
                rfbridge::mqtt_service_config_is_valid(source),
            "configured MQTT service is valid");
    std::array<uint8_t, rfbridge::kMqttServiceMaxRecordSize> record{};
    std::size_t size = 0;
    require(rfbridge::encode_mqtt_service_record(source, record.data(), record.size(), &size) ==
                rfbridge::MqttConfigFormatResult::kOk &&
                size == 47,
            "service record encodes to its exact length");
    rfbridge::MqttServiceConfig decoded{};
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kOk &&
                decoded.requested_services == source.requested_services &&
                decoded.retirement_state == source.retirement_state &&
                decoded.broker_ipv4 == source.broker_ipv4 &&
                decoded.port == source.port && decoded.generation == source.generation &&
                std::strcmp(decoded.username, source.username) == 0 &&
                std::strcmp(decoded.password, source.password) == 0,
            "service record round trips all persisted fields");

    record[20] ^= 1U;
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidCrc,
            "service record rejects CRC corruption");
    record[20] ^= 1U;
    record[4] = 3;
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidVersion,
            "service record rejects unknown versions");
    record[4] = 2;
    record[5] = 99;
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "service record rejects unknown state values");
    record[5] = static_cast<uint8_t>(rfbridge::NetworkServiceMask::kMqtt);
    rewrite_crc(record.data(), size);
    record[0] = 'X';
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "service decoder distinguishes the record type");
    record[0] = 'M';
    require(rfbridge::decode_mqtt_service_record(record.data(), size - 1U, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "service decoder rejects truncated records");
    require(rfbridge::encode_mqtt_service_record(source, record.data(), size - 1U, &size) ==
                rfbridge::MqttConfigFormatResult::kBufferTooSmall,
            "service encoder rejects short output buffers");

    source.requested_services = rfbridge::NetworkServiceMask::kWeb;
    source.broker_ipv4 = 0;
    source.port = 0;
    source.username[0] = '\0';
    source.password[0] = '\0';
    require(rfbridge::mqtt_service_config_is_valid(source),
            "Web profile permits an unconfigured service record");
    source.requested_services = rfbridge::NetworkServiceMask::kMqtt;
    require(!rfbridge::mqtt_service_config_is_valid(source),
            "MQTT profile requires credentials");
    source.retirement_state = rfbridge::MqttRetirementState::kRetiring;
    require(!rfbridge::mqtt_service_config_is_valid(source),
            "retiring state requires an MQTT request and credentials");
    source = service_config();
    source.retirement_state = rfbridge::MqttRetirementState::kRetiring;
    require(rfbridge::mqtt_service_config_is_valid(source),
            "MQTT retirement remains resumable with its endpoint credentials");
    source.requested_services = rfbridge::NetworkServiceMask::kWeb;
    require(!rfbridge::mqtt_service_config_is_valid(source),
            "Web and retiring is not a valid persisted transition");
    source.retirement_state = rfbridge::MqttRetirementState::kRetired;
    require(rfbridge::mqtt_service_config_is_valid(source),
            "Web and retired is the durable cleanup checkpoint");
    source.requested_services = rfbridge::NetworkServiceMask::kMqtt;
    require(!rfbridge::mqtt_service_config_is_valid(source),
            "retired state cannot request MQTT startup");
    source = service_config();
    source.password[3] = '\n';
    require(!rfbridge::mqtt_service_has_credentials(source),
            "service credentials reject non-printable bytes");

    const rfbridge::MqttServiceConfig v2_source = service_config();
    require(rfbridge::encode_mqtt_service_record(v2_source, record.data(), record.size(), &size) ==
                rfbridge::MqttConfigFormatResult::kOk && record[4] == 2 &&
                record[5] == static_cast<uint8_t>(rfbridge::NetworkServiceMask::kMqtt) &&
                record[14] == static_cast<uint8_t>(rfbridge::MqttRetirementState::kActive),
            "service mutations encode the v2 mask and retirement fields");
    const uint8_t legacy_states[] = {0, 1, 2, 3};
    const rfbridge::NetworkServiceMask expected_masks[] = {
        rfbridge::NetworkServiceMask::kWeb, rfbridge::NetworkServiceMask::kMqtt,
        rfbridge::NetworkServiceMask::kMqtt, rfbridge::NetworkServiceMask::kWeb};
    const rfbridge::MqttRetirementState expected_retirement[] = {
        rfbridge::MqttRetirementState::kActive, rfbridge::MqttRetirementState::kActive,
        rfbridge::MqttRetirementState::kRetiring, rfbridge::MqttRetirementState::kRetired};
    for (std::size_t index = 0; index < std::size(legacy_states); ++index) {
        record[4] = 1;
        record[5] = legacy_states[index];
        record[14] = 0;
        rewrite_crc(record.data(), size);
        require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                    rfbridge::MqttConfigFormatResult::kOk &&
                    decoded.requested_services == expected_masks[index] &&
                    decoded.retirement_state == expected_retirement[index],
                "legacy v1 service state maps to v2 semantics");
    }
    record[4] = 2;
    record[5] = static_cast<uint8_t>(rfbridge::NetworkServiceMask::kBoth);
    record[14] = static_cast<uint8_t>(rfbridge::MqttRetirementState::kRetiring);
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kOk &&
                decoded.requested_services == rfbridge::NetworkServiceMask::kBoth &&
                decoded.retirement_state == rfbridge::MqttRetirementState::kRetiring,
            "v2 both/retiring service record decodes");
    record[15] = 1;
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "v2 service record rejects nonzero reserved bytes");
}

void test_advertised_record()
{
    rfbridge::MqttAdvertisedLedger source = advertised_ledger();
    source.rule_count = 1;
    std::strcpy(source.names[2].value, "rule_gate");
    require(rfbridge::mqtt_advertised_ledger_is_valid(source),
            "sorted unique advertised ledger is valid");
    std::array<uint8_t, rfbridge::kMqttAdvertisedMaxRecordSize> record{};
    std::size_t size = 0;
    require(rfbridge::encode_mqtt_advertised_record(source, record.data(), record.size(), &size) ==
                rfbridge::MqttConfigFormatResult::kOk &&
                size == 68,
            "advertised ledger encodes to its exact length");
    rfbridge::MqttAdvertisedLedger decoded{};
    require(rfbridge::decode_mqtt_advertised_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kOk &&
                decoded.broker_ipv4 == source.broker_ipv4 && decoded.port == source.port &&
                decoded.count == source.count &&
                std::strcmp(decoded.names[0].value, "gate") == 0 &&
                std::strcmp(decoded.names[1].value, "porch") == 0,
            "advertised ledger round trips endpoint and names");
    std::array<uint8_t, 64> legacy{};
    legacy[0] = 'M';
    legacy[1] = 'Q';
    legacy[2] = 'A';
    legacy[3] = 'D';
    legacy[4] = 1;
    legacy[5] = 2;
    legacy[6] = static_cast<uint8_t>(source.port);
    legacy[7] = static_cast<uint8_t>(source.port >> 8U);
    for (std::size_t index = 0; index < 4; ++index) {
        legacy[8 + index] = static_cast<uint8_t>(source.broker_ipv4 >> (index * 8U));
    }
    std::memcpy(legacy.data() + 12, source.names[0].value, rfbridge::kRfStorageNameCapacity);
    std::memcpy(legacy.data() + 12 + rfbridge::kRfStorageNameCapacity,
                source.names[1].value, rfbridge::kRfStorageNameCapacity);
    const std::size_t legacy_size = 12 + 2 * rfbridge::kRfStorageNameCapacity + 4;
    rewrite_crc(legacy.data(), legacy_size);
    require(rfbridge::decode_mqtt_advertised_record(legacy.data(), legacy_size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kOk && decoded.format_version == 1 &&
                decoded.rule_count == 0 &&
                decoded.count == source.count &&
                std::strcmp(decoded.names[1].value, "porch") == 0,
            "legacy v1 advertised ledger decodes with an empty rule range");
    record[16] ^= 1U;
    require(rfbridge::decode_mqtt_advertised_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidCrc,
            "advertised ledger rejects CRC corruption");
    record[16] ^= 1U;
    record[4] = 3;
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_advertised_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidVersion,
            "advertised ledger rejects unknown versions");
    record[4] = 2;
    std::memcpy(record.data() + 16U + rfbridge::kRfStorageNameCapacity,
                record.data() + 16U, rfbridge::kRfStorageNameCapacity);
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_advertised_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "advertised ledger rejects duplicate names");
    source = advertised_ledger();
    source.names[0].value[5] = 'x';
    require(!rfbridge::mqtt_advertised_ledger_is_valid(source),
            "advertised ledger rejects non-canonical bytes after a name terminator");
    source.names[0] = source.names[1];
    require(!rfbridge::mqtt_advertised_ledger_is_valid(source),
            "advertised ledger validator rejects duplicate names");
    source = advertised_ledger();
    std::swap(source.names[0], source.names[1]);
    require(!rfbridge::mqtt_advertised_ledger_is_valid(source),
            "advertised ledger validator rejects unsorted names");

    const rfbridge::MqttAdvertisedLedger left = advertised_ledger();
    rfbridge::MqttAdvertisedLedger right{};
    right.broker_ipv4 = left.broker_ipv4;
    right.port = left.port;
    right.count = 2;
    std::strcpy(right.names[0].value, "garage");
    std::strcpy(right.names[1].value, "gate");
    rfbridge::MqttAdvertisedLedger merged{};
    require(rfbridge::merge_mqtt_advertised_ledgers(left, right, &merged) ==
                rfbridge::MqttConfigFormatResult::kOk &&
                merged.count == 3 && std::strcmp(merged.names[0].value, "garage") == 0 &&
                std::strcmp(merged.names[1].value, "gate") == 0 &&
                std::strcmp(merged.names[2].value, "porch") == 0,
            "precommit merge produces a sorted unique union");
    rfbridge::MqttAdvertisedLedger aliased = left;
    require(rfbridge::merge_mqtt_advertised_ledgers(aliased, right, &aliased) ==
                rfbridge::MqttConfigFormatResult::kInvalidArgument,
            "precommit merge rejects an aliased destination");
    right.broker_ipv4++;
    require(rfbridge::merge_mqtt_advertised_ledgers(left, right, &merged) ==
                rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "precommit merge rejects endpoint changes");

    rfbridge::MqttAdvertisedLedger empty_left{};
    rfbridge::MqttAdvertisedLedger empty_right{};
    empty_left.broker_ipv4 = left.broker_ipv4;
    empty_left.port = left.port;
    empty_right.broker_ipv4 = left.broker_ipv4 + 1U;
    empty_right.port = left.port;
    require(!rfbridge::mqtt_advertised_ledger_matches_endpoint(
                empty_left, empty_right.broker_ipv4, empty_right.port) &&
                rfbridge::merge_mqtt_advertised_ledgers(empty_left, empty_right, &merged) ==
                    rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "empty fixed-entity ledgers remain endpoint-bound");

    rfbridge::MqttAdvertisedLedger even{};
    rfbridge::MqttAdvertisedLedger odd{};
    even.broker_ipv4 = left.broker_ipv4;
    even.port = left.port;
    odd.broker_ipv4 = left.broker_ipv4;
    odd.port = left.port;
    even.count = rfbridge::kMqttMaximumAdvertisedSignals;
    odd.count = rfbridge::kMqttMaximumAdvertisedSignals;
    for (std::size_t index = 0; index < rfbridge::kMqttMaximumAdvertisedSignals; ++index) {
        std::snprintf(even.names[index].value, sizeof(even.names[index].value), "A%03zu", index * 2U);
        std::snprintf(odd.names[index].value, sizeof(odd.names[index].value), "A%03zu",
                      index * 2U + 1U);
    }
    require(rfbridge::merge_mqtt_advertised_ledgers(even, odd, &merged) ==
                rfbridge::MqttConfigFormatResult::kBufferTooSmall,
            "precommit merge reports a full-catalog replacement overflow");
}

rfbridge::MqttDeviceIdentity test_identity()
{
    const uint8_t mac[6] = {0x10, 0x20, 0x30, 0xa1, 0xb2, 0xc3};
    rfbridge::MqttDeviceIdentity identity{};
    require(rfbridge::derive_mqtt_device_identity(mac, &identity),
            "device identity derives from the full STA MAC");
    require(std::strcmp(identity.mac_hex, "102030a1b2c3") == 0 &&
                std::strcmp(identity.client_id, "rfbridge-102030a1b2c3") == 0 &&
                std::strcmp(identity.device_id, "rfbridge_102030a1b2c3") == 0 &&
                std::strcmp(identity.device_name, "RF Bridge A1B2C3") == 0,
            "device identity fields are stable and exact");
    return identity;
}

const rfbridge::BoardInfo &test_board(rfbridge::BoardProfile profile)
{
    const rfbridge::BoardInfo *board = rfbridge::board_info(profile);
    require(board != nullptr, "test board profile resolves");
    return *board;
}

void test_discovery_contract()
{
    const rfbridge::MqttDeviceIdentity identity = test_identity();
    const rfbridge::BoardInfo &board = test_board(rfbridge::BoardProfile::kEsp32Devkit);
    char text[rfbridge::kMqttDiscoveryPayloadCapacity]{};
    require(rfbridge::format_mqtt_command_filter(identity, text, sizeof(text)) &&
                std::strcmp(text, "rfbridge/102030a1b2c3/signal/+/press") == 0,
            "command subscription filter is exact");
    require(rfbridge::format_mqtt_availability_topic(identity, text, sizeof(text)) &&
                std::strcmp(text, "rfbridge/102030a1b2c3/availability") == 0,
            "availability topic is exact");
    require(rfbridge::format_mqtt_discovery_topic(identity, "gate", text, sizeof(text)) &&
                std::strcmp(text,
                            "homeassistant/button/rfbridge_102030a1b2c3/gate/config") == 0,
            "Home Assistant discovery topic is exact");
    require(rfbridge::format_mqtt_command_topic(identity, "gate", text, sizeof(text)) &&
                std::strcmp(text, "rfbridge/102030a1b2c3/signal/gate/press") == 0,
            "button command topic is exact");
    require(rfbridge::format_mqtt_discovery_payload(identity, "gate", board, "1.2.3", text,
                                                     sizeof(text)),
            "bounded discovery JSON formats");
    constexpr char expected[] =
        "{\"name\":\"gate\",\"unique_id\":\"rfbridge_102030a1b2c3_gate\","
        "\"command_topic\":\"rfbridge/102030a1b2c3/signal/gate/press\","
        "\"payload_press\":\"PRESS\",\"qos\":0,\"retain\":false,"
        "\"availability_topic\":\"rfbridge/102030a1b2c3/availability\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"device\":{\"identifiers\":[\"rfbridge_102030a1b2c3\"],"
        "\"name\":\"RF Bridge A1B2C3\",\"manufacturer\":\"RF Bridge\","
        "\"model\":\"ESP32 DevKit + CC1101\",\"hw_version\":\"esp32-devkit\","
        "\"sw_version\":\"1.2.3\"}}";
    require(std::strcmp(text, expected) == 0, "discovery JSON contract is exact");
    require(rfbridge::format_mqtt_discovery_payload(identity, "gate", board, "a\"b\\c", text,
                                                     sizeof(text)) &&
                std::strstr(text, "\"sw_version\":\"a\\\"b\\\\c\"") != nullptr,
            "discovery JSON escapes firmware metadata");
    char short_payload[128]{};
    require(!rfbridge::format_mqtt_discovery_payload(identity, "gate", board, "1.2.3",
                                                      short_payload, sizeof(short_payload)) &&
                !rfbridge::format_mqtt_discovery_topic(identity, "list", text, sizeof(text)),
            "discovery formatting rejects truncation and invalid signal names");

    const struct {
        rfbridge::BoardProfile profile;
        const char *model;
        const char *hardware;
    } boards[] = {
        {rfbridge::BoardProfile::kEsp32Devkit, "ESP32 DevKit + CC1101", "esp32-devkit"},
        {rfbridge::BoardProfile::kEsp32s3DevkitcN16r8,
         "ESP32-S3 DevKitC N16R8 + CC1101", "esp32s3-devkitc-n16r8"},
        {rfbridge::BoardProfile::kXiaoEsp32s3,
         "Seeed Studio XIAO ESP32-S3 + CC1101", "xiao-esp32s3"},
        {rfbridge::BoardProfile::kEsp32s3SuperminiFh4r2,
         "ESP32-S3 SuperMini FH4R2 + CC1101", "esp32s3-supermini-fh4r2"},
    };
    for (const auto &expected_board : boards) {
        require(rfbridge::format_mqtt_discovery_payload(
                    identity, "gate", test_board(expected_board.profile), "1.2.3", text,
                    sizeof(text)) && std::strstr(text, expected_board.model) != nullptr &&
                    std::strstr(text, expected_board.hardware) != nullptr,
                "discovery metadata matches each board profile");
    }
}

void test_incoming_messages()
{
    const rfbridge::MqttDeviceIdentity identity = test_identity();
    constexpr char topic[] = "rfbridge/102030a1b2c3/signal/gate/press";
    constexpr char payload[] = "PRESS";
    rfbridge::MqttIncomingMessage message{};
    message.topic = topic;
    message.topic_length = sizeof(topic) - 1U;
    message.data = payload;
    message.data_length = sizeof(payload) - 1U;
    message.total_data_length = message.data_length;
    char name[rfbridge::kRfStorageNameCapacity]{};
    require(rfbridge::parse_mqtt_button_command(identity, message, name) &&
                std::strcmp(name, "gate") == 0,
            "exact QoS-zero PRESS command is accepted");
    message.qos = 1;
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "QoS-one commands are rejected to prevent redelivery");
    message.qos = 0;
    message.retain = true;
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "retained commands are rejected");
    message.retain = false;
    message.duplicate = true;
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "duplicate commands are rejected");
    message.duplicate = false;
    message.current_data_offset = 1;
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "fragmented commands are rejected");
    message.current_data_offset = 0;
    message.total_data_length = message.data_length + 1U;
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "partial command payloads are rejected");
    message.total_data_length = message.data_length;
    message.data = "press";
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "command payload is case-sensitive");
    message.data = payload;
    message.topic = "rfbridge/102030a1b2c3/signal/list/press";
    message.topic_length = std::strlen(message.topic);
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "reserved signal names are rejected");
    message.topic = "rfbridge/ffffffffffff/signal/gate/press";
    message.topic_length = std::strlen(message.topic);
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "commands for another device are rejected");
    constexpr char embedded_nul_topic[] =
        "rfbridge/102030a1b2c3/signal/g\0ate/press";
    message.topic = embedded_nul_topic;
    message.topic_length = sizeof(embedded_nul_topic) - 1U;
    require(!rfbridge::parse_mqtt_button_command(identity, message, name),
            "commands with embedded topic NUL bytes are rejected");

    constexpr char birth_topic[] = "homeassistant/status";
    constexpr char birth_payload[] = "online";
    message = {};
    message.topic = birth_topic;
    message.topic_length = sizeof(birth_topic) - 1U;
    message.data = birth_payload;
    message.data_length = sizeof(birth_payload) - 1U;
    message.total_data_length = message.data_length;
    message.qos = 1;
    message.retain = true;
    message.duplicate = true;
    require(rfbridge::mqtt_message_is_home_assistant_birth(message),
            "idempotent retained duplicate Home Assistant birth is accepted");
    message.qos = 2;
    require(!rfbridge::mqtt_message_is_home_assistant_birth(message),
            "unsupported birth QoS is rejected");
    message.qos = 0;
    message.total_data_length++;
    require(!rfbridge::mqtt_message_is_home_assistant_birth(message),
            "fragmented Home Assistant birth is rejected");
}

void test_automation_discovery_and_telemetry()
{
    const rfbridge::MqttDeviceIdentity identity = test_identity();
    const rfbridge::BoardInfo &board =
        test_board(rfbridge::BoardProfile::kEsp32s3DevkitcN16r8);
    char text[rfbridge::kMqttDiscoveryPayloadCapacity]{};
    require(rfbridge::format_mqtt_event_topic(identity, rfbridge::MqttEventTopicKind::kRx, text,
                                              sizeof(text)) &&
                std::strcmp(text, "rfbridge/102030a1b2c3/event/rx") == 0,
            "RX event topic is stable");
    require(rfbridge::format_mqtt_state_topic(identity, rfbridge::MqttStateTopicKind::kRule,
                                              "gate", text, sizeof(text)) &&
                std::strcmp(text, "rfbridge/102030a1b2c3/state/rule/gate") == 0,
            "rule state topic is stable");
    require(rfbridge::format_mqtt_automation_command_topic(
                identity, rfbridge::MqttAutomationCommandKind::kEnabled, text, sizeof(text)) &&
                std::strcmp(text, "rfbridge/102030a1b2c3/automation/enabled/set") == 0,
            "automation enable topic is stable");
    require(rfbridge::format_mqtt_entity_discovery_topic(
                identity, rfbridge::MqttDiscoveryEntityKind::kRuleSensor, "gate", text,
                sizeof(text)) &&
                std::strcmp(text, "homeassistant/sensor/rfbridge_102030a1b2c3/rule_gate/config") == 0,
            "rule discovery topic is stable");
    require(rfbridge::format_mqtt_entity_discovery_payload(
                identity, rfbridge::MqttDiscoveryEntityKind::kAutomationSwitch, nullptr, board,
                "1.2.3",
                text, sizeof(text)) &&
                std::strstr(text, "\"command_topic\":\"rfbridge/102030a1b2c3/automation/enabled/set\"") !=
                    nullptr &&
                std::strstr(text, "\"value_template\":\"{{ value_json.enabled }}\"") != nullptr,
            "automation switch discovery contains state and command contracts");
    require(rfbridge::format_mqtt_entity_discovery_payload(
                identity, rfbridge::MqttDiscoveryEntityKind::kRuleSensor, "gate", board, "1.2.3",
                text, sizeof(text)) &&
                std::strstr(text, "\"json_attributes_topic\":\"rfbridge/102030a1b2c3/state/rule/gate\"") !=
                    nullptr,
            "rule discovery exposes retained attributes");

    constexpr char enabled_topic[] = "rfbridge/102030a1b2c3/automation/enabled/set";
    constexpr char enabled_payload[] = "ON";
    rfbridge::MqttIncomingMessage message{};
    message.topic = enabled_topic;
    message.topic_length = sizeof(enabled_topic) - 1U;
    message.data = enabled_payload;
    message.data_length = sizeof(enabled_payload) - 1U;
    message.total_data_length = message.data_length;
    rfbridge::MqttAutomationCommandKind command{};
    bool enabled = false;
    uint8_t log_mode = 0;
    require(rfbridge::parse_mqtt_automation_command(identity, message, &command, &enabled,
                                                    &log_mode) &&
                command == rfbridge::MqttAutomationCommandKind::kEnabled && enabled,
            "exact ON automation command is accepted");
    message.data = "verbose";
    message.data_length = 7;
    message.total_data_length = 7;
    message.topic = "rfbridge/102030a1b2c3/automation/log_mode/set";
    message.topic_length = std::strlen(message.topic);
    require(rfbridge::parse_mqtt_automation_command(identity, message, &command, &enabled,
                                                    &log_mode) &&
                command == rfbridge::MqttAutomationCommandKind::kLogMode && log_mode == 2,
            "exact verbose log-mode command is accepted");
    message.retain = true;
    require(!rfbridge::parse_mqtt_automation_command(identity, message, &command, &enabled,
                                                     &log_mode),
            "retained automation commands are rejected");

    rfbridge::MqttRxTelemetry rx{};
    rx.sequence = 42;
    rx.encoding = rfbridge::MqttTelemetryEncoding::kDecoded;
    rx.match = rfbridge::MqttTelemetryMatch::kUnique;
    rx.fingerprint = 0x1234;
    rx.observed_repeats = 3;
    std::strcpy(rx.learned_name, "gate");
    rx.code = 123;
    rx.bits = 24;
    rx.protocol = 1;
    rx.pulse_us = 350;
    require(rfbridge::format_mqtt_rx_event_payload(rx, text, sizeof(text)) &&
                std::strstr(text, "\"event_type\":\"received\"") != nullptr &&
                std::strstr(text, "\"name\":\"gate\"") != nullptr,
            "decoded RX telemetry is bounded JSON");
    rfbridge::MqttAutomationStateTelemetry state{};
    state.enabled = true;
    state.enabled_known = true;
    state.log_mode_known = true;
    state.log_mode = 1;
    state.rules = 2;
    require(rfbridge::format_mqtt_automation_state_payload(state, text, sizeof(text)) &&
                std::strstr(text, "\"enabled\":\"ON\"") != nullptr &&
                std::strstr(text, "\"rules\":2") != nullptr,
            "retained automation state contains persistent controls");
}

void test_system_discovery_and_telemetry()
{
    const rfbridge::MqttDeviceIdentity identity = test_identity();
    const rfbridge::BoardInfo &board =
        test_board(rfbridge::BoardProfile::kEsp32s3DevkitcN16r8);
    char text[rfbridge::kMqttDiscoveryPayloadCapacity]{};
    require(rfbridge::format_mqtt_state_topic(identity, rfbridge::MqttStateTopicKind::kSystem,
                                              nullptr, text, sizeof(text)) &&
                std::strcmp(text, "rfbridge/102030a1b2c3/state/system") == 0,
            "system state topic is stable");

    const struct {
        rfbridge::MqttDiscoveryEntityKind kind;
        const char *object;
        const char *path;
    } sensors[] = {
        {rfbridge::MqttDiscoveryEntityKind::kInternalFreeSensor, "internal_free",
         "memory.internal.free"},
        {rfbridge::MqttDiscoveryEntityKind::kInternalMinimumSensor, "internal_minimum",
         "memory.internal.minimum"},
        {rfbridge::MqttDiscoveryEntityKind::kInternalLargestSensor, "internal_largest",
         "memory.internal.largest"},
        {rfbridge::MqttDiscoveryEntityKind::kPsramFreeSensor, "psram_free",
         "memory.psram.free"},
    };
    for (const auto &sensor : sensors) {
        char expected_topic[128]{};
        std::snprintf(expected_topic, sizeof(expected_topic),
                      "homeassistant/sensor/rfbridge_102030a1b2c3/%s/config", sensor.object);
        require(rfbridge::format_mqtt_entity_discovery_topic(
                    identity, sensor.kind, nullptr, text, sizeof(text)) &&
                    std::strcmp(text, expected_topic) == 0,
                "system sensor discovery topic is exact");
        require(rfbridge::format_mqtt_entity_discovery_payload(
                    identity, sensor.kind, nullptr, board, "1.2.3", text, sizeof(text)) &&
                    std::strstr(text, "\"state_topic\":\"rfbridge/102030a1b2c3/state/system\"") !=
                        nullptr &&
                    std::strstr(text, sensor.path) != nullptr &&
                    std::strstr(text, "\"device_class\":\"data_size\"") != nullptr &&
                    std::strstr(text, "\"unit_of_measurement\":\"B\"") != nullptr &&
                    std::strstr(text, "\"entity_category\":\"diagnostic\"") != nullptr &&
                    std::strstr(text, "\"expire_after\":180") != nullptr,
                "system sensor discovery payload is exact and expiring");
    }

    rfbridge::MqttSystemTelemetry system{};
    system.board_profile = board.profile_name;
    system.board_target = board.target_name;
    system.requested_services = "both";
    system.effective_services = "both";
    system.uptime_s = 123;
    system.internal = {.total = 332187, .free = 53143, .minimum = 51883, .largest = 30720};
    system.psram = {
        .total = 8388608, .free = 8302836, .minimum = 8295624, .largest = 8257536};
    system.flash_mib = board.flash_mib;
    system.psram_mib = board.psram_mib;
    require(rfbridge::format_mqtt_system_state_payload(system, text, sizeof(text)),
            "bounded system telemetry formats");
    constexpr char expected[] =
        "{\"board\":{\"profile\":\"esp32s3-devkitc-n16r8\",\"target\":\"esp32s3\","
        "\"flash_mib\":16,\"psram_mib\":8},\"services\":{\"requested\":\"both\","
        "\"effective\":\"both\",\"reboot_required\":false},\"uptime_s\":123,"
        "\"memory\":{\"internal\":{\"total\":332187,\"free\":53143,\"minimum\":51883,"
        "\"largest\":30720},\"psram\":{\"total\":8388608,\"free\":8302836,"
        "\"minimum\":8295624,\"largest\":8257536}}}";
    require(std::strcmp(text, expected) == 0, "system telemetry JSON contract is exact");
    char short_payload[128]{};
    require(!rfbridge::format_mqtt_system_state_payload(system, short_payload,
                                                         sizeof(short_payload)),
            "system telemetry rejects truncation");
    system.board_profile = nullptr;
    require(!rfbridge::format_mqtt_system_state_payload(system, text, sizeof(text)),
            "system telemetry rejects missing metadata");
}

}  // namespace

int main()
{
    test_ipv4_contract();
    test_service_record();
    test_advertised_record();
    test_discovery_contract();
    test_incoming_messages();
    test_automation_discovery_and_telemetry();
    test_system_discovery_and_telemetry();
    test_network_availability_policy();
    test_service_mode_policy();
    std::puts("All MQTT host tests passed");
    return 0;
}
