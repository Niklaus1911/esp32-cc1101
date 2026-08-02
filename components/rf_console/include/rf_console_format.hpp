#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

enum class ConsoleStyle : uint8_t {
    kPretty,
    kPlain,
};

enum class ConsoleTone : uint8_t {
    kDefault,
    kMuted,
    kInfo,
    kSuccess,
    kAction,
    kWarning,
    kError,
};

const char *console_style_name(ConsoleStyle style);
bool parse_console_style(const char *text, ConsoleStyle *style);
uint8_t ota_progress_percent(uint32_t received, uint32_t total);
bool console_wrap_chunk(const char *text, std::size_t maximum_width,
                        std::size_t *chunk_length, std::size_t *consumed_length);
bool format_console_wifi_disconnected_message(ConsoleStyle style, const char *ssid,
                                               int32_t reason, const char *plain_state_name,
                                               char *output, std::size_t capacity);

bool format_console_tagged_line(ConsoleStyle style, ConsoleTone tone, const char *tag,
                                const char *plain_line, const char *pretty_message,
                                char *output, std::size_t capacity);
bool decorate_console_message(ConsoleStyle style, ConsoleTone tone, const char *tag,
                              const char *plain_prefix, char *message, std::size_t capacity);
bool format_ota_progress_line(ConsoleStyle style, uint32_t received, uint32_t total,
                              char *output, std::size_t capacity);
bool format_console_section_header(const char *title, char *output, std::size_t capacity);
bool format_console_section_footer(char *output, std::size_t capacity);
bool format_console_dashboard_row(const char *left_label, const char *left_value,
                                  ConsoleTone left_tone, const char *right_label,
                                  const char *right_value, ConsoleTone right_tone,
                                  char *output, std::size_t capacity);
bool format_console_dashboard_value(const char *label, const char *value, ConsoleTone tone,
                                    char *output, std::size_t capacity);

}  // namespace rfbridge
