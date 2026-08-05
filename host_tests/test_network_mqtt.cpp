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
    config.state = rfbridge::MqttServiceState::kMqtt;
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
                decoded.state == source.state && decoded.broker_ipv4 == source.broker_ipv4 &&
                decoded.port == source.port && decoded.generation == source.generation &&
                std::strcmp(decoded.username, source.username) == 0 &&
                std::strcmp(decoded.password, source.password) == 0,
            "service record round trips all persisted fields");

    record[20] ^= 1U;
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidCrc,
            "service record rejects CRC corruption");
    record[20] ^= 1U;
    record[4] = 2;
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidVersion,
            "service record rejects unknown versions");
    record[4] = 1;
    record[5] = 99;
    rewrite_crc(record.data(), size);
    require(rfbridge::decode_mqtt_service_record(record.data(), size, &decoded) ==
                rfbridge::MqttConfigFormatResult::kInvalidRecord,
            "service record rejects unknown state values");
    record[5] = static_cast<uint8_t>(rfbridge::MqttServiceState::kMqtt);
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

    source.state = rfbridge::MqttServiceState::kWeb;
    source.broker_ipv4 = 0;
    source.port = 0;
    source.username[0] = '\0';
    source.password[0] = '\0';
    require(rfbridge::mqtt_service_config_is_valid(source),
            "Web profile permits an unconfigured service record");
    source.state = rfbridge::MqttServiceState::kMqtt;
    require(!rfbridge::mqtt_service_config_is_valid(source),
            "MQTT profile requires credentials");
    source = service_config();
    source.password[3] = '\n';
    require(!rfbridge::mqtt_service_has_credentials(source),
            "service credentials reject non-printable bytes");
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

void test_discovery_contract()
{
    const rfbridge::MqttDeviceIdentity identity = test_identity();
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
    require(rfbridge::format_mqtt_discovery_payload(identity, "gate", "1.2.3", text,
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
        "\"model\":\"ESP32 + CC1101\",\"sw_version\":\"1.2.3\"}}";
    require(std::strcmp(text, expected) == 0, "discovery JSON contract is exact");
    require(rfbridge::format_mqtt_discovery_payload(identity, "gate", "a\"b\\c", text,
                                                     sizeof(text)) &&
                std::strstr(text, "\"sw_version\":\"a\\\"b\\\\c\"") != nullptr,
            "discovery JSON escapes firmware metadata");
    char short_payload[128]{};
    require(!rfbridge::format_mqtt_discovery_payload(identity, "gate", "1.2.3",
                                                      short_payload, sizeof(short_payload)) &&
                !rfbridge::format_mqtt_discovery_topic(identity, "list", text, sizeof(text)),
            "discovery formatting rejects truncation and invalid signal names");
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
                identity, rfbridge::MqttDiscoveryEntityKind::kAutomationSwitch, nullptr, "1.2.3",
                text, sizeof(text)) &&
                std::strstr(text, "\"command_topic\":\"rfbridge/102030a1b2c3/automation/enabled/set\"") !=
                    nullptr &&
                std::strstr(text, "\"value_template\":\"{{ value_json.enabled }}\"") != nullptr,
            "automation switch discovery contains state and command contracts");
    require(rfbridge::format_mqtt_entity_discovery_payload(
                identity, rfbridge::MqttDiscoveryEntityKind::kRuleSensor, "gate", "1.2.3",
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

}  // namespace

int main()
{
    test_ipv4_contract();
    test_service_record();
    test_advertised_record();
    test_discovery_contract();
    test_incoming_messages();
    test_automation_discovery_and_telemetry();
    test_network_availability_policy();
    std::puts("All MQTT host tests passed");
    return 0;
}
