// Network transport layer — the implementation behind net.hpp.
//
// This file was carved out of main.cpp, where the WiFi station, its endless-reconnect
// handler, the credential-rollback boot window and the gateway-ICMP watchdog all lived as
// file-scope statics next to app_main(). Nothing about their BEHAVIOUR changes here; what
// changes is that the rest of the firmware now asks "is the link up / which netif is active"
// through a contract instead of through `extern bool wifi_is_connected()` and a hardcoded
// "WIFI_STA_DEF" ifkey, so a second transport can exist at all.
//
// The WiFi specifics that must NOT be generalised away, and why:
//   • WIFI_PS_MIN_MODEM — WiFi and BLE share ONE radio on every chip this firmware targets,
//     and IDF coexistence hands BLE the radio during modem sleep. WIFI_PS_NONE starves
//     NimBLE badly enough that every GATT connect to the car times out.
//   • ALL_CHANNEL_SCAN + CONNECT_AP_BY_SIGNAL — this device is stationary near the car and
//     never roams; picking the first BSSID heard latches it to a far AP for the whole boot.
//   • The reason-aware rollback deadline — rolling back DELETES credentials the user just
//     typed, so only a sustained authentication refusal may spend them.

#include "net.hpp"

#include <atomic>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

#include "boot_fatal.hpp"
#include "task_config.hpp"
#include "logic/net_link.hpp"
#include "logic/wifi_rollback.hpp"

static const char* TAG = "net";

// The DHCP client hostname, requested BEFORE the lease so the router can register it in its
// local DNS (e.g. http://tesla-key-esp32.fritz.box). Same name main.cpp gives mDNS, so the
// two agree; kept here because it must be set on the netif at creation time.
static const char* kNetHostname = "tesla-key-esp32";

