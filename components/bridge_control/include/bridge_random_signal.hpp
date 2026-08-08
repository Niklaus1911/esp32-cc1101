#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_storage_format.hpp"

namespace rfbridge {

constexpr uint8_t kRandomSignalBits = 24;
constexpr uint8_t kRandomSignalProtocol = 1;
constexpr std::size_t kRandomSignalMaximumAttempts = 8;

struct RandomSignalSaveResult {
    RfStorageName name{};
    DecodedSignal decoded{};
};

bool make_random_signal_candidate(uint32_t random_word, RandomSignalSaveResult *candidate);

}  // namespace rfbridge
