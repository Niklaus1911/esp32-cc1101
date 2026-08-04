#include "mqtt_config_format.hpp"

#include <cstdio>
#include <cstring>

namespace rfbridge {
namespace {

constexpr uint8_t kServiceMagic[] = {'M', 'Q', 'S', 'C'};
constexpr uint8_t kAdvertisedMagic[] = {'M', 'Q', 'A', 'D'};
constexpr uint8_t kFormatVersion = 1;
constexpr std::size_t kServiceHeaderSize = 20;
constexpr std::size_t kAdvertisedHeaderSize = 12;
constexpr std::size_t kCrcSize = 4;

std::size_t bounded_length(const char *text, std::size_t capacity)
{
    std::size_t length = 0;
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

bool printable_ascii(const char *text, std::size_t length)
{
    for (std::size_t index = 0; index < length; ++index) {
        const uint8_t value = static_cast<uint8_t>(text[index]);
        if (value < 0x20 || value > 0x7e) {
            return false;
        }
    }
    return true;
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

void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
}

void write_u32(uint8_t *output, uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

uint16_t read_u16(const uint8_t *input)
{
    return static_cast<uint16_t>(input[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t read_u32(const uint8_t *input)
{
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(input[index]) << (index * 8U);
    }
    return value;
}

bool service_state_is_valid(MqttServiceState state)
{
    return state == MqttServiceState::kWeb || state == MqttServiceState::kMqtt ||
           state == MqttServiceState::kRetiring || state == MqttServiceState::kRetired;
}

}  // namespace

bool mqtt_broker_ipv4_is_valid(uint32_t address)
{
    const uint8_t first = static_cast<uint8_t>(address >> 24U);
    return address != 0 && address != UINT32_MAX && first != 0 && first != 127 && first < 224;
}

bool parse_mqtt_broker_ipv4(const char *text, uint32_t *address)
{
    if (text == nullptr || address == nullptr) {
        return false;
    }
    uint32_t octets[4]{};
    std::size_t octet = 0;
    bool digit_seen = false;
    for (const char *cursor = text;; ++cursor) {
        const char value = *cursor;
        if (value >= '0' && value <= '9') {
            digit_seen = true;
            octets[octet] = octets[octet] * 10U + static_cast<uint32_t>(value - '0');
            if (octets[octet] > 255U) {
                return false;
            }
            continue;
        }
        if ((value == '.' && octet < 3U) || (value == '\0' && octet == 3U)) {
            if (!digit_seen) {
                return false;
            }
            if (value == '\0') {
                break;
            }
            ++octet;
            digit_seen = false;
            continue;
        }
        return false;
    }
    const uint32_t parsed = (octets[0] << 24U) | (octets[1] << 16U) |
                            (octets[2] << 8U) | octets[3];
    if (!mqtt_broker_ipv4_is_valid(parsed)) {
        return false;
    }
    *address = parsed;
    return true;
}

bool format_mqtt_broker_ipv4(uint32_t address, char *output, std::size_t capacity)
{
    if (output == nullptr || capacity == 0 || !mqtt_broker_ipv4_is_valid(address)) {
        return false;
    }
    const int written = std::snprintf(
        output, capacity, "%u.%u.%u.%u", static_cast<unsigned>(address >> 24U),
        static_cast<unsigned>((address >> 16U) & 0xffU),
        static_cast<unsigned>((address >> 8U) & 0xffU), static_cast<unsigned>(address & 0xffU));
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

bool mqtt_service_has_credentials(const MqttServiceConfig &config)
{
    const std::size_t username_length = bounded_length(config.username, sizeof(config.username));
    const std::size_t password_length = bounded_length(config.password, sizeof(config.password));
    return mqtt_broker_ipv4_is_valid(config.broker_ipv4) && config.port != 0 &&
           username_length > 0 && username_length < sizeof(config.username) &&
           password_length > 0 && password_length < sizeof(config.password) &&
           printable_ascii(config.username, username_length) &&
           printable_ascii(config.password, password_length);
}

bool mqtt_service_config_is_valid(const MqttServiceConfig &config)
{
    if (!service_state_is_valid(config.state) || config.generation == 0) {
        return false;
    }
    const bool empty = config.broker_ipv4 == 0 && config.port == 0 && config.username[0] == '\0' &&
                       config.password[0] == '\0';
    if (config.state == MqttServiceState::kWeb) {
        return empty || mqtt_service_has_credentials(config);
    }
    return mqtt_service_has_credentials(config);
}

bool mqtt_advertised_ledger_is_valid(const MqttAdvertisedLedger &ledger)
{
    if (ledger.count > ledger.names.size()) {
        return false;
    }
    if (ledger.count == 0) {
        return (ledger.broker_ipv4 == 0 && ledger.port == 0) ||
               (mqtt_broker_ipv4_is_valid(ledger.broker_ipv4) && ledger.port != 0);
    }
    if (!mqtt_broker_ipv4_is_valid(ledger.broker_ipv4) || ledger.port == 0) {
        return false;
    }
    for (std::size_t index = 0; index < ledger.count; ++index) {
        if (!rf_storage_name_is_valid(ledger.names[index].value)) {
            return false;
        }
        const std::size_t name_length = bounded_length(ledger.names[index].value,
                                                       kRfStorageNameCapacity);
        for (std::size_t byte = name_length + 1U; byte < kRfStorageNameCapacity; ++byte) {
            if (ledger.names[index].value[byte] != '\0') {
                return false;
            }
        }
        if (index > 0 && std::strcmp(ledger.names[index - 1U].value,
                                     ledger.names[index].value) >= 0) {
            return false;
        }
    }
    return true;
}

bool mqtt_advertised_ledger_matches_endpoint(const MqttAdvertisedLedger &ledger,
                                             uint32_t broker_ipv4, uint16_t port)
{
    return ledger.count == 0 ||
           (ledger.broker_ipv4 == broker_ipv4 && ledger.port == port);
}

MqttConfigFormatResult merge_mqtt_advertised_ledgers(
    const MqttAdvertisedLedger &left, const MqttAdvertisedLedger &right,
    MqttAdvertisedLedger *merged)
{
    if (merged == nullptr || !mqtt_advertised_ledger_is_valid(left) ||
        !mqtt_advertised_ledger_is_valid(right)) {
        return MqttConfigFormatResult::kInvalidArgument;
    }
    const MqttAdvertisedLedger *endpoint = left.count > 0 ? &left : &right;
    if ((left.count > 0 && !mqtt_advertised_ledger_matches_endpoint(
                               left, endpoint->broker_ipv4, endpoint->port)) ||
        (right.count > 0 && !mqtt_advertised_ledger_matches_endpoint(
                                right, endpoint->broker_ipv4, endpoint->port))) {
        return MqttConfigFormatResult::kInvalidRecord;
    }

    MqttAdvertisedLedger result{};
    result.broker_ipv4 = endpoint->broker_ipv4;
    result.port = endpoint->port;
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.count || right_index < right.count) {
        const char *selected = nullptr;
        if (right_index >= right.count ||
            (left_index < left.count &&
             std::strcmp(left.names[left_index].value,
                         right.names[right_index].value) < 0)) {
            selected = left.names[left_index++].value;
        } else if (left_index >= left.count ||
                   std::strcmp(right.names[right_index].value,
                               left.names[left_index].value) < 0) {
            selected = right.names[right_index++].value;
        } else {
            selected = left.names[left_index++].value;
            ++right_index;
        }
        if (result.count >= result.names.size()) {
            return MqttConfigFormatResult::kBufferTooSmall;
        }
        std::memcpy(result.names[result.count].value, selected, kRfStorageNameCapacity);
        ++result.count;
    }
    if (!mqtt_advertised_ledger_is_valid(result)) {
        return MqttConfigFormatResult::kInvalidRecord;
    }
    *merged = result;
    return MqttConfigFormatResult::kOk;
}

MqttConfigFormatResult encode_mqtt_service_record(const MqttServiceConfig &config,
                                                  uint8_t *output, std::size_t capacity,
                                                  std::size_t *encoded_size)
{
    if (output == nullptr || encoded_size == nullptr || !mqtt_service_config_is_valid(config)) {
        return MqttConfigFormatResult::kInvalidArgument;
    }
    const std::size_t username_length = std::strlen(config.username);
    const std::size_t password_length = std::strlen(config.password);
    const std::size_t record_size =
        kServiceHeaderSize + username_length + password_length + kCrcSize;
    if (record_size > capacity) {
        return MqttConfigFormatResult::kBufferTooSmall;
    }
    std::memcpy(output, kServiceMagic, sizeof(kServiceMagic));
    output[4] = kFormatVersion;
    output[5] = static_cast<uint8_t>(config.state);
    output[6] = static_cast<uint8_t>(username_length);
    output[7] = static_cast<uint8_t>(password_length);
    write_u32(output + 8, config.broker_ipv4);
    write_u16(output + 12, config.port);
    output[14] = 0;
    output[15] = 0;
    write_u32(output + 16, config.generation);
    std::memcpy(output + kServiceHeaderSize, config.username, username_length);
    std::memcpy(output + kServiceHeaderSize + username_length, config.password, password_length);
    write_u32(output + record_size - kCrcSize, crc32(output, record_size - kCrcSize));
    *encoded_size = record_size;
    return MqttConfigFormatResult::kOk;
}

MqttConfigFormatResult decode_mqtt_service_record(const uint8_t *record, std::size_t size,
                                                  MqttServiceConfig *config)
{
    if (record == nullptr || config == nullptr) {
        return MqttConfigFormatResult::kInvalidArgument;
    }
    if (size < kServiceHeaderSize + kCrcSize || size > kMqttServiceMaxRecordSize ||
        std::memcmp(record, kServiceMagic, sizeof(kServiceMagic)) != 0 || record[14] != 0 ||
        record[15] != 0) {
        return MqttConfigFormatResult::kInvalidRecord;
    }
    if (record[4] != kFormatVersion) {
        return MqttConfigFormatResult::kInvalidVersion;
    }
    const std::size_t username_length = record[6];
    const std::size_t password_length = record[7];
    if (username_length >= kMqttUsernameCapacity || password_length >= kMqttPasswordCapacity ||
        size != kServiceHeaderSize + username_length + password_length + kCrcSize) {
        return MqttConfigFormatResult::kInvalidRecord;
    }
    if (read_u32(record + size - kCrcSize) != crc32(record, size - kCrcSize)) {
        return MqttConfigFormatResult::kInvalidCrc;
    }
    MqttServiceConfig decoded{};
    decoded.state = static_cast<MqttServiceState>(record[5]);
    decoded.broker_ipv4 = read_u32(record + 8);
    decoded.port = read_u16(record + 12);
    decoded.generation = read_u32(record + 16);
    std::memcpy(decoded.username, record + kServiceHeaderSize, username_length);
    std::memcpy(decoded.password, record + kServiceHeaderSize + username_length, password_length);
    if (!mqtt_service_config_is_valid(decoded)) {
        return MqttConfigFormatResult::kInvalidRecord;
    }
    *config = decoded;
    return MqttConfigFormatResult::kOk;
}

MqttConfigFormatResult encode_mqtt_advertised_record(const MqttAdvertisedLedger &ledger,
                                                     uint8_t *output, std::size_t capacity,
                                                     std::size_t *encoded_size)
{
    if (output == nullptr || encoded_size == nullptr || !mqtt_advertised_ledger_is_valid(ledger)) {
        return MqttConfigFormatResult::kInvalidArgument;
    }
    const std::size_t record_size = kAdvertisedHeaderSize +
                                    static_cast<std::size_t>(ledger.count) *
                                        kRfStorageNameCapacity +
                                    kCrcSize;
    if (record_size > capacity) {
        return MqttConfigFormatResult::kBufferTooSmall;
    }
    std::memcpy(output, kAdvertisedMagic, sizeof(kAdvertisedMagic));
    output[4] = kFormatVersion;
    output[5] = ledger.count;
    write_u16(output + 6, ledger.port);
    write_u32(output + 8, ledger.broker_ipv4);
    for (std::size_t index = 0; index < ledger.count; ++index) {
        std::memcpy(output + kAdvertisedHeaderSize + index * kRfStorageNameCapacity,
                    ledger.names[index].value, kRfStorageNameCapacity);
    }
    write_u32(output + record_size - kCrcSize, crc32(output, record_size - kCrcSize));
    *encoded_size = record_size;
    return MqttConfigFormatResult::kOk;
}

MqttConfigFormatResult decode_mqtt_advertised_record(const uint8_t *record, std::size_t size,
                                                     MqttAdvertisedLedger *ledger)
{
    if (record == nullptr || ledger == nullptr) {
        return MqttConfigFormatResult::kInvalidArgument;
    }
    if (size < kAdvertisedHeaderSize + kCrcSize || size > kMqttAdvertisedMaxRecordSize ||
        std::memcmp(record, kAdvertisedMagic, sizeof(kAdvertisedMagic)) != 0) {
        return MqttConfigFormatResult::kInvalidRecord;
    }
    if (record[4] != kFormatVersion) {
        return MqttConfigFormatResult::kInvalidVersion;
    }
    const std::size_t count = record[5];
    if (count > kMqttMaximumAdvertisedSignals ||
        size != kAdvertisedHeaderSize + count * kRfStorageNameCapacity + kCrcSize) {
        return MqttConfigFormatResult::kInvalidRecord;
    }
    if (read_u32(record + size - kCrcSize) != crc32(record, size - kCrcSize)) {
        return MqttConfigFormatResult::kInvalidCrc;
    }
    MqttAdvertisedLedger decoded{};
    decoded.count = static_cast<uint8_t>(count);
    decoded.port = read_u16(record + 6);
    decoded.broker_ipv4 = read_u32(record + 8);
    for (std::size_t index = 0; index < count; ++index) {
        std::memcpy(decoded.names[index].value,
                    record + kAdvertisedHeaderSize + index * kRfStorageNameCapacity,
                    kRfStorageNameCapacity);
    }
    if (!mqtt_advertised_ledger_is_valid(decoded)) {
        return MqttConfigFormatResult::kInvalidRecord;
    }
    *ledger = decoded;
    return MqttConfigFormatResult::kOk;
}

}  // namespace rfbridge
