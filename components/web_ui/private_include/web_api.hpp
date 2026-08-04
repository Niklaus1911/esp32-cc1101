#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

namespace rfbridge {

esp_err_t register_web_handlers(httpd_handle_t server);
esp_err_t authorize_web_ota_request(httpd_req_t *request, void *context);

}  // namespace rfbridge
