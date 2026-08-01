#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "esp_random.h"
#include "rf_storage.hpp"
#include "unity.h"

namespace {

uint32_t test_crc32(const uint8_t *data, std::size_t size)
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

void write_u32(uint8_t *output, uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void rewrite_crc(uint8_t *record, std::size_t size)
{
    write_u32(record + size - 4U, test_crc32(record, size - 4U));
}

rfbridge::RfStoredSignal decoded_signal()
{
    rfbridge::RfStoredSignal signal{};
    signal.encoding = rfbridge::RfStoredEncoding::kDecoded;
    signal.decoded.code = 0xA88142;
    signal.decoded.pulse_us = 386;
    signal.decoded.bits = 24;
    signal.decoded.protocol = 1;
    return signal;
}

}  // namespace

TEST_CASE("RF storage names enforce NVS-safe rules", "[rf_storage]")
{
    TEST_ASSERT_TRUE(rfbridge::rf_storage_name_is_valid("Remote_1"));
    TEST_ASSERT_TRUE(rfbridge::rf_storage_name_is_valid("a-2345678901234"));
    TEST_ASSERT_FALSE(rfbridge::rf_storage_name_is_valid(nullptr));
    TEST_ASSERT_FALSE(rfbridge::rf_storage_name_is_valid(""));
    TEST_ASSERT_FALSE(rfbridge::rf_storage_name_is_valid("1remote"));
    TEST_ASSERT_FALSE(rfbridge::rf_storage_name_is_valid("remote space"));
    TEST_ASSERT_FALSE(rfbridge::rf_storage_name_is_valid("a-23456789012345"));
    TEST_ASSERT_FALSE(rfbridge::rf_storage_name_is_valid("list"));
}

TEST_CASE("decoded RF storage record round trips", "[rf_storage]")
{
    const rfbridge::RfStoredSignal source = decoded_signal();
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    TEST_ASSERT_EQUAL_UINT32(25, size);

    rfbridge::RfStoredSignal loaded{};
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStoredEncoding::kDecoded),
                      static_cast<int>(loaded.encoding));
    TEST_ASSERT_EQUAL_UINT64(source.decoded.code, loaded.decoded.code);
    TEST_ASSERT_EQUAL_UINT16(source.decoded.pulse_us, loaded.decoded.pulse_us);
    TEST_ASSERT_EQUAL_UINT8(source.decoded.bits, loaded.decoded.bits);
    TEST_ASSERT_EQUAL_UINT8(source.decoded.protocol, loaded.decoded.protocol);
    TEST_ASSERT_EQUAL_UINT8(rfbridge::rf_protocol(source.decoded.protocol)->inverted, loaded.decoded.inverted);
}

TEST_CASE("maximum raw RF storage record round trips", "[rf_storage]")
{
    rfbridge::RfStoredSignal source{};
    source.encoding = rfbridge::RfStoredEncoding::kRaw;
    source.raw.count = rfbridge::kMaxRawPulses;
    source.raw.start_level = 1;
    for (std::size_t index = 0; index < source.raw.count; ++index) {
        source.raw.durations_us[index] = static_cast<uint16_t>(100U + index);
    }
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    TEST_ASSERT_EQUAL_UINT32(rfbridge::kRfStorageMaxRecordSize, size);

    rfbridge::RfStoredSignal loaded{};
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStoredEncoding::kRaw), static_cast<int>(loaded.encoding));
    TEST_ASSERT_EQUAL_UINT16(source.raw.count, loaded.raw.count);
    TEST_ASSERT_EQUAL_UINT8(source.raw.start_level, loaded.raw.start_level);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(source.raw.durations_us, loaded.raw.durations_us, source.raw.count);
}

TEST_CASE("RF storage encoder rejects invalid signals and short buffers", "[rf_storage]")
{
    rfbridge::RfStoredSignal signal = decoded_signal();
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t size = 0;
    signal.decoded.bits = 3;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidArgument),
                      static_cast<int>(rfbridge::encode_rf_storage_record(signal, record, sizeof(record), &size)));
    signal = decoded_signal();
    signal.decoded.inverted = 1;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidArgument),
                      static_cast<int>(rfbridge::encode_rf_storage_record(signal, record, sizeof(record), &size)));
    signal = decoded_signal();
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kBufferTooSmall),
                      static_cast<int>(rfbridge::encode_rf_storage_record(signal, record, 23, &size)));
}

TEST_CASE("RF storage decoder rejects bad magic CRC and version", "[rf_storage]")
{
    const rfbridge::RfStoredSignal source = decoded_signal();
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    rfbridge::RfStoredSignal loaded{};

    record[0] ^= 1U;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));
    record[0] ^= 1U;
    record[8] ^= 1U;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidCrc),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));
    record[8] ^= 1U;
    record[4] = 2;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidVersion),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));
}

