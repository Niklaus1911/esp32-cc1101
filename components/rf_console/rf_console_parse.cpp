#include "rf_console_parse.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>

#include "rf_storage_format.hpp"

namespace rfbridge {

bool parse_unsigned_value(const char *text, uint64_t maximum, uint64_t *value)
{
    if (text == nullptr || value == nullptr || text[0] == '\0' || text[0] == '-' ||
        std::isspace(static_cast<unsigned char>(text[0])) != 0) {
        return false;
    }
    const bool hexadecimal = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hexadecimal && text[2] == '\0') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, hexadecimal ? 16 : 10);
    if (errno == ERANGE || end == text || end == nullptr || *end != '\0' || parsed > maximum) {
        return false;
    }
    *value = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_replay_arguments(int argc, const char *const *argv, uint16_t default_repeats,
                            ReplayArguments *arguments)
{
    if (argv == nullptr || arguments == nullptr || default_repeats < 1 || default_repeats > 20) {
        return false;
    }
    ReplayArguments parsed{};
    uint64_t repeats = default_repeats;
    if (argc == 1) {
        parsed.target = ReplayTarget::kRam;
    } else if (argc == 2) {
        if (!parse_unsigned_value(argv[1], 20, &repeats) || repeats < 1) {
            return false;
        }
        parsed.target = ReplayTarget::kRam;
    } else if (argc == 3) {
        if (!rf_storage_name_is_valid(argv[1]) || !parse_unsigned_value(argv[2], 20, &repeats) || repeats < 1) {
            return false;
        }
        parsed.target = ReplayTarget::kNamed;
    } else {
        return false;
    }
    parsed.repeats = static_cast<uint16_t>(repeats);
    *arguments = parsed;
    return true;
}

LearnFrameDisposition classify_learn_frame(int64_t armed_us, int64_t deadline_us, int64_t frame_event_us,
                                           int64_t frame_capture_start_us)
{
    if (armed_us < 0 || deadline_us <= armed_us || frame_event_us < armed_us ||
        frame_capture_start_us < armed_us) {
        return LearnFrameDisposition::kIgnore;
    }
    return frame_event_us < deadline_us ? LearnFrameDisposition::kCapture : LearnFrameDisposition::kTimeout;
}

}  // namespace rfbridge
