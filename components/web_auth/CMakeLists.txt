idf_component_register(
    SRCS "web_auth.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer mbedtls nvs_flash
)
