cmake_minimum_required(VERSION 3.22)
project(esp32_cc1101_host_tests LANGUAGES CXX)

enable_testing()
find_program(BASH_EXECUTABLE bash REQUIRED)

add_executable(rf_host_tests
    host_tests.cpp
    ../components/network_wifi/network_wifi_config.cpp
    ../components/network_wifi/network_wifi_state.cpp
    ../components/ota_update/ota_update_policy.cpp
    ../components/rf_automation/rf_automation_engine.cpp
    ../components/rf_codec/rf_codec.cpp
    ../components/rf_console/rf_console_format.cpp
    ../components/rf_console/rf_console_parse.cpp
    ../components/rf_storage/rf_storage_format.cpp
    ../components/rf_storage/rf_storage_rule_format.cpp
)
target_include_directories(rf_host_tests PRIVATE
    ../components/network_wifi/include
    ../components/ota_update/include
    ../components/rf_automation/include
    ../components/rf_codec/include
    ../components/rf_console/include
    ../components/rf_storage/include
)
target_compile_features(rf_host_tests PRIVATE cxx_std_17)
target_compile_options(rf_host_tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME rf_host_tests COMMAND rf_host_tests)
add_test(NAME push_ota_script_tests
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_push_ota.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
