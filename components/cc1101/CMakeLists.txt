idf_component_register(
    SRCS "cc1101.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_spi esp_timer platform_board
)
