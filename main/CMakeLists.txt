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
        "command_exec.cpp"
        "http_status.cpp"
        "http_ota.cpp"
        "http_config.cpp"
        "mcp_server.cpp"
        "provisioning.cpp"
        "diag_log.cpp"
        "diag_crash.cpp"
        "safe_mode.cpp"
        "heap_trend.cpp"
        "config_blob.cpp"
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
        espcoredump
)

# ── Warning policy for THIS component only ──
# main/logic/ was never the exposed half: it is compiled by the host mock build with
# -Wall -Wextra -Werror and covered by test/test_logic.cpp. main/*.cpp was — it is where every
# shipped crash actually happened, and it carried no warning contract of its own.
#
#   -Werror=return-type    a function that falls off its end returns garbage; on a bool health
#                          check that reads as "healthy" and silently changes behaviour.
#   -Werror=unused-result  nvs_storage.hpp's setters are [[nodiscard]]. A dropped NVS write is
#                          SILENT (a full partition, worn flash), and this firmware persists the
#                          things it cannot rebuild — the private key, the VIN, the safe-mode boot
#                          counter. `save_str(...)` on its own still compiles and still lies; what
#                          [[nodiscard]] buys is that the ignored result is a BUILD error, which it
#                          only is if this warning class is fatal.
#
# NOT -Werror=format, deliberately, and this is the one that would look like an oversight: the
# root CMakeLists.txt appends a global -Wno-error=format because yoziru/tesla-ble prints uint32_t
# with %u (which is `unsigned long` on xtensa). A PRIVATE -Werror=format here would re-arm the
# class for our own sources only — which is the right end state — but our format strings have
# never been compiled under it on all four targets, int32_t is `long int` on xtensa and plain
# `int` on riscv/host, and scripts/run-mock-tests.sh compiles NONE of main/*.cpp, so nothing
# available in a cloud session can tell whether that turns the build red. Converting the call
# sites to PRIu32/PRId32 first, then arming it, is a change of its own with a real build behind it.
#
# PRIVATE on ${COMPONENT_LIB} rather than a CONFIG_COMPILER_* key in sdkconfig.defaults, for two
# independent reasons: a Kconfig key is global, so it would hold ESP-IDF itself and the managed
# components (tesla-ble, mdns, mqtt) to a contract that is not ours to demand and that breaks on
# any bump; and sdkconfig.defaults is part of CI's ccache key, so a diagnostic-only change there
# would discard every cached object it cannot possibly invalidate.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Werror=return-type
    -Werror=unused-result
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
