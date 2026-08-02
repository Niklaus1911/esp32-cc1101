#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

constexpr char kOtaUploadContentType[] = "application/octet-stream";

bool ota_http_upload_request_is_valid(const char *content_type, int64_t content_length,
                                      std::size_t partition_size, std::size_t minimum_image_size);
bool ota_project_name_is_compatible(const char *candidate_project, const char *running_project);

}  // namespace rfbridge
