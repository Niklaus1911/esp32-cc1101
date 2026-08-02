#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "esp_random.h"
#include "nvs.h"
#include "../private_include/rf_storage_rule_backend.hpp"
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

esp_err_t accept_rule_validator(const rfbridge::RfStoredSignal &, const rfbridge::RfStoredSignal &, void *)
{
    return ESP_OK;
}

esp_err_t reject_rule_validator(const rfbridge::RfStoredSignal &, const rfbridge::RfStoredSignal &, void *)
{
    return ESP_ERR_INVALID_STATE;
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


TEST_CASE("RF automation rule record matches golden bytes and round trips", "[rf_storage][rf_automation]")
{
    rfbridge::RfStoredRule rule{};
    std::strcpy(rule.target_name, "A");
    rule.repeats = 8;
    rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    uint8_t record[rfbridge::kRfStorageMaxRuleRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size)));
    constexpr uint8_t golden[] = {0x52, 0x46, 0x52, 0x4C, 0x01, 0x00, 0x07, 0x00, 0x01, 0x08,
                                  0xE8, 0x03, 0x00, 0x00, 0x41, 0x6E, 0xFB, 0x09, 0xF0};
    TEST_ASSERT_EQUAL_UINT32(sizeof(golden), size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(golden, record, sizeof(golden));

    rfbridge::RfStoredRule loaded{};
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kOk),
                      static_cast<int>(rfbridge::decode_rf_rule_record(record, size, &loaded)));
    TEST_ASSERT_EQUAL_STRING("A", loaded.target_name);
    TEST_ASSERT_EQUAL_UINT8(8, loaded.repeats);
    TEST_ASSERT_EQUAL_UINT32(rfbridge::kRfAutomationCooldownMs, loaded.cooldown_ms);
}

TEST_CASE("RF automation rule record rejects malformed fields", "[rf_storage][rf_automation]")
{
    rfbridge::RfStoredRule rule{};
    std::strcpy(rule.target_name, "target");
    rule.repeats = 8;
    rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    uint8_t record[rfbridge::kRfStorageMaxRuleRecordSize]{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size)));
    rfbridge::RfStoredRule loaded{};

    record[8] = 0;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_rule_record(record, size, &loaded)));

    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size)));
    record[9] = 21;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_rule_record(record, size, &loaded)));

    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size)));
    record[15] = 0;
    rewrite_crc(record, size);
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kInvalidRecord),
                      static_cast<int>(rfbridge::decode_rf_rule_record(record, size, &loaded)));

    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kOk),
                      static_cast<int>(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size)));
    record[10] ^= 1U;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kInvalidCrc),
                      static_cast<int>(rfbridge::decode_rf_rule_record(record, size, &loaded)));

    rule.repeats = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(rfbridge::RfRuleFormatResult::kInvalidArgument),
                      static_cast<int>(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size)));
}


