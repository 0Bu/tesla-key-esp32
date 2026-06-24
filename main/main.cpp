#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "esp_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_netif.h"

#include "ble_client.hpp"
#include "nvs_storage.hpp"
#include "vehicle_ctrl.hpp"
#include "http_server.hpp"
#include "provisioning.hpp"
#include "diag_log.hpp"
#include "mqtt_ha.hpp"

static const char* MDNS_HOSTNAME = "tesla-key-esp32";  // → http://tesla-key-esp32.local

static const char* TAG = "main";

// ─── Wall clock ───────────────────────────────────────────────────────────────
// NTP (esp_sntp) is the primary time source; the browser (POST /set_time) is only a
// fallback for networks that block NTP. on_time_sync() flips s_ntp_synced and, on the
// first sync, refreshes the NVS cache so a later offline reboot restores a recent
// accurate time instead of sitting at 1970.
static volatile bool      s_ntp_synced = false;
static NvsStorageAdapter* s_cfg_store  = nullptr;

static void on_time_sync(struct timeval*) {
    if (!s_ntp_synced && s_cfg_store) {
        s_cfg_store->save_str("last_time", std::to_string((long long)time(nullptr)));
    }
    s_ntp_synced = true;
    ESP_LOGI(TAG, "NTP time synced");
}

// Queried by the HTTP /set_time handler so the browser clock is applied only as a
// fallback while NTP has not synced this boot.
bool clock_synced_via_ntp() { return s_ntp_synced; }

// ─── WiFi ─────────────────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT      = BIT1;
static int s_retry_num              = 0;
static const int MAX_RETRY          = 10;

// True only while the STA holds an IP. Gates esp_wifi_sta_get_ap_info() callers
// (/status, MQTT) so none reads the AP/station record WHILE WiFi is initialising or
// churning through a disconnect→reconnect — that record has transiently-null fields
// mid-association and a concurrent read faults (LoadProhibited, EXCVADDR=0x1).
// volatile: written from the event-loop task, read from the http/mqtt tasks.
static volatile bool s_wifi_connected = false;
bool wifi_is_connected() { return s_wifi_connected; }

static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t event_id, void* data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(const char* ssid, const char* password) {
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();

    // DHCP client hostname: set BEFORE the lease is requested so the router can
    // register it in its local DNS (e.g. http://tesla-key-esp32.fritz.box). Setting
    // it later (at mDNS init, after WiFi is up) is too late — the DHCP DISCOVER has
    // already gone out. Same name as the mDNS hostname so both agree.
    if (esp_netif_set_hostname(sta_netif, MDNS_HOSTNAME) != ESP_OK)
        ESP_LOGW(TAG, "could not set DHCP hostname '%s'", MDNS_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, &h2));

    wifi_config_t wifi_cfg{};
    strncpy((char*)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char*)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    // Pick the STRONGEST AP for the SSID, not the first one heard. The default
    // WIFI_FAST_SCAN stops at the first matching BSSID (channel-order/timing
    // dependent), so on a multi-AP network (mesh / several APs, same SSID) this
    // device — which is stationary near the car — would latch onto whatever
    // answers first, often a far/weak AP, and the ESP32 STA never roams off it.
    // ALL_CHANNEL_SCAN scans every channel; BY_SIGNAL then connects to the highest
    // RSSI. Costs ~1-2 s more at connect; applies on every (re)connect too.
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Keep WiFi modem-sleep at MIN_MODEM (the IDF default). Modem-sleep parks the radio
    // between DTIM beacons, which DOES add ~100 ms per round-trip (the original cause of
    // the sluggish web UI) — but WIFI_PS_NONE is NOT an option here: on the ESP32-S3 WiFi
    // and BLE share ONE radio, and ESP-IDF WiFi/BT coexistence relies on WiFi modem-sleep
    // to hand the radio to BLE. Setting WIFI_PS_NONE starves BLE so badly that GATT
    // connections to the car time out (live-verified: every connect failed with NimBLE
    // "connect error: 13"), breaking evcc and pairing. So we MUST leave power-save on and
    // tackle web-UI latency elsewhere — the page is gzipped (~13 KB vs 41 KB) and the TCP
    // window is enlarged (sdkconfig.defaults), which together clear it in ~1–2 RTTs.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected to '%s'", ssid);
        return true;
    }
    ESP_LOGE(TAG, "WiFi connection failed");
    return false;
}

