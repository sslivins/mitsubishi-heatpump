idf_component_register(
    SRCS "main.cpp"
         "wifi_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_driver_i2c
             driver esp_timer esp_app_format
             cn105 m5pm1 hvac_mqtt
)
