#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "esp_sntp.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
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

// Human-readable DNS-SD instance name, and the prefix of the per-device default
// hostname ("<MDNS_INSTANCE>-<mac3>"). The actual hostname is resolved at runtime by
// device_hostname() (NVS "hostname" override, else the unique per-board default).
static const char* MDNS_INSTANCE = "tesla-key-esp32";

// The hostname this boot actually advertised (mDNS + DHCP). Resolved once in app_main
// and exposed to http_server via device_hostname_current() so /status reports the real
// .local name and /set_hostname can detect an unchanged value.
static std::string g_hostname;

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

static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t event_id, void* data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
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
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(const char* ssid, const char* password, const char* hostname) {
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();

    // DHCP client hostname: set BEFORE the lease is requested so the router can
    // register it in its local DNS (e.g. http://tesla-key-esp32-<mac3>.fritz.box).
    // Setting it later (at mDNS init, after WiFi is up) is too late — the DHCP
    // DISCOVER has already gone out. Same name as the mDNS hostname so both agree.
    if (esp_netif_set_hostname(sta_netif, hostname) != ESP_OK)
        ESP_LOGW(TAG, "could not set DHCP hostname '%s'", hostname);

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

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

// Reduce a user-supplied name to a valid single DNS label: lowercase, [a-z0-9-] only
// (space/underscore/dot collapse to '-'), no leading/trailing or doubled '-', ≤32
// chars; "" if nothing usable remains. Single authority — applied both when reading
// the NVS override below and when /set_hostname stores it (declared in http_server),
// so the stored value and the advertised name can never disagree. Mirrored as a
// client-side hint in index.html / setup.html. External linkage (used across TUs).
std::string sanitize_hostname(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            out += c;
        } else if (c == '-' || c == ' ' || c == '_' || c == '.') {
            if (!out.empty() && out.back() != '-') out += '-';
        }   // any other character is dropped
        if (out.size() >= 32) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// The per-device default hostname, "<MDNS_INSTANCE>-<mac3>" from the WiFi STA MAC — the
// SAME <mac3> the MQTT node id (teslakey_<mac3>) derives, so a board's names all agree,
// and two devices on one LAN never collide on .local. No NVS: also called by the setup
// portal to show the exact .local address before the override is known. External linkage.
std::string default_hostname() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[40];
    snprintf(buf, sizeof(buf), "%s-%02x%02x%02x", MDNS_INSTANCE, mac[3], mac[4], mac[5]);
    return buf;
}

// Effective mDNS / DHCP hostname for this board: a usable NVS "hostname" override (set
// in the setup portal or via POST /set_hostname) wins, else the per-device default.
static std::string device_hostname(NvsStorageAdapter& cfg) {
    std::string h;
    cfg.load_str("hostname", h);
    h = sanitize_hostname(h);          // defends against any malformed stored value
    return h.empty() ? default_hostname() : h;
}

// Exposed to http_server (declared there): the hostname this boot actually used.
std::string device_hostname_current() { return g_hostname; }

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

    // One-time hostname migration (first boot of this firmware, gated by "hn_migrated").
    // A device upgraded over OTA from the old fixed-name firmware already has WiFi creds
    // but no "hostname" key — preserve its existing name "tesla-key-esp32" so the routine
    // OTA does not silently rename it (which would break tesla-key-esp32.local bookmarks
    // and strand the post-update reconnect: the web UI polls /status on the same origin,
    // and the old .local would stop resolving). A factory-fresh device has no creds yet,
    // so no legacy name is written and it falls through to the unique per-device default.
    // The marker is set in BOTH cases so a fresh device, once configured, isn't mistaken
    // for an upgrade on its next boot (its blank hostname must stay = unique default).
    {
        std::string migrated;
        config_store.load_str("hn_migrated", migrated);
        if (migrated.empty()) {
            std::string existing_host;
            config_store.load_str("hostname", existing_host);
            if (existing_host.empty() && !ssid.empty()) {
                config_store.save_str("hostname", MDNS_INSTANCE);   // "tesla-key-esp32"
                ESP_LOGW(TAG, "hostname migration: kept legacy name '%s' (upgraded device)",
                         MDNS_INSTANCE);
            }
            config_store.save_str("hn_migrated", "1");
        }
    }

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

    // Resolve the mDNS / DHCP hostname (NVS "hostname" override, else the unique
    // per-board default) into the file-scope g_hostname — it outlives app_main and is
    // exposed to http_server via device_hostname_current().
    g_hostname = device_hostname(config_store);
    ESP_LOGI(TAG, "hostname: %s → http://%s.local", g_hostname.c_str(), g_hostname.c_str());

    // Connect to WiFi. With stored credentials, a failure is usually a transient
    // outage (e.g. router rebooting), but if it persists (e.g. wrong password),
    // fallback to the setup portal so the user can reconfigure it.
    log_heap("preinit");
    if (!wifi_connect(ssid.c_str(), password.c_str(), g_hostname.c_str())) {
        ESP_LOGW(TAG, "WiFi connection failed — starting setup portal");
        provisioning_run(config_store); // never returns; reboots on save
    }
    log_heap("wifi");

    // mDNS: advertise http://<hostname>.local so users need not find the IP. The
    // hostname is unique per device (see device_hostname) so 2+ boards on one LAN
    // don't collide; the instance name stays the friendly product name.
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_hostname.c_str());
        mdns_instance_name_set(MDNS_INSTANCE);
        // TXT records so discovery tools (dns-sd -B / avahi-browse / our own /scan)
        // can tell multiple devices apart without first resolving each .local host.
        // value pointers must outlive the call only — mdns copies them internally.
        mdns_txt_item_t txt[] = {
            { "vin", vin.c_str() },
            { "ver", esp_app_get_description()->version },
        };
        mdns_service_add(nullptr, "_http", "_tcp", 80, txt, 2);
        ESP_LOGI(TAG, "mDNS: http://%s.local", g_hostname.c_str());
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

    // BLE + NVS storage for Tesla sessions/key
    static NvsStorageAdapter tesla_store("tesla_ble");
    tesla_store.initialize();

    static BleClient ble_client;

    static VehicleController vehicle;
    // init() wires the connected + rx callbacks onto ble_client.
    // It also passes the config_store so it can save the discovered MAC.
    vehicle.init(vin, ble_client, tesla_store, config_store, ble_mac);

    // Create the ECDSA key on first boot so a key always exists (and a fingerprint is
    // shown). Regeneration is an explicit, confirmed action in the web UI. This never
    // overwrites an existing key — only generates when none is present.
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

    // Match the vehicle by its VIN-derived BLE name during scan/connect.
    ble_client.set_target_vin(vin);

    // Start NimBLE host. Discovery scanning is manual/time-limited; the client
    // connects on demand when a command is issued.
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
