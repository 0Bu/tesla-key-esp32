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
#include "bootloader_random.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "freertos/semphr.h"

#include "ble_client.hpp"
#include "nvs_storage.hpp"
#include "vehicle_ctrl.hpp"
#include "http_server.hpp"
#include "provisioning.hpp"
#include "diag_log.hpp"
#include "mqtt_ha.hpp"
#include "syslog.hpp"
#include "display.hpp"
#include "led_status.hpp"

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

// Set true on the first IP_EVENT_STA_GOT_IP, never cleared. Distinguishes a boot-time
// connect (exhausting the retry budget here means the stored credentials are wrong →
// fall back to the setup portal) from a runtime drop (credentials known-good →
// reconnect forever, never surrender to the setup portal).
static volatile bool s_wifi_ever_connected = false;

static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t event_id, void* data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (!s_wifi_ever_connected && s_retry_num >= MAX_RETRY) {
            // Never been online AND the boot retry budget is spent → credentials are
            // almost certainly wrong. Stop so wifi_connect() times out and falls back
            // to the setup portal.
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        } else {
            // Still within the boot budget, OR we have been online before (a runtime
            // drop: router reboot, roaming, a delivered deauth). Credentials are
            // known-good → reconnect FOREVER. Surrendering here is what previously
            // stranded the device off-WiFi until a manual reset.
            esp_wifi_connect();
            s_retry_num++;
            if (s_retry_num <= MAX_RETRY || s_retry_num % 20 == 0)
                ESP_LOGI(TAG, "WiFi (re)connect attempt %d", s_retry_num);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        s_wifi_connected = true;
        s_wifi_ever_connected = true;
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

#ifdef CONFIG_TESLA_WIFI_PREFER_5G
    // ESP32-C5 (dual-band Wi-Fi 6) opt-in: when the SSID is band-steered onto both bands,
    // give 5 GHz APs an RSSI bonus so the BY_SIGNAL sort above prefers 5 GHz. WiFi and BLE
    // still share ONE RF path on the C5 (time-division coexistence — see the PS_MIN_MODEM note
    // below; 5 GHz does NOT remove that airtime contention), but keeping OUR WiFi off 2.4 GHz
    // frees the band BLE lives on. Band mode stays AUTO (default), so a device out of 5 GHz
    // range still connects on 2.4 GHz — no reconnect trap. Gated behind CONFIG_TESLA_WIFI_PREFER_5G
    // (Kconfig depends on SOC_WIFI_SUPPORT_5G → the field + this block exist only on the C5).
    // 10 dB: prefer 5 GHz unless it is more than ~10 dB weaker than the 2.4 GHz AP.
    wifi_cfg.sta.threshold.rssi_5g_adjustment = 10;
    ESP_LOGI(TAG, "5 GHz WiFi preference enabled (rssi_5g_adjustment=10 dB)");
#endif

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

// ─── WiFi connectivity watchdog ───────────────────────────────────────────────
// Two failure modes strand the device off the LAN while the firmware otherwise runs
// fine (BLE stays up, heap healthy, no reboot — exactly the state we debugged live):
//   (A) the STA exhausted its reconnect attempts and gave up, or
//   (B) a *missed deauth* leaves a "ghost" association — the stack still believes it
//       is connected (keeps the IP, keeps emitting TCP that times out) but the AP
//       forwards nothing and NO WIFI_EVENT_STA_DISCONNECTED ever fires, so the
//       reconnect handler never runs.
// (A) is handled by the endless-retry handler above, which owns recovery whenever the
// link already knows it is down. (B) has NO event to react to, so this task closes the
// gap: it probes real L3 connectivity (ICMP echo to the default gateway) every
// kWdPeriodS and, only for the ghost case (link believes it is up yet a gateway that
// HAS answered before now doesn't), forces a single re-association — esp_wifi_disconnect()
// drops the stale link and the handler reconnects with the known-good credentials. It
// deliberately NEVER reboots: a reboot during an AP outage would hit wifi_connect()'s
// 30 s boot timeout and drop into the setup portal, abandoning good credentials.

static const int kWdPeriodS       = 30;   // connectivity-check cadence
static const int kWdFailToReassoc = 2;    // consecutive failed checks (~60 s) → re-associate
static const int kWdPingTimeoutMs = 1000; // per-echo timeout
static const int kWdPingCount     = 3;    // echoes per check; healthy if ≥1 replies

// Persistent across probes (the single watchdog task calls gateway_reachable() serially).
// The control block and its semaphore MUST outlive any in-flight esp_ping session: the
// ping's internal thread is NOT joined by esp_ping_delete_session() and calls
// wd_on_ping_end() unconditionally once started. If that callback ran against a per-call
// stack frame after a take() timeout it would write freed memory / give a deleted
// semaphore (use-after-free). File-scope storage removes the window entirely; a stale give
// from a late completion is harmlessly drained at the next probe.
struct WdPing { SemaphoreHandle_t done; uint32_t received; };
static WdPing s_wd = { nullptr, 0 };

// Set true the first time the gateway answers ICMP, never cleared. Until a baseline exists
// we have NO evidence this gateway answers echo at all, so a never-replying gateway (a
// router/firewall that drops LAN ICMP) must NOT be read as "link dead" — that would
// re-associate a perfectly healthy link every ~60 s forever. A genuine ghost association,
// by contrast, replied before and then stops, so it still trips the watchdog.
static volatile bool s_gw_ever_reachable = false;

static void wd_on_ping_end(esp_ping_handle_t hdl, void* args) {
    auto* p = (WdPing*) args;
    uint32_t recv = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    p->received = recv;
    xSemaphoreGive(p->done);
}

// Blocking ICMP echo to the current default gateway. True if ≥1 reply came back. Returns
// true (no false alarm) when the probe can't even be set up — the watchdog must act only on
// a *proven* failure to reach a gateway that DOES answer ICMP, never on its own inability to
// measure. The per-cycle esp_ping session is a deliberate, accepted minor cost (a transient
// ~2.5 KB ping task ~1.5 s out of every 30 s; same-size alloc/free, no monotonic growth).
static bool gateway_reachable() {
    if (!s_wd.done) return true;  // watchdog not fully initialised yet

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0)
        return false;  // no gateway/lease → not reachable

    char gw[16];
    esp_ip4addr_ntoa(&ip.gw, gw, sizeof(gw));
    ip_addr_t target{};
    if (!ipaddr_aton(gw, &target))
        return true;  // unparseable → don't false-alarm

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = kWdPingCount;
    cfg.timeout_ms  = kWdPingTimeoutMs;
    cfg.interval_ms = 250;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args     = &s_wd;
    cbs.on_ping_end = wd_on_ping_end;

    esp_ping_handle_t hdl = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || !hdl)
        return true;  // probe setup failed → don't false-alarm

    xSemaphoreTake(s_wd.done, 0);  // drain any stale give from a prior timed-out probe
    s_wd.received = 0;
    esp_ping_start(hdl);
    // Wait out the whole sequence (count × (timeout + interval)) plus generous margin. A
    // take() timeout is harmless here because s_wd is persistent (see above).
    xSemaphoreTake(s_wd.done,
        pdMS_TO_TICKS(kWdPingCount * (kWdPingTimeoutMs + 250) + 2000));
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);

    bool ok = s_wd.received > 0;
    if (ok) s_gw_ever_reachable = true;
    return ok;
}

