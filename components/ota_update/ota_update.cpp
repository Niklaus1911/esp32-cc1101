#include "ota_update.hpp"

#include "ota_update_policy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "app_maintenance.hpp"
#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_wifi.hpp"
#include "platform_board.hpp"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

constexpr TickType_t kMutexWait = pdMS_TO_TICKS(1000);
constexpr uint32_t kRebootTaskStackSize = 2048;
constexpr UBaseType_t kRebootTaskPriority = 3;
constexpr std::size_t kReceiveChunkSize = 2048;
constexpr uint32_t kProgressStepBytes = 65536;
constexpr char kStatusUri[] = "/api/v1/ota/status";
constexpr char kUploadUri[] = "/api/v1/ota";
constexpr char kJsonContentType[] = "application/json";
constexpr std::size_t kEscapedAppTextCapacity = 32U * 6U + 1U;
constexpr std::size_t kImagePrefixSize =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) +
    sizeof(RfBoardImageDescriptor);

SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_reboot_task = nullptr;
std::atomic<bool> s_initialization_started{false};
std::atomic<bool> s_available{false};
std::atomic<bool> s_upload_active{false};
std::atomic<bool> s_reboot_pending{false};
std::atomic<bool> s_external_reboot_pending{false};
std::atomic<uint32_t> s_sink_callbacks_in_flight{0};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};
OtaHttpAuthorize s_http_authorize = nullptr;
void *s_http_authorize_context = nullptr;
OtaUpdateStatus s_status{};
OtaUpdateEventSink s_event_sink = nullptr;
void *s_event_sink_context = nullptr;

class StatusLock {
public:
    StatusLock() : locked_(s_mutex != nullptr && xSemaphoreTake(s_mutex, kMutexWait) == pdTRUE) {}
    ~StatusLock()
    {
        if (locked_) {
            xSemaphoreGive(s_mutex);
        }
    }
    bool locked() const { return locked_; }

private:
    bool locked_;
};


