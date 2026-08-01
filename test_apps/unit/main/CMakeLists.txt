idf_component_register(
    SRCS
        "test_app_main.cpp"
        "test_cc1101_component.cpp"
        "test_rf_console_component.cpp"
        "test_rf_ook_component.cpp"
        "test_rf_storage_component.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES cc1101 rf_console rf_ook rf_storage esp_hw_support unity
    WHOLE_ARCHIVE
)
