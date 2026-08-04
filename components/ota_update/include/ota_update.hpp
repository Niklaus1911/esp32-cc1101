#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_http_server.h"

namespace rfbridge {

enum class OtaUpdateState : uint8_t {
    kUnavailable,
    kIdle,
    kReceiving,
    kValidating,
    kPendingReboot,
    kFailed,
};

enum class OtaUpdateEventType : uint8_t {
    kServerStarted,
    kServerStopped,
    kProgress,
    kCompleted,
    kFailed,
};

struct OtaUpdateEvent {
    OtaUpdateEventType type = OtaUpdateEventType::kProgress;
    uint32_t bytes_received = 0;
    uint32_t content_length = 0;
    esp_err_t error = ESP_OK;
    esp_err_t maintenance_error = ESP_OK;
    char candidate_version[33]{};
};

struct OtaUpdateStatus {
    bool available = false;
    bool server_running = false;
    bool upload_active = false;
    bool pending_verification = false;
    bool rollback_possible = false;
    OtaUpdateState state = OtaUpdateState::kUnavailable;
    uint16_t port = 0;
    uint32_t bytes_received = 0;
    uint32_t content_length = 0;
    esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
    esp_err_t last_error = ESP_OK;
    esp_err_t maintenance_error = ESP_OK;
    char running_partition[17]{};
    char update_partition[17]{};
    char running_version[33]{};
    char candidate_version[33]{};
};

// Runs on the OTA service or HTTP task. A sink must use only bounded zero-wait operations.
using OtaUpdateEventSink = bool (*)(const OtaUpdateEvent &event, void *context);
using OtaHttpAuthorize = esp_err_t (*)(httpd_req_t *request, void *context);

esp_err_t initialize_ota_update();
esp_err_t register_ota_http_handlers(httpd_handle_t server, OtaHttpAuthorize authorize,
                                     void *authorize_context);
void set_ota_http_server_running(bool running, esp_err_t error = ESP_OK);
bool ota_update_upload_is_active();
esp_err_t get_ota_update_status(OtaUpdateStatus *status);
esp_err_t set_ota_update_event_sink(OtaUpdateEventSink sink, void *context);
esp_err_t confirm_running_ota_image();
const char *ota_update_state_name(OtaUpdateState state);

}  // namespace rfbridge
