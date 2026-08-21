#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

namespace rfbridge {

esp_err_t web_events_start(httpd_handle_t server);
esp_err_t web_events_stop();
esp_err_t web_events_handler(httpd_req_t *request);

}  // namespace rfbridge
