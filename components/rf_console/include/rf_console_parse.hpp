#pragma once

#include <cstdint>

namespace rfbridge {

bool parse_unsigned_value(const char *text, uint64_t maximum, uint64_t *value);

}  // namespace rfbridge
