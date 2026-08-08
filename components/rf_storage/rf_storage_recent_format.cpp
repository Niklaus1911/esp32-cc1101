#include "rf_storage_recent_format.hpp"

#include <cstring>

namespace rfbridge {
namespace {

constexpr uint8_t kMagic[] = {'R', 'F', 'R', 'H'};
constexpr uint8_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kNextIdSize = 8;
constexpr std::size_t kEntrySize = 21;
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

void write_u64(uint8_t *output, uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
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

bool decoded_is_valid(const DecodedSignal &decoded)
{
    const RfProtocol *protocol = rf_protocol(decoded.protocol);
    return decoded_signal_is_valid(decoded) && decoded.inverted <= 1 && protocol != nullptr &&
           decoded.inverted == static_cast<uint8_t>(protocol->inverted);
}

}  // namespace

bool rf_recent_history_is_valid(const RfRecentHistory &history)
{
    if (history.next_id == 0 || history.count > kRfRecentSignalCapacity) {
        return false;
    }
    uint64_t previous_id = history.next_id;
    for (std::size_t index = 0; index < history.count; ++index) {
        const RfRecentSignal &entry = history.entries[index];
        if (entry.id == 0 || entry.id >= previous_id || !decoded_is_valid(entry.decoded)) {
            return false;
        }
        previous_id = entry.id;
    }
    return true;
}

RfRecentFormatResult append_rf_recent_history(RfRecentHistory *history,
                                              const DecodedSignal &decoded,
                                              RfRecentSignal *appended)
{
    if (history == nullptr || !rf_recent_history_is_valid(*history) ||
        !decoded_is_valid(decoded) || history->next_id == UINT64_MAX) {
        return RfRecentFormatResult::kInvalidArgument;
    }
    const std::size_t retained = history->count < kRfRecentSignalCapacity
                                     ? history->count
                                     : kRfRecentSignalCapacity - 1U;
    for (std::size_t index = retained; index > 0; --index) {
        history->entries[index] = history->entries[index - 1U];
    }
    history->entries[0].id = history->next_id++;
    history->entries[0].decoded = decoded;
    history->count = retained + 1U;
    if (appended != nullptr) {
        *appended = history->entries[0];
    }
    return RfRecentFormatResult::kOk;
}

RfRecentFormatResult encode_rf_recent_record(const RfRecentHistory &history, uint8_t *output,
                                             std::size_t capacity, std::size_t *output_size)
{
    if (output == nullptr || output_size == nullptr || !rf_recent_history_is_valid(history)) {
        return RfRecentFormatResult::kInvalidArgument;
    }
    const std::size_t payload_size = kNextIdSize + history.count * kEntrySize;
    const std::size_t record_size = kHeaderSize + payload_size + kCrcSize;
    if (capacity < record_size) {
        return RfRecentFormatResult::kBufferTooSmall;
    }

    std::memcpy(output, kMagic, sizeof(kMagic));
    output[4] = kFormatVersion;
    output[5] = static_cast<uint8_t>(history.count);
    write_u16(output + 6, static_cast<uint16_t>(payload_size));
    uint8_t *payload = output + kHeaderSize;
    write_u64(payload, history.next_id);
    for (std::size_t index = 0; index < history.count; ++index) {
        uint8_t *entry = payload + kNextIdSize + index * kEntrySize;
        write_u64(entry, history.entries[index].id);
        write_u64(entry + 8, history.entries[index].decoded.code);
        write_u16(entry + 16, history.entries[index].decoded.pulse_us);
        entry[18] = history.entries[index].decoded.bits;
        entry[19] = history.entries[index].decoded.protocol;
        entry[20] = history.entries[index].decoded.inverted;
    }
    write_u32(output + record_size - kCrcSize, crc32(output, record_size - kCrcSize));
    *output_size = record_size;
    return RfRecentFormatResult::kOk;
}

RfRecentFormatResult decode_rf_recent_record(const uint8_t *record, std::size_t size,
                                             RfRecentHistory *history)
{
    if (record == nullptr || history == nullptr) {
        return RfRecentFormatResult::kInvalidArgument;
    }
    if (size < kHeaderSize + kNextIdSize + kCrcSize || size > kRfRecentMaxRecordSize ||
        std::memcmp(record, kMagic, sizeof(kMagic)) != 0) {
        return RfRecentFormatResult::kInvalidRecord;
    }
    const uint32_t stored_crc = read_u32(record + size - kCrcSize);
    if (stored_crc != crc32(record, size - kCrcSize)) {
        return RfRecentFormatResult::kInvalidCrc;
    }
    if (record[4] != kFormatVersion) {
        return RfRecentFormatResult::kInvalidVersion;
    }
    const std::size_t count = record[5];
    const std::size_t payload_size = read_u16(record + 6);
    if (count > kRfRecentSignalCapacity || payload_size != kNextIdSize + count * kEntrySize ||
        size != kHeaderSize + payload_size + kCrcSize) {
        return RfRecentFormatResult::kInvalidRecord;
    }

    RfRecentHistory decoded{};
    const uint8_t *payload = record + kHeaderSize;
    decoded.next_id = read_u64(payload);
    decoded.count = count;
    for (std::size_t index = 0; index < count; ++index) {
        const uint8_t *entry = payload + kNextIdSize + index * kEntrySize;
        decoded.entries[index].id = read_u64(entry);
        decoded.entries[index].decoded.code = read_u64(entry + 8);
        decoded.entries[index].decoded.pulse_us = read_u16(entry + 16);
        decoded.entries[index].decoded.bits = entry[18];
        decoded.entries[index].decoded.protocol = entry[19];
        decoded.entries[index].decoded.inverted = entry[20];
    }
    if (!rf_recent_history_is_valid(decoded)) {
        return RfRecentFormatResult::kInvalidRecord;
    }
    *history = decoded;
    return RfRecentFormatResult::kOk;
}

}  // namespace rfbridge