namespace tk {

// ── shared link state ─────────────────────────────────────────────────────────
// Written on the event-loop task, read from the http / mqtt / display / led tasks. Atomics,
// not volatile: the readers need the happens-before edge, not merely a non-elided load.

static std::atomic<NetLink>  s_kind{NetLink::None};
static std::atomic<unsigned> s_reconnects{0};

// The netif holding the default route. Written on the event task alongside s_kind; a plain
// pointer in an atomic because readers only ever pass it back into esp_netif_* calls, which
// tolerate a netif that went down between the read and the call (they return an error).
static std::atomic<esp_netif_t*> s_active_netif{nullptr};

// Set once at boot by the Ethernet backend when it finds its controller. Kept here rather
// than behind an #ifdef in every caller: the presenters ask "does this board have a wire?"
// and must compile identically on a build with no Ethernet support at all.
static std::atomic<bool> s_eth_present{false};

bool         net_eth_present()    { return s_eth_present.load(); }
bool         net_is_up()          { return s_kind.load() != NetLink::None; }
NetLink      net_kind()           { return s_kind.load(); }
esp_netif_t* net_active_netif()   { return s_active_netif.load(); }
unsigned     net_reconnect_count(){ return s_reconnects.load(); }

// Set true on the first lease of ANY transport, never cleared. Distinguishes a boot-time
// failure (budget spent, never online → the credentials are suspect) from a runtime drop
// (known-good → retry forever). Also gates the reconnect counter so the first lease of a
// boot is not counted.
static std::atomic<bool> s_ever_up{false};

// Called by each transport backend when it gains or loses its lease. Keeping the bookkeeping
// in one place is what guarantees the reconnect counter and the active-netif pointer cannot
// disagree with s_kind — the exact class of drift the old five-`extern` arrangement invited.
static void link_up(NetLink kind, esp_netif_t* netif) {
    s_active_netif.store(netif);
    if (s_ever_up.load()) s_reconnects.fetch_add(1);
    s_ever_up.store(true);
    s_kind.store(kind);
}

static void link_down(NetLink kind) {
    // Only the transport that OWNS the current lease may clear it. Without this guard a WiFi
    // disconnect event arriving while Ethernet carries the route would blank the link for
    // everything above the seam.
    if (s_kind.load() != kind) return;
    s_kind.store(NetLink::None);
    s_active_netif.store(nullptr);
}

// ── shared substrate ──────────────────────────────────────────────────────────

void net_init() {
    // Both are idempotent-by-error: provisioning_run() may already have created them on the
    // setup-AP path, and ESP_ERR_INVALID_STATE means exactly "already done".
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
}

// ── WiFi station ──────────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT      = BIT1;
static int s_retry_num              = 0;
static const int MAX_RETRY          = 10;

static esp_netif_t* s_sta_netif = nullptr;

// The reason code of the most recent WIFI_EVENT_STA_DISCONNECTED. Written on the event task,
// read by the boot window below — atomic, because the credential-rollback decision reads it
// while associations are still churning. 0 = nothing has failed yet.
static std::atomic<int> s_last_disco_reason{0};

static void wifi_event_handler(void*, esp_event_base_t base, int32_t event_id, void* data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        link_down(NetLink::Wifi);
        // Keep the reason: it is the only evidence available about WHY the association failed,
        // and the credential-rollback decision turns on the difference between "the AP refused
        // these credentials" and "the AP was not there". Stored, never acted on here — this runs
        // on the event task and the decision belongs to the boot window in net_start_wifi().
        if (data) s_last_disco_reason.store(((wifi_event_sta_disconnected_t*)data)->reason);
        if (!s_ever_up.load() && s_retry_num >= MAX_RETRY) {
            // Never been online AND the boot retry budget is spent → credentials are almost
            // certainly wrong. Stop so net_start_wifi() times out and main.cpp falls back to
            // the setup portal.
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        } else {
            // Still within the boot budget, OR we have been online before (a runtime drop:
            // router reboot, roaming, a delivered deauth). Credentials are known-good →
            // reconnect FOREVER. Surrendering here is what previously stranded the device off
            // WiFi until a manual reset.
            esp_wifi_connect();
            s_retry_num++;
            if (s_retry_num <= MAX_RETRY || s_retry_num % 20 == 0)
                ESP_LOGI(TAG, "WiFi (re)connect attempt %d", s_retry_num);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        // Retire the stored disconnect reason: an earlier refusal must not outlive the
        // association that disproved it, or a device that got on the network at attempt three
        // still rolls its credentials back on the evidence of attempt one
        // (logic/wifi_rollback.hpp states this as the caller's obligation).
        s_last_disco_reason.store(0);
        link_up(NetLink::Wifi, s_sta_netif);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

bool net_start_wifi(const char* ssid, const char* password, bool rollback_pending) {
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) boot_fatal("WiFi event group");

    net_init();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) boot_fatal("WiFi station netif");

    // DHCP client hostname: set BEFORE the lease is requested so the router can register it in
    // its local DNS. Setting it later (at mDNS init, after WiFi is up) is too late — the DHCP
    // DISCOVER has already gone out.
    if (esp_netif_set_hostname(s_sta_netif, kNetHostname) != ESP_OK)
        ESP_LOGW(TAG, "could not set DHCP hostname '%s'", kNetHostname);

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
    // Pick the STRONGEST AP for the SSID, not the first one heard. The default WIFI_FAST_SCAN
    // stops at the first matching BSSID (channel-order/timing dependent), so on a multi-AP
    // network this device — stationary near the car — would latch onto whatever answers first,
    // often a far/weak AP, and the ESP32 STA never roams off it. ALL_CHANNEL_SCAN scans every
    // channel; BY_SIGNAL then connects to the highest RSSI. Costs ~1-2 s more at connect.
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Keep WiFi modem-sleep at MIN_MODEM (the IDF default). Modem-sleep parks the radio between
    // DTIM beacons, which DOES add ~100 ms per round-trip (the original cause of the sluggish
    // web UI) — but WIFI_PS_NONE is NOT an option here: WiFi and BLE share ONE radio, and
    // ESP-IDF coexistence relies on WiFi modem-sleep to hand it to BLE. Setting WIFI_PS_NONE
    // starves BLE so badly that GATT connections to the car time out (live-verified: every
    // connect failed with NimBLE "connect error: 13"), breaking evcc and pairing. So we MUST
    // leave power-save on and tackle web-UI latency elsewhere — the page is gzipped (~13 KB vs
    // 41 KB) and the TCP window is enlarged (sdkconfig.defaults), clearing it in ~1-2 RTTs.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    // Without a pending credential change this is the long-standing behaviour: one 30 s budget,
    // then fall back to the setup portal.
    if (!rollback_pending) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(tk::kWifiBootWindowS * 1000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi connected to '%s'", ssid);
            return true;
        }
        ESP_LOGE(TAG, "WiFi connection failed");
        return false;
    }

