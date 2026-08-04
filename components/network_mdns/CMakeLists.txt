idf_component_register(
    SRCS "network_mdns.cpp" "network_mdns_policy.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format esp_timer mdns network_wifi
)
