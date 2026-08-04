idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bridge_events network_mdns network_wifi ota_update platform_nvs rf_automation rf_console rf_ook rf_signals rf_storage web_ui
)