    // A /set_wifi change is on trial. The deadline is REASON-AWARE, because rolling back is
    // destructive — it deletes credentials the user just typed. Only an AP that SUSTAINS an
    // authentication refusal across two checkpoints spends them; anything else (an absent SSID
    // because the router is still rebooting, a slow DHCP) is not evidence against the
    // credentials and gets the full grace window instead. The policy is the host-tested
    // logic/wifi_rollback.hpp; this loop only supplies the samples.
    tk::RollbackWatch watch{};
    for (int elapsed = 0;; elapsed += (int)tk::kWifiBootWindowS) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
            WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(tk::kWifiBootWindowS * 1000));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi connected to '%s'", ssid);
            return true;
        }
        const int checked = elapsed + (int)tk::kWifiBootWindowS;
        const tk::DiscoClass cls = tk::disco_class(s_last_disco_reason.load());
        if (tk::rollback_step(watch, cls, checked) == tk::RollbackAction::RollBack) {
            ESP_LOGE(TAG, "WiFi still not up %d s after a credential change (last disconnect "
                          "reason %d) — rolling back to the previous network",
                     checked, s_last_disco_reason.load());
            return false;
        }
        ESP_LOGW(TAG, "WiFi not up yet %d s after a credential change (last disconnect reason "
                      "%d) — still waiting before rolling back", checked,
                 s_last_disco_reason.load());
    }
}

// ── WiFi-only readings ────────────────────────────────────────────────────────
// Both gate on net_kind() == Wifi, not merely on "something is up": esp_wifi_sta_get_ap_info()
// has transiently-null fields while the station is associating and a concurrent read there
// faults (LoadProhibited, EXCVADDR=0x1). Centralised here so no caller can forget the gate —
// /status, MQTT and the display presenter each used to re-derive it.

bool net_wifi_signal(int* rssi_dbm, char* ssid) {
    if (s_kind.load() != NetLink::Wifi) return false;
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (rssi_dbm) *rssi_dbm = ap.rssi;
    if (ssid) {
        // ap.ssid is a 33-byte field that is NOT guaranteed NUL-terminated when the SSID is a
        // full 32 bytes; copy the payload and terminate ourselves.
        std::memcpy(ssid, ap.ssid, 32);
        ssid[32] = '\0';
    }
    return true;
}

// No Ethernet backend is compiled in yet — the W5500 driver lands in the next change. The
// accessor exists now so /status, the display and the LED already read the transport through
// ONE seam; until then it simply reports "no wire", which is the truth on every current board.
bool net_eth_phy(int* speed_mbps, bool* full_duplex) {
    (void)speed_mbps; (void)full_duplex;
    return false;
}

const char* net_wifi_standard() {
    if (s_kind.load() != NetLink::Wifi) return nullptr;
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return nullptr;
    // The highest 802.11 generation the AP advertises. The ESP32 radio itself may top out
    // lower, but the flags reflect the AP, so a Wi-Fi 6 router reads "Wi-Fi 6".
    return ap.phy_11ax ? "Wi-Fi 6"
         : ap.phy_11ac ? "Wi-Fi 5"
         : ap.phy_11n  ? "Wi-Fi 4"
         : ap.phy_11g  ? "802.11g"
         : ap.phy_11b  ? "802.11b" : nullptr;
}

// ── connectivity watchdog ─────────────────────────────────────────────────────
// The decision logic is logic/net_link.hpp's watch_step(); this half only supplies samples and
// carries out the verdict. See that header for why a never-answering gateway must not count.

static const int kWdPeriodS       = 30;   // connectivity-check cadence
static const int kWdPingTimeoutMs = 1000; // per-echo timeout
static const int kWdPingCount     = 3;    // echoes per check; healthy if ≥1 replies

