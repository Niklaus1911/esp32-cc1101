#include "web_form.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace rfbridge {
namespace {

struct Field {
    const char *key = nullptr;
    std::size_t key_length = 0;
    const char *value = nullptr;
    std::size_t value_length = 0;
};

bool slice_equals(const char *value, std::size_t length, const char *expected)
{
    return value != nullptr && expected != nullptr && length == std::strlen(expected) &&
           std::memcmp(value, expected, length) == 0;
}

bool parse_fields(const char *body, std::size_t length, Field *fields, std::size_t capacity,
                  std::size_t *count)
{
    if (body == nullptr || fields == nullptr || count == nullptr || length == 0) {
        return false;
    }
    std::size_t offset = 0;
    std::size_t field_count = 0;
    while (offset < length) {
        const std::size_t key_start = offset;
        while (offset < length && body[offset] != '=' && body[offset] != '&') {
            if (body[offset] == '%' || body[offset] == '+') {
                return false;
            }
            ++offset;
        }
        if (offset == key_start || offset >= length || body[offset] != '=') {
            return false;
        }
        const std::size_t key_length = offset - key_start;
        ++offset;
        const std::size_t value_start = offset;
        while (offset < length && body[offset] != '&') {
            if (body[offset] == '%' || body[offset] == '+') {
                return false;
            }
            ++offset;
        }
        if (field_count >= capacity) {
            return false;
        }
        for (std::size_t index = 0; index < field_count; ++index) {
            if (fields[index].key_length == key_length &&
                std::memcmp(fields[index].key, body + key_start, key_length) == 0) {
                return false;
            }
        }
        fields[field_count++] = {body + key_start, key_length, body + value_start,
                                 offset - value_start};
        if (offset < length) {
            ++offset;
            if (offset == length) {
                return false;
            }
        }
    }
    *count = field_count;
    return field_count > 0;
}

const Field *find_field(const Field *fields, std::size_t count, const char *key)
{
    for (std::size_t index = 0; index < count; ++index) {
        if (slice_equals(fields[index].key, fields[index].key_length, key)) {
            return &fields[index];
        }
    }
    return nullptr;
}

bool exact_fields(const Field *fields, std::size_t count, const char *const *keys,
                  std::size_t key_count)
{
    if (count != key_count) {
        return false;
    }
    for (std::size_t index = 0; index < key_count; ++index) {
        if (find_field(fields, count, keys[index]) == nullptr) {
            return false;
        }
    }
    return true;
}

bool parse_unsigned(const Field &field, uint64_t maximum, uint64_t *output)
{
    if (output == nullptr || field.value_length == 0) {
        return false;
    }
    std::size_t index = 0;
    uint64_t base = 10;
    if (field.value_length >= 2 && field.value[0] == '0' && field.value[1] == 'x') {
        base = 16;
        index = 2;
    }
    if (index == field.value_length) {
        return false;
    }
    uint64_t value = 0;
    for (; index < field.value_length; ++index) {
        const unsigned char character = static_cast<unsigned char>(field.value[index]);
        uint64_t digit = UINT64_MAX;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (base == 16 && character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10U;
        } else if (base == 16 && character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10U;
        }
        if (digit >= base || value > (maximum - digit) / base) {
            return false;
        }
        value = value * base + digit;
    }
    *output = value;
    return true;
}

bool parse_bounded_field(const Field *field, uint64_t minimum, uint64_t maximum,
                         uint64_t *output)
{
    return field != nullptr && parse_unsigned(*field, maximum, output) && *output >= minimum;
}

bool valid_name(const Field &field, char *output, std::size_t capacity)
{
    if (output == nullptr || capacity == 0 || field.value_length == 0 ||
        field.value_length >= capacity) {
        return false;
    }
    for (std::size_t index = 0; index < field.value_length; ++index) {
        const char value = field.value[index];
        const bool alpha = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if ((!alpha && !digit && value != '_' && value != '-') ||
            (index == 0 && !alpha)) {
            return false;
        }
    }
    std::memcpy(output, field.value, field.value_length);
    output[field.value_length] = '\0';
    return true;
}

bool valid_name_or_empty(const Field &field, char *output, std::size_t capacity)
{
    if (field.value_length == 0) {
        if (output != nullptr && capacity > 0) {
            output[0] = '\0';
        }
        return true;
    }
    return valid_name(field, output, capacity);
}

bool host_matches_ipv4_text(const char *host, const char *address, uint16_t port)
{
    if (host == nullptr || address == nullptr) {
        return false;
    }
    if (std::strcmp(host, address) == 0) {
        return port == 80;
    }
    char expected[48]{};
    const int length = std::snprintf(expected, sizeof(expected), "%s:%u", address,
                                     static_cast<unsigned>(port));
    return length > 0 && static_cast<std::size_t>(length) < sizeof(expected) &&
           std::strcmp(host, expected) == 0;
}

bool same_default_port_host(const char *left, const char *right)
{
    if (std::strcmp(left, right) == 0) {
        return true;
    }
    constexpr char kPort[] = ":80";
    const std::size_t left_length = std::strlen(left);
    const std::size_t right_length = std::strlen(right);
    if (left_length > sizeof(kPort) - 1U &&
        std::strcmp(left + left_length - (sizeof(kPort) - 1U), kPort) == 0 &&
        left_length - (sizeof(kPort) - 1U) == right_length &&
        std::memcmp(left, right, right_length) == 0) {
        return true;
    }
    return right_length > sizeof(kPort) - 1U &&
           std::strcmp(right + right_length - (sizeof(kPort) - 1U), kPort) == 0 &&
           right_length - (sizeof(kPort) - 1U) == left_length &&
           std::memcmp(left, right, left_length) == 0;
}

bool strip_expected_port(const char *input, uint16_t expected_port, char *output,
                         std::size_t capacity)
{
    if (input == nullptr || output == nullptr || capacity == 0) {
        return false;
    }
    const std::size_t length = std::strlen(input);
    char port[8]{};
    const int port_length = std::snprintf(port, sizeof(port), ":%u",
                                          static_cast<unsigned>(expected_port));
    if (port_length <= 0 || static_cast<std::size_t>(port_length) >= sizeof(port)) {
        return false;
    }
    std::size_t host_length = length;
    if (length > static_cast<std::size_t>(port_length) &&
        std::memcmp(input + length - port_length, port, port_length) == 0) {
        host_length -= static_cast<std::size_t>(port_length);
    } else if (std::strchr(input, ':') != nullptr) {
        return false;
    }
    if (host_length == 0 || host_length >= capacity) {
        return false;
    }
    for (std::size_t index = 0; index < host_length; ++index) {
        const unsigned char value = static_cast<unsigned char>(input[index]);
        if (value < 0x21U || value > 0x7eU || value == '/' || value == '@' || value == '#') {
            return false;
        }
        output[index] = value >= 'A' && value <= 'Z'
                            ? static_cast<char>(value - 'A' + 'a')
                            : static_cast<char>(value);
    }
    output[host_length] = '\0';
    return true;
}

bool hostname_local_equals(const char *normalized, const char *hostname)
{
    if (normalized == nullptr || hostname == nullptr || hostname[0] == '\0') {
        return false;
    }
    char expected[kWebDeviceHostCapacity]{};
    const int length = std::snprintf(expected, sizeof(expected), "%s.local", hostname);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(expected)) {
        return false;
    }
    for (std::size_t index = 0; expected[index] != '\0'; ++index) {
        const unsigned char value = static_cast<unsigned char>(expected[index]);
        if (value >= 'A' && value <= 'Z') {
            expected[index] = static_cast<char>(value - 'A' + 'a');
        }
    }
    return std::strcmp(normalized, expected) == 0;
}

}  // namespace

