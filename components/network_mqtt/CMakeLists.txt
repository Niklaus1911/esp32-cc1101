idf_component_register(
    SRCS "network_mqtt.cpp" "network_mqtt_storage.cpp" "mqtt_config_format.cpp" "mqtt_discovery.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES bridge_control bridge_events mqtt platform_nvs rf_signals rf_storage
    PRIV_REQUIRES app_update esp_event esp_hw_support esp_timer nvs_flash
)