// Persistent across probes (the single watchdog task calls gateway_reachable() serially). The
// control block and its semaphore MUST outlive any in-flight esp_ping session: the ping's
// internal thread is NOT joined by esp_ping_delete_session() and calls wd_on_ping_end()
// unconditionally once started. If that callback ran against a per-call stack frame after a
// take() timeout it would write freed memory / give a deleted semaphore (use-after-free).
// File-scope storage removes the window entirely; a stale give from a late completion is
// harmlessly drained at the next probe.
struct WdPing { SemaphoreHandle_t done; uint32_t received; };
static WdPing s_wd = { nullptr, 0 };

// Set true the first time the gateway answers ICMP, never cleared — the baseline watch_step()
// requires before it will act.
static std::atomic<bool> s_gw_ever_reachable{false};

static void wd_on_ping_end(esp_ping_handle_t hdl, void* args) {
    auto* p = (WdPing*) args;
    uint32_t recv = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    p->received = recv;
    xSemaphoreGive(p->done);
}

// Blocking ICMP echo to the current default gateway. True if ≥1 reply came back. Returns true
// (no false alarm) when the probe can't even be set up — the watchdog must act only on a
// PROVEN failure to reach a gateway that DOES answer ICMP, never on its own inability to
// measure. The per-cycle esp_ping session is a deliberate, accepted minor cost (a transient
// ~2.5 KB ping task ~1.5 s out of every 30 s; same-size alloc/free, no monotonic growth).
static bool gateway_reachable() {
    if (!s_wd.done) return true;  // watchdog not fully initialised yet

    esp_netif_t* netif = net_active_netif();
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
    // Wait out the whole sequence (count × (timeout + interval)) plus generous margin. A take()
    // timeout is harmless here because s_wd is persistent (see above).
    xSemaphoreTake(s_wd.done, pdMS_TO_TICKS(kWdPingCount * (kWdPingTimeoutMs + 250) + 2000));
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);

    bool ok = s_wd.received > 0;
    if (ok) s_gw_ever_reachable = true;
    return ok;
}

// Force the ACTIVE transport to re-establish its link. WiFi drops the ghost association and
// the event handler reconnects with the known-good credentials (s_ever_up is true by
// definition here, so we must not call esp_wifi_connect() ourselves — that would race the
// handler into a double-connect).
static void net_recover() {
    switch (s_kind.load()) {
        case NetLink::Wifi:
            ESP_LOGW(TAG, "watchdog: ghost association — forcing WiFi re-association");
            esp_wifi_disconnect();
            break;
        default:
            // NetLink::None cannot reach here (watch_step returns Idle on a down link), and
            // a transport with no recovery action simply keeps its lease; the next probe
            // re-evaluates.
            break;
    }
}

static void net_watchdog_task(void*) {
    tk::LinkWatch watch{};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));

        // When the link already knows it is down, the transport's own reconnect path owns
        // recovery — there is nothing to detect (the ghost case is link=up by definition) and
        // logging every period would fill the 16 KB /diag ring across a long router outage.
        const bool up = net_is_up();
        const bool gw = up && gateway_reachable();

        switch (tk::watch_step(watch, up, gw, s_gw_ever_reachable.load())) {
            case tk::WatchAction::Idle:
                break;
            case tk::WatchAction::Wait:
                ESP_LOGW(TAG, "watchdog: no LAN connectivity (%d/%d, link=up)",
                         watch.fails, tk::kWatchFailsToRecover);
                break;
            case tk::WatchAction::NoBaseline:
                ESP_LOGW(TAG, "watchdog: gateway has never answered ICMP — not forcing a "
                              "re-establish");
                break;
            case tk::WatchAction::Recover:
                net_recover();
                break;
        }
    }
}

bool net_watchdog_start() {
    s_wd.done = xSemaphoreCreateBinary();
    if (!s_wd.done) return false;
    return xTaskCreate(net_watchdog_task, "net_wd", 3072, nullptr,
                       kPrioWifiWatchdog, nullptr) == pdPASS;
}

}  // namespace tk
