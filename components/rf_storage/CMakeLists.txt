idf_component_register(
    SRCS "rf_storage.cpp" "rf_storage_format.cpp"
    INCLUDE_DIRS "include"
    REQUIRES rf_codec
    PRIV_REQUIRES nvs_flash
)
