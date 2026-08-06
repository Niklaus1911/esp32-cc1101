idf_component_register(
    SRCS "platform_board.cpp" "platform_board_policy.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_system heap
)
