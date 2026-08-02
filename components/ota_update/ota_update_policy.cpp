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

}  // namespace rfbridge
