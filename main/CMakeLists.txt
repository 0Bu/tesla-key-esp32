idf_component_register(
    SRCS
        "main.cpp"
        "ble_client.cpp"
        "nvs_storage.cpp"
        "vehicle_ctrl.cpp"
        "vehicle_commands.cpp"
        "vehicle_telemetry.cpp"
        "vehicle_pairing.cpp"
        "http_server.cpp"
        "http_common.cpp"
        "http_api.cpp"
        "http_status.cpp"
        "http_events.cpp"
        "http_ota.cpp"
        "http_config.cpp"
        "mcp_server.cpp"
        "provisioning.cpp"
        "diag_log.cpp"
        "ota_update.cpp"
        "mqtt_ha.cpp"
        "syslog.cpp"
        "display.cpp"
        "led_status.cpp"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
        "${CMAKE_CURRENT_BINARY_DIR}/setup.html.gz"
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

# Assemble + pre-compress the web UI at build time. The sources are split for edit
# locality — www/index.html (markup with inline markers), www/style.css, www/app.js —
# and inline_assets.cmake splices them back into the ONE self-contained page the device
# serves (byte-identical to the former monolithic index.html). Browsers always accept
# gzip, so shipping the page pre-gzipped (~13 KB vs 41 KB) cuts the bytes on the wire
# ~3× — the single biggest lever for first-paint over a high-latency WiFi link.
# Regenerated on every build from the source files, so the served page never goes stale
# (the UI changes with each flash/OTA). The EMBED_FILES path above points at the final
# artifact; the symbols are _binary_index_html_gz_{start,end} (see handle_index in
# http_status.cpp).
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/index_inlined.html"
    COMMAND ${CMAKE_COMMAND}
            -DHTML=${CMAKE_CURRENT_SOURCE_DIR}/www/index.html
            -DCSS=${CMAKE_CURRENT_SOURCE_DIR}/www/style.css
            -DJS=${CMAKE_CURRENT_SOURCE_DIR}/www/app.js
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/index_inlined.html
            -P ${CMAKE_CURRENT_SOURCE_DIR}/www/inline_assets.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/www/style.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/www/app.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/www/inline_assets.cmake"
    COMMENT "Inlining web UI assets (index.html + style.css + app.js)"
    VERBATIM)
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
    COMMAND gzip -9 -n -c "${CMAKE_CURRENT_BINARY_DIR}/index_inlined.html"
            > "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/index_inlined.html"
    COMMENT "Gzipping web UI (index_inlined.html -> index.html.gz)"
    VERBATIM)
add_custom_target(gen_index_gz DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_dependencies(${COMPONENT_LIB} gen_index_gz)

# Pre-compress the captive-portal setup page the same way. It's a static source (no asset
# splicing), so just gzip main/www/setup.html at build time and serve it with
# Content-Encoding: gzip (see form_get in provisioning.cpp). ~3.6 KB saved on the wire and
# in flash. Symbols: _binary_setup_html_gz_{start,end}.
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/setup.html.gz"
    COMMAND gzip -9 -n -c "${CMAKE_CURRENT_SOURCE_DIR}/www/setup.html"
            > "${CMAKE_CURRENT_BINARY_DIR}/setup.html.gz"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/setup.html"
    COMMENT "Gzipping setup portal (www/setup.html -> setup.html.gz)"
    VERBATIM)
add_custom_target(gen_setup_gz DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/setup.html.gz")
add_dependencies(${COMPONENT_LIB} gen_setup_gz)