static void wifi_watchdog_task(void*) {
    s_wd.done = xSemaphoreCreateBinary();
    int fails = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));

        // When the link already knows it is down, the endless-retry handler owns recovery —
        // there is nothing for the watchdog to detect (the ghost case is link=up by
        // definition), and counting/logging every period would only fill the 16 KB /diag
        // ring with noise across a long router outage.
        if (!s_wifi_connected) {
            fails = 0;
            continue;
        }
        if (gateway_reachable()) {
            fails = 0;
            continue;
        }

        fails++;
        ESP_LOGW(TAG, "watchdog: no LAN connectivity (%d/%d, link=up)",
                 fails, kWdFailToReassoc);
        if (fails < kWdFailToReassoc)
            continue;
        fails = 0;

        // Act ONLY on a true ghost association: the link still believes it is up AND this
        // gateway has answered ICMP before (so its silence is real, not a firewall).
        // One disconnect is enough: the handler's else-branch reconnects
        // (s_wifi_ever_connected is true), so we don't call esp_wifi_connect() ourselves
        // and avoid a cross-task double-connect.
        if (!s_gw_ever_reachable) {
            ESP_LOGW(TAG, "watchdog: gateway has never answered ICMP — not forcing re-assoc");
            continue;
        }

        ESP_LOGW(TAG, "watchdog: ghost association — forcing WiFi re-association");
        esp_wifi_disconnect();   // drops the ghost link → handler reconnects (known-good creds)
    }
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

// OTA rollback health gate. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE leaves a freshly-flashed
// image in ESP_OTA_IMG_PENDING_VERIFY until the app calls
// esp_ota_mark_app_valid_cancel_rollback(); if the device reboots before that call, the
// bootloader reverts to the previous slot. We defer the call to this task so the new image
// has to RUN healthily for a window first — catching a "boots fine, then crashes/OOM-reboots
// under load" image, which the old mark-at-startup placement would have already committed.
// A clean survival of the window (during which polling/HTTP/MQTT/BLE exercise the image) is
// the health signal; a crash inside it reboots still-PENDING and rolls back. No-op otherwise.
static constexpr int kOtaHealthGateS = 90;

