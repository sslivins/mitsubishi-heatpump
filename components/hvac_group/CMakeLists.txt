idf_component_register(
    SRCS "group_config.cpp" "group_proto.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls esp_hw_support esp_timer
)
