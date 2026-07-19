# Gzip the web assets so they can be embedded in flash.
# execute_process runs at configure time to guarantee the .gz exists for the
# initial EMBED_FILES resolution; the add_custom_command rules below re-gzip at
# build time whenever the source HTML (or the gzip script) changes, so editing a
# page and running `idf.py build` always re-embeds the latest markup.
set(WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
foreach(page index portal)
    execute_process(
        COMMAND python ${WEB_DIR}/gzip_html.py
                ${WEB_DIR}/${page}.html ${WEB_DIR}/${page}.html.gz
    )
endforeach()

idf_component_register(
    SRCS "main.cpp"
         "wifi_manager.cpp"
         "web_ui.cpp"
         "dns_server.cpp"
         "ota.cpp"
         "auth_manager.cpp"
         "diag.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html.gz"
                "web/portal.html.gz"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_driver_i2c
             driver esp_timer esp_app_format esp_http_server json lwip
             app_update esp_https_ota esp_http_client mbedtls
             cn105 m5pm1 hvac_mqtt hvac_group
)

# Re-gzip at build time when the source HTML or the gzip script changes.
foreach(page index portal)
    add_custom_command(
        OUTPUT ${WEB_DIR}/${page}.html.gz
        COMMAND python ${WEB_DIR}/gzip_html.py
                ${WEB_DIR}/${page}.html ${WEB_DIR}/${page}.html.gz
        DEPENDS ${WEB_DIR}/${page}.html ${WEB_DIR}/gzip_html.py
        COMMENT "Gzipping web/${page}.html"
        VERBATIM
    )
endforeach()
add_custom_target(web_assets DEPENDS
    ${WEB_DIR}/index.html.gz ${WEB_DIR}/portal.html.gz)
add_dependencies(${COMPONENT_LIB} web_assets)
