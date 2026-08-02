idf_component_register(
    SRCS
        "test_app_main.cpp"
        "test_cc1101_component.cpp"
        "test_network_wifi_component.cpp"
        "test_ota_update_component.cpp"
        "test_rf_automation_component.cpp"
        "test_rf_console_component.cpp"
        "test_rf_ook_component.cpp"
        "test_rf_storage_component.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES platform_nvs network_wifi app_maintenance ota_update cc1101 rf_automation rf_console rf_ook rf_storage esp_hw_support nvs_flash unity
    WHOLE_ARCHIVE
)
