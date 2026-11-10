# ESP-IDF feature matrix

This is the source-linked inventory of ESP-IDF and official Espressif features considered by
`tesla-key-esp32`. It separates APIs the firmware calls directly, linked backends and managed
components from features that were evaluated and are not used. It also records the ULP and
LP-core assessment. Behavior, trust boundaries and build contracts stay with their owners:
[Architecture](ARCHITECTURE.md), [Security](SECURITY.md) and [Development](DEVELOPMENT.md).
[FEATURES](FEATURES.md) locates mechanisms by concern; this matrix inventories ESP-IDF components,
configuration and adoption decisions.

`esp-idf-toolchain.txt` pins **ESP-IDF v6.1** and its container digest. Every per-target
`dependencies.lock.<target>` resolves exact **ESP-IDF 6.1.0**, and `main/idf_component.yml`
requires `idf: ">=6.1,<7.0"`. The firmware targets **esp32, esp32s3, esp32c3 and esp32c6**.
Documentation links use the ESP-IDF `v6.1` channel. A generic feature links the esp32 page; a
target-specific feature links the page of its target. Official components extracted from ESP-IDF
link to the Espressif Component Registry.

<!-- esp-idf-matrix:metadata {"targets":["esp32","esp32s3","esp32c3","esp32c6"],"version":"6.1.0","idf_floor":">=6.1,<7.0"} -->

## Scope and status

The **used** table covers the current firmware surface. That surface is the explicit CMake
`REQUIRES`, the direct managed dependencies, and every active assignment in the five application
`sdkconfig.defaults*` files and the five maintenance overlays. It also covers the ESP-IDF headers
included under `main/`.

