idf_component_register(
    SRCS "network_wifi.cpp" "network_wifi_config.cpp" "network_wifi_state.cpp" "network_wifi_storage.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_event esp_netif esp_timer esp_wifi platform_nvs
    PRIV_REQUIRES nvs_flash
)
