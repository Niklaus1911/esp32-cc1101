#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

constexpr char kOtaUploadContentType[] = "application/octet-stream";
constexpr std::size_t kOtaSha256Size = 32;
constexpr std::size_t kOtaSha256HexCapacity = kOtaSha256Size * 2U + 1U;

enum class OtaImageState : uint8_t {
    kUnknown,
    kUndefined,
    kNew,
    kPendingVerify,
    kValid,
    kInvalid,
    kAborted,
};

bool ota_http_upload_request_is_valid(const char *content_type, int64_t content_length,
                                      std::size_t partition_size, std::size_t minimum_image_size);
bool ota_project_name_is_compatible(const char *candidate_project, const char *running_project);
bool format_ota_sha256(const uint8_t *sha256, std::size_t size, char *output,
                       std::size_t capacity);
OtaImageState ota_image_state_from_raw(uint32_t state);
const char *ota_image_state_name(OtaImageState state);

}  // namespace rfbridge
