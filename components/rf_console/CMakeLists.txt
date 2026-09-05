idf_component_register(
    SRCS "rf_console.cpp" "rf_console_format.cpp" "rf_console_linenoise.c" "rf_console_parse.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bridge_control bridge_events console esp_driver_uart esp_driver_usb_serial_jtag network_mdns network_mqtt network_wifi ota_update platform_board rf_automation rf_ook rf_signals rf_storage cc1101 esp_timer
)
