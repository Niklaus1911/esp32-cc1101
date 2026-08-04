idf_component_register(
    SRCS "rf_console.cpp" "rf_console_format.cpp" "rf_console_parse.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bridge_control bridge_events console network_wifi ota_update rf_automation rf_ook rf_signals rf_storage web_auth cc1101 esp_timer
)
