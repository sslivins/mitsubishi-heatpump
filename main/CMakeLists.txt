# Gzip the web assets at configure time so they can be embedded in flash.
execute_process(
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html.gz
)
execute_process(
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py
            ${CMAKE_CURRENT_SOURCE_DIR}/web/portal.html
            ${CMAKE_CURRENT_SOURCE_DIR}/web/portal.html.gz
)

idf_component_register(
    SRCS "main.cpp"
         "wifi_manager.cpp"
         "web_ui.cpp"
         "dns_server.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html.gz"
                "web/portal.html.gz"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_driver_i2c
             driver esp_timer esp_app_format esp_http_server json lwip
             cn105 m5pm1 hvac_mqtt
)
