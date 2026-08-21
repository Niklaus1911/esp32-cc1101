#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

bool format_web_sse_event(uint64_t sequence, const char *event_name, const char *json,
                          char *output, std::size_t capacity, std::size_t *output_length);

}  // namespace rfbridge
