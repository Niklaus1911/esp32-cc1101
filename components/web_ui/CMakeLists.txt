idf_component_register(
    SRCS "web_ui.cpp" "web_api.cpp" "web_form.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES bridge_control bridge_events esp_http_server esp_system esp_timer heap network_wifi ota_update rf_automation rf_ook rf_signals rf_storage
    EMBED_TXTFILES "assets/index.html" "assets/app.css" "assets/app.js"
)