bool parse_web_learn_form(const char *body, std::size_t length, char *name,
                          std::size_t name_capacity)
{
    Field fields[2]{};
    std::size_t count = 0;
    constexpr const char *keys[] = {"name"};
    return parse_fields(body, length, fields, std::size(fields), &count) &&
           exact_fields(fields, count, keys, std::size(keys)) &&
           valid_name(*find_field(fields, count, "name"), name, name_capacity);
}

bool parse_web_replay_form(const char *body, std::size_t length, WebReplayForm *output)
{
    if (output == nullptr) {
        return false;
    }
    Field fields[3]{};
    std::size_t count = 0;
    constexpr const char *keys[] = {"name", "repeats"};
    if (!parse_fields(body, length, fields, std::size(fields), &count) ||
        !exact_fields(fields, count, keys, std::size(keys)) ||
        !valid_name_or_empty(*find_field(fields, count, "name"), output->name,
                             sizeof(output->name))) {
        return false;
    }
    uint64_t repeats = 0;
    if (!parse_bounded_field(find_field(fields, count, "repeats"), 1, 20, &repeats)) {
        return false;
    }
    output->latest = output->name[0] == '\0';
    output->repeats = static_cast<uint16_t>(repeats);
    return true;
}

bool parse_web_signal_name_form(const char *body, std::size_t length, char *name,
                                std::size_t name_capacity)
{
    return parse_web_learn_form(body, length, name, name_capacity);
}

