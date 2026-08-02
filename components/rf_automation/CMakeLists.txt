idf_component_register(
    SRCS "rf_automation.cpp" "rf_automation_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES rf_ook rf_storage
    PRIV_REQUIRES esp_timer
)