static void ota_health_gate_task(void*) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(nullptr);   // normal boot — nothing pending to confirm
        return;
    }

    ESP_LOGI(TAG, "OTA image pending verify — holding rollback armed for %ds health window",
             kOtaHealthGateS);
    vTaskDelay(pdMS_TO_TICKS(kOtaHealthGateS * 1000));

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGI(TAG, "OTA image healthy for %ds — marked valid (rollback cancelled, largest block %u)",
                 kOtaHealthGateS,
                 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    vTaskDelete(nullptr);
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

    // UDP Syslog forwarder for the diag log (NVS "syslog_uri" / CONFIG_TESLA_SYSLOG_SERVER;
    // "" = disabled). Started before WiFi so it captures boot-time log lines too — its own
    // task blocks on wifi_is_connected() until the link is up, so this is safe even though
    // esp_netif_init() itself hasn't run yet (that happens inside wifi_connect() below).
    syslog_start(config_store);

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
        ESP_LOGW(TAG, "VIN not configured — pairing disabled until a VIN is set "
                      "(setup AP or POST /set_vin / CONFIG_TESLA_VIN / NVS key 'vin'). "
                      "Nearby Teslas are still listed by /scan, but none is connected/enrolled.");
        vin = "UNKNOWN";  // placeholder for display/logging only — kept out of the BLE matching path
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
        // The ESP32 hardware RNG only returns TRUE random numbers while an entropy source is
        // active: RF (WiFi/BT) enabled, the bootloader running, or bootloader_random_enable()
        // (SAR-ADC entropy). Neither WiFi (wifi_connect) nor BLE (ble_client.start) is up yet,
        // so without this the EC private key — the device's sole authenticator to the car and
        // the OTA trust root — would be seeded from PSEUDO-random data (tesla-ble seeds its DRBG
        // once and reuses it, so same-boot re-keys inherit the weak seed). Enable the SAR-ADC
        // entropy source for the key generation, then disable it again before WiFi/ADC start
        // (Espressif's documented pattern for "true random before RF is up").
        bootloader_random_enable();
        bool key_ok = vehicle.generate_key();
        bootloader_random_disable();
        if (key_ok) {
            ESP_LOGI(TAG, "initial key generated, fingerprint %s",
                     vehicle.key_fingerprint().c_str());
        } else {
            ESP_LOGE(TAG, "initial key generation failed");
        }
    } else {
        ESP_LOGI(TAG, "key present, fingerprint %s", vehicle.key_fingerprint().c_str());
    }
    // Match by the VIN-derived BLE name on scan. Pass the real VIN only when it is a plausible
    // 17-char VIN; with none configured we pass an EMPTY target so the scanner lists nearby
    // Teslas but never connects/enrols on one. The "UNKNOWN" placeholder must stay out of the
    // matching path — it would hash to a name that just happens never to collide, making the
    // safe outcome accidental rather than designed. Pairing is gated on a real VIN.
    ble_client.set_target_vin(vehicle.has_plausible_vin() ? vin : std::string{});

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

    // Wall clock: TLS cert validation (OTA) and the human-readable key_created/paired_at
    // timestamps need real UTC. (tesla-ble signed-command freshness does NOT — expires_at
    // is the vehicle's SessionInfo.ClockTime plus a monotonic delta, see peer.cpp.) NTP is
    // the primary source; the browser (POST /set_time) is a fallback for networks that
    // block NTP. First restore the last cached time from NVS
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

    // On-device status display (LilyGo T-Dongle-C5 / T-Dongle-S3). No-op unless the board
    // build selects CONFIG_TESLA_DISPLAY_ENABLED — and on esp32s3 also a no-op unless the
    // T-Dongle-S3 is auto-detected (a generic ESP32-S3 has no panel). Reads only cached
    // state (never wakes the car) in its own task, so it can't queue behind a BLE poll.
    display_start(vehicle);
    log_heap("display");

    // On-device status LED (LilyGo T-Dongle underside APA102). No-op unless the board build
    // selects CONFIG_TESLA_LED_ENABLED; reads only cached state via the same UiSnapshot the
    // display uses (never wakes the car), independent of the display / MQTT.
    led_status_start(vehicle);
    log_heap("led");

    // WiFi connectivity watchdog: re-associates if the LAN link silently dies,
    // including the "ghost association" case that fires no disconnect event (see the
    // task definition above). Without it the device can sit reachable-over-BLE but
    // off the LAN indefinitely, recoverable only by a manual reset.
    xTaskCreate(wifi_watchdog_task, "wifi_wd", 3072, nullptr, 4, nullptr);

    // Confirm a freshly OTA-flashed image only after it has proven it can RUN — not merely
    // reach this line. Marking it valid here (the old behaviour) would disarm rollback the
    // instant the tasks start, so an image that boots but then crashes/OOM-reboots only under
    // load would already have cancelled its safety net. The health-gate task instead holds
    // rollback armed for a window of healthy uptime; if the image dies first it reboots while
    // still PENDING_VERIFY and the bootloader reverts to the previous slot. A no-op on a normal
    // (non-pending-verify) boot.
    xTaskCreate(ota_health_gate_task, "ota_gate", 3072, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "tesla-key-esp32 running. API on port 80.");
    // Main task is no longer needed; Vehicle loop + HTTP server run in their own tasks.
    vTaskDelete(nullptr);
}