TEST_CASE("RF storage decoder rejects malformed payload metadata", "[rf_storage]")
{
    const rfbridge::RfStoredSignal source = decoded_signal();
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    rfbridge::RfStoredSignal loaded{};

    record[5] = 99;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));

    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    record[6] = 12;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));

    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    record[18] = 3;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));
}

TEST_CASE("RF storage decoder rejects malformed raw and trailing data", "[rf_storage]")
{
    rfbridge::RfStoredSignal source{};
    source.encoding = rfbridge::RfStoredEncoding::kRaw;
    source.raw.count = 8;
    source.raw.start_level = 0;
    for (std::size_t index = 0; index < source.raw.count; ++index) {
        source.raw.durations_us[index] = static_cast<uint16_t>(300U + index);
    }
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    rfbridge::RfStoredSignal loaded{};

    record[8] = 10;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));

    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(source, record, sizeof(record), &size)));
    record[11] = 99;
    record[12] = 0;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));

    const rfbridge::RfStoredSignal decoded = decoded_signal();
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_storage_record(decoded, record, sizeof(record), &size)));
    std::memmove(record + size - 3U, record + size - 4U, 4U);
    record[size - 4U] = 0xAA;
    ++size;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size, &loaded)));
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfStorageFormatResult::kInvalidCrc),
                      static_cast<int>(rfbridge::decode_rf_storage_record(record, size - 1U, &loaded)));
}

TEST_CASE("RF storage NVS backend is create only and forgets one key", "[rf_storage][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_rf_storage());

    char test_name[rfbridge::kRfStorageNameCapacity]{};
    bool exists = true;
    for (int attempt = 0; attempt < 8 && exists; ++attempt) {
        std::snprintf(test_name, sizeof(test_name), "T%08lX", static_cast<unsigned long>(esp_random()));
        TEST_ASSERT_EQUAL(ESP_OK, rfbridge::rf_storage_exists(test_name, &exists));
    }
    TEST_ASSERT_FALSE_MESSAGE(exists, "could not allocate a collision-free temporary NVS test key");

    const rfbridge::RfStoredSignal source = decoded_signal();
    const esp_err_t create_error = rfbridge::rf_storage_create(test_name, source);
    if (create_error != ESP_OK) {
        rfbridge::rf_storage_forget(test_name);
        TEST_ASSERT_EQUAL(ESP_OK, create_error);
        return;
    }

    rfbridge::RfStoredSignal conflicting = source;
    conflicting.decoded.code ^= 1U;
    const esp_err_t duplicate_error = rfbridge::rf_storage_create(test_name, conflicting);
    rfbridge::RfStoredSignal loaded{};
    const esp_err_t load_error = rfbridge::rf_storage_load(test_name, &loaded);
    const bool load_matches = load_error == ESP_OK && loaded.encoding == rfbridge::RfStoredEncoding::kDecoded &&
                              rfbridge::decoded_signals_match(source.decoded, loaded.decoded) &&
                              !rfbridge::decoded_signals_match(conflicting.decoded, loaded.decoded);

    std::size_t count = 0;
    const esp_err_t count_error = rfbridge::rf_storage_list(nullptr, 0, &count);
    rfbridge::RfStorageName names[128]{};
    esp_err_t list_error = ESP_ERR_INVALID_SIZE;
    bool listed = false;
    if (count_error == ESP_OK && count <= std::size(names)) {
        list_error = rfbridge::rf_storage_list(names, std::size(names), &count);
        if (list_error == ESP_OK) {
            for (std::size_t index = 0; index < count; ++index) {
                listed = listed || std::strcmp(names[index].value, test_name) == 0;
            }
        }
    }

    const esp_err_t forget_error = rfbridge::rf_storage_forget(test_name);
    const esp_err_t missing_forget_error = rfbridge::rf_storage_forget(test_name);
    const esp_err_t final_exists_error = rfbridge::rf_storage_exists(test_name, &exists);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, duplicate_error);
    TEST_ASSERT_EQUAL(ESP_OK, load_error);
    TEST_ASSERT_TRUE(load_matches);
    TEST_ASSERT_EQUAL(ESP_OK, count_error);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(std::size(names), count);
    TEST_ASSERT_EQUAL(ESP_OK, list_error);
    TEST_ASSERT_TRUE(listed);
    TEST_ASSERT_EQUAL(ESP_OK, forget_error);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, missing_forget_error);
    TEST_ASSERT_EQUAL(ESP_OK, final_exists_error);
    TEST_ASSERT_FALSE(exists);
}
