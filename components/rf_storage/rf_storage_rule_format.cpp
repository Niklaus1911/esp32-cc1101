#include "rf_storage_rule_format.hpp"

#include <cstring>

namespace rfbridge {
namespace {

constexpr uint8_t kMagic[] = {'R', 'F', 'R', 'L'};
constexpr uint8_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kPayloadPrefixSize = 6;
constexpr std::size_t kCrcSize = 4;

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
    return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t read_u32(const uint8_t *input)
{
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(input[index]) << (index * 8U);
    }
    return value;
}

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

} // namespace

bool rf_stored_rule_is_valid(const RfStoredRule &rule)
{
    return rf_storage_name_is_valid(rule.target_name) && rule.repeats >= 1 && rule.repeats <= 20 &&
           rule.cooldown_ms == kRfAutomationCooldownMs;
}

RfRuleFormatResult encode_rf_rule_record(const RfStoredRule &rule, uint8_t *output, std::size_t capacity,
                                         std::size_t *output_size)
{
    if (output == nullptr || output_size == nullptr || !rf_stored_rule_is_valid(rule)) {
        return RfRuleFormatResult::kInvalidArgument;
    }
    const std::size_t target_length = std::strlen(rule.target_name);
    const std::size_t payload_size = kPayloadPrefixSize + target_length;
    const std::size_t record_size = kHeaderSize + payload_size + kCrcSize;
    if (capacity < record_size) {
        return RfRuleFormatResult::kBufferTooSmall;
    }

    std::memcpy(output, kMagic, sizeof(kMagic));
    output[4] = kFormatVersion;
    output[5] = 0;
    write_u16(output + 6, static_cast<uint16_t>(payload_size));
    uint8_t *payload = output + kHeaderSize;
    payload[0] = static_cast<uint8_t>(target_length);
    payload[1] = rule.repeats;
    write_u32(payload + 2, rule.cooldown_ms);
    std::memcpy(payload + kPayloadPrefixSize, rule.target_name, target_length);
    write_u32(output + record_size - kCrcSize, crc32(output, record_size - kCrcSize));
    *output_size = record_size;
    return RfRuleFormatResult::kOk;
}

RfRuleFormatResult decode_rf_rule_record(const uint8_t *record, std::size_t size, RfStoredRule *rule)
{
    if (record == nullptr || rule == nullptr) {
        return RfRuleFormatResult::kInvalidArgument;
    }
    if (size < kHeaderSize + kPayloadPrefixSize + 1U + kCrcSize || size > kRfStorageMaxRuleRecordSize ||
        std::memcmp(record, kMagic, sizeof(kMagic)) != 0) {
        return RfRuleFormatResult::kInvalidRecord;
    }
    if (read_u32(record + size - kCrcSize) != crc32(record, size - kCrcSize)) {
        return RfRuleFormatResult::kInvalidCrc;
    }
    if (record[4] != kFormatVersion) {
        return RfRuleFormatResult::kInvalidVersion;
    }
    if (record[5] != 0) {
        return RfRuleFormatResult::kInvalidRecord;
    }

    const std::size_t payload_size = read_u16(record + 6);
    if (size != kHeaderSize + payload_size + kCrcSize || payload_size < kPayloadPrefixSize + 1U) {
        return RfRuleFormatResult::kInvalidRecord;
    }
    const uint8_t *payload = record + kHeaderSize;
    const std::size_t target_length = payload[0];
    if (target_length == 0 || target_length >= kRfStorageNameCapacity ||
        payload_size != kPayloadPrefixSize + target_length ||
        std::memchr(payload + kPayloadPrefixSize, '\0', target_length) != nullptr) {
        return RfRuleFormatResult::kInvalidRecord;
    }

    RfStoredRule decoded{};
    std::memcpy(decoded.target_name, payload + kPayloadPrefixSize, target_length);
    decoded.target_name[target_length] = '\0';
    decoded.repeats = payload[1];
    decoded.cooldown_ms = read_u32(payload + 2);
    if (!rf_stored_rule_is_valid(decoded)) {
        return RfRuleFormatResult::kInvalidRecord;
    }
    *rule = decoded;
    return RfRuleFormatResult::kOk;
}

} // namespace rfbridge
