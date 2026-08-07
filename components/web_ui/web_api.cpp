#include "web_api.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "bridge_control.hpp"
#include "esp_system.h"
#include "esp_timer.h"
#include "network_mdns.hpp"
#include "network_mqtt.hpp"
#include "network_wifi.hpp"
#include "platform_board.hpp"
#include "rf_automation.hpp"
#include "rf_automation_event.hpp"
#include "rf_ook.hpp"
#include "rf_signals.hpp"
#include "rf_storage.hpp"
#include "rf_storage_format.hpp"
#include "sdkconfig.h"
#include "web_form.hpp"

namespace rfbridge {
namespace {

constexpr uint16_t kHttpPort = 80;
constexpr std::size_t kMaximumActionBodySize = 2048;
constexpr std::size_t kScratchSize = 512;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");

struct StaticAsset {
    const uint8_t *start;
    const uint8_t *end;
    const char *content_type;
    bool html;
};

constexpr StaticAsset kIndexAsset{index_html_start, index_html_end, "text/html; charset=utf-8", true};
constexpr StaticAsset kCssAsset{app_css_start, app_css_end, "text/css; charset=utf-8", false};
constexpr StaticAsset kJsAsset{app_js_start, app_js_end, "text/javascript; charset=utf-8", false};

void set_security_headers(httpd_req_t *request, bool html)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Referrer-Policy", "same-origin");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    if (html) {
        httpd_resp_set_hdr(request, "Content-Security-Policy",
                           "default-src 'none'; style-src 'self'; script-src 'self'; "
                           "connect-src 'self'; img-src 'self' data:; base-uri 'none'; "
                           "frame-ancestors 'none'; form-action 'self'");
    }
}