TEST_CASE("RF automation rule storage preserves references and shared targets", "[rf_storage][rf_automation][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_rf_storage());

    char trigger_one[rfbridge::kRfStorageNameCapacity]{};
    char trigger_two[rfbridge::kRfStorageNameCapacity]{};
    char target[rfbridge::kRfStorageNameCapacity]{};
    auto allocate_name = [](char *name) {
        bool exists = true;
        for (int attempt = 0; attempt < 8 && exists; ++attempt) {
            std::snprintf(name, rfbridge::kRfStorageNameCapacity, "R%08lX",
                          static_cast<unsigned long>(esp_random()));
            if (rfbridge::rf_storage_exists(name, &exists) != ESP_OK) {
                return false;
            }
        }
        return !exists;
    };
    TEST_ASSERT_TRUE(allocate_name(trigger_one));
    TEST_ASSERT_TRUE(allocate_name(trigger_two));
    TEST_ASSERT_TRUE(allocate_name(target));

    rfbridge::RfStoredSignal first = decoded_signal();
    rfbridge::RfStoredSignal second = first;
    rfbridge::RfStoredSignal action = first;
    first.decoded.code = 0xA88140;
    second.decoded.code = 0xA88141;
    action.decoded.code = 0xA88142;

    const esp_err_t create_first = rfbridge::rf_storage_create(trigger_one, first);
    const esp_err_t create_second = rfbridge::rf_storage_create(trigger_two, second);
    const esp_err_t create_target = rfbridge::rf_storage_create(target, action);

    rfbridge::RfStoredRule rule{};
    std::strncpy(rule.target_name, target, sizeof(rule.target_name) - 1U);
    rule.repeats = 8;
    rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    const esp_err_t rejected_rule = rfbridge::rf_storage_rule_create_validated(
        trigger_one, rule, reject_rule_validator, nullptr, nullptr, nullptr);
    rfbridge::RfStoredRule rejected_load{};
    const esp_err_t rejected_load_error = rfbridge::rf_storage_rule_load(trigger_one, &rejected_load);
    const esp_err_t create_rule_one = rfbridge::rf_storage_rule_create_validated(trigger_one, rule, accept_rule_validator, nullptr, nullptr, nullptr);
    rfbridge::RfStoredRule conflicting = rule;
    std::strncpy(conflicting.target_name, trigger_two, sizeof(conflicting.target_name) - 1U);
    const esp_err_t duplicate_trigger = rfbridge::rf_storage_rule_create_validated(trigger_one, conflicting, accept_rule_validator, nullptr, nullptr, nullptr);
    const esp_err_t create_rule_two = rfbridge::rf_storage_rule_create_validated(trigger_two, rule, accept_rule_validator, nullptr, nullptr, nullptr);

    bool previous_enabled = true;
    const esp_err_t get_previous_enabled = rfbridge::rf_storage_rule_enabled_get(&previous_enabled);
    uint8_t previous_log_mode = 1;
    bool previous_log_mode_present = false;
    nvs_handle_t previous_meta_handle = 0;
    esp_err_t inspect_previous_log_mode = nvs_open("rf_rule_meta", NVS_READONLY, &previous_meta_handle);
    if (inspect_previous_log_mode == ESP_OK) {
        const esp_err_t raw_error = nvs_get_u8(previous_meta_handle, "log_mode", &previous_log_mode);
        previous_log_mode_present = raw_error == ESP_OK;
        inspect_previous_log_mode = raw_error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : raw_error;
        nvs_close(previous_meta_handle);
    }
    const esp_err_t get_previous_log_mode = rfbridge::rf_storage_rule_log_mode_get(&previous_log_mode);
    nvs_handle_t meta_handle = 0;
    esp_err_t malformed_meta_write = nvs_open("rf_rule_meta", NVS_READWRITE, &meta_handle);
    if (malformed_meta_write == ESP_OK) {
        const esp_err_t erase_error = nvs_erase_key(meta_handle, "enabled");
        malformed_meta_write = erase_error == ESP_OK || erase_error == ESP_ERR_NVS_NOT_FOUND
                                   ? ESP_OK
                                   : erase_error;
    }
    if (malformed_meta_write == ESP_OK) {
        const esp_err_t erase_error = nvs_erase_key(meta_handle, "log_mode");
        malformed_meta_write = erase_error == ESP_OK || erase_error == ESP_ERR_NVS_NOT_FOUND
                                   ? ESP_OK
                                   : erase_error;
    }
    if (malformed_meta_write == ESP_OK) {
        malformed_meta_write = nvs_set_u32(meta_handle, "enabled", 1);
    }
    if (malformed_meta_write == ESP_OK) {
        malformed_meta_write = nvs_commit(meta_handle);
    }
    if (meta_handle != 0) {
        nvs_close(meta_handle);
    }
    uint8_t default_log_mode = 0;
    const esp_err_t default_log_mode_get = rfbridge::rf_storage_rule_log_mode_get(&default_log_mode);
    nvs_handle_t log_meta_handle = 0;
    esp_err_t malformed_log_write = nvs_open("rf_rule_meta", NVS_READWRITE, &log_meta_handle);
    if (malformed_log_write == ESP_OK) {
        malformed_log_write = nvs_set_u32(log_meta_handle, "log_mode", 2);
    }
    if (malformed_log_write == ESP_OK) {
        malformed_log_write = nvs_commit(log_meta_handle);
    }
    if (log_meta_handle != 0) {
        nvs_close(log_meta_handle);
    }
    uint8_t malformed_log_mode = 0;
    const esp_err_t malformed_log_get = rfbridge::rf_storage_rule_log_mode_get(&malformed_log_mode);
    const esp_err_t malformed_log_get_again = rfbridge::rf_storage_rule_log_mode_get(&malformed_log_mode);
    uint32_t preserved_wrong_type_value = 0;
    nvs_handle_t wrong_type_inspect_handle = 0;
    esp_err_t inspect_wrong_type =
        nvs_open("rf_rule_meta", NVS_READONLY, &wrong_type_inspect_handle);
    if (inspect_wrong_type == ESP_OK) {
        inspect_wrong_type =
            nvs_get_u32(wrong_type_inspect_handle, "log_mode", &preserved_wrong_type_value);
        nvs_close(wrong_type_inspect_handle);
    }
    const esp_err_t repair_log_mode = rfbridge::rf_storage_rule_log_mode_set(2);
    uint8_t verbose_log_mode = 0;
    const esp_err_t get_verbose_log_mode = rfbridge::rf_storage_rule_log_mode_get(&verbose_log_mode);

    nvs_handle_t out_of_range_handle = 0;
    esp_err_t write_out_of_range = nvs_open("rf_rule_meta", NVS_READWRITE, &out_of_range_handle);
    if (write_out_of_range == ESP_OK) {
        write_out_of_range = nvs_set_u8(out_of_range_handle, "log_mode", 3);
    }
    if (write_out_of_range == ESP_OK) {
        write_out_of_range = nvs_commit(out_of_range_handle);
    }
    if (out_of_range_handle != 0) {
        nvs_close(out_of_range_handle);
    }
    uint8_t out_of_range_log_mode = 0;
    const esp_err_t out_of_range_log_get =
        rfbridge::rf_storage_rule_log_mode_get(&out_of_range_log_mode);
    const esp_err_t out_of_range_log_get_again =
        rfbridge::rf_storage_rule_log_mode_get(&out_of_range_log_mode);
    const esp_err_t invalid_log_mode = rfbridge::rf_storage_rule_log_mode_set(3);
    uint8_t preserved_out_of_range_value = 0;
    nvs_handle_t out_of_range_inspect_handle = 0;
    esp_err_t inspect_out_of_range =
        nvs_open("rf_rule_meta", NVS_READONLY, &out_of_range_inspect_handle);
    if (inspect_out_of_range == ESP_OK) {
        inspect_out_of_range =
            nvs_get_u8(out_of_range_inspect_handle, "log_mode", &preserved_out_of_range_value);
        nvs_close(out_of_range_inspect_handle);
    }

    esp_err_t restore_log_mode = ESP_OK;
    if (previous_log_mode_present) {
        restore_log_mode = rfbridge::rf_storage_rule_log_mode_set(previous_log_mode);
    } else {
        nvs_handle_t restore_handle = 0;
        restore_log_mode = nvs_open("rf_rule_meta", NVS_READWRITE, &restore_handle);
        if (restore_log_mode == ESP_OK) {
            const esp_err_t erase_error = nvs_erase_key(restore_handle, "log_mode");
            restore_log_mode = erase_error == ESP_OK || erase_error == ESP_ERR_NVS_NOT_FOUND
                                   ? ESP_OK
                                   : erase_error;
        }
        if (restore_log_mode == ESP_OK) {
            restore_log_mode = nvs_commit(restore_handle);
        }
        if (restore_handle != 0) {
            nvs_close(restore_handle);
        }
    }

    bool malformed_enabled = false;
    const esp_err_t malformed_meta_get = rfbridge::rf_storage_rule_enabled_get(&malformed_enabled);
    const esp_err_t disable_error = rfbridge::rf_storage_rule_enabled_set(false);
    bool disabled = true;
    const esp_err_t get_disabled = rfbridge::rf_storage_rule_enabled_get(&disabled);
    const esp_err_t restore_enabled = rfbridge::rf_storage_rule_enabled_set(previous_enabled);

    const esp_err_t forget_trigger_referenced = rfbridge::rf_storage_forget(trigger_one);
    const esp_err_t forget_target_referenced = rfbridge::rf_storage_forget(target);

    std::size_t rule_count = 0;
    const esp_err_t count_error = rfbridge::rf_storage_rule_list(nullptr, 0, &rule_count);
    rfbridge::RfStorageRuleEntry listed_rules[128]{};
    esp_err_t list_error = ESP_ERR_INVALID_SIZE;
    bool found_first = false;
    bool found_second = false;
    if (count_error == ESP_OK && rule_count <= std::size(listed_rules)) {
        list_error = rfbridge::rf_storage_rule_list(listed_rules, std::size(listed_rules), &rule_count);
        for (std::size_t index = 0; list_error == ESP_OK && index < rule_count; ++index) {
            found_first = found_first || std::strcmp(listed_rules[index].trigger_name, trigger_one) == 0;
            found_second = found_second || std::strcmp(listed_rules[index].trigger_name, trigger_two) == 0;
        }
    }

    const esp_err_t remove_first = rfbridge::rf_storage_rule_remove(trigger_one);
    const esp_err_t remove_second = rfbridge::rf_storage_rule_remove(trigger_two);
    const esp_err_t forget_first = rfbridge::rf_storage_forget(trigger_one);
    const esp_err_t forget_second = rfbridge::rf_storage_forget(trigger_two);
    const esp_err_t forget_target = rfbridge::rf_storage_forget(target);

    TEST_ASSERT_EQUAL(ESP_OK, create_first);
    TEST_ASSERT_EQUAL(ESP_OK, create_second);
    TEST_ASSERT_EQUAL(ESP_OK, create_target);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rejected_rule);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, rejected_load_error);
    TEST_ASSERT_EQUAL(ESP_OK, create_rule_one);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, duplicate_trigger);
    TEST_ASSERT_EQUAL(ESP_OK, create_rule_two);
    TEST_ASSERT_EQUAL(ESP_OK, get_previous_enabled);
    TEST_ASSERT_EQUAL(ESP_OK, inspect_previous_log_mode);
    TEST_ASSERT_EQUAL(ESP_OK, get_previous_log_mode);
    TEST_ASSERT_EQUAL(ESP_OK, malformed_meta_write);
    TEST_ASSERT_EQUAL(ESP_OK, default_log_mode_get);
    TEST_ASSERT_EQUAL_UINT8(1, default_log_mode);
    TEST_ASSERT_EQUAL(ESP_OK, malformed_log_write);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, malformed_log_get);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, malformed_log_get_again);
    TEST_ASSERT_EQUAL(ESP_OK, inspect_wrong_type);
    TEST_ASSERT_EQUAL_UINT32(2, preserved_wrong_type_value);
    TEST_ASSERT_EQUAL(ESP_OK, repair_log_mode);
    TEST_ASSERT_EQUAL(ESP_OK, get_verbose_log_mode);
    TEST_ASSERT_EQUAL_UINT8(2, verbose_log_mode);
    TEST_ASSERT_EQUAL(ESP_OK, write_out_of_range);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, out_of_range_log_get);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, out_of_range_log_get_again);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, invalid_log_mode);
    TEST_ASSERT_EQUAL(ESP_OK, inspect_out_of_range);
    TEST_ASSERT_EQUAL_UINT8(3, preserved_out_of_range_value);
    TEST_ASSERT_EQUAL(ESP_OK, restore_log_mode);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, malformed_meta_get);
    TEST_ASSERT_EQUAL(ESP_OK, disable_error);
    TEST_ASSERT_EQUAL(ESP_OK, get_disabled);
    TEST_ASSERT_FALSE(disabled);
    TEST_ASSERT_EQUAL(ESP_OK, restore_enabled);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, forget_trigger_referenced);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, forget_target_referenced);
    TEST_ASSERT_EQUAL(ESP_OK, count_error);
    TEST_ASSERT_EQUAL(ESP_OK, list_error);
    TEST_ASSERT_TRUE(found_first);
    TEST_ASSERT_TRUE(found_second);
    TEST_ASSERT_EQUAL(ESP_OK, remove_first);
    TEST_ASSERT_EQUAL(ESP_OK, remove_second);
    TEST_ASSERT_EQUAL(ESP_OK, forget_first);
    TEST_ASSERT_EQUAL(ESP_OK, forget_second);
    TEST_ASSERT_EQUAL(ESP_OK, forget_target);
}


