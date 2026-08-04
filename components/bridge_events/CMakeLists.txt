idf_component_register(
    SRCS "bridge_events.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer network_wifi ota_update rf_automation rf_ook rf_storage
)
