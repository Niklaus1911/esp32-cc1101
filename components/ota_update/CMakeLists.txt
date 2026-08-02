idf_component_register(
    SRCS "ota_update.cpp" "ota_update_policy.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_maintenance app_update bootloader_support esp_app_format esp_http_server esp_system network_wifi
)
