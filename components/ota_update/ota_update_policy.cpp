#include "ota_update_policy.hpp"

#include <cstring>

namespace rfbridge {

bool ota_http_upload_request_is_valid(const char *content_type, int64_t content_length,
                                      std::size_t partition_size, std::size_t minimum_image_size)
{
    return content_type != nullptr && std::strcmp(content_type, kOtaUploadContentType) == 0 &&
           content_length >= 0 && static_cast<uint64_t>(content_length) >= minimum_image_size &&
           static_cast<uint64_t>(content_length) <= partition_size;
}

bool ota_project_name_is_compatible(const char *candidate_project, const char *running_project)
{
    return candidate_project != nullptr && running_project != nullptr && candidate_project[0] != '\0' &&
           std::strcmp(candidate_project, running_project) == 0;
}

bool format_ota_sha256(const uint8_t *sha256, std::size_t size, char *output,
                       std::size_t capacity)
{
    constexpr char kHex[] = "0123456789abcdef";
    if (sha256 == nullptr || size != kOtaSha256Size || output == nullptr ||
        capacity < kOtaSha256HexCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < size; ++index) {
        output[index * 2U] = kHex[sha256[index] >> 4U];
        output[index * 2U + 1U] = kHex[sha256[index] & 0x0fU];
    }
    output[size * 2U] = '\0';
    return true;
}

OtaImageState ota_image_state_from_raw(uint32_t state)
{
    switch (state) {
        case 0U:
            return OtaImageState::kNew;
        case 1U:
            return OtaImageState::kPendingVerify;
        case 2U:
            return OtaImageState::kValid;
        case 3U:
            return OtaImageState::kInvalid;
        case 4U:
            return OtaImageState::kAborted;
        case UINT32_MAX:
            return OtaImageState::kUndefined;
        default:
            return OtaImageState::kUnknown;
    }
}

const char *ota_image_state_name(OtaImageState state)
{
    switch (state) {
        case OtaImageState::kUnknown:
            return "unknown";
        case OtaImageState::kUndefined:
            return "undefined";
        case OtaImageState::kNew:
            return "new";
        case OtaImageState::kPendingVerify:
            return "pending_verify";
        case OtaImageState::kValid:
            return "valid";
        case OtaImageState::kInvalid:
            return "invalid";
        case OtaImageState::kAborted:
            return "aborted";
    }
    return "unknown";
}

}  // namespace rfbridge
