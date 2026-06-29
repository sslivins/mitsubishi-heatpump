idf_component_register(
    SRCS "hvac_mqtt.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mqtt json cn105 nvs_flash
)
