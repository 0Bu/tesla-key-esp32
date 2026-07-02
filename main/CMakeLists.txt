idf_component_register(
    SRCS
        "main.cpp"
        "ble_client.cpp"
        "nvs_storage.cpp"
        "vehicle_ctrl.cpp"
        "http_server.cpp"
        "mcp_server.cpp"
        "provisioning.cpp"
        "diag_log.cpp"
        "ota_update.cpp"
        "mqtt_ha.cpp"
    INCLUDE_DIRS
        "."
    EMBED_TXTFILES
        "www/setup.html"
    EMBED_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_app_format
        esp_https_ota
        app_update
        esp_http_client
        esp-tls
        bt
        json
        lwip
        driver
        mqtt
        esp_timer
        bootloader_support
)

# Pre-compress the web UI at build time. Browsers always accept gzip, so shipping the
# page pre-gzipped (~13 KB vs 41 KB) cuts the bytes on the wire ~3× — the single biggest
# lever for first-paint over a high-latency WiFi link. Regenerated on every build from the
# source HTML, so the served page never goes stale (the UI changes with each flash/OTA).
# The EMBED_FILES path above points at this generated artifact; the symbols are
# _binary_index_html_gz_{start,end} (see handle_index in http_server.cpp).
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
    COMMAND gzip -9 -n -c "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html"
            > "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html"
    COMMENT "Gzipping web UI (index.html -> index.html.gz)"
    VERBATIM)
add_custom_target(gen_index_gz DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_dependencies(${COMPONENT_LIB} gen_index_gz)
