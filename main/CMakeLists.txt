idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES network_wifi ota_update platform_nvs rf_automation rf_console rf_ook rf_storage
)
