idf_component_register(
    SRCS "rf_ook.cpp" "rf_activity_led.cpp" "rf_activity_led_policy.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES cc1101 rf_codec rf_storage esp_driver_gpio esp_driver_rmt esp_timer platform_board
)
