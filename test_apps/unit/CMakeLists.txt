cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/platform_nvs"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/network_wifi"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_maintenance"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/ota_update"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/cc1101"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_codec"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_automation"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_console"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_ook"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_storage"
)
set(COMPONENTS main platform_nvs network_wifi app_maintenance ota_update cc1101 rf_automation rf_codec rf_console rf_ook rf_storage unity)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_cc1101_unit_tests)
