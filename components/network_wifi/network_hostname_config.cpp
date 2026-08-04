#include "network_hostname_config.hpp"

#include <cstdio>
#include <cstring>

namespace rfbridge {
namespace {

constexpr uint8_t kMagic[] = {'N', 'H', 'C', 'F'};
constexpr uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kCrcSize = 4;

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

bool canonicalize_network_hostname(const char *input, char *output, std::size_t capacity)
{
    if (input == nullptr || output == nullptr || capacity < kNetworkHostnameCapacity) {
        return false;
    }
    std::size_t length = 0;
    while (length < kNetworkHostnameCapacity && input[length] != '\0') {
        ++length;
    }
    if (length == 0 || length > kNetworkHostnameMaxLength) {
        output[0] = '\0';
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        const char value = input[index];
        const bool lower = value >= 'a' && value <= 'z';
        const bool upper = value >= 'A' && value <= 'Z';
        const bool digit = value >= '0' && value <= '9';
        if (!lower && !upper && !digit && value != '-') {
            output[0] = '\0';
            return false;
        }
        if (value == '-' && (index == 0 || index + 1U == length)) {
            output[0] = '\0';
            return false;
        }
        output[index] = upper ? static_cast<char>(value - 'A' + 'a') : value;
    }
    output[length] = '\0';
    return true;
}

bool derive_default_network_hostname(const uint8_t mac[6], char *hostname,
                                     std::size_t hostname_capacity, char *mac_suffix,
                                     std::size_t suffix_capacity)
{
    if (mac == nullptr || hostname == nullptr || mac_suffix == nullptr ||
        hostname_capacity < kNetworkHostnameCapacity ||
        suffix_capacity < kNetworkHostnameMacSuffixCapacity) {
        return false;
    }
    const int suffix_length = std::snprintf(mac_suffix, suffix_capacity, "%02x%02x%02x",
                                            mac[3], mac[4], mac[5]);
    const int hostname_length = std::snprintf(hostname, hostname_capacity,
                                              "esp32-cc1101-%s", mac_suffix);
    return suffix_length == 6 && hostname_length > 0 &&
           static_cast<std::size_t>(hostname_length) < hostname_capacity;
}

NetworkHostnameFormatResult encode_network_hostname_record(const char *hostname, uint8_t *output,
                                                           std::size_t capacity,
                                                           std::size_t *encoded_size)
{
    char canonical[kNetworkHostnameCapacity]{};
    if (hostname == nullptr || output == nullptr || encoded_size == nullptr ||
        !canonicalize_network_hostname(hostname, canonical, sizeof(canonical)) ||
        std::strcmp(hostname, canonical) != 0) {
        return NetworkHostnameFormatResult::kInvalidArgument;
    }
    const std::size_t length = std::strlen(canonical);
    const std::size_t record_size = kHeaderSize + length + kCrcSize;
    if (record_size > capacity) {
        return NetworkHostnameFormatResult::kBufferTooSmall;
    }
    std::memcpy(output, kMagic, sizeof(kMagic));
    output[4] = kVersion;
    output[5] = static_cast<uint8_t>(length);
    output[6] = 0;
    output[7] = 0;
    std::memcpy(output + kHeaderSize, canonical, length);
    write_u32(output + record_size - kCrcSize, crc32(output, record_size - kCrcSize));
    *encoded_size = record_size;
    return NetworkHostnameFormatResult::kOk;
}

NetworkHostnameFormatResult decode_network_hostname_record(const uint8_t *record, std::size_t size,
                                                           char *hostname,
                                                           std::size_t hostname_capacity)
{
    if (record == nullptr || hostname == nullptr || hostname_capacity < kNetworkHostnameCapacity) {
        return NetworkHostnameFormatResult::kInvalidArgument;
    }
    if (size < kHeaderSize + 1U + kCrcSize || size > kNetworkHostnameRecordMaxSize ||
        std::memcmp(record, kMagic, sizeof(kMagic)) != 0 || record[6] != 0 || record[7] != 0) {
        return NetworkHostnameFormatResult::kInvalidRecord;
    }
    if (record[4] != kVersion) {
        return NetworkHostnameFormatResult::kInvalidVersion;
    }
    const std::size_t length = record[5];
    if (length == 0 || length > kNetworkHostnameMaxLength ||
        size != kHeaderSize + length + kCrcSize) {
        return NetworkHostnameFormatResult::kInvalidRecord;
    }
    if (read_u32(record + size - kCrcSize) != crc32(record, size - kCrcSize)) {
        return NetworkHostnameFormatResult::kInvalidCrc;
    }
    char decoded[kNetworkHostnameCapacity]{};
    std::memcpy(decoded, record + kHeaderSize, length);
    char canonical[kNetworkHostnameCapacity]{};
    if (!canonicalize_network_hostname(decoded, canonical, sizeof(canonical)) ||
        std::strcmp(decoded, canonical) != 0) {
        return NetworkHostnameFormatResult::kInvalidRecord;
    }
    std::memcpy(hostname, decoded, length + 1U);
    return NetworkHostnameFormatResult::kOk;
}

}  // namespace rfbridge