// ─── app_main ─────────────────────────────────────────────────────────────────

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW(ota/restart)";
        case ESP_RST_PANIC:     return "PANIC(abort/exception)";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Heap-attribution probe: logs free + largest contiguous block at each init milestone.
// The LARGEST free block (not total free) is what bounds big allocations (OTA TLS record
// buffers, the tesla-ble session), so it's the number that decides whether the device
// OOM-crashes. Measured budget on this board: WiFi −57 KB, NimBLE −86 KB (dominant),
// HTTP −12 KB, MQTT −20 KB of largest-block — handy when tuning the footprint further.
static void log_heap(const char* where) {
    ESP_LOGW(TAG, "HEAP @%-9s free=%u largest=%u min=%u", where,
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

extern "C" void app_main() {
    // Capture console output into the in-memory diagnostic ring (GET /diag).
    diag_log_init();

    // Log why we (re)booted — survives in /diag across reboots, so a crash cause is visible
    // without a serial console. PANIC = abort()/uncaught C++ exception, BROWNOUT = power dip,
    // *_WDT = a stuck task. Pair with the free-heap baseline to spot OOM-driven aborts.
    ESP_LOGW(TAG, "BOOT reset_reason=%s free_heap=%u min_free=%u largest_block=%u",
             reset_reason_str(esp_reset_reason()),
             (unsigned) esp_get_free_heap_size(),
             (unsigned) esp_get_minimum_free_heap_size(),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // NimBLE logs every GAP/GATT procedure at INFO — tens of lines per connect.
    // That noise buries the pairing/key-lifecycle messages in /diag (and fills the
    // ring fast). Raise its threshold to WARN so /diag reads as a clean lifecycle log;
    // our own components (vehicle_ctrl, ble_client, …) keep logging at INFO.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Static so they outlive app_main() (which deletes itself via vTaskDelete)
    static NvsStorageAdapter config_store("tesla_cfg");
    config_store.initialize();

    // Resolve WiFi credentials: NVS overrides Kconfig defaults
    static std::string ssid     = CONFIG_TESLA_WIFI_SSID;
    static std::string password = CONFIG_TESLA_WIFI_PASSWORD;
    config_store.load_str("wifi_ssid", ssid);
    config_store.load_str("wifi_pass", password);

    if (ssid.empty()) {
        ESP_LOGW(TAG, "No WiFi configured — starting setup portal (join WiFi '%s')",
                 "tesla-key-esp32-setup");
        provisioning_run(config_store);  // never returns; reboots on save
    }

    // Resolve VIN
    static std::string vin = CONFIG_TESLA_VIN;
    config_store.load_str("vin", vin);
    if (vin.empty()) {
        ESP_LOGW(TAG, "VIN not configured — using 'UNKNOWN'. Set CONFIG_TESLA_VIN or NVS key 'vin'.");
        vin = "UNKNOWN";
    }

    // Resolve BLE MAC (persisted after first successful scan)
    static std::string ble_mac = CONFIG_TESLA_BLE_MAC;
    config_store.load_str("ble_mac", ble_mac);

    ESP_LOGI(TAG, "VIN: %s  BLE MAC: %s", vin.c_str(),
             ble_mac.empty() ? "(scan)" : ble_mac.c_str());

    log_heap("preinit");

    // ── Tesla BLE controller ─────────────────────────────────────────────────
    // Construct the controller (NVS + key) here; NimBLE itself (ble_client.start)
    // is started after WiFi is up. The controller's accessors are safe to call
    // before that — they report "not connected" until the link comes up.
    static NvsStorageAdapter tesla_store("tesla_ble");
    tesla_store.initialize();
    static BleClient ble_client;
    static VehicleController vehicle;
    // init() wires the connected + rx callbacks onto ble_client and passes the
    // config_store so it can save the discovered MAC.
    vehicle.init(vin, ble_client, tesla_store, config_store, ble_mac);

    // Create the ECDSA key on first boot so a key always exists (and a fingerprint
    // is shown). Regeneration is an explicit, confirmed action in the web UI; this
    // never overwrites an existing key — only generates when none is present.
    if (!vehicle.has_key()) {
        ESP_LOGI(TAG, "no key in storage — generating initial key");
        if (vehicle.generate_key()) {
            ESP_LOGI(TAG, "initial key generated, fingerprint %s",
                     vehicle.key_fingerprint().c_str());
        } else {
            ESP_LOGE(TAG, "initial key generation failed");
        }
    } else {
        ESP_LOGI(TAG, "key present, fingerprint %s", vehicle.key_fingerprint().c_str());
    }
    ble_client.set_target_vin(vin);   // match by the VIN-derived BLE name on scan

    // Connect to WiFi. With stored credentials, a failure is usually a transient
    // outage (e.g. router rebooting), but if it persists (e.g. wrong password),
    // fallback to the setup portal so the user can reconfigure it.
    if (!wifi_connect(ssid.c_str(), password.c_str())) {
        ESP_LOGW(TAG, "WiFi connection failed — starting setup portal");
        provisioning_run(config_store); // never returns; reboots on save
    }
    log_heap("wifi");

    // mDNS: advertise http://tesla-key-esp32.local so users need not find the IP
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("tesla-key-esp32");
        // TXT records so discovery tools (dns-sd -B / avahi-browse / our own /scan)
        // can tell multiple devices apart without first resolving each .local host.
        // mdns copies these internally, so the pointers need only outlive the call.
        mdns_txt_item_t txt[] = {
            { "vin", vin.c_str() },
            { "ver", esp_app_get_description()->version },
        };
        mdns_service_add(nullptr, "_http", "_tcp", 80, txt, 2);
        ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

    // Wall clock: the tesla-ble session-freshness checks (and TLS cert validation for
    // OTA) need real UTC. NTP is the primary source; the browser (POST /set_time) is a
    // fallback for networks that block NTP. First restore the last cached time from NVS
    // so we never sit at 1970 while NTP syncs (or if it never does) — this also covers
    // a headless reboot (evcc only, no browser visit). Then start SNTP; on sync it
    // refreshes the cache and takes over from any restored/fallback value.
    {
        std::string last_time;
        if (config_store.load_str("last_time", last_time) && !last_time.empty()) {
            struct timeval tv = { (time_t)atoll(last_time.c_str()), 0 };
            settimeofday(&tv, nullptr);
            ESP_LOGI(TAG, "clock restored from NVS: %s (NTP will refine it)",
                     last_time.c_str());
        }
    }
    s_cfg_store = &config_store;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();

    // Start NimBLE host. Discovery scanning is manual/time-limited; the client
    // connects on demand when a command is issued. (The controller was set up
    // before WiFi, above.)
    ble_client.start();
    log_heap("ble");

    http_server_start(vehicle);
    log_heap("http");

    // Home Assistant MQTT bridge: publishes all telemetry + device status (read-only)
    // if a broker is configured (NVS "mqtt_uri" / CONFIG_TESLA_MQTT_BROKER_URI); a
    // no-op otherwise. Runs in its own task, independent of evcc/BLE/pairing.
    mqtt_ha_start(vehicle, config_store);
    log_heap("mqtt");

    // We reached a healthy steady state (WiFi up, server running). If we just
    // booted a freshly OTA-flashed image in the "pending verify" state, mark it
    // valid so the bootloader keeps it; otherwise an early crash would auto-roll
    // back to the previous slot. A no-op for images that aren't pending verify.
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
                ESP_LOGI(TAG, "OTA image marked valid (rollback cancelled)");
        }
    }

    ESP_LOGI(TAG, "tesla-key-esp32 running. API on port 80.");
    // Main task is no longer needed; Vehicle loop + HTTP server run in their own tasks.
    vTaskDelete(nullptr);
}
