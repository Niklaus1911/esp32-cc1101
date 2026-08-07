#include "rf_storage_format.hpp"

#include <cstring>

namespace rfbridge {
namespace {

constexpr uint8_t kMagic[] = {'R', 'F', 'S', 'R'};
constexpr uint8_t kFormatVersion = 1;
constexpr uint8_t kDecodedKind = 1;
constexpr uint8_t kRawKind = 2;
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kCrcSize = 4;
constexpr std::size_t kDecodedPayloadSize = 13;
constexpr std::size_t kRawPrefixSize = 3;
constexpr uint8_t kHardwareMagic[] = {'R', 'F', 'H', 'W'};
constexpr uint8_t kHardwareFormatVersion = 1;
constexpr std::size_t kHardwareHeaderSize = 8;
constexpr std::size_t kHardwareRecordSize = kHardwareHeaderSize + 1U + 4U;

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

void write_u64(uint8_t *output, uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
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

uint64_t read_u64(const uint8_t *input)
{
    uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(input[index]) << (index * 8U);
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

bool ascii_letter(char character)
{
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

bool ascii_digit(char character)
{
    return character >= '0' && character <= '9';
}

}  // namespace

const char *rf_hardware_name(RfHardware hardware)
{
    switch (hardware) {
        case RfHardware::kCc1101: return "cc1101";
        case RfHardware::kGeneric: return "generic";
    }
    return "invalid";
}

bool rf_hardware_from_name(const char *name, RfHardware *hardware)
{
    if (name == nullptr || hardware == nullptr) {
        return false;
    }
    if (std::strcmp(name, "cc1101") == 0) {
        *hardware = RfHardware::kCc1101;
        return true;
    }
    if (std::strcmp(name, "generic") == 0) {
        *hardware = RfHardware::kGeneric;
        return true;
    }
    return false;
}

bool rf_storage_name_is_valid(const char *name)
{
    if (name == nullptr || !ascii_letter(name[0])) {
        return false;
    }
    std::size_t length = 1;
    while (length < kRfStorageNameCapacity && name[length] != '\0') {
        const char character = name[length];
        if (!ascii_letter(character) && !ascii_digit(character) && character != '_' && character != '-') {
            return false;
        }
        ++length;
    }
    if (length == kRfStorageNameCapacity || name[length] != '\0') {
        return false;
    }
    return std::strcmp(name, "list") != 0;
}

RfStorageFormatResult encode_rf_storage_record(const RfStoredSignal &signal, uint8_t *output,
                                               std::size_t capacity, std::size_t *output_size)
{
    if (output == nullptr || output_size == nullptr) {
        return RfStorageFormatResult::kInvalidArgument;
    }

    uint8_t kind = 0;
    std::size_t payload_size = 0;
    if (signal.encoding == RfStoredEncoding::kDecoded) {
        const RfProtocol *protocol = rf_protocol(signal.decoded.protocol);
        if (!decoded_signal_is_valid(signal.decoded) || signal.decoded.inverted > 1 || protocol == nullptr ||
            signal.decoded.inverted != static_cast<uint8_t>(protocol->inverted)) {
            return RfStorageFormatResult::kInvalidArgument;
        }
        kind = kDecodedKind;
        payload_size = kDecodedPayloadSize;
    } else if (signal.encoding == RfStoredEncoding::kRaw) {
        if (!raw_signal_is_valid(signal.raw)) {
            return RfStorageFormatResult::kInvalidArgument;
        }
        kind = kRawKind;
        payload_size = kRawPrefixSize + static_cast<std::size_t>(signal.raw.count) * sizeof(uint16_t);
    } else {
        return RfStorageFormatResult::kInvalidArgument;
    }

    const std::size_t record_size = kHeaderSize + payload_size + kCrcSize;
    if (capacity < record_size) {
        return RfStorageFormatResult::kBufferTooSmall;
    }

    std::memcpy(output, kMagic, sizeof(kMagic));
    output[4] = kFormatVersion;
    output[5] = kind;
    write_u16(output + 6, static_cast<uint16_t>(payload_size));
    uint8_t *payload = output + kHeaderSize;
    if (kind == kDecodedKind) {
        write_u64(payload, signal.decoded.code);
        write_u16(payload + 8, signal.decoded.pulse_us);
        payload[10] = signal.decoded.bits;
        payload[11] = signal.decoded.protocol;
        payload[12] = signal.decoded.inverted;
    } else {
        write_u16(payload, signal.raw.count);
        payload[2] = signal.raw.start_level;
        for (std::size_t index = 0; index < signal.raw.count; ++index) {
            write_u16(payload + kRawPrefixSize + index * sizeof(uint16_t), signal.raw.durations_us[index]);
        }
    }
    write_u32(output + record_size - kCrcSize, crc32(output, record_size - kCrcSize));
    *output_size = record_size;
    return RfStorageFormatResult::kOk;
}

RfStorageFormatResult decode_rf_storage_record(const uint8_t *record, std::size_t size, RfStoredSignal *signal)
{
    if (record == nullptr || signal == nullptr) {
        return RfStorageFormatResult::kInvalidArgument;
    }
    if (size < kHeaderSize + kCrcSize || size > kRfStorageMaxRecordSize ||
        std::memcmp(record, kMagic, sizeof(kMagic)) != 0) {
        return RfStorageFormatResult::kInvalidRecord;
    }
    const uint32_t stored_crc = read_u32(record + size - kCrcSize);
    if (stored_crc != crc32(record, size - kCrcSize)) {
        return RfStorageFormatResult::kInvalidCrc;
    }
    if (record[4] != kFormatVersion) {
        return RfStorageFormatResult::kInvalidVersion;
    }

    const uint8_t kind = record[5];
    const std::size_t payload_size = read_u16(record + 6);
    if (size != kHeaderSize + payload_size + kCrcSize) {
        return RfStorageFormatResult::kInvalidRecord;
    }

    const uint8_t *payload = record + kHeaderSize;
    RfStoredSignal decoded{};
    if (kind == kDecodedKind) {
        if (payload_size != kDecodedPayloadSize) {
            return RfStorageFormatResult::kInvalidRecord;
        }
        decoded.encoding = RfStoredEncoding::kDecoded;
        decoded.decoded.code = read_u64(payload);
        decoded.decoded.pulse_us = read_u16(payload + 8);
        decoded.decoded.bits = payload[10];
        decoded.decoded.protocol = payload[11];
        decoded.decoded.inverted = payload[12];
        const RfProtocol *protocol = rf_protocol(decoded.decoded.protocol);
        if (protocol == nullptr || decoded.decoded.inverted > 1 ||
            decoded.decoded.inverted != static_cast<uint8_t>(protocol->inverted) ||
            !decoded_signal_is_valid(decoded.decoded)) {
            return RfStorageFormatResult::kInvalidRecord;
        }
    } else if (kind == kRawKind) {
        if (payload_size < kRawPrefixSize) {
            return RfStorageFormatResult::kInvalidRecord;
        }
        decoded.encoding = RfStoredEncoding::kRaw;
        decoded.raw.count = read_u16(payload);
        decoded.raw.start_level = payload[2];
        const std::size_t expected_payload =
            kRawPrefixSize + static_cast<std::size_t>(decoded.raw.count) * sizeof(uint16_t);
        if (decoded.raw.count > kMaxRawPulses || payload_size != expected_payload) {
            return RfStorageFormatResult::kInvalidRecord;
        }
        for (std::size_t index = 0; index < decoded.raw.count; ++index) {
            decoded.raw.durations_us[index] = read_u16(payload + kRawPrefixSize + index * sizeof(uint16_t));
        }
        if (!raw_signal_is_valid(decoded.raw)) {
            return RfStorageFormatResult::kInvalidRecord;
        }
    } else {
        return RfStorageFormatResult::kInvalidRecord;
    }

    *signal = decoded;
    return RfStorageFormatResult::kOk;
}

RfHardwareFormatResult encode_rf_hardware_record(RfHardware hardware, uint8_t *output,
                                                 std::size_t capacity, std::size_t *output_size)
{
    if (output == nullptr || output_size == nullptr ||
        (hardware != RfHardware::kCc1101 && hardware != RfHardware::kGeneric)) {
        return RfHardwareFormatResult::kInvalidArgument;
    }
    if (capacity < kHardwareRecordSize) {
        return RfHardwareFormatResult::kBufferTooSmall;
    }
    std::memcpy(output, kHardwareMagic, sizeof(kHardwareMagic));
    output[4] = kHardwareFormatVersion;
    output[5] = 1;
    write_u16(output + 6, 1);
    output[kHardwareHeaderSize] = static_cast<uint8_t>(hardware);
    write_u32(output + kHardwareRecordSize - 4U, crc32(output, kHardwareRecordSize - 4U));
    *output_size = kHardwareRecordSize;
    return RfHardwareFormatResult::kOk;
}

RfHardwareFormatResult decode_rf_hardware_record(const uint8_t *record, std::size_t size,
                                                 RfHardware *hardware)
{
    if (record == nullptr || hardware == nullptr) {
        return RfHardwareFormatResult::kInvalidArgument;
    }
    if (size != kHardwareRecordSize || std::memcmp(record, kHardwareMagic, sizeof(kHardwareMagic)) != 0) {
        return RfHardwareFormatResult::kInvalidRecord;
    }
    if (record[4] != kHardwareFormatVersion) {
        return RfHardwareFormatResult::kInvalidVersion;
    }
    if (record[5] != 1 || read_u16(record + 6) != 1 ||
        read_u32(record + size - 4U) != crc32(record, size - 4U)) {
        return read_u32(record + size - 4U) != crc32(record, size - 4U)
                   ? RfHardwareFormatResult::kInvalidCrc
                   : RfHardwareFormatResult::kInvalidRecord;
    }
    const auto value = static_cast<RfHardware>(record[kHardwareHeaderSize]);
    if (value != RfHardware::kCc1101 && value != RfHardware::kGeneric) {
        return RfHardwareFormatResult::kInvalidRecord;
    }
    *hardware = value;
    return RfHardwareFormatResult::kOk;
}

}  // namespace rfbridge