void copy_text(char *destination, std::size_t capacity, const char *source)
{
    std::strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

void copy_bounded_text(char *destination, std::size_t capacity, const char *source,
                       std::size_t source_capacity)
{
    if (destination == nullptr || capacity == 0 || source == nullptr) {
        return;
    }
    const std::size_t length = std::min(capacity - 1U, strnlen(source, source_capacity));
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

bool escape_json_text(const char *input, char *output, std::size_t capacity)
{
    constexpr char kHex[] = "0123456789abcdef";
    if (input == nullptr || output == nullptr || capacity == 0) {
        return false;
    }
    std::size_t used = 0;
    for (std::size_t index = 0; input[index] != '\0'; ++index) {
        const uint8_t value = static_cast<uint8_t>(input[index]);
        if (value == '"' || value == '\\') {
            if (used + 2U >= capacity) {
                return false;
            }
            output[used++] = '\\';
            output[used++] = static_cast<char>(value);
        } else if (value < 0x20U || value > 0x7eU) {
            if (used + 6U >= capacity) {
                return false;
            }
            output[used++] = '\\';
            output[used++] = 'u';
            output[used++] = '0';
            output[used++] = '0';
            output[used++] = kHex[value >> 4U];
            output[used++] = kHex[value & 0x0fU];
        } else {
            if (used + 1U >= capacity) {
                return false;
            }
            output[used++] = static_cast<char>(value);
        }
    }
    output[used] = '\0';
    return true;
}

bool emit_event(const OtaUpdateEvent &event)
{
    OtaUpdateEventSink sink = nullptr;
    void *context = nullptr;
    {
        StatusLock lock;
        if (!lock.locked()) {
            return false;
        }
        sink = s_event_sink;
        context = s_event_sink_context;
        if (sink != nullptr) {
            s_sink_callbacks_in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (sink == nullptr) {
        return true;
    }
    const bool accepted = sink(event, context);
    s_sink_callbacks_in_flight.fetch_sub(1, std::memory_order_release);
    return accepted;
}

void set_status_state(OtaUpdateState state, esp_err_t error)
{
    StatusLock lock;
    if (!lock.locked()) {
        return;
    }
    s_status.state = state;
    s_status.last_error = error;
    s_status.upload_active = s_upload_active.load(std::memory_order_relaxed);
}

void update_progress(uint32_t received, uint32_t total, uint32_t *next_report)
{
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status.bytes_received = received;
            s_status.content_length = total;
        }
    }
    if (received < *next_report && received != total) {
        return;
    }
    *next_report = received + kProgressStepBytes;
    OtaUpdateEvent event{};
    event.type = OtaUpdateEventType::kProgress;
    event.bytes_received = received;
    event.content_length = total;
    emit_event(event);
}

esp_err_t send_json(httpd_req_t *request, const char *status, const char *json)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, kJsonContentType);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Referrer-Policy", "same-origin");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}

bool make_status_json(char *output, std::size_t capacity, const OtaUpdateStatus &status)
{
    char running_version[kEscapedAppTextCapacity]{};
    char candidate_version[kEscapedAppTextCapacity]{};
    if (!escape_json_text(status.running_version, running_version, sizeof(running_version)) ||
        !escape_json_text(status.candidate_version, candidate_version,
                          sizeof(candidate_version))) {
        return false;
    }
    const int length = std::snprintf(output, capacity,
                  "{\"state\":\"%s\",\"server\":%s,\"upload\":%s,\"port\":%u,"
                   "\"board_profile\":\"%s\","
                   "\"running_partition\":\"%s\",\"update_partition\":\"%s\","
                   "\"running_version\":\"%s\",\"candidate_version\":\"%s\","
                   "\"running_elf_sha256\":\"%s\",\"candidate_elf_sha256\":\"%s\","
                   "\"running_image_state\":\"%s\","
                   "\"running_image_state_error\":\"%s\","
                   "\"confirmation_error\":\"%s\","
                   "\"rollback_possible\":%s,\"pending_verification\":%s,"
                   "\"bytes\":%lu,\"total\":%lu,\"error\":\"%s\","
                   "\"maintenance_error\":\"%s\",\"initialization_error\":\"%s\"}",
                  ota_update_state_name(status.state), status.server_running ? "true" : "false",
                  status.upload_active ? "true" : "false", status.port, status.board_profile,
                   status.running_partition, status.update_partition, running_version,
                   candidate_version, status.running_elf_sha256, status.candidate_elf_sha256,
                   ota_image_state_name(status.running_image_state),
                   esp_err_to_name(status.running_image_state_error),
                   esp_err_to_name(status.confirmation_error),
                   status.rollback_possible ? "true" : "false",
                   status.pending_verification ? "true" : "false",
                   static_cast<unsigned long>(status.bytes_received),
                   static_cast<unsigned long>(status.content_length), esp_err_to_name(status.last_error),
                   esp_err_to_name(status.maintenance_error),
                   esp_err_to_name(status.initialization_error));
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

esp_err_t status_handler(httpd_req_t *request)
{
    if (s_http_authorize != nullptr && s_http_authorize(request, s_http_authorize_context) != ESP_OK) {
        return ESP_FAIL;
    }
    OtaUpdateStatus status{};
    const esp_err_t error = get_ota_update_status(&status);
    if (error != ESP_OK) {
        return send_json(request, "500 Internal Server Error", "{\"error\":\"status_unavailable\"}");
    }
    char response[1024]{};
    if (!make_status_json(response, sizeof(response), status)) {
        return send_json(request, "500 Internal Server Error", "{\"error\":\"status_too_large\"}");
    }
    return send_json(request, "200 OK", response);
}

esp_err_t receive_exact(httpd_req_t *request, uint8_t *output, std::size_t size)
{
    std::size_t received = 0;
    while (received < size) {
        const int result = httpd_req_recv(request, reinterpret_cast<char *>(output + received), size - received);
        if (result <= 0) {
            return result == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += static_cast<std::size_t>(result);
    }
    return ESP_OK;
}

esp_err_t validate_image_prefix(const uint8_t *prefix, std::size_t size, esp_app_desc_t *candidate,
                                RfBoardImageDescriptor *candidate_board)
{
    if (prefix == nullptr || candidate == nullptr || candidate_board == nullptr ||
        size < kImagePrefixSize) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_image_header_t image_header{};
    esp_image_segment_header_t segment_header{};
    std::memcpy(&image_header, prefix, sizeof(image_header));
    std::memcpy(&segment_header, prefix + sizeof(image_header), sizeof(segment_header));
    std::memcpy(candidate, prefix + sizeof(image_header) + sizeof(segment_header), sizeof(*candidate));
    std::memcpy(candidate_board,
                prefix + sizeof(image_header) + sizeof(segment_header) + sizeof(*candidate),
                sizeof(*candidate_board));
    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC || image_header.segment_count == 0 ||
        image_header.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
        image_header.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
        !bootloader_common_check_chip_revision_validity(&image_header, true) ||
        segment_header.data_len < sizeof(esp_app_desc_t) + sizeof(RfBoardImageDescriptor) ||
        candidate->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !board_image_descriptor_is_compatible(*candidate_board,
                                              current_board_image_descriptor())) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    char candidate_project[sizeof(candidate->project_name) + 1U]{};
    std::memcpy(candidate_project, candidate->project_name, sizeof(candidate->project_name));
    if (!ota_project_name_is_compatible(candidate_project, esp_app_get_description()->project_name)) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

void schedule_reboot()
{
    xTaskNotifyGive(s_reboot_task);
}

esp_err_t upload_handler(httpd_req_t *request)
{
    if (s_http_authorize != nullptr && s_http_authorize(request, s_http_authorize_context) != ESP_OK) {
        return ESP_FAIL;
    }
    const auto reject_busy = [&]() {
        httpd_resp_set_hdr(request, "Connection", "close");
        (void)send_json(request, "409 Conflict", "{\"error\":\"ota_busy\"}");
    };
    if (s_reboot_pending.load(std::memory_order_acquire) ||
        s_external_reboot_pending.load(std::memory_order_acquire)) {
        reject_busy();
        return ESP_FAIL;
    }
    bool expected = false;
    if (!s_upload_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        reject_busy();
        return ESP_FAIL;
    }
    if (s_reboot_pending.load(std::memory_order_acquire) ||
        s_external_reboot_pending.load(std::memory_order_acquire)) {
        s_upload_active.store(false, std::memory_order_release);
        reject_busy();
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    esp_err_t maintenance_error = ESP_OK;
    bool maintenance_started = false;
    bool ota_handle_open = false;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    const uint32_t content_length = request->content_len > 0 ? static_cast<uint32_t>(request->content_len) : 0;
    uint32_t bytes_received = 0;
    uint32_t next_report = 0;
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status.bytes_received = 0;
            s_status.content_length = content_length;
            s_status.candidate_version[0] = '\0';
            s_status.candidate_elf_sha256[0] = '\0';
            s_status.maintenance_error = ESP_OK;
        }
    }
    char content_type[48]{};
    const std::size_t content_type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (content_type_length == 0 || content_type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        update_partition == nullptr || !network_wifi_is_online() ||
        !ota_http_upload_request_is_valid(content_type, request->content_len,
                                          update_partition == nullptr ? 0 : update_partition->size,
                                          kImagePrefixSize)) {
        result = ESP_ERR_INVALID_ARG;
    }

    std::array<uint8_t, kImagePrefixSize> prefix{};
    esp_app_desc_t candidate{};
    RfBoardImageDescriptor candidate_board{};
    char candidate_version[sizeof(candidate.version) + 1U]{};
    char candidate_elf_sha256[kOtaSha256HexCapacity]{};
    if (result == ESP_OK) {
        result = receive_exact(request, prefix.data(), prefix.size());
        bytes_received = result == ESP_OK ? static_cast<uint32_t>(prefix.size()) : 0;
    }
    if (result == ESP_OK) {
        result = validate_image_prefix(prefix.data(), prefix.size(), &candidate, &candidate_board);
    }
    if (result == ESP_OK) {
        copy_bounded_text(candidate_version, sizeof(candidate_version), candidate.version,
                          sizeof(candidate.version));
        if (!format_ota_sha256(candidate.app_elf_sha256, sizeof(candidate.app_elf_sha256),
                               candidate_elf_sha256, sizeof(candidate_elf_sha256))) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    if (result == ESP_OK) {
        StatusLock lock;
        if (!lock.locked()) {
            result = ESP_ERR_TIMEOUT;
        } else {
            s_status.state = OtaUpdateState::kReceiving;
            s_status.upload_active = true;
            s_status.bytes_received = bytes_received;
            s_status.content_length = content_length;
            s_status.last_error = ESP_OK;
            s_status.maintenance_error = ESP_OK;
            copy_text(s_status.update_partition, sizeof(s_status.update_partition), update_partition->label);
            copy_text(s_status.candidate_version, sizeof(s_status.candidate_version), candidate_version);
            copy_text(s_status.candidate_elf_sha256,
                      sizeof(s_status.candidate_elf_sha256), candidate_elf_sha256);
        }
    }
    if (result == ESP_OK) {
        result = begin_ota_maintenance();
        maintenance_started = result == ESP_OK;
    }
    if (result == ESP_OK) {
        result = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
        ota_handle_open = result == ESP_OK;
    }
    if (result == ESP_OK) {
        result = esp_ota_write(ota_handle, prefix.data(), prefix.size());
        update_progress(bytes_received, content_length, &next_report);
    }

    std::array<uint8_t, kReceiveChunkSize> buffer{};
    while (result == ESP_OK && bytes_received < content_length) {
        const std::size_t remaining = content_length - bytes_received;
        const std::size_t requested = std::min<std::size_t>(remaining, buffer.size());
        const int received = httpd_req_recv(request, reinterpret_cast<char *>(buffer.data()), requested);
        if (received <= 0) {
            result = received == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        result = esp_ota_write(ota_handle, buffer.data(), static_cast<std::size_t>(received));
        if (result == ESP_OK) {
            bytes_received += static_cast<uint32_t>(received);
            update_progress(bytes_received, content_length, &next_report);
        }
    }

    if (result == ESP_OK) {
        set_status_state(OtaUpdateState::kValidating, ESP_OK);
        result = esp_ota_end(ota_handle);
        ota_handle_open = false;
    }
    if (result == ESP_OK) {
        result = esp_ota_set_boot_partition(update_partition);
    }

    if (result != ESP_OK) {
        if (ota_handle_open) {
            esp_ota_abort(ota_handle);
        }
        if (maintenance_started || ota_maintenance_is_active()) {
            for (uint32_t attempt = 0; attempt < 3; ++attempt) {
                maintenance_error = end_ota_maintenance();
                if (maintenance_error == ESP_OK) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        set_status_state(OtaUpdateState::kFailed, result);
        {
            StatusLock lock;
            if (lock.locked()) {
                s_status.maintenance_error = maintenance_error;
            }
        }
        OtaUpdateEvent event{};
        event.type = OtaUpdateEventType::kFailed;
        event.bytes_received = bytes_received;
        event.content_length = content_length;
        event.error = result;
        event.maintenance_error = maintenance_error;
        emit_event(event);
        const bool body_unread = bytes_received < content_length;
        if (body_unread) {
            httpd_resp_set_hdr(request, "Connection", "close");
        }
        char response[192]{};
        std::snprintf(response, sizeof(response),
                      "{\"error\":\"%s\",\"code\":%d,\"maintenance_error\":\"%s\"}",
                      esp_err_to_name(result), static_cast<int>(result),
                      esp_err_to_name(maintenance_error));
        const esp_err_t response_error = send_json(
            request, result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_VERSION ||
                             result == ESP_ERR_OTA_VALIDATE_FAILED
                         ? "400 Bad Request"
                         : "500 Internal Server Error",
            response);
        s_upload_active.store(false, std::memory_order_release);
        return body_unread ? ESP_FAIL : response_error;
    }

    s_reboot_pending.store(true, std::memory_order_release);
    set_status_state(OtaUpdateState::kPendingReboot, ESP_OK);
    OtaUpdateEvent event{};
    event.type = OtaUpdateEventType::kCompleted;
    event.bytes_received = bytes_received;
    event.content_length = content_length;
    std::memcpy(event.candidate_version, candidate.version, sizeof(candidate.version));
    emit_event(event);
    char escaped_version[kEscapedAppTextCapacity]{};
    char response[512]{};
    esp_err_t response_error = ESP_FAIL;
    if (escape_json_text(candidate_version, escaped_version, sizeof(escaped_version))) {
        const int length = std::snprintf(
            response, sizeof(response),
            "{\"ok\":true,\"rebooting\":true,\"target_partition\":\"%s\","
            "\"candidate_version\":\"%s\",\"candidate_elf_sha256\":\"%s\"}",
            update_partition->label, escaped_version, candidate_elf_sha256);
        if (length >= 0 && static_cast<std::size_t>(length) < sizeof(response)) {
            response_error = send_json(request, "200 OK", response);
        }
    }
    s_upload_active.store(false, std::memory_order_release);
    schedule_reboot();
    return response_error;
}

void reboot_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

}  // namespace

esp_err_t initialize_ota_update()
{
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        s_status = {};
        s_status.state = OtaUpdateState::kUnavailable;
        s_status.initialization_error = ESP_ERR_NO_MEM;
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t *otadata =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (running == nullptr || update == nullptr || otadata == nullptr) {
        StatusLock lock;
        if (lock.locked()) {
            s_status = {};
            s_status.state = OtaUpdateState::kUnavailable;
            s_status.initialization_error = ESP_ERR_NOT_FOUND;
        }
        s_initialization_error.store(ESP_ERR_NOT_FOUND, std::memory_order_release);
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_img_states_t image_state{};
    const esp_err_t image_state_error = esp_ota_get_state_partition(running, &image_state);
    const OtaImageState running_image_state =
        image_state_error == ESP_OK
            ? ota_image_state_from_raw(static_cast<uint32_t>(image_state))
            : OtaImageState::kUnknown;
    const bool pending = running_image_state == OtaImageState::kPendingVerify;
    const esp_app_desc_t *running_description = esp_app_get_description();
    char running_elf_sha256[kOtaSha256HexCapacity]{};
    if (running_description == nullptr ||
        !format_ota_sha256(running_description->app_elf_sha256,
                           sizeof(running_description->app_elf_sha256), running_elf_sha256,
                           sizeof(running_elf_sha256))) {
        StatusLock lock;
        if (lock.locked()) {
            s_status = {};
            s_status.state = OtaUpdateState::kUnavailable;
            s_status.initialization_error = ESP_ERR_INVALID_STATE;
        }
        s_initialization_error.store(ESP_ERR_INVALID_STATE, std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(reboot_task, "ota_reboot", kRebootTaskStackSize, nullptr, kRebootTaskPriority,
                    &s_reboot_task) != pdPASS) {
        StatusLock lock;
        if (lock.locked()) {
            s_status = {};
            s_status.state = OtaUpdateState::kUnavailable;
            s_status.initialization_error = ESP_ERR_NO_MEM;
            s_status.last_error = ESP_ERR_NO_MEM;
        }
        s_initialization_error.store(ESP_ERR_NO_MEM, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status = {};
            s_status.available = true;
            s_status.state = OtaUpdateState::kIdle;
            s_status.port = CONFIG_OTA_HTTP_PORT;
            s_status.initialization_error = ESP_OK;
            s_status.pending_verification = pending;
            s_status.rollback_possible = esp_ota_check_rollback_is_possible();
            s_status.running_image_state = running_image_state;
            s_status.running_image_state_error = image_state_error;
            s_status.confirmation_error =
                image_state_error == ESP_OK
                    ? (pending ? ESP_ERR_INVALID_STATE : ESP_OK)
                    : image_state_error;
            copy_text(s_status.board_profile, sizeof(s_status.board_profile),
                      current_board_info().profile_name);
            copy_text(s_status.running_partition, sizeof(s_status.running_partition), running->label);
            copy_text(s_status.update_partition, sizeof(s_status.update_partition), update->label);
            copy_bounded_text(s_status.running_version, sizeof(s_status.running_version),
                              running_description->version,
                              sizeof(running_description->version));
            copy_text(s_status.running_elf_sha256, sizeof(s_status.running_elf_sha256),
                      running_elf_sha256);
        }
    }
    s_available.store(true, std::memory_order_release);
    s_initialization_error.store(ESP_OK, std::memory_order_release);
    return ESP_OK;
}

esp_err_t register_ota_http_handlers(httpd_handle_t server, OtaHttpAuthorize authorize,
                                     void *authorize_context)
{
    if (server == nullptr || authorize == nullptr || !s_available.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_http_authorize = authorize;
    s_http_authorize_context = authorize_context;
    httpd_uri_t status_uri{};
    status_uri.uri = kStatusUri;
    status_uri.method = HTTP_GET;
    status_uri.handler = status_handler;
    esp_err_t error = httpd_register_uri_handler(server, &status_uri);
    if (error != ESP_OK) {
        return error;
    }
    httpd_uri_t upload_uri{};
    upload_uri.uri = kUploadUri;
    upload_uri.method = HTTP_POST;
    upload_uri.handler = upload_handler;
    error = httpd_register_uri_handler(server, &upload_uri);
    if (error != ESP_OK) {
        httpd_unregister_uri_handler(server, kStatusUri, HTTP_GET);
    }
    return error;
}

void set_ota_http_server_running(bool running, esp_err_t error)
{
    if (!s_available.load(std::memory_order_acquire)) {
        return;
    }
    bool changed = false;
    {
        StatusLock lock;
        if (lock.locked()) {
            changed = s_status.server_running != running || s_status.last_error != error;
            s_status.server_running = running;
            s_status.last_error = error;
            if (error == ESP_OK && !s_upload_active.load(std::memory_order_relaxed) &&
                !s_reboot_pending.load(std::memory_order_relaxed)) {
                s_status.state = OtaUpdateState::kIdle;
            }
        }
    }
    if (!changed) {
        return;
    }
    OtaUpdateEvent event{};
    event.type = error != ESP_OK ? OtaUpdateEventType::kFailed
                                 : (running ? OtaUpdateEventType::kServerStarted
                                            : OtaUpdateEventType::kServerStopped);
    event.error = error;
    emit_event(event);
}

bool ota_update_upload_is_active()
{
    return s_upload_active.load(std::memory_order_acquire);
}

esp_err_t reserve_system_reboot()
{
    if (!s_available.load(std::memory_order_acquire) || s_reboot_task == nullptr ||
        s_upload_active.load(std::memory_order_acquire) ||
        s_reboot_pending.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!s_external_reboot_pending.compare_exchange_strong(expected, true,
                                                            std::memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_upload_active.load(std::memory_order_acquire) ||
        s_reboot_pending.load(std::memory_order_acquire)) {
        s_external_reboot_pending.store(false, std::memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void cancel_system_reboot()
{
    s_external_reboot_pending.store(false, std::memory_order_release);
}

void commit_system_reboot()
{
    schedule_reboot();
}

esp_err_t get_ota_update_status(OtaUpdateStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available.load(std::memory_order_acquire) && s_mutex == nullptr) {
        *status = {};
        status->state = OtaUpdateState::kUnavailable;
        status->initialization_error = s_initialization_error.load(std::memory_order_acquire);
        status->upload_active = s_upload_active.load(std::memory_order_relaxed);
        return ESP_OK;
    }
    StatusLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_status;
    status->upload_active = s_upload_active.load(std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t set_ota_update_event_sink(OtaUpdateEventSink sink, void *context)
{
    {
        StatusLock lock;
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        s_event_sink_context = context;
        s_event_sink = sink;
    }
    if (sink == nullptr) {
        const TickType_t started = xTaskGetTickCount();
        while (s_sink_callbacks_in_flight.load(std::memory_order_acquire) != 0 &&
               xTaskGetTickCount() - started < pdMS_TO_TICKS(1000)) {
            vTaskDelay(1);
        }
        if (s_sink_callbacks_in_flight.load(std::memory_order_acquire) != 0) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t confirm_running_ota_image()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_img_states_t state{};
    const esp_err_t state_error = esp_ota_get_state_partition(running, &state);
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status.running_image_state =
                state_error == ESP_OK
                    ? ota_image_state_from_raw(static_cast<uint32_t>(state))
                    : OtaImageState::kUnknown;
            s_status.running_image_state_error = state_error;
            s_status.pending_verification =
                state_error == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
            s_status.confirmation_error = state_error;
        }
    }
    if (state_error != ESP_OK) {
        return state_error;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        StatusLock lock;
        if (lock.locked()) {
            s_status.confirmation_error = ESP_OK;
        }
        return ESP_OK;
    }
    const esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
    {
        StatusLock lock;
        if (lock.locked()) {
            s_status.confirmation_error = error;
            if (error == ESP_OK) {
                s_status.pending_verification = false;
                s_status.running_image_state = OtaImageState::kValid;
                s_status.running_image_state_error = ESP_OK;
                s_status.rollback_possible = esp_ota_check_rollback_is_possible();
            }
        }
    }
    return error;
}

const char *ota_update_state_name(OtaUpdateState state)
{
    switch (state) {
        case OtaUpdateState::kUnavailable:
            return "unavailable";
        case OtaUpdateState::kIdle:
            return "idle";
        case OtaUpdateState::kReceiving:
            return "receiving";
        case OtaUpdateState::kValidating:
            return "validating";
        case OtaUpdateState::kPendingReboot:
            return "pending_reboot";
        case OtaUpdateState::kFailed:
            return "failed";
    }
    return "invalid";
}

}  // namespace rfbridge
