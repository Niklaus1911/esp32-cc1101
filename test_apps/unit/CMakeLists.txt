cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/platform_nvs"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/network_wifi"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/network_mdns"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/network_mqtt"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/bridge_events"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/bridge_control"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_maintenance"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/ota_update"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/cc1101"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_codec"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_automation"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_console"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_ook"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_storage"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_signals"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_auth"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/web_ui"
)
set(COMPONENTS main platform_nvs network_wifi network_mdns network_mqtt app_maintenance ota_update cc1101 bridge_control bridge_events rf_automation rf_codec rf_console rf_ook rf_signals rf_storage web_auth web_ui unity)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_cc1101_unit_tests)
