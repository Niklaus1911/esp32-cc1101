#include "rf_console_parse.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>

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

}  // namespace rfbridge
