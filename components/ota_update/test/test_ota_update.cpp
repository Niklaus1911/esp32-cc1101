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