esp_err_t send_json(httpd_req_t *request, const char *status, const char *json)
{
    set_security_headers(request, false);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_api_error(httpd_req_t *request, const char *status, const char *error,
                         esp_err_t code)
{
    char response[kScratchSize]{};
    const int length = std::snprintf(response, sizeof(response),
                                     "{\"ok\":false,\"error\":\"%s\",\"code\":%d}",
                                     error, static_cast<int>(code));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(response)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_json(request, status, response);
}

esp_err_t reject_request(httpd_req_t *request, const char *status, const char *error,
                         esp_err_t code)
{
    httpd_resp_set_hdr(request, "Connection", "close");
    (void)send_api_error(request, status, error, code);
    return ESP_FAIL;
}

const char *status_for_error(esp_err_t error)
{
    switch (error) {
        case ESP_ERR_INVALID_ARG: return "400 Bad Request";
        case ESP_ERR_NOT_FOUND: return "404 Not Found";
        case ESP_ERR_INVALID_STATE: return "409 Conflict";
        case ESP_ERR_TIMEOUT: return "503 Service Unavailable";
        case ESP_ERR_NO_MEM: return "503 Service Unavailable";
        default: return "500 Internal Server Error";
    }
}

esp_err_t send_operation_result(httpd_req_t *request, esp_err_t error,
                                const char *success_status = "200 OK")
{
    if (error == ESP_OK) {
        return send_json(request, success_status, "{\"ok\":true}");
    }
    return send_api_error(request, status_for_error(error), esp_err_to_name(error), error);
}

bool read_header(httpd_req_t *request, const char *name, char *output, std::size_t capacity)
{
    const std::size_t length = httpd_req_get_hdr_value_len(request, name);
    return length > 0 && length < capacity &&
           httpd_req_get_hdr_value_str(request, name, output, capacity) == ESP_OK;
}

bool request_host_matches_device(httpd_req_t *request, WebDeviceHost *validated_host)
{
    NetworkWifiStatus wifi{};
    NetworkHostnameStatus hostname{};
    NetworkMdnsStatus mdns{};
    char host[96]{};
    if (!read_header(request, "Host", host, sizeof(host)) ||
        get_network_wifi_status(&wifi) != ESP_OK ||
        get_network_hostname_status(&hostname) != ESP_OK) {
        return false;
    }
    (void)get_network_mdns_status(&mdns);
    WebDeviceHost parsed{};
    if (!parse_web_device_host(host, wifi.ip, hostname.configured_hostname,
                               mdns.effective_known ? mdns.effective_hostname : nullptr,
                               kHttpPort, &parsed)) {
        return false;
    }
    if (validated_host != nullptr) {
        *validated_host = parsed;
    }
    return true;
}

bool request_origin_matches_device(httpd_req_t *request, const WebDeviceHost &host)
{
    char origin[112]{};
    return read_header(request, "Origin", origin, sizeof(origin)) &&
           web_origin_matches_device_host(origin, host, kHttpPort);
}

bool request_content_type_is(httpd_req_t *request,
                             bool (*validator)(const char *content_type))
{
    char content_type[64]{};
    return read_header(request, "Content-Type", content_type, sizeof(content_type)) &&
           validator(content_type);
}

bool validate_mutation_headers(httpd_req_t *request, bool require_form_content_type)
{
    WebDeviceHost host{};
    return request_host_matches_device(request, &host) &&
           request_origin_matches_device(request, host) &&
           (!require_form_content_type ||
            request_content_type_is(request, web_form_content_type_is_valid));
}

esp_err_t require_empty_get(httpd_req_t *request)
{
    if (request->content_len != 0) {
        return reject_request(request, "413 Content Too Large", "get_body_rejected",
                              ESP_ERR_INVALID_SIZE);
    }
    if (!request_host_matches_device(request, nullptr)) {
        return reject_request(request, "403 Forbidden", "host_rejected",
                              ESP_ERR_INVALID_STATE);
    }
    return ESP_OK;
}

esp_err_t receive_form_body(httpd_req_t *request, std::unique_ptr<char[]> *body,
                            std::size_t *length)
{
    if (body == nullptr || length == nullptr || request->content_len <= 0 ||
        static_cast<std::size_t>(request->content_len) > kMaximumActionBodySize) {
        return ESP_ERR_INVALID_SIZE;
    }
    const std::size_t expected = static_cast<std::size_t>(request->content_len);
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[expected + 1U]);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    std::size_t offset = 0;
    while (offset < expected) {
        const int received = httpd_req_recv(request, buffer.get() + offset, expected - offset);
        if (received <= 0) {
            return received == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        offset += static_cast<std::size_t>(received);
    }
    buffer[offset] = '\0';
    *body = std::move(buffer);
    *length = offset;
    return ESP_OK;
}

esp_err_t receive_action_form(httpd_req_t *request, std::unique_ptr<char[]> *body,
                              std::size_t *length)
{
    if (!validate_mutation_headers(request, true)) {
        return reject_request(request, "403 Forbidden", "origin_rejected",
                              ESP_ERR_INVALID_STATE);
    }
    const esp_err_t error = receive_form_body(request, body, length);
    if (error == ESP_ERR_INVALID_SIZE) {
        return reject_request(request, "413 Content Too Large", "body_rejected", error);
    }
    if (error != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t start_chunked_json(httpd_req_t *request)
{
    set_security_headers(request, false);
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "application/json");
    return ESP_OK;
}

esp_err_t send_chunk(httpd_req_t *request, const char *text)
{
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_formatted_chunk(httpd_req_t *request, const char *buffer, int length,
                               std::size_t capacity)
{
    if (length < 0 || static_cast<std::size_t>(length) >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_send_chunk(request, buffer, length);
}

const char *learning_state_name(RfLearningState state)
{
    switch (state) {
        case RfLearningState::kIdle: return "idle";
        case RfLearningState::kArmed: return "armed";
        case RfLearningState::kCompleted: return "completed";
        case RfLearningState::kCancelled: return "cancelled";
        case RfLearningState::kTimedOut: return "timed_out";
        case RfLearningState::kFailed: return "failed";
    }
    return "invalid";
}

const char *match_kind_name(LearnedMatchKind kind)
{
    switch (kind) {
        case LearnedMatchKind::kNone: return "none";
        case LearnedMatchKind::kUnique: return "unique";
        case LearnedMatchKind::kAmbiguous: return "ambiguous";
        case LearnedMatchKind::kUnavailable: return "unavailable";
    }
    return "invalid";
}

const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN: return "unknown";
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    }
    return "unknown";
}

esp_err_t static_asset_handler(httpd_req_t *request)
{
    const esp_err_t validation = require_empty_get(request);
    if (validation != ESP_OK) {
        return validation;
    }
    const auto *asset = static_cast<const StaticAsset *>(request->user_ctx);
    if (asset == nullptr || asset->start == nullptr || asset->end <= asset->start) {
        return send_api_error(request, "500 Internal Server Error", "asset_unavailable",
                              ESP_ERR_INVALID_STATE);
    }
    set_security_headers(request, asset->html);
    httpd_resp_set_type(request, asset->content_type);
    return httpd_resp_send(request, reinterpret_cast<const char *>(asset->start),
                           static_cast<ssize_t>(asset->end - asset->start - 1U));
}

esp_err_t live_handler(httpd_req_t *request)
{
    const esp_err_t validation = require_empty_get(request);
    if (validation != ESP_OK) {
        return validation;
    }
    RfRadioStatus radio{};
    RfSignalsStatus signals{};
    RfAutomationStatus automation{};
    NetworkWifiStatus wifi{};
    NetworkMdnsStatus mdns{};
    NetworkMqttStatus mqtt{};
    RfFrame frame{};
    LearnedMatch match{};
    const esp_err_t radio_error = get_rf_radio_status(&radio);
    const esp_err_t signals_error = get_rf_signals_status(&signals);
    const esp_err_t automation_error = rf_automation_get_status(&automation);
    const esp_err_t wifi_error = get_network_wifi_status(&wifi);
    const esp_err_t mdns_error = get_network_mdns_status(&mdns);
    const esp_err_t mqtt_error = get_network_mqtt_status(&mqtt);
    const esp_err_t frame_error = get_last_rf_frame_with_match(&frame, &match);
    const BoardInfo &board = current_board_info();
    const BoardMemorySnapshot memory = board_memory_snapshot();

    char escaped_ssid[kWifiSsidCapacity * 6U]{};
    char escaped_saved_ssid[kWifiSsidCapacity * 6U]{};
    char ip[16]{};
    char netmask[16]{};
    char gateway[16]{};
    char dns[16]{};
    if (!escape_web_json_string(wifi.active_ssid, escaped_ssid, sizeof(escaped_ssid)) ||
        !escape_web_json_string(wifi.saved_ssid, escaped_saved_ssid,
                                sizeof(escaped_saved_ssid)) ||
        !format_web_ipv4(wifi.ip, ip, sizeof(ip)) ||
        !format_web_ipv4(wifi.netmask, netmask, sizeof(netmask)) ||
        !format_web_ipv4(wifi.gateway, gateway, sizeof(gateway)) ||
        !format_web_ipv4(wifi.dns, dns, sizeof(dns))) {
        return send_api_error(request, "500 Internal Server Error", "status_format_failed",
                              ESP_ERR_INVALID_SIZE);
    }

    esp_err_t error = start_chunked_json(request);
    char scratch[kScratchSize]{};
    if (error == ESP_OK) {
        const char *rx_state = radio_error != ESP_OK || !radio.running
                                   ? "faulted"
                                   : (radio.transmitting || radio.maintenance_active
                                          ? "paused"
                                          : (radio.receive_active ? "active" : "recovering"));
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "{\"radio\":{\"available\":%s,\"running\":%s,\"hardware\":\"%s\","
            "\"hardware_switch_error\":\"%s\",\"hardware_switches\":%lu,\"rx\":\"%s\","
            "\"transmitting\":%s,\"maintenance\":%s,\"accepted\":%lu,"
            "\"duplicates\":%lu,\"queue_drops\":%lu,\"timeouts\":%lu,"
            "\"truncated\":%lu,\"frequency_hz\":%lu,\"tx_power_dbm\":%d,"
            "\"cc1101\":{\"available\":%s,\"error\":\"%s\"",
            radio_error == ESP_OK ? "true" : "false",
            radio_error == ESP_OK && radio.running ? "true" : "false",
            rf_hardware_name(radio.hardware), esp_err_to_name(radio.hardware_switch_error),
            static_cast<unsigned long>(radio.hardware_switches), rx_state,
            radio.transmitting ? "true" : "false", radio.maintenance_active ? "true" : "false",
            static_cast<unsigned long>(radio.accepted_frames),
            static_cast<unsigned long>(radio.suppressed_duplicates),
            static_cast<unsigned long>(radio.rx_queue_drops),
            static_cast<unsigned long>(radio.command_timeouts),
            static_cast<unsigned long>(radio.truncated_captures),
            static_cast<unsigned long>(CONFIG_CC1101_FREQUENCY_HZ),
            static_cast<int>(CONFIG_CC1101_TX_POWER_DBM),
            radio.cc1101_info_valid ? "true" : "false", esp_err_to_name(radio.cc1101_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK && radio.cc1101_info_valid) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            ",\"part\":%u,\"version\":%u,\"marc_state\":%u,\"rssi_dbm_x2\":%d,"
            "\"carrier_sense\":%s,\"clear_channel\":%s,\"resets\":%lu,"
            "\"recoveries\":%lu,\"ready_timeouts\":%lu,\"state_timeouts\":%lu}},",
            radio.cc1101.part_number, radio.cc1101.version, radio.cc1101.marc_state,
            static_cast<int>(radio.cc1101.rssi_dbm_x2),
            radio.cc1101.carrier_sense ? "true" : "false",
            radio.cc1101.clear_channel ? "true" : "false",
            static_cast<unsigned long>(radio.cc1101.reset_count),
            static_cast<unsigned long>(radio.cc1101.recovery_count),
            static_cast<unsigned long>(radio.cc1101.ready_timeout_count),
            static_cast<unsigned long>(radio.cc1101.state_timeout_count));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    } else if (error == ESP_OK) {
        error = send_chunk(request, "}},");
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"learning\":{\"available\":%s,\"state\":\"%s\",\"revision\":%lu,"
            "\"name\":\"%s\",\"result\":\"%s\",\"count\":%u,"
            "\"catalog_available\":%s,\"queue_drops\":%lu,\"catalog_errors\":%lu,"
            "\"initialization_error\":\"%s\"},",
            signals_error == ESP_OK && signals.available ? "true" : "false",
            learning_state_name(signals.learning_state),
            static_cast<unsigned long>(signals.learning_revision), signals.learning_name,
            esp_err_to_name(signals.learning_result), signals.learned_count,
            signals.catalog_available ? "true" : "false",
            static_cast<unsigned long>(signals.queue_drops),
            static_cast<unsigned long>(signals.catalog_errors),
            esp_err_to_name(signals.initialization_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK && frame_error == ESP_ERR_NOT_FOUND) {
        error = send_chunk(request, "\"last\":null,");
    } else if (error == ESP_OK && frame_error != ESP_OK) {
        const int length = std::snprintf(scratch, sizeof(scratch),
                                         "\"last\":{\"error\":\"%s\"},",
                                         esp_err_to_name(frame_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    } else if (error == ESP_OK && frame.encoding == RfEncoding::kDecoded) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"last\":{\"encoding\":\"decoded\",\"code\":\"0x%llX\","
            "\"bits\":%u,\"protocol\":%u,\"pulse_us\":%u,\"repeats\":%u,"
            "\"captured_us\":%lld,\"match\":\"%s\",\"match_name\":\"%s\","
            "\"match_count\":%u},",
            static_cast<unsigned long long>(frame.decoded.code), frame.decoded.bits,
            frame.decoded.protocol, frame.decoded.pulse_us, frame.observed_repeats,
            static_cast<long long>(frame.captured_us), match_kind_name(match.kind), match.name,
            match.count);
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    } else if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"last\":{\"encoding\":\"raw\",\"pulses\":%u,\"start_level\":%u,"
            "\"fingerprint\":\"0x%08lX\",\"repeats\":%u,\"captured_us\":%lld,"
            "\"match\":\"%s\",\"match_name\":\"%s\",\"match_count\":%u},",
            frame.raw.count, frame.raw.start_level, static_cast<unsigned long>(frame.fingerprint),
            frame.observed_repeats, static_cast<long long>(frame.captured_us),
            match_kind_name(match.kind), match.name, match.count);
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"automation\":{\"available\":%s,\"enabled\":%s,\"runtime_paused\":%s,"
            "\"log_mode\":\"%s\",\"rules\":%u,\"frames\":%lu,\"matches\":%lu,"
            "\"stale\":%lu,\"ambiguous\":%lu,",
            automation_error == ESP_OK && automation.available ? "true" : "false",
            automation.enabled ? "true" : "false", automation.runtime_paused ? "true" : "false",
            automation.log_mode_known ? rf_automation_log_mode_name(automation.log_mode) : "unknown",
            automation.rule_count, static_cast<unsigned long>(automation.frames_seen),
            static_cast<unsigned long>(automation.matches),
            static_cast<unsigned long>(automation.stale_frames),
            static_cast<unsigned long>(automation.ambiguous_frames));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"actions\":%lu,\"suppressed\":%lu,\"tx_errors\":%lu,"
            "\"queue_drops\":%lu,\"log_events\":%lu,\"log_drops\":%lu,"
            "\"initialization_error\":\"%s\",\"last_error\":\"%s\","
            "\"last_trigger\":\"%s\",\"last_target\":\"%s\"},",
            static_cast<unsigned long>(automation.actions_succeeded),
            static_cast<unsigned long>(automation.cooldown_suppressed),
            static_cast<unsigned long>(automation.tx_errors),
            static_cast<unsigned long>(automation.queue_drops),
            static_cast<unsigned long>(automation.log_events),
            static_cast<unsigned long>(automation.log_drops),
            esp_err_to_name(automation.initialization_error),
            esp_err_to_name(automation.last_error), automation.last_trigger,
            automation.last_target);
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"network\":{\"available\":%s,\"online\":%s,\"rssi\":%d,"
            "\"state\":\"%s\",\"driver_initialized\":%s,\"driver_started\":%s,"
            "\"scan_running\":%s,\"ota_locked\":%s,",
            wifi_error == ESP_OK && wifi.available ? "true" : "false",
            wifi_error == ESP_OK && wifi.available &&
                    wifi.state == NetworkWifiState::kOnline && wifi.ip != 0
                ? "true"
                : "false",
            static_cast<int>(wifi.rssi),
            network_wifi_state_name(wifi.state), wifi.driver_initialized ? "true" : "false",
            wifi.driver_started ? "true" : "false", wifi.scan_running ? "true" : "false",
            wifi.ota_locked ? "true" : "false");
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"ssid\":\"%s\",\"saved_ssid\":\"%s\",\"saved_known\":%s,"
            "\"saved\":%s,\"active_saved\":%s,",
            escaped_ssid, escaped_saved_ssid, wifi.saved_known ? "true" : "false",
            wifi.saved ? "true" : "false", wifi.active_saved ? "true" : "false");
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\","
            "\"retries\":%lu,\"event_drops\":%lu,\"disconnect_reason\":%ld,"
            "\"initialization_error\":\"%s\",\"persistence_error\":\"%s\","
            "\"last_error\":\"%s\"},",
            ip, netmask, gateway, dns, static_cast<unsigned long>(wifi.retry_count),
            static_cast<unsigned long>(wifi.event_drops),
            static_cast<long>(wifi.disconnect_reason), esp_err_to_name(wifi.initialization_error),
            esp_err_to_name(wifi.persistence_error), esp_err_to_name(wifi.last_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"mdns\":{\"available\":%s,\"state\":\"%s\","
            "\"configured_hostname\":\"%s\",\"hostname_custom\":%s,"
            "\"effective_hostname\":\"%s\",\"effective_known\":%s,"
            "\"conflict_renamed\":%s,\"http_registered\":%s,"
            "\"rfbridge_registered\":%s,",
            mdns_error == ESP_OK && mdns.available ? "true" : "false",
            network_mdns_state_name(mdns.state), mdns.configured_hostname,
            mdns.hostname_custom ? "true" : "false", mdns.effective_hostname,
            mdns.effective_known ? "true" : "false",
            mdns.conflict_renamed ? "true" : "false",
            mdns.http_service_registered ? "true" : "false",
            mdns.rfbridge_service_registered ? "true" : "false");
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"configured_generation\":%lu,\"applied_generation\":%lu,"
            "\"heartbeat_age_ms\":%lu,\"initialization_error\":\"%s\","
            "\"last_error\":\"%s\"},",
            static_cast<unsigned long>(mdns.configured_generation),
            static_cast<unsigned long>(mdns.applied_generation),
            static_cast<unsigned long>(mdns.owner_heartbeat_age_ms),
            esp_err_to_name(mdns.initialization_error), esp_err_to_name(mdns.last_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"board\":{\"profile\":\"%s\",\"model\":\"%s\",\"target\":\"%s\",\"flash_mib\":%u,"
            "\"psram_mib\":%u,\"console\":\"%s\",\"combined_services\":%s,"
            "\"activity_led_enabled\":%s,\"activity_led_gpio\":%d,"
            "\"activity_led_active_high\":%s,\"cc1101\":{\"sclk\":%d,\"miso\":%d,"
            "\"mosi\":%d,\"cs\":%d,\"gdo0_tx\":%d,\"gdo2_rx\":%d},"
            "\"generic\":{\"tx\":%d,\"rx\":%d}},",
            board.profile_name, board.model_name, board.target_name, board.flash_mib,
            board.psram_mib,
            console_transport_name(board.console),
            board.combined_services ? "true" : "false",
            board.activity_led_enabled ? "true" : "false", board.activity_led_gpio,
            board.activity_led_active_high ? "true" : "false", board.cc1101.sclk,
            board.cc1101.miso, board.cc1101.mosi, board.cc1101.cs, board.cc1101.gdo0,
            board.cc1101.gdo2, board.cc1101.generic_tx, board.cc1101.generic_rx);
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"services\":{\"requested\":\"%s\",\"boot\":\"%s\","
            "\"effective\":\"%s\",\"configured\":%s,\"reboot_required\":%s,"
            "\"fallback\":%s,\"retirement\":\"%s\",\"maintenance\":%s,"
            "\"config_error\":\"%s\",\"web_error\":\"%s\",\"mqtt_error\":\"%s\","
            "\"status_error\":\"%s\"},",
            network_service_mask_name(mqtt.requested_services),
            network_service_mask_name(mqtt.boot_services),
            network_service_mask_name(mqtt.effective_services),
            mqtt.configured ? "true" : "false", mqtt.reboot_required ? "true" : "false",
            mqtt.current_boot_fallback ? "true" : "false",
            mqtt_retirement_state_name(mqtt.retirement_state),
            mqtt.maintenance_active ? "true" : "false", esp_err_to_name(mqtt.config_error),
            esp_err_to_name(mqtt.web_error), esp_err_to_name(mqtt.mqtt_error),
            esp_err_to_name(mqtt_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"system\":{\"uptime_ms\":%llu,\"reset_reason\":\"%s\",\"heap_free\":%u,"
            "\"heap_minimum\":%u,\"heap_largest\":%u,"
            "\"internal\":{\"total\":%lu,\"free\":%lu,\"minimum\":%lu,\"largest\":%lu},"
            "\"psram\":{\"total\":%lu,\"free\":%lu,\"minimum\":%lu,\"largest\":%lu}},",
            static_cast<unsigned long long>(esp_timer_get_time() / 1000),
            reset_reason_name(esp_reset_reason()),
            static_cast<unsigned>(memory.internal.free),
            static_cast<unsigned>(memory.internal.minimum_free),
            static_cast<unsigned>(memory.internal.largest_free_block),
            static_cast<unsigned long>(memory.internal.total),
            static_cast<unsigned long>(memory.internal.free),
            static_cast<unsigned long>(memory.internal.minimum_free),
            static_cast<unsigned long>(memory.internal.largest_free_block),
            static_cast<unsigned long>(memory.psram.total),
            static_cast<unsigned long>(memory.psram.free),
            static_cast<unsigned long>(memory.psram.minimum_free),
            static_cast<unsigned long>(memory.psram.largest_free_block));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "\"errors\":{\"radio\":\"%s\",\"signals\":\"%s\","
            "\"automation\":\"%s\",\"wifi\":\"%s\",\"mdns\":\"%s\","
            "\"services\":\"%s\"}}",
            esp_err_to_name(radio_error), esp_err_to_name(signals_error),
            esp_err_to_name(automation_error), esp_err_to_name(wifi_error),
            esp_err_to_name(mdns_error), esp_err_to_name(mqtt_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error != ESP_OK) {
        return error;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t signals_handler(httpd_req_t *request)
{
    const esp_err_t validation = require_empty_get(request);
    if (validation != ESP_OK) {
        return validation;
    }
    std::array<RfStorageName, CONFIG_RF_MAX_LEARNED_SIGNALS> names{};
    std::size_t count = 0;
    const esp_err_t list_error = rf_storage_list(names.data(), names.size(), &count);
    if (list_error != ESP_OK) {
        return send_api_error(request, status_for_error(list_error), esp_err_to_name(list_error),
                              list_error);
    }
    esp_err_t error = start_chunked_json(request);
    if (error == ESP_OK) {
        error = send_chunk(request, "{\"signals\":[");
    }
    char scratch[kScratchSize]{};
    for (std::size_t index = 0; error == ESP_OK && index < count; ++index) {
        RfStoredSignal signal{};
        const esp_err_t load_error = rf_storage_load(names[index].value, &signal);
        int length = 0;
        if (load_error != ESP_OK) {
            length = std::snprintf(scratch, sizeof(scratch),
                                   "%s{\"name\":\"%s\",\"error\":\"%s\"}",
                                   index == 0 ? "" : ",", names[index].value,
                                   esp_err_to_name(load_error));
        } else if (signal.encoding == RfStoredEncoding::kDecoded) {
            length = std::snprintf(
                scratch, sizeof(scratch),
                "%s{\"name\":\"%s\",\"encoding\":\"decoded\",\"code\":\"0x%llX\","
                "\"bits\":%u,\"protocol\":%u,\"pulse_us\":%u}",
                index == 0 ? "" : ",", names[index].value,
                static_cast<unsigned long long>(signal.decoded.code), signal.decoded.bits,
                signal.decoded.protocol, signal.decoded.pulse_us);
        } else {
            length = std::snprintf(
                scratch, sizeof(scratch),
                "%s{\"name\":\"%s\",\"encoding\":\"raw\",\"pulses\":%u,"
                "\"start_level\":%u}",
                index == 0 ? "" : ",", names[index].value, signal.raw.count,
                signal.raw.start_level);
        }
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        error = send_chunk(request, "]}");
    }
    if (error != ESP_OK) {
        return error;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t rules_handler(httpd_req_t *request)
{
    const esp_err_t validation = require_empty_get(request);
    if (validation != ESP_OK) {
        return validation;
    }
    RfAutomationStatus status{};
    const esp_err_t status_error = rf_automation_get_status(&status);
    std::size_t count = 0;
    esp_err_t error = rf_automation_list_rule_info(nullptr, 0, &count);
    std::unique_ptr<RfAutomationRuleInfo[]> rules;
    if (error == ESP_OK && count > 0) {
        rules.reset(new (std::nothrow) RfAutomationRuleInfo[count]);
        if (!rules) {
            error = ESP_ERR_NO_MEM;
        } else {
            error = rf_automation_list_rule_info(rules.get(), count, &count);
        }
    }
    if (status_error != ESP_OK || error != ESP_OK) {
        const esp_err_t result = status_error != ESP_OK ? status_error : error;
        return send_api_error(request, status_for_error(result), esp_err_to_name(result), result);
    }
    error = start_chunked_json(request);
    char scratch[kScratchSize]{};
    if (error == ESP_OK) {
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "{\"enabled\":%s,\"available\":%s,\"log_mode\":\"%s\",\"rules\":[",
            status.enabled ? "true" : "false", status.available ? "true" : "false",
            status.log_mode_known ? rf_automation_log_mode_name(status.log_mode) : "unknown");
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    for (std::size_t index = 0; error == ESP_OK && index < count; ++index) {
        const RfAutomationRuleInfo &rule = rules[index];
        const int length = std::snprintf(
            scratch, sizeof(scratch),
            "%s{\"trigger\":\"%s\",\"target\":\"%s\",\"repeats\":%u,"
            "\"cooldown_ms\":%lu,\"valid\":%s,\"error\":\"%s\"}",
            index == 0 ? "" : ",", rule.entry.trigger_name, rule.entry.rule.target_name,
            rule.entry.rule.repeats, static_cast<unsigned long>(rule.entry.rule.cooldown_ms),
            rule.validation_error == ESP_OK ? "true" : "false",
            esp_err_to_name(rule.validation_error));
        error = send_formatted_chunk(request, scratch, length, sizeof(scratch));
    }
    if (error == ESP_OK) {
        error = send_chunk(request, "]}");
    }
    if (error != ESP_OK) {
        return error;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t learn_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    char name[kRfStorageNameCapacity]{};
    if (!parse_web_learn_form(body.get(), length, name, sizeof(name)) ||
        !rf_storage_name_is_valid(name)) {
        return send_api_error(request, "400 Bad Request", "invalid_learning_request",
                              ESP_ERR_INVALID_ARG);
    }
    error = rf_signals_arm_learning(name, BridgeEventSource::kWeb);
    return send_operation_result(request, error, "202 Accepted");
}

esp_err_t cancel_learn_handler(httpd_req_t *request)
{
    if (!validate_mutation_headers(request, false)) {
        return reject_request(request, "403 Forbidden", "origin_rejected",
                              ESP_ERR_INVALID_STATE);
    }
    if (request->content_len != 0) {
        return reject_request(request, "413 Content Too Large", "body_rejected",
                              ESP_ERR_INVALID_SIZE);
    }
    return send_operation_result(request,
                                 rf_signals_cancel_learning(BridgeEventSource::kWeb),
                                 "202 Accepted");
}

esp_err_t replay_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    WebReplayForm form{};
    if (!parse_web_replay_form(body.get(), length, &form)) {
        return send_api_error(request, "400 Bad Request", "invalid_replay_request",
                              ESP_ERR_INVALID_ARG);
    }
    error = form.latest
                ? bridge_control_replay_last(form.repeats, BridgeEventSource::kWeb)
                : bridge_control_replay_named(form.name, form.repeats, BridgeEventSource::kWeb);
    return send_operation_result(request, error);
}

esp_err_t delete_signal_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    char name[kRfStorageNameCapacity]{};
    if (!parse_web_signal_name_form(body.get(), length, name, sizeof(name)) ||
        !rf_storage_name_is_valid(name)) {
        return send_api_error(request, "400 Bad Request", "invalid_signal_request",
                              ESP_ERR_INVALID_ARG);
    }
    return send_operation_result(request, bridge_control_forget_signal(name));
}

esp_err_t decoded_transmit_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    WebDecodedForm form{};
    if (!parse_web_decoded_form(body.get(), length, &form)) {
        return send_api_error(request, "400 Bad Request", "invalid_decoded_request",
                              ESP_ERR_INVALID_ARG);
    }
    DecodedTransmitRequest transmit{};
    transmit.code = form.code;
    transmit.bits = static_cast<uint8_t>(form.bits);
    transmit.protocol = form.protocol;
    transmit.pulse_us = form.pulse_us;
    transmit.repeats = form.repeats;
    return send_operation_result(
        request, bridge_control_transmit_decoded(transmit, BridgeEventSource::kWeb));
}

esp_err_t raw_transmit_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    WebRawForm form{};
    if (!parse_web_raw_form(body.get(), length, &form) || form.count > kMaxRawPulses) {
        return send_api_error(request, "400 Bad Request", "invalid_raw_request",
                              ESP_ERR_INVALID_ARG);
    }
    RawTransmitRequest transmit{};
    transmit.signal.start_level = form.start_level;
    transmit.signal.count = static_cast<uint16_t>(form.count);
    std::memcpy(transmit.signal.durations_us, form.durations,
                form.count * sizeof(form.durations[0]));
    transmit.repeats = form.repeats;
    return send_operation_result(request,
                                 bridge_control_transmit_raw(transmit, BridgeEventSource::kWeb));
}

