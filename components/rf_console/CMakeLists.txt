idf_component_register(
    SRCS "rf_console.cpp" "rf_console_parse.cpp"
    INCLUDE_DIRS "include"
    REQUIRES console rf_ook cc1101
)