bool parse_web_decoded_form(const char *body, std::size_t length, WebDecodedForm *output)
{
    if (output == nullptr) {
        return false;
    }
    Field fields[6]{};
    std::size_t count = 0;
    constexpr const char *keys[] = {"code", "bits", "protocol", "pulse_us", "repeats"};
    if (!parse_fields(body, length, fields, std::size(fields), &count) ||
        !exact_fields(fields, count, keys, std::size(keys))) {
        return false;
    }
    uint64_t code = 0;
    uint64_t bits = 0;
    uint64_t protocol = 0;
    uint64_t pulse = 0;
    uint64_t repeats = 0;
    if (!parse_bounded_field(find_field(fields, count, "code"), 0, UINT64_MAX, &code) ||
        !parse_bounded_field(find_field(fields, count, "bits"), 4, 64, &bits) ||
        !parse_bounded_field(find_field(fields, count, "protocol"), 1, 12, &protocol) ||
        !parse_bounded_field(find_field(fields, count, "pulse_us"), 0, 29000, &pulse) ||
        !parse_bounded_field(find_field(fields, count, "repeats"), 1, 20, &repeats)) {
        return false;
    }
    output->code = code;
    output->bits = static_cast<uint16_t>(bits);
    output->protocol = static_cast<uint8_t>(protocol);
    output->pulse_us = static_cast<uint16_t>(pulse);
    output->repeats = static_cast<uint16_t>(repeats);
    return true;
}

bool parse_web_raw_form(const char *body, std::size_t length, WebRawForm *output)
{
    if (output == nullptr) {
        return false;
    }
    Field fields[4]{};
    std::size_t count = 0;
    constexpr const char *keys[] = {"start_level", "durations", "repeats"};
    if (!parse_fields(body, length, fields, std::size(fields), &count) ||
        !exact_fields(fields, count, keys, std::size(keys))) {
        return false;
    }
    uint64_t start = 0;
    uint64_t repeats = 0;
    if (!parse_bounded_field(find_field(fields, count, "start_level"), 0, 1, &start) ||
        !parse_bounded_field(find_field(fields, count, "repeats"), 1, 20, &repeats)) {
        return false;
    }
    const Field &durations = *find_field(fields, count, "durations");
    std::size_t offset = 0;
    std::size_t pulse_count = 0;
    while (offset < durations.value_length) {
        const std::size_t start_offset = offset;
        while (offset < durations.value_length && durations.value[offset] != ',') {
            ++offset;
        }
        Field pulse{nullptr, 0, durations.value + start_offset, offset - start_offset};
        uint64_t value = 0;
        if (pulse_count >= kWebMaxRawPulses ||
            !parse_bounded_field(&pulse, 100, 29000, &value)) {
            return false;
        }
        output->durations[pulse_count++] = static_cast<uint16_t>(value);
        if (offset < durations.value_length) {
            ++offset;
            if (offset == durations.value_length) {
                return false;
            }
        }
    }
    output->start_level = static_cast<uint8_t>(start);
    output->count = pulse_count;
    output->repeats = static_cast<uint16_t>(repeats);
    return pulse_count >= 8 && (pulse_count % 2U) == 0;
}

bool parse_web_rule_add_form(const char *body, std::size_t length, WebRuleAddForm *output)
{
    if (output == nullptr) {
        return false;
    }
    Field fields[4]{};
    std::size_t count = 0;
    constexpr const char *keys[] = {"trigger", "target", "repeats"};
    if (!parse_fields(body, length, fields, std::size(fields), &count) ||
        !exact_fields(fields, count, keys, std::size(keys)) ||
        !valid_name(*find_field(fields, count, "trigger"), output->trigger,
                    sizeof(output->trigger)) ||
        !valid_name(*find_field(fields, count, "target"), output->target,
                    sizeof(output->target))) {
        return false;
    }
    uint64_t repeats = 0;
    if (!parse_bounded_field(find_field(fields, count, "repeats"), 1, 20, &repeats) ||
        std::strcmp(output->trigger, output->target) == 0) {
        return false;
    }
    output->repeats = static_cast<uint8_t>(repeats);
    return true;
}