esp_err_t add_rule_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    WebRuleAddForm form{};
    if (!parse_web_rule_add_form(body.get(), length, &form) ||
        !rf_storage_name_is_valid(form.trigger) || !rf_storage_name_is_valid(form.target)) {
        return send_api_error(request, "400 Bad Request", "invalid_rule_request",
                              ESP_ERR_INVALID_ARG);
    }
    return send_operation_result(
        request, rf_automation_add_rule(form.trigger, form.target, form.repeats));
}

esp_err_t remove_rule_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    WebRuleRemoveForm form{};
    if (!parse_web_rule_remove_form(body.get(), length, &form) ||
        !rf_storage_name_is_valid(form.trigger)) {
        return send_api_error(request, "400 Bad Request", "invalid_rule_request",
                              ESP_ERR_INVALID_ARG);
    }
    return send_operation_result(request, rf_automation_remove_rule(form.trigger));
}

esp_err_t patch_rule_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    WebRulePatchForm form{};
    if (!parse_web_rule_patch_form(body.get(), length, &form)) {
        return send_api_error(request, "400 Bad Request", "invalid_rule_setting",
                              ESP_ERR_INVALID_ARG);
    }
    if (form.patch == WebRulePatch::kEnabled) {
        error = rf_automation_set_enabled(form.enabled);
    } else {
        RfAutomationLogMode mode = RfAutomationLogMode::kOff;
        if (std::strcmp(form.log_mode, "actions") == 0) {
            mode = RfAutomationLogMode::kActions;
        } else if (std::strcmp(form.log_mode, "verbose") == 0) {
            mode = RfAutomationLogMode::kVerbose;
        }
        error = rf_automation_set_log_mode(mode);
    }
    return send_operation_result(request, error);
}