TEST_CASE("RF rule administration identifies malformed persisted records", "[rf_storage][rf_automation][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_rf_storage());
    char trigger[rfbridge::kRfStorageNameCapacity]{};
    esp_err_t load_before = ESP_OK;
    for (int attempt = 0; attempt < 8 && load_before != ESP_ERR_NOT_FOUND; ++attempt) {
        std::snprintf(trigger, sizeof(trigger), "M%08lX", static_cast<unsigned long>(esp_random()));
        rfbridge::RfStoredRule unused{};
        load_before = rfbridge::rf_storage_rule_load(trigger, &unused);
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, load_before);

    nvs_handle_t handle = 0;
    const uint8_t malformed[] = {0x52, 0x46, 0x52, 0x4C};
    esp_err_t write_error = nvs_open("rf_rules", NVS_READWRITE, &handle);
    if (write_error == ESP_OK) {
        write_error = nvs_set_blob(handle, trigger, malformed, sizeof(malformed));
    }
    if (write_error == ESP_OK) {
        write_error = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }

    rfbridge::RfStoredRule loaded{};
    const esp_err_t malformed_error =
        write_error == ESP_OK ? rfbridge::rf_storage_rule_load(trigger, &loaded) : write_error;
    std::size_t count = 0;
    const esp_err_t count_error = rfbridge::rf_storage_rule_name_list(nullptr, 0, &count);
    rfbridge::RfStorageName names[128]{};
    esp_err_t list_error = ESP_ERR_INVALID_SIZE;
    bool listed = false;
    if (count_error == ESP_OK && count <= std::size(names)) {
        list_error = rfbridge::rf_storage_rule_name_list(names, std::size(names), &count);
        for (std::size_t index = 0; list_error == ESP_OK && index < count; ++index) {
            listed = listed || std::strcmp(names[index].value, trigger) == 0;
        }
    }
    const esp_err_t remove_error = rfbridge::rf_storage_rule_remove(trigger);

    TEST_ASSERT_EQUAL(ESP_OK, write_error);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, malformed_error);
    TEST_ASSERT_EQUAL(ESP_OK, count_error);
    TEST_ASSERT_EQUAL(ESP_OK, list_error);
    TEST_ASSERT_TRUE(listed);
    TEST_ASSERT_EQUAL(ESP_OK, remove_error);
}


