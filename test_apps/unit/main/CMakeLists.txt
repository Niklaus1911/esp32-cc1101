idf_component_register(
    SRCS
        "test_app_main.cpp"
        "test_bridge_events_component.cpp"
        "test_cc1101_component.cpp"
        "test_network_wifi_component.cpp"
        "test_ota_update_component.cpp"
        "test_rf_automation_component.cpp"
        "test_rf_console_component.cpp"
        "test_rf_ook_component.cpp"
        "test_rf_signals_component.cpp"
        "test_rf_storage_component.cpp"
        "test_web_form_component.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES platform_nvs network_wifi app_maintenance ota_update cc1101 bridge_control bridge_events rf_automation rf_console rf_ook rf_signals rf_storage web_auth web_ui esp_hw_support nvs_flash unity
    WHOLE_ARCHIVE
)
