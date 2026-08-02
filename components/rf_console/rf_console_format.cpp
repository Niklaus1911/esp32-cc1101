#include "rf_console_format.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace rfbridge {
namespace {

constexpr char kAnsiReset[] = "\x1b[0m";
constexpr char kAnsiMuted[] = "\x1b[2;37m";
constexpr char kAnsiInfo[] = "\x1b[1;36m";
constexpr char kAnsiSuccess[] = "\x1b[1;32m";
constexpr char kAnsiAction[] = "\x1b[1;35m";
constexpr char kAnsiWarning[] = "\x1b[1;33m";
constexpr char kAnsiError[] = "\x1b[1;31m";
constexpr std::size_t kDashboardInnerWidth = 76;
constexpr std::size_t kDashboardLabelWidth = 13;
constexpr std::size_t kDashboardValueWidth =
    (kDashboardInnerWidth - (2U * kDashboardLabelWidth) - 6U) / 2U;
constexpr std::size_t kDashboardFullValueWidth =
    kDashboardInnerWidth - kDashboardLabelWidth - 3U;
constexpr std::size_t kProgressBarWidth = 20;
static_assert(2U + kDashboardLabelWidth + 1U + kDashboardValueWidth + 2U +
                  kDashboardLabelWidth + 1U + kDashboardValueWidth + 2U ==
              kDashboardInnerWidth + 2U);
static_assert(2U + kDashboardLabelWidth + 1U + kDashboardFullValueWidth + 2U ==
              kDashboardInnerWidth + 2U);

const char *tone_sequence(ConsoleTone tone)
{
    switch (tone) {
        case ConsoleTone::kDefault:
            return "";
        case ConsoleTone::kMuted:
            return kAnsiMuted;
        case ConsoleTone::kInfo:
            return kAnsiInfo;
        case ConsoleTone::kSuccess:
            return kAnsiSuccess;
        case ConsoleTone::kAction:
            return kAnsiAction;
        case ConsoleTone::kWarning:
            return kAnsiWarning;
        case ConsoleTone::kError:
            return kAnsiError;
    }
    return "";
}

bool write_repeated(char value, std::size_t count, char *output, std::size_t capacity)
{
    if (output == nullptr || capacity <= count) {
        return false;
    }
    std::memset(output, value, count);
    output[count] = '\0';
    return true;
}

bool fits(const char *text, std::size_t width)
{
    return text != nullptr && std::strlen(text) <= width;
}

bool is_blank(const char *text)
{
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (std::isspace(static_cast<unsigned char>(*cursor)) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char *console_style_name(ConsoleStyle style)
{
    switch (style) {
        case ConsoleStyle::kPretty:
            return "pretty";
        case ConsoleStyle::kPlain:
            return "plain";
    }
    return "invalid";
}

bool parse_console_style(const char *text, ConsoleStyle *style)
{
    if (text == nullptr || style == nullptr) {
        return false;
    }
    if (std::strcmp(text, "pretty") == 0) {
        *style = ConsoleStyle::kPretty;
        return true;
    }
    if (std::strcmp(text, "plain") == 0) {
        *style = ConsoleStyle::kPlain;
        return true;
    }
    return false;
}

uint8_t ota_progress_percent(uint32_t received, uint32_t total)
{
    if (total == 0) {
        return 0;
    }
    const uint64_t bounded = received > total ? total : received;
    return static_cast<uint8_t>((bounded * 100U) / total);
}

bool console_wrap_chunk(const char *text, std::size_t maximum_width,
                        std::size_t *chunk_length, std::size_t *consumed_length)
{
    if (text == nullptr || maximum_width == 0 || chunk_length == nullptr ||
        consumed_length == nullptr) {
        return false;
    }
    const std::size_t remaining = std::strlen(text);
    *chunk_length = remaining;
    *consumed_length = remaining;
    if (remaining <= maximum_width) {
        return true;
    }
    if (text[maximum_width] == ' ') {
        *chunk_length = maximum_width;
        *consumed_length = maximum_width + 1U;
        return true;
    }
    *chunk_length = maximum_width;
    *consumed_length = maximum_width;
    for (std::size_t index = maximum_width; index > 0; --index) {
        if (text[index - 1U] == ' ') {
            *chunk_length = index - 1U;
            *consumed_length = index;
            break;
        }
        if (text[index - 1U] == ',') {
            *chunk_length = index;
            *consumed_length = index;
            break;
        }
    }
    return true;
}

bool format_console_wifi_disconnected_message(ConsoleStyle style, const char *ssid,
                                               int32_t reason, const char *plain_state_name,
                                               char *output, std::size_t capacity)
{
    if (ssid == nullptr || plain_state_name == nullptr || output == nullptr || capacity == 0) {
        return false;
    }
    const int written = style == ConsoleStyle::kPlain
                            ? std::snprintf(output, capacity,
                                            "WIFI DISCONNECTED ssid=%s reason=%ld state=%s", ssid,
                                            static_cast<long>(reason), plain_state_name)
                            : std::snprintf(output, capacity, "Disconnected from %s | reason %ld",
                                            ssid, static_cast<long>(reason));
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool format_console_tagged_line(ConsoleStyle style, ConsoleTone tone, const char *tag,
                                const char *plain_line, const char *pretty_message,
                                char *output, std::size_t capacity)
{
    if (!fits(tag, 5U) || plain_line == nullptr || pretty_message == nullptr || output == nullptr ||
        capacity == 0) {
        return false;
    }
    int written = -1;
    if (style == ConsoleStyle::kPlain) {
        written = std::snprintf(output, capacity, "%s", plain_line);
    } else if (is_blank(tag)) {
        written = std::snprintf(output, capacity, "      %s|%s %s%s", tone_sequence(tone),
                                kAnsiReset, pretty_message, kAnsiReset);
    } else {
        written = std::snprintf(output, capacity, "%s[%-5.5s]%s %s%s", tone_sequence(tone), tag,
                                kAnsiReset, pretty_message, kAnsiReset);
    }
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool decorate_console_message(ConsoleStyle style, ConsoleTone tone, const char *tag,
                              const char *plain_prefix, char *message, std::size_t capacity)
{
    if (!fits(tag, 5U) || plain_prefix == nullptr || message == nullptr || capacity == 0) {
        return false;
    }
    char prefix[48]{};
    const int prefix_written = style == ConsoleStyle::kPlain
                                   ? std::snprintf(prefix, sizeof(prefix), "%s", plain_prefix)
                                   : std::snprintf(prefix, sizeof(prefix), "%s[%-5.5s]%s ",
                                                   tone_sequence(tone), tag, kAnsiReset);
    if (prefix_written < 0 || static_cast<std::size_t>(prefix_written) >= sizeof(prefix)) {
        return false;
    }
    const std::size_t prefix_length = static_cast<std::size_t>(prefix_written);
    const std::size_t message_length = std::strlen(message);
    const std::size_t suffix_length = style == ConsoleStyle::kPretty ? sizeof(kAnsiReset) - 1U : 0U;
    if (prefix_length + message_length + suffix_length >= capacity) {
        return false;
    }
    std::memmove(message + prefix_length, message, message_length + 1U);
    std::memcpy(message, prefix, prefix_length);
    if (suffix_length != 0) {
        std::memcpy(message + prefix_length + message_length, kAnsiReset, suffix_length + 1U);
    }
    return true;
}

bool format_ota_progress_line(ConsoleStyle style, uint32_t received, uint32_t total,
                              char *output, std::size_t capacity)
{
    if (output == nullptr || capacity == 0) {
        return false;
    }
    if (style == ConsoleStyle::kPlain) {
        const int written = std::snprintf(output, capacity, "OTA PROGRESS bytes=%lu total=%lu",
                                          static_cast<unsigned long>(received),
                                          static_cast<unsigned long>(total));
        return written >= 0 && static_cast<std::size_t>(written) < capacity;
    }

    const uint8_t percent = ota_progress_percent(received, total);
    const std::size_t filled = static_cast<std::size_t>(percent) * kProgressBarWidth / 100U;
    char bar[kProgressBarWidth + 1U]{};
    std::memset(bar, '#', filled);
    std::memset(bar + filled, '-', kProgressBarWidth - filled);
    const uint32_t received_kib = received == 0
                                      ? 0
                                      : static_cast<uint32_t>((static_cast<uint64_t>(received) + 1023U) /
                                                              1024U);
    const uint32_t total_kib = total == 0
                                   ? 0
                                   : static_cast<uint32_t>((static_cast<uint64_t>(total) + 1023U) /
                                                           1024U);
    const int written = std::snprintf(output, capacity,
                                      "%s[OTA  ]%s %3u%% [%s] %lu KiB / %lu KiB%s",
                                      kAnsiWarning, kAnsiReset, percent, bar,
                                      static_cast<unsigned long>(received_kib),
                                      static_cast<unsigned long>(total_kib), kAnsiReset);
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool format_console_section_header(const char *title, char *output, std::size_t capacity)
{
    if (title == nullptr || output == nullptr || capacity == 0) {
        return false;
    }
    const std::size_t title_length = std::strlen(title);
    if (title_length + 5U > kDashboardInnerWidth) {
        return false;
    }
    char border[kDashboardInnerWidth + 1U]{};
    if (!write_repeated('-', kDashboardInnerWidth, border, sizeof(border))) {
        return false;
    }
    border[2] = ' ';
    std::memcpy(border + 3U, title, title_length);
    border[title_length + 3U] = ' ';
    const int written = std::snprintf(output, capacity, "%s+%s+%s", kAnsiInfo, border, kAnsiReset);
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool format_console_section_footer(char *output, std::size_t capacity)
{
    if (output == nullptr || capacity == 0) {
        return false;
    }
    char border[kDashboardInnerWidth + 1U]{};
    if (!write_repeated('-', kDashboardInnerWidth, border, sizeof(border))) {
        return false;
    }
    const int written = std::snprintf(output, capacity, "%s+%s+%s", kAnsiMuted, border, kAnsiReset);
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool format_console_dashboard_row(const char *left_label, const char *left_value,
                                  ConsoleTone left_tone, const char *right_label,
                                  const char *right_value, ConsoleTone right_tone,
                                  char *output, std::size_t capacity)
{
    if (!fits(left_label, kDashboardLabelWidth) || !fits(left_value, kDashboardValueWidth) ||
        !fits(right_label, kDashboardLabelWidth) || !fits(right_value, kDashboardValueWidth) ||
        output == nullptr || capacity == 0) {
        return false;
    }
    const int written = std::snprintf(
        output, capacity,
        "| %s%-*s%s %s%-*s%s  %s%-*s%s %s%-*s%s |",
        kAnsiMuted, static_cast<int>(kDashboardLabelWidth), left_label, kAnsiReset,
        tone_sequence(left_tone), static_cast<int>(kDashboardValueWidth), left_value, kAnsiReset,
        kAnsiMuted, static_cast<int>(kDashboardLabelWidth), right_label, kAnsiReset,
        tone_sequence(right_tone), static_cast<int>(kDashboardValueWidth), right_value, kAnsiReset);
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool format_console_dashboard_value(const char *label, const char *value, ConsoleTone tone,
                                    char *output, std::size_t capacity)
{
    if (!fits(label, kDashboardLabelWidth) || !fits(value, kDashboardFullValueWidth) ||
        output == nullptr || capacity == 0) {
        return false;
    }
    const int written = std::snprintf(
        output, capacity, "| %s%-*s%s %s%-*s%s |", kAnsiMuted,
        static_cast<int>(kDashboardLabelWidth), label, kAnsiReset, tone_sequence(tone),
        static_cast<int>(kDashboardFullValueWidth), value, kAnsiReset);
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

}  // namespace rfbridge
