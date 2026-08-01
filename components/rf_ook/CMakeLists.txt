idf_component_register(
    SRCS "rf_ook.cpp"
    INCLUDE_DIRS "include"
    REQUIRES cc1101 rf_codec esp_driver_rmt esp_timer
)