esp_err_t hardware_handler(httpd_req_t *request)
{
    std::unique_ptr<char[]> body;
    std::size_t length = 0;
    esp_err_t error = receive_action_form(request, &body, &length);
    if (error != ESP_OK) {
        return error;
    }
    WebHardwareForm form{};
    if (!parse_web_hardware_form(body.get(), length, &form)) {
        return send_api_error(request, "400 Bad Request", "invalid_hardware_request",
                              ESP_ERR_INVALID_ARG);
    }
    return send_operation_result(
        request, bridge_control_set_rf_hardware(form.hardware, BridgeEventSource::kWeb));
}

esp_err_t register_handler(httpd_handle_t server, const char *uri, httpd_method_t method,
                           esp_err_t (*handler)(httpd_req_t *), void *context = nullptr)
{
    httpd_uri_t route{};
    route.uri = uri;
    route.method = method;
    route.handler = handler;
    route.user_ctx = context;
    return httpd_register_uri_handler(server, &route);
}

}  // namespace

esp_err_t authorize_web_ota_request(httpd_req_t *request, void *)
{
    WebDeviceHost host{};
    if (request == nullptr || !request_host_matches_device(request, &host)) {
        if (request != nullptr) {
            reject_request(request, "403 Forbidden", "host_rejected", ESP_ERR_INVALID_STATE);
        }
        return ESP_FAIL;
    }
    if (request->method == HTTP_GET) {
        if (request->content_len != 0) {
            reject_request(request, "413 Content Too Large", "get_body_rejected",
                           ESP_ERR_INVALID_SIZE);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (request->method != HTTP_POST || !request_origin_matches_device(request, host) ||
        !request_content_type_is(request, web_octet_stream_content_type_is_valid)) {
        reject_request(request, "403 Forbidden", "origin_rejected", ESP_ERR_INVALID_STATE);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t register_web_handlers(httpd_handle_t server)
{
    if (server == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    struct Route {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
        void *context;
    };
    const Route routes[] = {
        {"/", HTTP_GET, static_asset_handler, const_cast<StaticAsset *>(&kIndexAsset)},
        {"/app.css", HTTP_GET, static_asset_handler, const_cast<StaticAsset *>(&kCssAsset)},
        {"/app.js", HTTP_GET, static_asset_handler, const_cast<StaticAsset *>(&kJsAsset)},
        {"/api/live", HTTP_GET, live_handler, nullptr},
        {"/api/signals", HTTP_GET, signals_handler, nullptr},
        {"/api/rules", HTTP_GET, rules_handler, nullptr},
        {"/api/learn", HTTP_POST, learn_handler, nullptr},
        {"/api/learn", HTTP_DELETE, cancel_learn_handler, nullptr},
        {"/api/replay", HTTP_POST, replay_handler, nullptr},
        {"/api/signals", HTTP_DELETE, delete_signal_handler, nullptr},
        {"/api/transmit/decoded", HTTP_POST, decoded_transmit_handler, nullptr},
        {"/api/transmit/raw", HTTP_POST, raw_transmit_handler, nullptr},
        {"/api/rules", HTTP_POST, add_rule_handler, nullptr},
        {"/api/rules", HTTP_DELETE, remove_rule_handler, nullptr},
        {"/api/rules", HTTP_PATCH, patch_rule_handler, nullptr},
        {"/api/radio/hardware", HTTP_POST, hardware_handler, nullptr},
    };
    for (const Route &route : routes) {
        const esp_err_t error = register_handler(server, route.uri, route.method, route.handler,
                                                 route.context);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

}  // namespace rfbridge