bool parse_web_rule_remove_form(const char *body, std::size_t length, WebRuleRemoveForm *output)
{
    return output != nullptr && parse_web_signal_name_form(body, length, output->trigger,
                                                            sizeof(output->trigger));
}

bool parse_web_rule_patch_form(const char *body, std::size_t length, WebRulePatchForm *output)
{
    if (output == nullptr) {
        return false;
    }
    Field fields[2]{};
    std::size_t count = 0;
    if (!parse_fields(body, length, fields, std::size(fields), &count) || count != 1) {
        return false;
    }
    if (const Field *enabled = find_field(fields, count, "enabled"); enabled != nullptr) {
        uint64_t value = 0;
        if (!parse_bounded_field(enabled, 0, 1, &value)) {
            return false;
        }
        output->patch = WebRulePatch::kEnabled;
        output->enabled = value != 0;
        return true;
    }
    const Field *log_mode = find_field(fields, count, "log_mode");
    if (log_mode == nullptr || log_mode->value_length >= sizeof(output->log_mode)) {
        return false;
    }
    std::memcpy(output->log_mode, log_mode->value, log_mode->value_length);
    output->log_mode[log_mode->value_length] = '\0';
    output->patch = WebRulePatch::kLogMode;
    return std::strcmp(output->log_mode, "off") == 0 ||
           std::strcmp(output->log_mode, "actions") == 0 ||
           std::strcmp(output->log_mode, "verbose") == 0;
}

bool parse_web_hardware_form(const char *body, std::size_t length, WebHardwareForm *output)
{
    if (output == nullptr) {
        return false;
    }
    Field fields[2]{};
    std::size_t count = 0;
    constexpr const char *keys[] = {"hardware"};
    if (!parse_fields(body, length, fields, std::size(fields), &count) ||
        !exact_fields(fields, count, keys, std::size(keys))) {
        return false;
    }
    const Field *field = find_field(fields, count, "hardware");
    if (field != nullptr && slice_equals(field->value, field->value_length, "cc1101")) {
        output->hardware = RfHardware::kCc1101;
        return true;
    }
    if (field != nullptr && slice_equals(field->value, field->value_length, "generic")) {
        output->hardware = RfHardware::kGeneric;
        return true;
    }
    return false;
}

bool web_form_content_type_is_valid(const char *content_type)
{
    constexpr char expected[] = "application/x-www-form-urlencoded";
    if (content_type == nullptr || std::strncmp(content_type, expected, sizeof(expected) - 1U) != 0) {
        return false;
    }
    const char suffix = content_type[sizeof(expected) - 1U];
    return suffix == '\0' || suffix == ';';
}

bool web_octet_stream_content_type_is_valid(const char *content_type)
{
    return content_type != nullptr && std::strcmp(content_type, "application/octet-stream") == 0;
}

bool web_host_matches_ipv4(const char *host, uint32_t ipv4, uint16_t expected_port)
{
    if (host == nullptr || ipv4 == 0) {
        return false;
    }
    char address[32]{};
    return format_web_ipv4(ipv4, address, sizeof(address)) &&
           host_matches_ipv4_text(host, address, expected_port);
}

