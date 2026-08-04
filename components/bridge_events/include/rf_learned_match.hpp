#pragma once

#include <cstdint>

#include "rf_storage_format.hpp"

namespace rfbridge {

enum class LearnedMatchKind : uint8_t {
    kNone,
    kUnique,
    kAmbiguous,
    kUnavailable,
};

struct LearnedMatch {
    LearnedMatchKind kind = LearnedMatchKind::kNone;
    uint16_t count = 0;
    char name[kRfStorageNameCapacity]{};
};

}  // namespace rfbridge
