#include "network_wifi_config.hpp"

#include <cstring>

namespace rfbridge {
namespace {

constexpr uint8_t kMagic[] = {'N', 'W', 'C', 'F'};
constexpr uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kCrcSize = 4;

std::size_t bounded_length(const char *text, std::size_t capacity)
{
    std::size_t length = 0;
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

bool is_printable_ascii(const char *text, std::size_t length)
{
    for (std::size_t index = 0; index < length; ++index) {
        const uint8_t value = static_cast<uint8_t>(text[index]);
        if (value < 0x20 || value > 0x7e) {
            return false;
        }
    }
    return true;
}

bool is_hex_password(const char *text, std::size_t length)
{
    if (length != 64) {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        const char value = text[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
              (value >= 'A' && value <= 'F'))) {
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

void write_u32(uint8_t *output, uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

uint32_t read_u32(const uint8_t *input)
{
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(input[index]) << (index * 8U);
    }
    return value;
}

}  // namespace

bool wifi_credentials_are_valid(const WifiCredentials &credentials)
{
    const std::size_t ssid_length = bounded_length(credentials.ssid, sizeof(credentials.ssid));
    const std::size_t password_length = bounded_length(credentials.password, sizeof(credentials.password));
    if (ssid_length == 0 || ssid_length >= sizeof(credentials.ssid) ||
        password_length >= sizeof(credentials.password) ||
        !is_printable_ascii(credentials.ssid, ssid_length) ||
        !is_printable_ascii(credentials.password, password_length)) {
        return false;
    }
    return password_length == 0 || (password_length >= 8 && password_length <= 63) ||
           is_hex_password(credentials.password, password_length);
}

WifiConfigFormatResult encode_wifi_config_record(const WifiCredentials &credentials, uint8_t *output,
                                                  std::size_t capacity, std::size_t *encoded_size)
{
    if (output == nullptr || encoded_size == nullptr || !wifi_credentials_are_valid(credentials)) {
        return WifiConfigFormatResult::kInvalidArgument;
    }
    const std::size_t ssid_length = std::strlen(credentials.ssid);
    const std::size_t password_length = std::strlen(credentials.password);
    const std::size_t record_size = kHeaderSize + ssid_length + password_length + kCrcSize;
    if (record_size > capacity) {
        return WifiConfigFormatResult::kBufferTooSmall;
    }

    std::memcpy(output, kMagic, sizeof(kMagic));
    output[4] = kVersion;
    output[5] = static_cast<uint8_t>(ssid_length);
    output[6] = static_cast<uint8_t>(password_length);
    output[7] = 0;
    std::memcpy(output + kHeaderSize, credentials.ssid, ssid_length);
    std::memcpy(output + kHeaderSize + ssid_length, credentials.password, password_length);
    write_u32(output + record_size - kCrcSize, crc32(output, record_size - kCrcSize));
    *encoded_size = record_size;
    return WifiConfigFormatResult::kOk;
}

WifiConfigFormatResult decode_wifi_config_record(const uint8_t *record, std::size_t size,
                                                  WifiCredentials *credentials)
{
    if (record == nullptr || credentials == nullptr) {
        return WifiConfigFormatResult::kInvalidArgument;
    }
    if (size < kHeaderSize + 1U + kCrcSize || size > kWifiConfigMaxRecordSize) {
        return WifiConfigFormatResult::kInvalidRecord;
    }
    if (std::memcmp(record, kMagic, sizeof(kMagic)) != 0 || record[7] != 0) {
        return WifiConfigFormatResult::kInvalidRecord;
    }
    if (record[4] != kVersion) {
        return WifiConfigFormatResult::kInvalidVersion;
    }
    const std::size_t ssid_length = record[5];
    const std::size_t password_length = record[6];
    if (ssid_length == 0 || ssid_length >= kWifiSsidCapacity || password_length >= kWifiPasswordCapacity ||
        size != kHeaderSize + ssid_length + password_length + kCrcSize) {
        return WifiConfigFormatResult::kInvalidRecord;
    }
    if (read_u32(record + size - kCrcSize) != crc32(record, size - kCrcSize)) {
        return WifiConfigFormatResult::kInvalidCrc;
    }

    WifiCredentials decoded{};
    std::memcpy(decoded.ssid, record + kHeaderSize, ssid_length);
    std::memcpy(decoded.password, record + kHeaderSize + ssid_length, password_length);
    if (!wifi_credentials_are_valid(decoded)) {
        return WifiConfigFormatResult::kInvalidRecord;
    }
    *credentials = decoded;
    return WifiConfigFormatResult::kOk;
}

}  // namespace rfbridge
