idf_component_register(
    SRCS "rf_storage.cpp" "rf_storage_format.cpp" "rf_storage_rule_format.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES rf_codec
    PRIV_REQUIRES nvs_flash
)
