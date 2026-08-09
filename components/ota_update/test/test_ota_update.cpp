#include "ota_update_policy.hpp"
#include "unity.h"

TEST_CASE("OTA upload request policy is strict", "[ota_update]")
{
    TEST_ASSERT_TRUE(rfbridge::ota_http_upload_request_is_valid(
        "application/octet-stream", 4096, 8192, 512));
    TEST_ASSERT_FALSE(rfbridge::ota_http_upload_request_is_valid(
        "application/json", 4096, 8192, 512));
    TEST_ASSERT_FALSE(rfbridge::ota_http_upload_request_is_valid(
        "application/octet-stream", 511, 8192, 512));
    TEST_ASSERT_FALSE(rfbridge::ota_http_upload_request_is_valid(
        "application/octet-stream", 8193, 8192, 512));
    TEST_ASSERT_FALSE(rfbridge::ota_http_upload_request_is_valid(
        "application/octet-stream", -1, 8192, 512));
}

TEST_CASE("OTA project identity must match exactly", "[ota_update]")
{
    TEST_ASSERT_TRUE(rfbridge::ota_project_name_is_compatible("esp32-cc1101", "esp32-cc1101"));
    TEST_ASSERT_FALSE(rfbridge::ota_project_name_is_compatible("other", "esp32-cc1101"));
    TEST_ASSERT_FALSE(rfbridge::ota_project_name_is_compatible("", "esp32-cc1101"));
    TEST_ASSERT_FALSE(rfbridge::ota_project_name_is_compatible(nullptr, "esp32-cc1101"));
}

TEST_CASE("OTA image identity formatting and state names are stable", "[ota_update]")
{
    uint8_t sha256[rfbridge::kOtaSha256Size]{};
    for (std::size_t index = 0; index < sizeof(sha256); ++index) {
        sha256[index] = static_cast<uint8_t>(0xffU - index);
    }
    char digest[rfbridge::kOtaSha256HexCapacity]{};
    TEST_ASSERT_TRUE(rfbridge::format_ota_sha256(sha256, sizeof(sha256), digest,
                                                 sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING(
        "fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0", digest);
    TEST_ASSERT_EQUAL_STRING(
        "undefined",
        rfbridge::ota_image_state_name(rfbridge::ota_image_state_from_raw(UINT32_MAX)));
    TEST_ASSERT_EQUAL_STRING(
        "pending_verify",
        rfbridge::ota_image_state_name(rfbridge::ota_image_state_from_raw(1)));
    TEST_ASSERT_EQUAL_STRING(
        "valid", rfbridge::ota_image_state_name(rfbridge::ota_image_state_from_raw(2)));
    TEST_ASSERT_EQUAL_STRING(
        "new", rfbridge::ota_image_state_name(rfbridge::ota_image_state_from_raw(0)));
    TEST_ASSERT_EQUAL_STRING(
        "invalid", rfbridge::ota_image_state_name(rfbridge::ota_image_state_from_raw(3)));
    TEST_ASSERT_EQUAL_STRING(
        "aborted", rfbridge::ota_image_state_name(rfbridge::ota_image_state_from_raw(4)));
    TEST_ASSERT_EQUAL_STRING(
        "unknown", rfbridge::ota_image_state_name(rfbridge::ota_image_state_from_raw(99)));
}
