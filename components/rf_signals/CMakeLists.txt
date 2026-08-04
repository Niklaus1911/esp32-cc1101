idf_component_register(
    SRCS "rf_signals.cpp" "rf_signals_match.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bridge_events esp_timer rf_automation rf_ook rf_storage
)