TEST_CASE("RF rule recovery exposes wrong types and malformed trigger keys", "[rf_storage][rf_automation][nvs]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rfbridge::initialize_rf_storage());
    nvs_handle_t handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("rf_rules", NVS_READWRITE, &handle));

    char valid_trigger[rfbridge::kRfStorageNameCapacity]{};
    char invalid_trigger[rfbridge::kRfStorageNameCapacity]{};
    esp_err_t valid_find = ESP_OK;
    esp_err_t invalid_find = ESP_OK;
    for (int attempt = 0; attempt < 8 && valid_find != ESP_ERR_NVS_NOT_FOUND; ++attempt) {
        std::snprintf(valid_trigger, sizeof(valid_trigger), "W%08lX", static_cast<unsigned long>(esp_random()));
        valid_find = nvs_find_key(handle, valid_trigger, nullptr);
    }
    for (int attempt = 0; attempt < 8 && invalid_find != ESP_ERR_NVS_NOT_FOUND; ++attempt) {
        std::snprintf(invalid_trigger, sizeof(invalid_trigger), "1%08lX", static_cast<unsigned long>(esp_random()));
        invalid_find = nvs_find_key(handle, invalid_trigger, nullptr);
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, valid_find);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, invalid_find);

    const esp_err_t create_signal = rfbridge::rf_storage_create(valid_trigger, decoded_signal());
    const uint8_t malformed_blob[] = {0x52, 0x46, 0x52, 0x4C};
    esp_err_t write_error = create_signal;
    if (write_error == ESP_OK) {
        write_error = nvs_set_u8(handle, valid_trigger, 1);
    }
    if (write_error == ESP_OK) {
        write_error = nvs_set_blob(handle, invalid_trigger, malformed_blob, sizeof(malformed_blob));
    }
    if (write_error == ESP_OK) {
        write_error = nvs_commit(handle);
    }
    nvs_close(handle);

    const esp_err_t protected_forget =
        write_error == ESP_OK ? rfbridge::rf_storage_forget(valid_trigger) : write_error;
    std::size_t count = 0;
    const esp_err_t count_error = rfbridge::rf_storage_rule_name_list(nullptr, 0, &count);
    rfbridge::RfStorageName names[128]{};
    esp_err_t names_error = ESP_ERR_INVALID_SIZE;
    bool found_valid = false;
    bool found_invalid = false;
    if (count_error == ESP_OK && count <= std::size(names)) {
        names_error = rfbridge::rf_storage_rule_name_list(names, std::size(names), &count);
        for (std::size_t index = 0; names_error == ESP_OK && index < count; ++index) {
            found_valid = found_valid || std::strcmp(names[index].value, valid_trigger) == 0;
            found_invalid = found_invalid || std::strcmp(names[index].value, invalid_trigger) == 0;
        }
    }
    rfbridge::RfStoredRule loaded{};
    const esp_err_t wrong_type_error = rfbridge::rf_storage_rule_load(valid_trigger, &loaded);
    const esp_err_t invalid_name_error = rfbridge::rf_storage_rule_load(invalid_trigger, &loaded);
    rfbridge::RfStorageRuleEntry strict_rules[128]{};
    std::size_t strict_count = 0;
    esp_err_t strict_error = rfbridge::rf_storage_rule_list(nullptr, 0, &strict_count);
    if (strict_error == ESP_OK && strict_count <= std::size(strict_rules)) {
        strict_error = rfbridge::rf_storage_rule_list(strict_rules, std::size(strict_rules), &strict_count);
    }

    const esp_err_t remove_valid = rfbridge::rf_storage_rule_remove_recovery(valid_trigger);
    const esp_err_t remove_invalid = rfbridge::rf_storage_rule_remove_recovery(invalid_trigger);
    const esp_err_t forget_signal = rfbridge::rf_storage_forget(valid_trigger);

    TEST_ASSERT_EQUAL(ESP_OK, create_signal);
    TEST_ASSERT_EQUAL(ESP_OK, write_error);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, protected_forget);
    TEST_ASSERT_EQUAL(ESP_OK, count_error);
    TEST_ASSERT_EQUAL(ESP_OK, names_error);
    TEST_ASSERT_TRUE(found_valid);
    TEST_ASSERT_TRUE(found_invalid);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, wrong_type_error);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, invalid_name_error);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, strict_error);
    TEST_ASSERT_EQUAL(ESP_OK, remove_valid);
    TEST_ASSERT_EQUAL(ESP_OK, remove_invalid);
    TEST_ASSERT_EQUAL(ESP_OK, forget_signal);
}
