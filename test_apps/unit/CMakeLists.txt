cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/cc1101"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_codec"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_automation"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_console"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_ook"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rf_storage"
)
set(COMPONENTS main cc1101 rf_automation rf_codec rf_console rf_ook rf_storage unity)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_cc1101_unit_tests)
