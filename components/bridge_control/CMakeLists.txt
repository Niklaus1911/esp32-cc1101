idf_component_register(
    SRCS "bridge_control.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bridge_events network_wifi ota_update rf_automation rf_codec rf_ook rf_signals rf_storage
)