The **evaluated** table is curated. It cannot prove that it names every feature in the upstream
ESP-IDF universe. [What the gate proves](#what-the-gate-proves) describes the offline check.

| Status | Meaning |
|---|---|
| ✅ Direct | Firmware code calls the API directly or enables the capability explicitly |
| 🧩 Backend | Linked or configured below another API; firmware code does not call it directly |
| 📦 Managed | Direct dependency resolved by the Component Manager |
| 🔬 Evaluate | A bounded change could plausibly improve the project |
| ⏸ Conditional | Useful only after a product, security or hardware requirement changes |
| 🚫 Not planned | Available, but no current use case justifies its cost |
| ⛔ Unsupported | Not native on the named target hardware or current toolchain contract |

**Targets** means all four targets unless the row names a subset.

## Used ESP-IDF and Espressif features

The **Component/source** column names the exact CMake component or managed dependency key.

<!-- esp-idf-matrix:used:start -->
| ID | Feature | Status | Targets | Component/source | Project use | Evidence | Official documentation |
|---|---|---|---|---|---|---|---|
| U01 | Build system, four targets and reproducible application metadata | ✅ Direct | all | IDF core | One `main` component, gnu++17 with C++ exceptions, 4 MiB flash, per-target component locks, an embedded gzipped web UI, silent assertions, an esp32 ECO3 floor, reproducible metadata, and an application role plus a default-off config-repair role | [`CMakeLists.txt`](../CMakeLists.txt), [`main/CMakeLists.txt`](../main/CMakeLists.txt), [`sdkconfig.defaults`](../sdkconfig.defaults), [`esp-idf-toolchain.txt`](../esp-idf-toolchain.txt), [`DEVELOPMENT.md`](DEVELOPMENT.md) | [Build system](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/build-system.html) |
| U02 | FreeRTOS kernel | ✅ Direct | all | IDF core | Fixed-priority tasks, queues, semaphores, event groups and `portMUX` critical sections. Also an 8 KiB main task, an end-of-stack watchpoint and stack high-water sampling | [`task_config.hpp`](../main/task_config.hpp), [`rtos_guard.hpp`](../main/rtos_guard.hpp), [`stack_watch.cpp`](../main/stack_watch.cpp), [`ARCHITECTURE.md`](ARCHITECTURE.md#concurrency-normative-contract) | [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/freertos.html) |
| U03 | System, heap, logging, MAC and ROM APIs | ✅ Direct | all | IDF core | Restart and reset reason, the INTERNAL largest-free-block measure, logging and the bounded diagnostic ring. Also MAC-derived identity, the application descriptor, and ROM microsecond delays for board probes | [`main.cpp`](../main/main.cpp), [`heap_supervisor.cpp`](../main/heap_supervisor.cpp), [`http_status.cpp`](../main/http_status.cpp), [`diag_log.cpp`](../main/diag_log.cpp), [`board.cpp`](../main/board.cpp) | [System API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/misc_system_api.html), [memory allocation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/mem_alloc.html), [logging](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/log.html) |
| U04 | Uninitialized DRAM retention (`.noinit`) | ✅ Direct | all | IDF core | The 24-hour heap trend can survive resets while its bytes remain intact. Adoption checks magic, layout fingerprint and CRC. OTA retention also needs compatible placement of the retained object ([placement](#retention-placement)) | [`heap_trend.cpp`](../main/heap_trend.cpp), [`logic/heap_history.hpp`](../main/logic/heap_history.hpp) | [Memory types](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/memory-types.html) |
| U05 | High-resolution timer and Task Watchdog | ✅ Direct | all | `esp_timer` | Monotonic deadlines, BLE and OTA budgets, and the timestamp read under the mailbox mux. The Task Watchdog runs at 60 s with panic and explicit subscriptions for blocking workers | [`task_watchdog_subscription.hpp`](../main/task_watchdog_subscription.hpp), [`vehicle_telemetry.cpp`](../main/vehicle_telemetry.cpp), [`mqtt_ha.cpp`](../main/mqtt_ha.cpp), [`sdkconfig.defaults`](../sdkconfig.defaults) | [ESP Timer](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/esp_timer.html), [watchdogs](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/wdts.html) |
| U06 | Random numbers and startup entropy | ✅ Direct + 🧩 Backend | all | `bootloader_support`, IDF core | SAR-ADC entropy (`bootloader_random_enable`) stays active through key load and first-boot generation, before the radios start. PSA draws use the hardware RNG; the config-repair challenge uses `esp_fill_random` | [`main.cpp`](../main/main.cpp), [`config_repair.cpp`](../main/config_repair.cpp) | [Random number generation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/random.html) |
| U07 | Non-volatile storage | ✅ Direct | all | `nvs_flash` | Secret material: WiFi, VIN, MQTT settings, private key and sessions. Also the safe-mode boot counter | [`nvs_storage.hpp`](../main/nvs_storage.hpp), [`nvs_storage.cpp`](../main/nvs_storage.cpp), [`SECURITY.md`](SECURITY.md#nvs-retention) | [NVS Flash](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/storage/nvs_flash.html) |
| U08 | Custom partition table and Partition API | ✅ Direct | all | IDF core | Immutable 4 MiB dual-OTA table with `nvs`, `otadata`, `phy_init`, `coredump`, `ota_0` and `ota_1`. Raw coredump reads serve the summary and the stream | [`partitions.csv`](../partitions.csv), [`diag_crash.cpp`](../main/diag_crash.cpp), [`http_status.cpp`](../main/http_status.cpp) | [Partition tables](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/partition-tables.html), [Partition API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/storage/partition.html) |
| U09 | Core dump to flash and panic diagnostics | ✅ Direct | all | `espcoredump` | ELF dump into the 48 KiB partition, with SDK boot checks and DRAM capture off. A bounded boot summary and the full 64-hex ELF SHA, `/coredump` export and explicit erase. Xtensa and RISC-V exception frames plus `esp_memory_utils` checks for crash records | [`diag_crash.cpp`](../main/diag_crash.cpp), [`coredump_hash.hpp`](../main/coredump_hash.hpp), [`http_status.cpp`](../main/http_status.cpp), [`ARCHITECTURE.md`](ARCHITECTURE.md#crash-diagnostics) | [Core dump](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/core_dump.html), [fatal errors](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/fatal-errors.html) |
| U10 | WiFi station and SoftAP | ✅ Direct | all | `esp_wifi` | Station with reconnect and the open setup AP; `WIFI_PS_MIN_MODEM` modem sleep, which BLE coexistence requires. No Enterprise, SAE-PK, WPA3-SoftAP or OWE. Static RX buffers are cut to 6 on esp32, esp32s3 and esp32c3; WiFi IRAM optimization is off on esp32s3 and esp32c3; esp32c3 TX power is capped at 10 dBm | [`net.cpp`](../main/net.cpp), [`provisioning.cpp`](../main/provisioning.cpp), [`sdkconfig.defaults`](../sdkconfig.defaults), [`USER_GUIDE.md`](USER_GUIDE.md#hardware) | [Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/network/esp_wifi.html), [RF coexistence](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/coexist.html) |
| U11 | Event loop and network interfaces | ✅ Direct | all | `esp_event`, `esp_netif` | Shared WiFi and Ethernet lifecycle, IP events and active-interface selection | [`net.cpp`](../main/net.cpp), [`net.hpp`](../main/net.hpp), [`main.cpp`](../main/main.cpp), [`ARCHITECTURE.md`](ARCHITECTURE.md#wifi--lan-connectivity) | [Event loop](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/esp_event.html), [ESP-NETIF](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/network/esp_netif.html) |
| U12 | lwIP sockets, ICMP echo and SNTP | ✅ Direct | all | `lwip` | HTTP and setup-portal sockets, UDP syslog with an advisory ARP hint, and a gateway ping probe. SNTP runs through a guarded `sntp_sync_time` hook. IPv4 only, 16 sockets, 8×MSS TCP windows | [`syslog.cpp`](../main/syslog.cpp), [`ping_probe.hpp`](../main/ping_probe.hpp), [`net.cpp`](../main/net.cpp), [`provisioning.cpp`](../main/provisioning.cpp), [`main.cpp`](../main/main.cpp) | [lwIP](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/lwip.html), [ICMP echo](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/protocols/icmp_echo.html), [system time](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/system_time.html) |
| U13 | External W5500 Ethernet over SPI | ✅ Direct + 📦 Managed | esp32s3 | `esp_eth`, `esp_driver_spi`, `espressif/w5500` | Optional wired transport, probed at boot for two verified boards. A lease during the initial Ethernet wait avoids starting WiFi; a later lease closes and deinitializes an AP-only setup portal | [`net.cpp`](../main/net.cpp), [`main.cpp`](../main/main.cpp), [`idf_component.yml`](../main/idf_component.yml), [`sdkconfig.defaults.esp32s3`](../sdkconfig.defaults.esp32s3), [`USER_GUIDE.md`](USER_GUIDE.md#wired-networking-optional-esp32s3) | [Ethernet](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_eth.html), [W5500 component](https://components.espressif.com/components/espressif/w5500) |
| U14 | HTTP server | ✅ Direct | all | `esp_http_server` | Web UI, the evcc-compatible API, MCP and the setup portal. Bounded headers and URI; OTA checks run through the nonblocking work queue | [`http_server.cpp`](../main/http_server.cpp), [`http_common.cpp`](../main/http_common.cpp), [`mcp_server.cpp`](../main/mcp_server.cpp), [`provisioning.cpp`](../main/provisioning.cpp), [`ota_update.cpp`](../main/ota_update.cpp) | [HTTP server](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/protocols/esp_http_server.html) |
| U15 | ESP-TLS, certificate bundle and HTTP parser | ✅ Direct + 🧩 Backend | all | `esp-tls`, `http_parser` | OTA transport over ESP-TLS with incremental `http_parser` parsing. Bundled CA roots verify OTA, MQTTS and the broker probe | [`ota_transport.cpp`](../main/ota_transport.cpp), [`ota_http.hpp`](../main/ota_http.hpp), [`mqtt_ha.cpp`](../main/mqtt_ha.cpp), [`http_config.cpp`](../main/http_config.cpp) | [ESP-TLS](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/protocols/esp_tls.html), [certificate bundle](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/protocols/esp_crt_bundle.html) |
| U16 | MQTT client | ✅ Direct + 📦 Managed | all | `espressif/mqtt` | Home Assistant discovery, retained state, LWT availability and MQTTS without cleartext fallback | [`mqtt_ha.cpp`](../main/mqtt_ha.cpp), [`http_config.cpp`](../main/http_config.cpp), [`idf_component.yml`](../main/idf_component.yml) | [ESP-MQTT component](https://components.espressif.com/components/espressif/mqtt) |
| U17 | Multicast DNS | ✅ Direct + 📦 Managed | all | `espressif/mdns` | `_http._tcp` advertisement under the device hostname | [`main.cpp`](../main/main.cpp), [`http_server.cpp`](../main/http_server.cpp), [`idf_component.yml`](../main/idf_component.yml) | [mDNS component](https://components.espressif.com/components/espressif/mdns) |
| U18 | cJSON | ✅ Direct + 📦 Managed | all | `espressif/cjson` | Bounded JSON builders and input validation for HTTP, MCP, MQTT and the OTA manifest | [`json_builder.hpp`](../main/json_builder.hpp), [`json_input_validation.hpp`](../main/json_input_validation.hpp), [`ota_manifest.hpp`](../main/ota_manifest.hpp) | [cJSON component](https://components.espressif.com/components/espressif/cjson) |
| U19 | OTA, app-image format, signed-app verification and rollback | ✅ Direct + 🧩 Backend | all | `app_update`, `esp_app_format`, `bootloader_support` | Raw inactive-slot writer (`esp_ota_begin`/`write`/`end`, `esp_ota_set_boot_partition`) with header checks. Rollback is driven by the health gate. RSA app signatures are checked on update, without hardware Secure Boot, and are never applied during the build | [`ota_writer.cpp`](../main/ota_writer.cpp), [`ota_update.cpp`](../main/ota_update.cpp), [`main.cpp`](../main/main.cpp), [`ARCHITECTURE.md`](ARCHITECTURE.md#ota-self-update), [`SECURITY.md`](SECURITY.md#signed-ota-images) | [OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/ota.html), [application image format](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/app_image_format.html), [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/security/secure-boot-v2.html) |
| U20 | Mbed TLS 4 and PSA Crypto | ✅ Direct + 🧩 Backend | all | `mbedtls` | Size-optimized, client-only TLS with trimmed curves and X.509 parsers. The P-256 key is imported to PSA through `mbedtls_pk`, and PSA SHA-256 hashes the core dump. The tesla-ble crypto bindings are ported to PSA | [`vehicle_pairing.cpp`](../main/vehicle_pairing.cpp), [`coredump_hash.hpp`](../main/coredump_hash.hpp), [`ota_transport.cpp`](../main/ota_transport.cpp), [ADR-0002](adr/0002-idf6-mbedtls4-crypto-seam.md) | [Mbed TLS](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/protocols/mbedtls.html) |
| U21 | Bluetooth LE with NimBLE | ✅ Direct | all | `bt` | Central and observer roles, one connection, a 247-byte preferred ATT MTU and an 8 KiB host task. No Security Manager, because Tesla crypto runs at the application layer. Controller trims per target: DTM, advertising and controller security on esp32s3/esp32c3, one BTDM connection on esp32 | [`ble_client.cpp`](../main/ble_client.cpp), [`ble_client.hpp`](../main/ble_client.hpp), [`sdkconfig.defaults`](../sdkconfig.defaults), [ADR-0005](adr/0005-tesla-ble-seam.md) | [NimBLE](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/bluetooth/nimble/index.html) |
| U22 | GPIO driver | ✅ Direct | esp32s3 | `esp_driver_gpio` | Board identification samples SD pull-ups on the T-Dongle-S3 | [`board.cpp`](../main/board.cpp) | [GPIO](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/peripherals/gpio.html) |
| U23 | SPI master driver | ✅ Direct | esp32s3 | `esp_driver_spi` | The ESP32-S3 W5500 bus (U13) | [`net.cpp`](../main/net.cpp) | [SPI master](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/spi_master.html) |
| U24 | USB Serial/JTAG console | ✅ Direct | esp32s3, esp32c3, esp32c6 | IDF core | Primary console on the native USB port; esp32 keeps the UART console | [`sdkconfig.defaults.esp32s3`](../sdkconfig.defaults.esp32s3), [`sdkconfig.defaults.esp32c3`](../sdkconfig.defaults.esp32c3), [`sdkconfig.defaults.esp32c6`](../sdkconfig.defaults.esp32c6) | [USB Serial/JTAG console](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/usb-serial-jtag-console.html) |
| U25 | Config-repair maintenance transport | ✅ Direct | all (maintenance role) | `esp_driver_uart`, `esp_driver_usb_serial_jtag` (not on esp32) | The default-off configuration-only role uses raw UART0 or USB Serial/JTAG driver exchange (`hal/uart_ll.h`), with consoles and logs disabled. The application role never links it | [`main/CMakeLists.txt`](../main/CMakeLists.txt), [`config_repair_console.cpp`](../main/config_repair_console.cpp), [`maintenance/config-repair/sdkconfig.defaults`](../maintenance/config-repair/sdkconfig.defaults), [`CONFIG_RECOVERY.md`](CONFIG_RECOVERY.md) | [UART](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/peripherals/uart.html), [USB Serial/JTAG console](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/usb-serial-jtag-console.html) |
| U26 | tesla-ble (non-Espressif managed dependency) | 📦 Managed | all | `yoziru/tesla-ble` | Vehicle protocol framing, sessions and nanopb messages over NimBLE and PSA. Pinned to v6.4.0 with a fail-closed ordered patch series. Listed so the managed-dependency inventory is complete | [`idf_component.yml`](../main/idf_component.yml), [`vehicle_ctrl.hpp`](../main/vehicle_ctrl.hpp), [`patches/tesla-ble/`](../patches/tesla-ble/), [ADR-0005](adr/0005-tesla-ble-seam.md) | [tesla-ble](https://github.com/yoziru/tesla-ble) |
<!-- esp-idf-matrix:used:end -->

## Evaluated but not used

These rows record deliberate decisions, not promises of future implementation. A status change must
move the feature into the used table and add real source evidence.

<!-- esp-idf-matrix:evaluated:start -->
| ID | Feature | Status | Targets | Decision | Checked against | Official documentation |
|---|---|---|---|---|---|---|
| N01 | ULP-FSM, ULP-RISC-V and LP core | 🚫 Not planned; ⛔ Unsupported on esp32c3 | esp32, esp32s3, esp32c6 | No coprocessor can reach the BLE radio, the NimBLE session or the network, so it cannot keep the vehicle feed during OTA or publish MQTT. esp32c3 has none. See the [assessment](#ulp-and-lp-core-assessment) | [`ble_client.cpp`](../main/ble_client.cpp), [`mqtt_ha.cpp`](../main/mqtt_ha.cpp), [`ota_update.cpp`](../main/ota_update.cpp) | [ULP-FSM](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/ulp.html), [ULP-RISC-V](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/ulp-risc-v.html), [LP core](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/api-reference/system/ulp-lp-core.html) |
| N02 | RTC retention memory (`RTC_NOINIT_ATTR`) for a warm-restart telemetry snapshot | 🔬 Evaluate | all | The cross-target way to keep last-known vehicle telemetry across an OTA restart without ULP or flash writes. It needs a contract change and on-device proof; see the [snapshot design](#warm-restart-snapshot-n02) | [`mqtt_ha.cpp`](../main/mqtt_ha.cpp), [`logic/mqtt_state_lifecycle.hpp`](../main/logic/mqtt_state_lifecycle.hpp), [`logic/wake_poll.hpp`](../main/logic/wake_poll.hpp) | [Memory types](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/memory-types.html) |
| N03 | Power management, light sleep and deep sleep | 🚫 Not planned | all | The BLE link, the HTTP API for evcc and MQTT must stay reachable. There is no power-management, light-sleep or deep-sleep configuration; only the WiFi modem sleep in U10. Vehicle sleep is a separate concern | [`sdkconfig.defaults`](../sdkconfig.defaults), [`vehicle_telemetry.cpp`](../main/vehicle_telemetry.cpp) | [Sleep modes](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/sleep_modes.html), [power management](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/power_management.html) |
| N04 | `esp_https_ota` and `esp_http_client` | 🚫 Not planned | all | The OTA coordinator owns a nonrenewable budget, memory admission before each allocation, an isolated network task and incremental parsing over ESP-TLS. It does not use these layers | [`ota_transport.cpp`](../main/ota_transport.cpp), [`ARCHITECTURE.md`](ARCHITECTURE.md#ota-self-update) | [ESP HTTPS OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/esp_https_ota.html), [HTTP client](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/protocols/esp_http_client.html) |
| N05 | Hardware Secure Boot v2 and Flash Encryption | ⏸ Conditional | all | eFuse effects are permanent. The reviewed contract is signed apps without hardware Secure Boot plus a production key pin, and key or pin changes need a USB fleet migration | [`sdkconfig.defaults`](../sdkconfig.defaults), [`SECURITY.md`](SECURITY.md) | [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/security/secure-boot-v2.html), [Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/security/flash-encryption.html) |
| N06 | NVS encryption | ⏸ Conditional | all | It would protect keys and sessions at rest. It needs per-device key provisioning and an explicitly approved NVS migration with backup and rollback | [`SECURITY.md`](SECURITY.md#nvs-retention), [`partitions.csv`](../partitions.csv) | [NVS encryption](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/storage/nvs_encryption.html) |
| N07 | Network provisioning and Protocomm | 🚫 Not planned | all | The setup AP and the transactional `provision.py` path already cover SSID, passphrase and VIN without a companion app | [`provisioning.cpp`](../main/provisioning.cpp), [`provision.py`](../provision.py) | [Provisioning](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/provisioning/index.html) |
| N08 | HTTPS server and HTTP WebSockets | ⏸ Conditional | all | TLS alone does not authenticate a LAN client and adds pressure on the INTERNAL largest block. The UI deliberately polls instead of using broadcast sends | [`http_server.cpp`](../main/http_server.cpp), [`ARCHITECTURE.md`](ARCHITECTURE.md#web-ui) | [HTTPS server](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/protocols/esp_https_server.html) |
| N09 | PSRAM | ⏸ Conditional; ⛔ Unsupported on esp32c3, esp32c6 | esp32, esp32s3 | One image per target serves boards without PSRAM. WiFi/lwIP buffers could move to PSRAM only on boards that have it, so the INTERNAL largest-block budget must hold without it | [`USER_GUIDE.md`](USER_GUIDE.md#hardware), [`sdkconfig.defaults`](../sdkconfig.defaults) | [External RAM](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/external-ram.html) |
| N10 | `esp_lcd`, `led_strip`, RMT and LEDC | 🚫 Not planned | all | The firmware reports status over HTTP and MQTT. | Reassess if a supported feature needs a hardware indicator. | [Peripheral API index](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/peripherals/index.html) |
| N11 | ESP Console (REPL) | 🚫 Not planned | all | Config-repair uses one bounded raw exchange; a REPL would widen the pre-HTTP recovery surface | [`config_repair_console.cpp`](../main/config_repair_console.cpp), [`CONFIG_RECOVERY.md`](CONFIG_RECOVERY.md) | [Console](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/console.html) |
| N12 | FATFS, SPIFFS, LittleFS and Wear Levelling | 🚫 Not planned | all | The UI is one embedded gzipped page and settings live in NVS. The immutable partition table leaves no file-system partition | [`partitions.csv`](../partitions.csv), [`main/CMakeLists.txt`](../main/CMakeLists.txt) | [File-system considerations](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/file-system-considerations.html) |
| N13 | ESP-NOW, Wi-Fi Mesh, SmartConfig and DPP | 🚫 Not planned | all | The product is one LAN bridge, not a peer network, and onboarding already has a setup path | [`net.cpp`](../main/net.cpp), [`provisioning.cpp`](../main/provisioning.cpp) | [ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/network/esp_now.html) |
| N14 | Bluetooth Classic | 🚫 Not planned on esp32; ⛔ Unsupported on esp32s3, esp32c3, esp32c6 | esp32 | The Tesla protocol is BLE, and the esp32 BTDM controller is configured for one BLE connection | [`sdkconfig.defaults`](../sdkconfig.defaults), [`sdkconfig.defaults.esp32`](../sdkconfig.defaults.esp32) | [Bluetooth API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/bluetooth/index.html) |
| N15 | Internal Ethernet EMAC | 🚫 Not planned on esp32; ⛔ Unsupported on esp32s3, esp32c3, esp32c6 | esp32 | No supported board uses it; wired networking uses the esp32s3 W5500 path (U13) | [`net.cpp`](../main/net.cpp), [`USER_GUIDE.md`](USER_GUIDE.md#wired-networking-optional-esp32s3) | [Ethernet](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/network/esp_eth.html) |
| N16 | IEEE 802.15.4: OpenThread and Zigbee | 🚫 Not planned on esp32c6; ⛔ Unsupported elsewhere | esp32c6 | Only esp32c6 has the radio, and no product requirement uses Thread or Zigbee | [`USER_GUIDE.md`](USER_GUIDE.md#hardware) | [OpenThread](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/api-guides/openthread.html) |
| N17 | Heap tracing, Application Trace and GDB Stub | ⏸ Conditional | all | Useful in explicit laboratory builds. They do not replace field core dumps, stack and heap telemetry or `/diag` | [`diag_crash.cpp`](../main/diag_crash.cpp), [`heap_supervisor.cpp`](../main/heap_supervisor.cpp) | [Heap debugging](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/heap_debug.html), [Application Trace](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/app_trace.html) |
| N18 | ESP Insights, RainMaker and ESP-Matter | 🚫 Not planned | all | Cloud diagnostics and commissioning duplicate the local core dump, syslog, HTTP, MCP and MQTT paths and add flash cost | [`diag_crash.cpp`](../main/diag_crash.cpp), [`syslog.cpp`](../main/syslog.cpp), [`mqtt_ha.cpp`](../main/mqtt_ha.cpp) | [ESP Insights](https://components.espressif.com/components/espressif/esp_insights), [ESP-Matter](https://github.com/espressif/esp-matter) |
| N19 | ADC, touch, temperature sensor, I2C, I2S, TWAI, PCNT, MCPWM and SDMMC | 🚫 Not planned; ⛔ where the target lacks the peripheral | all | No supported board or product path needs these peripherals. Startup entropy uses `bootloader_random` only (U06). esp32c3 has no PCNT, MCPWM, SDMMC or touch; esp32c6 has no SDMMC or touch; esp32 has no temperature-sensor driver | [`board.cpp`](../main/board.cpp), [`USER_GUIDE.md`](USER_GUIDE.md#hardware) | [Peripheral API index](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/peripherals/index.html) |
| N20 | Bluedroid BLE host | 🚫 Not planned | all | Available on all four targets, but NimBLE covers the central role with less flash and RAM | [`sdkconfig.defaults`](../sdkconfig.defaults), [ADR-0005](adr/0005-tesla-ble-seam.md) | [Bluetooth API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/bluetooth/index.html) |
<!-- esp-idf-matrix:evaluated:end -->

## ULP and LP-core assessment

This assessment asks two questions. Could a ULP or LP core keep vehicle telemetry flowing to MQTT
while an OTA runs and the device restarts? Which coprocessor uses from other ESP32 firmware fit
this product?

It concludes that no target can do so. The high-power (HP) CPUs keep BLE and MQTT running during
the download, and the remaining gap follows the restart. RTC retention memory (N02) can narrow that
gap without a coprocessor.

### Coprocessor per target

Sources: `components/ulp/Kconfig`, `components/soc/<target>/include/soc/soc_caps.h` and the ULP
documentation sources at ESP-IDF tag `v6.1`.

| Target | Coprocessor | Memory | Clock | Reachable resources |
|---|---|---|---|---|
| esp32 | ULP-FSM; assembly or macros only, 16-bit data words | 8 KB RTC slow memory; reserve 32–8176 B | about 8 MHz | RTC slow memory, RTC_CNTL/RTC_IO/SARADC registers: ADC, temperature sensor, RTC I2C, 18 RTC GPIOs |
| esp32s3 | ULP-FSM and ULP-RISC-V; one runs at a time | 8 KB RTC slow memory; reserve 32–8176 B | RISC-V about 17.5 MHz | RTC I2C, ADC, touch, RTC GPIO 0–21, RTC registers |
| esp32c3 | **None** (`SOC_ULP_SUPPORTED` is not defined) | 8 KB RTC fast memory, used for RTC sections and heap | – | – |
| esp32c6 | LP core (RISC-V) | 16 KB LP SRAM, shared with all RTC sections; reserve 32–16352 B | about 17.5–20 MHz | LP GPIO 0–7, LP I2C, LP UART, LP timer, mailbox, ETM. It reaches HP SRAM and peripherals only while the whole system is active |

### Why no coprocessor can keep the vehicle feed

- **No radio.** NimBLE, the BT controller and the Tesla session state run on the HP side. No ULP
  or LP-core API reaches Wi-Fi, BLE or the modem. `esp_restart()` also resets the radios on every
  target: see `esp_system_reset_modules_on_exit()` in
  [`system_internal.c`](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/port/soc/esp32c6/system_internal.c).
- **No network stack.** There is no Wi-Fi, TCP, TLS or MQTT. The v6.1 option
  `CONFIG_ULP_COPROC_RUN_FROM_HP_MEM` lifts the esp32c6 memory limit, but it adds no radio or
  network access.
- **Secret material.** Session keys and the private key would have to move into memory the
  coprocessor can read, which conflicts with the [secret-handling rules](SECURITY.md).
- **Target contract.** esp32c3 has no coprocessor, so the feature could never cover all four
  targets.

### OTA and restart behavior

1. **Download.** The firmware does not pause telemetry. Only the heap supervisor reads
   `ota_is_busy()` ([`heap_supervisor.cpp`](../main/heap_supervisor.cpp)).
2. **Commit.** The coordinator waits 1.2 s and calls `esp_restart()`, without a clean MQTT
   disconnect ([`ota_update.cpp`](../main/ota_update.cpp)). When the broker applies the retained
   `offline` LWT depends on its keepalive and session-takeover handling (not measured).
3. **Boot.** The restart is a clean boot, so the firmware seeds the five-minute active window
   ([`vehicle_ctrl.cpp`](../main/vehicle_ctrl.cpp)).
   - Within that window, the charge cadence and the domain rotation refill all five vehicle domains
     with `NO_WAKE_SKIP`, unless the car reports that it is asleep.
   - Vehicle-state topics that are still invalid at the first MQTT round are cleared once with
     `{}`. The device topic publishes immediately.
   - Owners: [read-only telemetry](ARCHITECTURE.md#read-only-telemetry-and-command-correctness) and
     [MQTT topics](API.md#mqtt-topics).
4. **Remaining gap.** If the car stays asleep, its domains stay unknown until it wakes by itself.
   After the window closes, charge and drive refill through the wake or bootstrap one-shot.
   Climate, tires and closures wait for a command or for charging. The firmware never wakes a
   parked car with an explicit wake command merely to refresh cached status. `NO_WAKE_SKIP` can still
   allow Infotainment and SessionInfo traffic when sleep state is unknown or awake; its physical
   wake effects have not been measured.

### Restart behavior that matters for retained data

- **CPU-level reset.** `esp_restart()` runs the shutdown handlers and then a CPU-level software
  reset (`esp_rom_software_reset_cpu`) on all four targets. ESP-IDF contains no code that stops the
  ULP or LP core on restart.
- **What the bootloader reloads.** On every boot that is not a deep-sleep wake, the bootloader
  reloads `.rtc.data` and `.rtc.text`; see `should_load()` in
  [`esp_image_format.c`](https://github.com/espressif/esp-idf/blob/v6.1/components/bootloader_support/src/esp_image_format.c).
  Startup also zeroes `.rtc.bss`.
  - `RTC_DATA_ATTR` therefore does not survive `esp_restart()`.
  - `RTC_NOINIT_ATTR` does survive it on all four targets, including esp32c3's RTC fast memory
    ([`esp_attr.h`](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_common/include/esp_attr.h)).
- **Reloading a ULP program.** `ulp_lp_core_load_binary()` stops the LP core and zeroes its whole
  reserve ([`lp_core.c`](https://github.com/espressif/esp-idf/blob/v6.1/components/ulp/lp_core/lp_core.c)).
- **When retention ends.** Retention is meaningful only across resets that keep power. Power-on
  and brownout end it. An RTC-watchdog system reset also resets the RTC domain, including a running
  coprocessor. Adoption therefore needs validation, never the reset reason alone.

### Warm-restart snapshot (N02)

N02 is not implemented. A separately reviewed change could keep the last vehicle telemetry in
`RTC_NOINIT_ATTR` memory, reusing the [`heap_trend.cpp`](../main/heap_trend.cpp) adoption pattern.

- **Contents:** fixed-size charge, climate, drive, tire and closure fields, about 0.5 KB. No VIN,
  key, session, Wi-Fi or MQTT material.
- **Identity binding:** a changed VIN, a pairing reset or a new key adopts nothing.
- **Age and evcc:** carry the true sample age across the restart, or discard the snapshot.
  Outside the active window, any valid charge, climate or drive cache is served regardless of its
  age, including to evcc (`logic/charge_control.hpp`, `logic/vehicle_data.hpp`). A restored domain must therefore stay off
  `/api/1/vehicles/{VIN}/vehicle_data` until a fresh sample replaces it, or carry an explicit
  restored marker that evcc-facing reads refuse.
- **Contract change:** restored values marked by their age would replace the `{}` clearing. That
  changes the [Architecture](ARCHITECTURE.md) and [API](API.md) contracts.
- **Cost and proof:** the snapshot reduces the RTC fast-memory heap on esp32c3 and esp32c6.
  On esp32s3, `RTC_NOINIT_ATTR` uses RTC slow memory by default; it consumes the fast-memory
  heap only with `CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM=y`
  ([placement](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/ld/esp32s3/sections.ld.in),
  [heap reservation](https://github.com/espressif/esp-idf/blob/v6.1/components/heap/port/esp32s3/memory_layout.c)).
  Measure the cost for the effective configuration. Proof needs host tests, all four target builds
  and an on-hardware OTA between two different builds.

### Retention placement

The v6.1 linker scripts place DRAM `.noinit` directly after `.dram0.data`
([`esp32/sections.ld.in`](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/ld/esp32/sections.ld.in),
[`esp32s3/sections.ld.in`](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/ld/esp32s3/sections.ld.in),
[`ld.dram.sections`](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/ld/ld.dram.sections)).

- **Why the address can move.** On esp32s3, esp32c3 and esp32c6, IRAM and DRAM share one region,
  so DRAM also starts after an IRAM-sized offset. The `.noinit` address therefore moves whenever
  `.dram0.data` changes size, or IRAM does on those three targets. How often an OTA moves it is
  not measured.
- **Effect on the heap trend.** Retention requires the actual `s_persist_raw` address and size
  to remain compatible and its bytes to survive boot loading and startup. Adoption then checks
  magic, layout fingerprint and CRC; otherwise the ring starts empty. Comparing `_noinit_start`
  alone is insufficient when other `.noinit` objects can move within the section. Compare the
  retained object's range in both release ELFs and verify adoption on the device; this has not
  been measured across different builds. [API](API.md) owns the diagnostic contract.
- **`.rtc_noinit` address.** It follows only other RTC sections (`.rtc.text`, `.rtc.force_fast`,
  `.rtc.data` and `.rtc.bss`, as each target places them;
  [`ld.rtc.sections`](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/ld/ld.rtc.sections)).
  These come mostly from ESP-IDF itself, so the address should be stable for one IDF pin and one
  sdkconfig. N02 would still need that ELF check.
- **Enabling a ULP moves RTC sections**, because `CONFIG_ULP_COPROC_RESERVE_MEM` shifts the RTC or
  LP data origin
  ([`esp32c6/memory.ld.in`](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/ld/esp32c6/memory.ld.in)).
- **No fixed-address reserve.** `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` needs a bootloader built with
  the same option, which means a USB fleet migration, because OTA never replaces the bootloader.

### ULP use cases evaluated for this firmware

| Use case | Possible here | Value |
|---|---|---|
| Receive vehicle values over BLE | No: HP-only radio and session | – |
| Serve MQTT, HTTP or evcc | No: no network stack | – |
| Keep values across a restart | Yes, without a ULP (N02) | Moderate: only for a car that stays asleep after an OTA |
| Watchdog or boot-loop supervisor | Technically yes | Low: the RTC watchdog, Task Watchdog, boot guard and safe mode already exist, and a spurious reset would be a safety failure |
| Deep-sleep wake (button, timer) | Yes | None: the device never sleeps (N03) |
| Pulse counter or sensors (S0 meter, BME280, DS18B20) | esp32, esp32s3, esp32c6; not esp32c3 | Outside the product scope; HP peripherals suffice while awake |
| Supply or brownout monitoring | Not on esp32c3, where it was reported | None: the HP ADC works while the device runs |

### Other ESP32 firmware

As of November 2026, open-source ESP32 firmware uses the coprocessor for sampling during deep sleep
on battery devices:

- [ESP-IDF examples](https://github.com/espressif/esp-idf/tree/v6.1/examples/system/ulp);
- the ESPHome [`esp32_ulp`](https://github.com/esphome/esphome/tree/dev/esphome/components/esp32_ulp)
  component;
- Tasmota's [Berry `ULP` module](https://github.com/arendst/Tasmota/blob/development/tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_ulp.ino);
- MicroPython [`esp32.ULP`](https://github.com/micropython/micropython/blob/master/ports/esp32/esp32_ulp.c)
  with [`micropython-esp32-ulp`](https://github.com/micropython/micropython-esp32-ulp);
- the Arduino [HULP](https://github.com/boarchuz/HULP) helper;
- Espressif's [low-power guidance](https://github.com/espressif/esp-iot-solution/blob/master/docs/en/low_power_solution/esp32_lowpower_solution.rst).

A source search of always-on radio bridges found no ULP API use: OpenMQTTGateway, ESPresense,
WLED, Theengs, Meshtastic and ESPHome `bluetooth_proxy`.

### Evidence status

- **Verified:** firmware sources, and ESP-IDF v6.1 sources including the linker scripts and
  `soc_caps.h`.
- **Not run:** builds, ELF section addresses, hardware, OTA, broker LWT timing and live vehicle
  behavior.
- **Not reachable:** the rendered docs.espressif.com pages; their facts come from the matching RST
  sources.

## Header inventory

Every non-project header included under `main/`, grouped by its feature row. C and C++ standard
headers and `sys/` headers are exempt: in ESP-IDF 6.1 these come from the C library (`esp_libc`)
or lwIP's POSIX compatibility layer, which U12 covers. Everything else must be listed here.

<!-- esp-idf-matrix:headers:start -->
| Headers | Feature ID |
|---|---|
| `sdkconfig.h` | U01 |
| `freertos/FreeRTOS.h`, `freertos/task.h`, `freertos/queue.h`, `freertos/semphr.h`, `freertos/event_groups.h` | U02 |
| `esp_system.h`, `esp_heap_caps.h`, `esp_log.h`, `esp_mac.h`, `esp_app_desc.h`, `esp_rom_sys.h` | U03 |
| `esp_attr.h` | U04 |
| `esp_timer.h`, `esp_task_wdt.h` | U05 |
| `bootloader_random.h`, `esp_random.h` | U06 |
| `nvs.h`, `nvs_flash.h` | U07 |
| `esp_partition.h` | U08 |
| `esp_core_dump.h`, `esp_memory_utils.h`, `xtensa_context.h`, `riscv/rvruntime-frames.h` | U09 |
| `esp_wifi.h` | U10 |
| `esp_event.h`, `esp_netif.h` | U11 |
| `lwip/sockets.h`, `lwip/netdb.h`, `lwip/netif.h`, `lwip/etharp.h`, `lwip/inet.h`, `lwip/ip_addr.h`, `ping/ping_sock.h`, `esp_sntp.h` | U12 |
| `esp_eth.h`, `esp_eth_mac_w5500.h`, `esp_eth_phy_w5500.h` | U13 |
| `esp_http_server.h` | U14 |
| `esp_tls.h`, `esp_tls_errors.h`, `esp_crt_bundle.h`, `http_parser.h` | U15 |
| `mqtt_client.h` | U16 |
| `mdns.h` | U17 |
| `cJSON.h` | U18 |
| `esp_ota_ops.h`, `esp_image_format.h` | U19 |
| `psa/crypto.h`, `mbedtls/pk.h`, `mbedtls/ssl.h` | U20 |
| `host/ble_hs.h`, `host/ble_gap.h`, `host/ble_gatt.h`, `host/ble_uuid.h`, `nimble/nimble_port.h`, `nimble/nimble_port_freertos.h` | U21 |
| `driver/gpio.h` | U22 |
| `driver/spi_master.h` | U23 |
| `driver/uart.h`, `driver/usb_serial_jtag.h`, `hal/uart_ll.h` | U25 |
| `client.h`, `adapters.h`, `crypto_context.h`, `vin_utils.h`, `car_server.pb.h`, `keys.pb.h`, `signatures.pb.h`, `universal_message.pb.h`, `vcsec.pb.h` | U26 |
<!-- esp-idf-matrix:headers:end -->

## Active ESP-IDF configuration contract

Every active assignment appears here exactly once per file, including explicit `=n` and
`is not set` decisions. Project `CONFIG_TESLA_*` options are listed with the feature row they
configure. The **File** column names `sdkconfig.defaults` as `base` and each target overlay by its
target. `repair` names `maintenance/config-repair/sdkconfig.defaults` and `repair.<target>` its
target overlays.

<!-- esp-idf-matrix:sdkconfig:start -->
| Setting | Value | File | Feature ID |
|---|---|---|---|
| `CONFIG_APP_REPRODUCIBLE_BUILD` | `y` | base | U01 |
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | `y` | base | U01 |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | base | U08 |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `"partitions.csv"` | base | U08 |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | `y` | base | U19 |
| `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` | `y` | base | U19 |
| `CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME` | `y` | base | U19 |
| `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` | `y` | base | U19 |
| `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` | `n` | base | U19 |
| `CONFIG_BT_ENABLED` | `y` | base | U21 |
| `CONFIG_BT_NIMBLE_ENABLED` | `y` | base | U21 |
| `CONFIG_BT_CONTROLLER_ENABLED` | `y` | base | U21 |
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | `y` | base | U21 |
| `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` | `n` | base | U21 |
| `CONFIG_BT_NIMBLE_ROLE_BROADCASTER` | `n` | base | U21 |
| `CONFIG_BT_NIMBLE_ROLE_OBSERVER` | `y` | base | U21 |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | `1` | base | U21 |
| `CONFIG_BT_NIMBLE_SECURITY_ENABLE` | `n` | base | U21 |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | `247` | base | U21 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | `10` | base | U10 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | `32` | base | U10 |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER` | `y` | base | U10 |
| `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT` | `n` | base | U10 |
| `CONFIG_ESP_WIFI_ENABLE_SAE_PK` | `n` | base | U10 |
| `CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT` | `n` | base | U10 |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | `1024` | base | U14 |
| `CONFIG_HTTPD_MAX_URI_LEN` | `512` | base | U14 |
| `CONFIG_LWIP_MAX_SOCKETS` | `16` | base | U12 |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | `11520` | base | U12 |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | `11520` | base | U12 |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | `8192` | base | U02 |
| `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | `8192` | base | U21 |
| `CONFIG_LOG_DEFAULT_LEVEL_INFO` | `y` | base | U03 |
| `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` | `y` | base | U02 |
| `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` | `y` | base | U09 |
| `CONFIG_APP_RETRIEVE_LEN_ELF_SHA` | `64` | base | U09 |
| `CONFIG_ESP_COREDUMP_CHECK_BOOT` | is not set | base | U09 |
| `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` | is not set | base | U09 |
| `CONFIG_ESP_TASK_WDT_EN` | `y` | base | U05 |
| `CONFIG_ESP_TASK_WDT_INIT` | `y` | base | U05 |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | `60` | base | U05 |
| `CONFIG_ESP_TASK_WDT_PANIC` | `y` | base | U05 |
| `CONFIG_MBEDTLS_ERROR_STRINGS` | `n` | base | U20 |
| `CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS` | `n` | base | U20 |
| `CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS` | `n` | base | U20 |
| `CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT` | `n` | base | U20 |
| `CONFIG_MBEDTLS_TLS_CLIENT_ONLY` | `y` | base | U20 |
| `CONFIG_MBEDTLS_FS_IO` | `n` | base | U20 |
| `CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_SIZE` | `y` | base | U20 |
| `CONFIG_MBEDTLS_ECP_DP_BP256R1_ENABLED` | `n` | base | U20 |
| `CONFIG_MBEDTLS_ECP_DP_BP384R1_ENABLED` | `n` | base | U20 |
| `CONFIG_MBEDTLS_ECP_DP_BP512R1_ENABLED` | `n` | base | U20 |
| `CONFIG_MBEDTLS_ECP_DP_SECP256K1_ENABLED` | `n` | base | U20 |
| `CONFIG_MBEDTLS_X509_CRL_PARSE_C` | `n` | base | U20 |
| `CONFIG_MBEDTLS_X509_CSR_PARSE_C` | `n` | base | U20 |
| `CONFIG_LWIP_IPV6` | `n` | base | U12 |
| `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT` | `y` | base | U01 |
| `CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA` | `n` | base | U10 |
| `CONFIG_COMPILER_CXX_EXCEPTIONS` | `y` | base | U01 |
| `CONFIG_ESP32_REV_MIN_3` | `y` | esp32 | U01 |
| `CONFIG_BTDM_CTRL_BLE_MAX_CONN` | `1` | esp32 | U21 |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` | `12` | esp32 | U21 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | `6` | esp32 | U10 |
| `CONFIG_TESLA_DIAG_LOG_BYTES` | `8192` | esp32 | U03 |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | `y` | esp32s3 | U24 |
| `CONFIG_ESP_WIFI_IRAM_OPT` | `n` | esp32s3 | U10 |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | `n` | esp32s3 | U10 |
| `CONFIG_BT_CTRL_DTM_ENABLE` | `n` | esp32s3 | U21 |
| `CONFIG_BT_NIMBLE_DTM_MODE_TEST` | `n` | esp32s3 | U21 |
| `CONFIG_BT_CTRL_BLE_ADV` | `n` | esp32s3 | U21 |
| `CONFIG_BT_CTRL_BLE_SECURITY_ENABLE` | `n` | esp32s3 | U21 |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | `3` | esp32s3 | U21 |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` | `12` | esp32s3 | U21 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | `6` | esp32s3 | U10 |
| `CONFIG_TESLA_ETH_ENABLED` | `y` | esp32s3 | U13 |
| `CONFIG_TESLA_ETH_SPI_SCLK` | `5` | esp32s3 | U13 |
| `CONFIG_TESLA_ETH_SPI_CS` | `6` | esp32s3 | U13 |
| `CONFIG_TESLA_ETH_SPI_MISO` | `7` | esp32s3 | U13 |
| `CONFIG_TESLA_ETH_SPI_MOSI` | `8` | esp32s3 | U13 |
| `CONFIG_TESLA_ETH_SPI_CLOCK_MHZ` | `20` | esp32s3 | U13 |
| `CONFIG_TESLA_ETH_POLL_MS` | `10` | esp32s3 | U13 |
| `CONFIG_TESLA_ETH_WAIT_S` | `20` | esp32s3 | U13 |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | `y` | esp32c3 | U24 |
| `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` | `10` | esp32c3 | U10 |
| `CONFIG_ESP_WIFI_IRAM_OPT` | `n` | esp32c3 | U10 |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | `n` | esp32c3 | U10 |
| `CONFIG_BT_CTRL_DTM_ENABLE` | `n` | esp32c3 | U21 |
| `CONFIG_BT_NIMBLE_DTM_MODE_TEST` | `n` | esp32c3 | U21 |
| `CONFIG_BT_CTRL_BLE_ADV` | `n` | esp32c3 | U21 |
| `CONFIG_BT_CTRL_BLE_SECURITY_ENABLE` | `n` | esp32c3 | U21 |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | `3` | esp32c3 | U21 |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` | `12` | esp32c3 | U21 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | `6` | esp32c3 | U10 |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | `y` | esp32c6 | U24 |
| `CONFIG_BT_NIMBLE_DTM_MODE_TEST` | `n` | esp32c6 | U21 |
| `CONFIG_BT_NIMBLE_PRINT_ERR_NAME` | `n` | esp32c6 | U21 |
| `CONFIG_TESLA_CONFIG_REPAIR_USB` | `y` | repair | U25 |
| `CONFIG_ESP_CONSOLE_NONE` | `y` | repair | U25 |
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE` | `y` | repair | U25 |
| `CONFIG_LOG_DEFAULT_LEVEL_NONE` | `y` | repair | U25 |
| `CONFIG_LOG_DEFAULT_LEVEL` | `0` | repair | U25 |
| `CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT` | `y` | repair | U25 |
| `CONFIG_LOG_MAXIMUM_LEVEL` | `0` | repair | U25 |
| `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG` | `y` | repair.esp32s3 | U25 |
| `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG` | `y` | repair.esp32c3 | U25 |
| `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG` | `y` | repair.esp32c6 | U25 |
<!-- esp-idf-matrix:sdkconfig:end -->

## What the gate proves

`scripts/check-esp-idf-matrix.py` runs through `scripts/repo-lint.sh`, and therefore in the CI
`logic-test` job of every pull request. It compares this document with the tree in both directions
and fails when:

- **Pins disagree.** The metadata targets, the ESP-IDF version in any per-target lockfile, the
  manifest floor or the `esp-idf-toolchain.txt` channel differ from this document. A
  docs.espressif.com link or an ESP-IDF GitHub or raw source link is off that channel, an ESP-IDF
  commit link appears, or a link names an unsupported target.
- **Components disagree.** A `REQUIRES` component of `main/CMakeLists.txt` (including the
  config-repair `list(APPEND ...)` entries) or a direct dependency in `main/idf_component.yml` has
  no used row. A used row names a component that is no longer required. `REQUIRES` contains an
  unknown CMake variable. Quoted component names are accepted; line and bracket comments do not
  count as dependencies.
- **Configuration disagrees.** An active assignment, including a repeated occurrence, in a `sdkconfig.defaults*` or
  `maintenance/config-repair/sdkconfig.defaults*` file is missing, stale or out of file order, or
  maps to a missing used row. A defaults overlay names an unsupported target.
- **Headers disagree.** A non-project, non-standard header included under `main/` is not listed, or
  a listed header is no longer included. A file type under `main/` (outside `www/`) that is neither
  C/C++ source nor a known non-source file fails closed. Includes inside line-leading block comments
  are ignored.
- **Rows are malformed.** A row has a duplicate id, an invalid status or no official documentation
  link. A table loses its expected columns, or the evaluated ULP or `esp_https_ota` decision
  disappears.

`--self-test` copies the tree, reintroduces each listed failure as a mutation, and also runs a
clean control, which must pass, with block-commented and standard includes.

**Limits:**

- The check reads source text, not the preprocessor. An include assembled by macros is not seen,
  an include under `#if 0` still counts, and a string literal whose line starts with `/*` could
  hide a following include until the next `*/`.
- It proves that the listed inventory matches the tree, not that a decision text is still right;
  `$project-review` judges those.
- An ESP-IDF pin change always sends the whole document back through review.
