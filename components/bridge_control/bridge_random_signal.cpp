#include "bridge_random_signal.hpp"

#include <cstdio>

namespace rfbridge {
namespace {

constexpr uint32_t kRandomSignalCodeMask = (1U << kRandomSignalBits) - 1U;
constexpr int kRandomSignalNameLength = 13;

static_assert(kRandomSignalBits < 32);
static_assert(kRfStorageNameCapacity > kRandomSignalNameLength);

}  // namespace

bool make_random_signal_candidate(uint32_t random_word, RandomSignalSaveResult *candidate)
{
    if (candidate == nullptr) {
        return false;
    }
    RandomSignalSaveResult generated{};
    if (!make_decoded_signal(random_word & kRandomSignalCodeMask, kRandomSignalBits,
                             kRandomSignalProtocol, 0, &generated.decoded)) {
        return false;
    }
    const int length = std::snprintf(generated.name.value, sizeof(generated.name.value),
                                     "random_%06X",
                                     static_cast<unsigned>(generated.decoded.code));
    if (length != kRandomSignalNameLength ||
        !rf_storage_name_is_valid(generated.name.value)) {
        return false;
    }
    *candidate = generated;
    return true;
}

}  // namespace rfbridge