bool web_origin_matches_ipv4(const char *origin, uint32_t ipv4, uint16_t expected_port)
{
    constexpr char prefix[] = "http://";
    if (origin == nullptr || std::strncmp(origin, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    return web_host_matches_ipv4(origin + sizeof(prefix) - 1U, ipv4, expected_port);
}

bool web_origin_matches_host(const char *origin, const char *host)
{
    constexpr char prefix[] = "http://";
    return origin != nullptr && host != nullptr &&
           std::strncmp(origin, prefix, sizeof(prefix) - 1U) == 0 &&
           same_default_port_host(origin + sizeof(prefix) - 1U, host);
}

bool parse_web_device_host(const char *host, uint32_t ipv4, const char *configured_hostname,
                           const char *effective_hostname, uint16_t expected_port,
                           WebDeviceHost *output)
{
    if (output == nullptr || ipv4 == 0) {
        return false;
    }
    char normalized[kWebDeviceHostCapacity]{};
    if (!strip_expected_port(host, expected_port, normalized, sizeof(normalized))) {
        return false;
    }
    char address[32]{};
    if (!format_web_ipv4(ipv4, address, sizeof(address))) {
        return false;
    }
    if (std::strcmp(normalized, address) != 0 &&
        !hostname_local_equals(normalized, configured_hostname) &&
        !hostname_local_equals(normalized, effective_hostname)) {
        return false;
    }
    std::memcpy(output->normalized, normalized, std::strlen(normalized) + 1U);
    return true;
}

bool web_origin_matches_device_host(const char *origin, const WebDeviceHost &host,
                                    uint16_t expected_port)
{
    constexpr char prefix[] = "http://";
    if (origin == nullptr || std::strncmp(origin, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    char normalized[kWebDeviceHostCapacity]{};
    return strip_expected_port(origin + sizeof(prefix) - 1U, expected_port, normalized,
                               sizeof(normalized)) &&
           std::strcmp(normalized, host.normalized) == 0;
}

bool escape_web_html(const char *input, char *output, std::size_t capacity,
                     std::size_t *output_length)
{
    if (input == nullptr || output == nullptr || capacity == 0) {
        return false;
    }
    std::size_t used = 0;
    for (const char *cursor = input; *cursor != '\0'; ++cursor) {
        const char *replacement = cursor;
        std::size_t replacement_length = 1;
        switch (*cursor) {
            case '&': replacement = "&amp;"; replacement_length = 5; break;
            case '<': replacement = "&lt;"; replacement_length = 4; break;
            case '>': replacement = "&gt;"; replacement_length = 4; break;
            case '"': replacement = "&quot;"; replacement_length = 6; break;
            case '\'': replacement = "&#39;"; replacement_length = 5; break;
            default: break;
        }
        if (used + replacement_length >= capacity) {
            output[0] = '\0';
            return false;
        }
        std::memcpy(output + used, replacement, replacement_length);
        used += replacement_length;
    }
    output[used] = '\0';
    if (output_length != nullptr) {
        *output_length = used;
    }
    return true;
}

bool escape_web_json_string(const char *input, char *output, std::size_t capacity,
                            std::size_t *output_length)
{
    if (output_length != nullptr) {
        *output_length = 0;
    }
    if (input == nullptr || output == nullptr || capacity == 0) {
        return false;
    }
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::size_t used = 0;
    for (const char *cursor = input; *cursor != '\0'; ++cursor) {
        const unsigned char character = static_cast<unsigned char>(*cursor);
        char escaped[6]{};
        const char *replacement = cursor;
        std::size_t replacement_length = 1;
        switch (character) {
            case '"': replacement = "\\\""; replacement_length = 2; break;
            case '\\': replacement = "\\\\"; replacement_length = 2; break;
            case '\b': replacement = "\\b"; replacement_length = 2; break;
            case '\f': replacement = "\\f"; replacement_length = 2; break;
            case '\n': replacement = "\\n"; replacement_length = 2; break;
            case '\r': replacement = "\\r"; replacement_length = 2; break;
            case '\t': replacement = "\\t"; replacement_length = 2; break;
            default:
                if (character < 0x20U) {
                    escaped[0] = '\\';
                    escaped[1] = 'u';
                    escaped[2] = '0';
                    escaped[3] = '0';
                    escaped[4] = kHexDigits[character >> 4U];
                    escaped[5] = kHexDigits[character & 0x0fU];
                    replacement = escaped;
                    replacement_length = sizeof(escaped);
                }
                break;
        }
        if (replacement_length >= capacity - used) {
            output[0] = '\0';
            return false;
        }
        std::memcpy(output + used, replacement, replacement_length);
        used += replacement_length;
    }
    output[used] = '\0';
    if (output_length != nullptr) {
        *output_length = used;
    }
    return true;
}

bool format_web_ipv4(uint32_t ipv4, char *output, std::size_t capacity)
{
    if (output == nullptr || capacity == 0) {
        return false;
    }
    const int length = std::snprintf(output, capacity, "%u.%u.%u.%u",
                                     static_cast<unsigned>(ipv4 & 0xffU),
                                     static_cast<unsigned>((ipv4 >> 8U) & 0xffU),
                                     static_cast<unsigned>((ipv4 >> 16U) & 0xffU),
                                     static_cast<unsigned>((ipv4 >> 24U) & 0xffU));
    if (length < 0 || static_cast<std::size_t>(length) >= capacity) {
        output[0] = '\0';
        return false;
    }
    return true;
}

}  // namespace rfbridge
