#pragma once

#include <cstddef>
#include <cstdint>

#include "rf_storage_format.hpp"

namespace rfbridge {

constexpr std::size_t kWebMaxRawPulses = 256;
constexpr std::size_t kWebNameCapacity = 16;
constexpr std::size_t kWebDeviceHostCapacity = 80;

struct WebDeviceHost {
    char normalized[kWebDeviceHostCapacity]{};
};

enum class WebRulePatch : uint8_t {
    kEnabled,
    kLogMode,
};

enum class WebRecentAction : uint8_t {
    kReplay,
    kSave,
    kClear,
};

struct WebReplayForm {
    bool latest = true;
    char name[kWebNameCapacity]{};
    uint16_t repeats = 1;
};

struct WebRecentForm {
    WebRecentAction action = WebRecentAction::kReplay;
    uint64_t id = 0;
    uint16_t repeats = 0;
    char name[kWebNameCapacity]{};
};

struct WebSignalSaveForm {
    char name[kWebNameCapacity]{};
    uint64_t code = 0;
    uint16_t pulse_us = 0;
    uint8_t bits = 0;
    uint8_t protocol = 0;
};

struct WebDecodedForm {
    uint64_t code = 0;
    uint16_t bits = 0;
    uint8_t protocol = 0;
    uint16_t pulse_us = 0;
    uint16_t repeats = 1;
};

struct WebRawForm {
    uint8_t start_level = 0;
    uint16_t durations[kWebMaxRawPulses]{};
    std::size_t count = 0;
    uint16_t repeats = 1;
};

struct WebRuleAddForm {
    char trigger[kWebNameCapacity]{};
    char target[kWebNameCapacity]{};
    uint8_t repeats = 1;
};

struct WebRuleRemoveForm {
    char trigger[kWebNameCapacity]{};
};

struct WebRulePatchForm {
    WebRulePatch patch = WebRulePatch::kEnabled;
    bool enabled = false;
    char log_mode[8]{};
};

struct WebHardwareForm {
    RfHardware hardware = RfHardware::kCc1101;
};

struct WebGenericGpioForm {
    uint8_t tx_gpio = 0;
    uint8_t rx_gpio = 0;
};

bool parse_web_learn_form(const char *body, std::size_t length, char *name,
                          std::size_t name_capacity);
bool parse_web_replay_form(const char *body, std::size_t length, WebReplayForm *output);
bool parse_web_recent_form(const char *body, std::size_t length, WebRecentForm *output);
bool parse_web_signal_save_form(const char *body, std::size_t length,
                                WebSignalSaveForm *output);
bool parse_web_signal_name_form(const char *body, std::size_t length, char *name,
                                std::size_t name_capacity);
bool parse_web_decoded_form(const char *body, std::size_t length, WebDecodedForm *output);
bool parse_web_raw_form(const char *body, std::size_t length, WebRawForm *output);
bool parse_web_rule_add_form(const char *body, std::size_t length, WebRuleAddForm *output);
bool parse_web_rule_remove_form(const char *body, std::size_t length, WebRuleRemoveForm *output);
bool parse_web_rule_patch_form(const char *body, std::size_t length, WebRulePatchForm *output);
bool parse_web_hardware_form(const char *body, std::size_t length, WebHardwareForm *output);
bool parse_web_generic_gpio_form(const char *body, std::size_t length,
                                 WebGenericGpioForm *output);
bool web_form_content_type_is_valid(const char *content_type);
bool web_octet_stream_content_type_is_valid(const char *content_type);
bool web_host_matches_ipv4(const char *host, uint32_t ipv4, uint16_t expected_port);
bool web_origin_matches_ipv4(const char *origin, uint32_t ipv4, uint16_t expected_port);
bool web_origin_matches_host(const char *origin, const char *host);
bool parse_web_device_host(const char *host, uint32_t ipv4, const char *configured_hostname,
                           const char *effective_hostname, uint16_t expected_port,
                           WebDeviceHost *output);
bool web_origin_matches_device_host(const char *origin, const WebDeviceHost &host,
                                    uint16_t expected_port);
bool escape_web_html(const char *input, char *output, std::size_t capacity,
                     std::size_t *output_length = nullptr);
bool escape_web_json_string(const char *input, char *output, std::size_t capacity,
                            std::size_t *output_length = nullptr);
bool format_web_ipv4(uint32_t ipv4, char *output, std::size_t capacity);

}  // namespace rfbridge
