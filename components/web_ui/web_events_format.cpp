#include "web_events_format.hpp"

#include <cstdio>
#include <cstring>

namespace rfbridge {
namespace {

bool event_name_is_valid(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (const char *cursor = name; *cursor != '\0'; ++cursor) {
        if ((*cursor < 'a' || *cursor > 'z') && (*cursor < '0' || *cursor > '9') &&
            *cursor != '_') {
            return false;
        }
    }
    return true;
}

}  // namespace

bool format_web_sse_event(uint64_t sequence, const char *event_name, const char *json,
                          char *output, std::size_t capacity, std::size_t *output_length)
{
    if (!event_name_is_valid(event_name) || json == nullptr || output == nullptr || capacity == 0 ||
        std::strchr(json, '\n') != nullptr || std::strchr(json, '\r') != nullptr) {
        return false;
    }
    const int length = std::snprintf(output, capacity, "id: %llu\nevent: %s\ndata:%s\n\n",
                                     static_cast<unsigned long long>(sequence), event_name, json);
    if (length < 0 || static_cast<std::size_t>(length) >= capacity) {
        return false;
    }
    if (output_length != nullptr) {
        *output_length = static_cast<std::size_t>(length);
    }
    return true;
}

}  // namespace rfbridge
