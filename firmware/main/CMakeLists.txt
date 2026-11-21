idf_component_register(
    SRCS "app_main.c" "settings.c" "bms.c" "net.c" "dns_server.c" "web.c" "mqtt.c" "status_led.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES seplos esp_driver_uart esp_driver_gpio nvs_flash esp_wifi esp_netif esp_event
                  esp_http_server app_update esp_timer esp_eth esp_app_format lwip esp_system mbedtls)

# Gzip the single-page web UI at build time and link it into the firmware.
idf_build_get_property(python PYTHON)
set(WWW_SRC "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html")
set(WWW_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${WWW_GZ}"
    COMMAND "${python}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py" "${WWW_SRC}" "${WWW_GZ}"
    DEPENDS "${WWW_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py"
    VERBATIM)
add_custom_target(seplos_www DEPENDS "${WWW_GZ}")
target_add_binary_data(${COMPONENT_LIB} "${WWW_GZ}" BINARY DEPENDS seplos_www)
