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
#include "logic/wifi_credentials.hpp"

#include <atomic>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

#if CONFIG_TESLA_ETH_ENABLED
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#endif

#include "board.hpp"
#include "boot_fatal.hpp"
#include "task_config.hpp"
#include "logic/eth_board.hpp"
#include "logic/net_link.hpp"
#include "logic/wifi_rollback.hpp"
#include "ping_probe.hpp"

static const char* TAG = "net";

// The DHCP client hostname, requested BEFORE the lease so the router can register the board for
// headerless API clients and diagnostics. Browser mutations deliberately use the canonical
// `tesla-key-esp32.local` name or current IP: accepting an arbitrary router-added DNS suffix as a
// trusted Host would reopen DNS rebinding. Same base name main.cpp gives mDNS; kept here because it
// must be set on the netif at creation time.
static const char* kNetHostname = "tesla-key-esp32";

namespace tk {

static void net_boot_require(esp_err_t err, const char* component) {
    if (err == ESP_OK) return;
    ESP_LOGE(TAG, "%s failed: %s", component, esp_err_to_name(err));
    boot_fatal(component);
}

// The public initializer remains fail-closed, but Ethernet needs the error before parking so it
// can release the SPI bus retained by its early hardware probe.  esp_netif itself cannot be
// deinitialized in IDF 5.5; this helper only makes the failure observable to the caller before
// the common boot-fatal boundary is entered.
static esp_err_t net_init_substrate(const char** failed_component) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        *failed_component = "network interface substrate";
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        *failed_component = "network event loop";
        return err;
    }
    *failed_component = nullptr;
    return ESP_OK;
}

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

// Per-transport lease state. BOTH can be true at once: a board whose W5500 found no lease at
// boot falls back to WiFi with the Ethernet driver still running, so a cable plugged in later
// brings the wire up ALONGSIDE the radio. s_kind is therefore DERIVED from these rather than
// written directly — the earlier "last event wins" version had a real hole: unplugging that
// cable cleared the link for everything above the seam (syslog stopped, the display showed
// "searching", MQTT dropped the RSSI) while a perfectly healthy WiFi lease was still in hand.
static std::atomic<bool> s_wifi_lease{false};
static std::atomic<bool> s_eth_lease{false};

// Each backend owns its netif handle; these let recompute_link() sit above both without
// reordering the file. Both return nullptr until their transport has been started.
static esp_netif_t* s_sta_netif_ptr();
static esp_netif_t* s_eth_netif_ptr();

// Ethernet outranks WiFi whenever both hold a lease: it is the transport that costs the BLE
// radio nothing. lwIP is made to agree by raising the Ethernet netif's route_prio above the
// station's (see kEthRoutePrio) — it does not agree on its own.
static void recompute_link() {
    const NetLink kind = net_link_active(s_eth_lease.load(), s_wifi_lease.load());
    esp_netif_t* netif = (kind == NetLink::Eth)  ? s_eth_netif_ptr()
                       : (kind == NetLink::Wifi) ? s_sta_netif_ptr()
                                                 : nullptr;
    // netif BEFORE kind: every reader that sees kind != None must find a usable handle, and
    // net_active_netif() is called straight into esp_netif_* by /status, MQTT and the watchdog.
    s_active_netif.store(netif);
    s_kind.store(kind);
}

// Called by each transport backend when it gains or loses its lease. Keeping the bookkeeping in
// one place is what guarantees the reconnect counter and the active-netif pointer cannot
// disagree with s_kind — the exact class of drift the old five-`extern` arrangement invited.
static void link_up(NetLink kind) {
    const bool was_up = (s_kind.load() != NetLink::None);
    if (kind == NetLink::Eth) s_eth_lease.store(true); else s_wifi_lease.store(true);
    recompute_link();
    // Count a RE-establishment, not a transport switch: going from one live transport to the
    // other is not an outage anybody needs to see in the flap counter.
    if (!was_up) {
        if (s_ever_up.load()) s_reconnects.fetch_add(1);
        s_ever_up.store(true);
    }
}

static void link_down(NetLink kind) {
    if (kind == NetLink::Eth) s_eth_lease.store(false); else s_wifi_lease.store(false);
    recompute_link();
}

// ── shared substrate ──────────────────────────────────────────────────────────

void net_init() {
    // Both are idempotent-by-error: provisioning_run() may already have created them on the
    // setup-AP path, and ESP_ERR_INVALID_STATE means exactly "already done".
    const char* failed_component = nullptr;
    net_boot_require(net_init_substrate(&failed_component), failed_component);
}

// ── WiFi station ──────────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT      = BIT1;
static int s_retry_num              = 0;
static const int MAX_RETRY          = 10;

static esp_netif_t* s_sta_netif = nullptr;
static esp_netif_t* s_sta_netif_ptr() { return s_sta_netif; }

// The reason code of the most recent WIFI_EVENT_STA_DISCONNECTED. Written on the event task,
// read by the boot window below — atomic, because the credential-rollback decision reads it
// while associations are still churning. 0 = nothing has failed yet.
static std::atomic<int> s_last_disco_reason{0};

static void wifi_event_handler(void*, esp_event_base_t base, int32_t event_id, void* data) {
    try {
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
        link_up(NetLink::Wifi);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
      }
    } catch (...) {
        ESP_LOGE(TAG, "WiFi event callback threw; event dropped");
    }
}

bool net_start_wifi(const char* ssid, const char* password, bool rollback_pending) {
    if (!ssid || !password) {
        ESP_LOGE(TAG, "WiFi credentials missing");
        return false;
    }
    const tk::WifiCredentialError credential_error =
        tk::wifi_credentials_error(ssid, password);
    if (credential_error != tk::WifiCredentialError::None) {
        ESP_LOGE(TAG, "stored WiFi credentials rejected: %s",
                 tk::wifi_credentials_reason(credential_error));
        return false;
    }
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
    net_boot_require(esp_wifi_init(&cfg), "WiFi driver initialization");

    esp_event_handler_instance_t h1, h2;
    net_boot_require(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, &h1),
        "WiFi event handler registration");
    net_boot_require(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, &h2),
        "WiFi IP handler registration");

    wifi_config_t wifi_cfg{};
    const size_t ssid_len = strlen(ssid);
    if (ssid_len == sizeof(wifi_cfg.sta.ssid)) {
        // Like the password field, ESP-IDF stores the maximum-length value in the full fixed
        // array without a terminator. A size-1 strncpy would silently turn a valid 32-byte SSID
        // into a different 31-byte network name.
        memcpy(wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    } else {
        strncpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    }
    const size_t password_len = strlen(password);
    if (password_len == sizeof(wifi_cfg.sta.password)) {
        // ESP-IDF accepts a 64-hex raw PSK in the full fixed-width field; it is intentionally
        // not NUL-terminated. Validation before persistence guarantees all 64 bytes are hex.
        memcpy(wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    } else {
        strncpy((char*)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    }
    wifi_cfg.sta.threshold.authmode = password_len == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    // Pick the STRONGEST AP for the SSID, not the first one heard. The default WIFI_FAST_SCAN
    // stops at the first matching BSSID (channel-order/timing dependent), so on a multi-AP
    // network this device — stationary near the car — would latch onto whatever answers first,
    // often a far/weak AP, and the ESP32 STA never roams off it. ALL_CHANNEL_SCAN scans every
    // channel; BY_SIGNAL then connects to the highest RSSI. Costs ~1-2 s more at connect.
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    net_boot_require(esp_wifi_set_mode(WIFI_MODE_STA), "WiFi station mode");
    net_boot_require(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), "WiFi station configuration");
    net_boot_require(esp_wifi_start(), "WiFi station start");

    // Keep WiFi modem-sleep at MIN_MODEM (the IDF default). Modem-sleep parks the radio between
    // DTIM beacons, which DOES add ~100 ms per round-trip (the original cause of the sluggish
    // web UI) — but WIFI_PS_NONE is NOT an option here: WiFi and BLE share ONE radio, and
    // ESP-IDF coexistence relies on WiFi modem-sleep to hand it to BLE. Setting WIFI_PS_NONE
    // starves BLE so badly that GATT connections to the car time out (live-verified: every
    // connect failed with NimBLE "connect error: 13"), breaking evcc and pairing. So we MUST
    // leave power-save on and tackle web-UI latency elsewhere — the page is gzipped (~13 KB vs
    // 41 KB) and the TCP window is enlarged (sdkconfig.defaults), clearing it in ~1-2 RTTs.
    net_boot_require(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), "WiFi/BLE coexistence power mode");

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
    // Gated on the WiFi LEASE, not on net_kind(): a board that fell back to WiFi and later had a
    // cable plugged in reports NetLink::Eth while the radio is still associated, and hiding the
    // SSID/RSSI there would make /status claim the WiFi link vanished when it did not. The lease
    // flag is exactly the window in which esp_wifi_sta_get_ap_info() is safe to read (it is set
    // on GOT_IP and cleared on DISCONNECTED), which is the guard that matters.
    if (!s_wifi_lease.load()) return false;
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

const char* net_wifi_standard() {
    if (!s_wifi_lease.load()) return nullptr;
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

// ── Ethernet (W5500 over SPI) ─────────────────────────────────────────────────
// Compiled in only where CONFIG_TESLA_ETH_ENABLED is set (esp32s3 today). Everything below is
// #if'd out otherwise, and the two accessors fall back to "no wire" — which keeps every caller
// above the seam free of #ifdefs.
#if CONFIG_TESLA_ETH_ENABLED

static esp_eth_handle_t s_eth_handle = nullptr;
// Above WIFI_STA_DEF's 100 (esp_netif_defaults.h), so the wire takes the DEFAULT ROUTE when
// both transports hold a lease. Be precise about what that does and does not buy — measured
// against lwIP's ip4_route() (components/lwip/.../ip4.c), not assumed:
//
//   • OFF-LINK destinations (anything via the gateway — NTP, OTA, an MQTT broker or syslog
//     collector outside the subnet) go to netif_default, and route_prio is exactly what
//     esp_netif uses to choose it. Without this the WiFi station would win them.
//   • ON-LINK destinations do NOT consult it at all. ip4_route() walks netif_list and returns
//     the FIRST netif that is up and whose subnet matches the destination. With both interfaces
//     on the same /24 — the normal home case — same-subnet traffic therefore leaves over
//     whichever netif was registered first, i.e. the WiFi station.
//
// That asymmetry is accepted rather than fought: forcing per-packet source selection across two
// netifs on one subnet means overriding the stack's routing, and the case it would improve is
// the runtime hot-plug — which never delivers this transport's actual benefit anyway, because
// WiFi is already running (coexistence paid, heap spent). The benefit lives in the boot-with-
// cable path, where WiFi is never started and there IS no second netif.
static constexpr int kEthRoutePrio = 128;

// How much longer to wait for a lease when the PHY says the cable IS connected. A live wire with
// a slow DHCP server must not cost this board its whole point (a WiFi stack running for the rest
// of the boot), but a segment with no DHCP server at all still has to end up somewhere — hence a
// cap rather than an unbounded wait.
static constexpr int kEthLeaseLinkedCapFactor = 3;

// How long to give the PHY to report a link before concluding there is no cable. Auto-negotiation
// on 10/100 finishes in a second or two — measured at 2.0 s on the ATOMIC PoE Base — so 4 s is
// generous without being a stall. This is deliberately NOT the lease deadline: "is a cable
// connected" is answerable in seconds, "will DHCP answer" is not, and conflating them made a
// board with no credentials sit dark for the whole lease window before its setup AP appeared —
// precisely when somebody is standing next to it waiting for that AP.
//
// A switch running spanning-tree without portfast does NOT need a longer grace: STP delays
// FORWARDING, not the PHY's auto-negotiation, so the link event still arrives on time and it is
// the DHCP wait in phase 2 — deliberately generous — that absorbs the blocked-then-learning
// interval.
static constexpr int kEthLinkGraceMs = 4000;
static constexpr int kEthLinkPollMs  = 250;

static esp_netif_t*     s_eth_netif  = nullptr;
static esp_netif_t* s_eth_netif_ptr() { return s_eth_netif; }
static esp_eth_netif_glue_handle_t s_eth_glue = nullptr;
static EventGroupHandle_t s_eth_events = nullptr;
static bool s_spi_bus_up = false;
static std::atomic<bool> s_eth_link{false};
static EthSpiCandidate s_latched_eth_pins = {};

static spi_host_device_t eth_spi_host();
static void eth_event_handler(void*, esp_event_base_t, int32_t, void*);

// net_eth_probe() deliberately retains the bus for the driver.  Any later construction failure
// must give it back or the WiFi/setup fallback inherits claimed pins and a retry would mistake the
// physical-presence bit for an initialized bus.
static bool eth_release_spi_bus() {
    if (!s_spi_bus_up) return true;
    const esp_err_t err = spi_bus_free(eth_spi_host());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "W5500 SPI bus release failed: %s", esp_err_to_name(err));
        return false;
    }
    s_spi_bus_up = false;
    s_latched_eth_pins = {};
    return true;
}

struct EthStartupResources {
    EventGroupHandle_t events = nullptr;
    esp_netif_t* netif = nullptr;
    esp_eth_mac_t* mac = nullptr;
    esp_eth_phy_t* phy = nullptr;
    esp_eth_handle_t handle = nullptr;
    esp_eth_netif_glue_handle_t glue = nullptr;
    bool eth_handler_registered = false;
    bool ip_handler_registered = false;
    bool start_attempted = false;
    bool published = false;
};

static bool eth_cleanup_startup(EthStartupResources& r) {
    bool clean = true;

    // esp_eth_start() changes the driver's FSM before auto-negotiation, event posting and timer
    // activation.  A failing call can therefore still require stop(); ESP_ERR_INVALID_STATE is
    // the harmless case where it never crossed that boundary (or already returned to STOP).
    if (r.start_attempted && r.handle) {
        const esp_err_t err = esp_eth_stop(r.handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "W5500 partial driver stop failed: %s", esp_err_to_name(err));
            clean = false;
        }
    }

    if (r.ip_handler_registered) {
        const esp_err_t err = esp_event_handler_unregister(
            IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "W5500 IP handler unregister failed: %s", esp_err_to_name(err));
            clean = false;
        }
        r.ip_handler_registered = false;
    }
    if (r.eth_handler_registered) {
        const esp_err_t err = esp_event_handler_unregister(
            ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "W5500 event handler unregister failed: %s", esp_err_to_name(err));
            clean = false;
        }
        r.eth_handler_registered = false;
    }

    // Callbacks need coherent globals during activation.  Retract them only after the driver is
    // stopped and the application handlers are gone, before any pointed-to object is destroyed.
    if (r.published) {
        if (s_eth_handle == r.handle) s_eth_handle = nullptr;
        if (s_eth_glue == r.glue) s_eth_glue = nullptr;
        if (s_eth_netif == r.netif) s_eth_netif = nullptr;
        if (s_eth_events == r.events) s_eth_events = nullptr;
        s_eth_link.store(false);
        link_down(NetLink::Eth);
        r.published = false;
    }

    if (r.glue) {
        const esp_err_t err = esp_eth_del_netif_glue(r.glue);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "W5500 netif glue release failed: %s", esp_err_to_name(err));
            clean = false;
        }
        r.glue = nullptr;
    }
    if (r.netif) {
        esp_netif_destroy(r.netif);
        r.netif = nullptr;
    }

    // The driver owns the initialized MAC/PHY relationship but not the objects themselves:
    // uninstall deinitializes them, then their explicit del methods release their allocations.
    // If uninstall fails, deleting either object or its SPI device would create a live dangling
    // driver; leave that tail intact and force a fatal boot instead of manufacturing a UAF.
    bool driver_released = true;
    if (r.handle) {
        const esp_err_t err = esp_eth_driver_uninstall(r.handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "W5500 driver uninstall failed: %s", esp_err_to_name(err));
            driver_released = false;
            clean = false;
        } else {
            r.handle = nullptr;
        }
    }
    if (driver_released) {
        if (r.phy) {
            const esp_err_t err = r.phy->del(r.phy);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "W5500 PHY release failed: %s", esp_err_to_name(err));
                clean = false;
            }
            r.phy = nullptr;
        }
        if (r.mac) {
            const esp_err_t err = r.mac->del(r.mac);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "W5500 MAC release failed: %s", esp_err_to_name(err));
                clean = false;
            }
            r.mac = nullptr;
        }
    }
    if (r.events) {
        vEventGroupDelete(r.events);
        r.events = nullptr;
    }
    if (driver_released && !eth_release_spi_bus()) clean = false;
    return clean;
}

static bool eth_startup_fallback(EthStartupResources& r) {
    if (!eth_cleanup_startup(r)) boot_fatal("Ethernet startup cleanup");
    return false;
}

[[noreturn]] static void eth_startup_fatal(EthStartupResources& r, const char* component) {
    if (!eth_cleanup_startup(r)) boot_fatal("Ethernet startup cleanup");
    boot_fatal(component);
}

// The W5500's identity register. VERSIONR lives at 0x0039 of the Common Register block and
// reads a fixed 0x04 on every part — the only positive way to tell "a W5500 is wired to these
// pins" from "these pins are floating". A floating MISO reads 0x00 or 0xFF, so the check has no
// realistic false positive.
static constexpr uint16_t kW5500VersionReg = 0x0039;
static constexpr uint8_t  kW5500VersionVal = 0x04;

static spi_host_device_t eth_spi_host() { return SPI2_HOST; }

// Read one byte from the W5500 Common Register block. The frame is 3 bytes of address phase
// (16-bit offset + a control byte whose BSB/RWB/OM fields select "common block, read, variable
// length") followed by the data byte, so a single 4-byte full-duplex transfer does it.
static bool w5500_read_common(spi_device_handle_t dev, uint16_t reg, uint8_t* out) {
    uint8_t tx[4] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), 0x00, 0x00 };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    spi_transaction_t t = {};
    t.length    = sizeof(tx) * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(dev, &t) != ESP_OK) return false;
    *out = rx[3];
    return true;
}

// Is a W5500 actually wired to the configured pins? Called ONCE, very early — before the setup
// portal decision, because a wired board with no stored SSID must not be sent to a captive AP it
// does not need.
//
// On success the SPI bus stays installed for net_start_eth() to reuse; on failure it is torn
// down completely, so a board without the base leaves those GPIOs exactly as it found them.
bool net_eth_probe() {
    if (s_eth_present.load() && s_spi_bus_up) return true;

    // NEVER probe on the T-Dongle-S3. Its ST7735 sits on MOSI 3 / SCK 5 / CS 4 — GPIO5 is
    // literally the pin this probe would drive as SPI clock. The display has not claimed the bus
    // yet at this point in boot, so the fight would not show up here; it would show up later as
    // a panel that never initialises, which is a miserable thing to debug.
    if (board_is_t_dongle_s3()) {
        ESP_LOGI(TAG, "T-Dongle-S3 detected — skipping the W5500 probe (its panel owns GPIO5)");
        return false;
    }

    // Curated candidate list. If Kconfig configured pins differ from candidate 0 (M5Stack 5/6/7/8),
    // prepend the Kconfig configuration so custom builds take precedence.
    const EthSpiCandidate custom_cand = {
        "Configured SPI",
        CONFIG_TESLA_ETH_SPI_SCLK,
        CONFIG_TESLA_ETH_SPI_CS,
        CONFIG_TESLA_ETH_SPI_MISO,
        CONFIG_TESLA_ETH_SPI_MOSI
    };

    const bool custom_differs =
        (custom_cand.sclk != kEthDefaultCandidates[0].sclk ||
         custom_cand.cs   != kEthDefaultCandidates[0].cs ||
         custom_cand.miso != kEthDefaultCandidates[0].miso ||
         custom_cand.mosi != kEthDefaultCandidates[0].mosi);

    EthSpiCandidate candidate_storage[kEthDefaultCandidateCount + 1];
    const EthSpiCandidate* candidates = kEthDefaultCandidates;
    size_t num_candidates = kEthDefaultCandidateCount;

    if (custom_differs) {
        candidate_storage[0] = custom_cand;
        for (size_t i = 0; i < kEthDefaultCandidateCount; ++i) {
            candidate_storage[i + 1] = kEthDefaultCandidates[i];
        }
        candidates = candidate_storage;
        num_candidates = kEthDefaultCandidateCount + 1;
    }

    for (size_t i = 0; i < num_candidates; ++i) {
        const auto& cand = candidates[i];
        if (!eth_candidate_pins_valid(cand)) {
            ESP_LOGW(TAG, "skipping invalid Ethernet SPI candidate '%s'", cand.name);
            continue;
        }

        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = cand.mosi;
        buscfg.miso_io_num = cand.miso;
        buscfg.sclk_io_num = cand.sclk;
        buscfg.quadwp_io_num = -1;
        buscfg.quadhd_io_num = -1;
        if (spi_bus_initialize(eth_spi_host(), &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
            ESP_LOGW(TAG, "W5500 probe (%s): SPI bus init failed", cand.name);
            continue;
        }

        spi_device_interface_config_t devcfg = {};
        devcfg.mode           = 0;                                        // W5500 is SPI mode 0
        devcfg.clock_speed_hz = CONFIG_TESLA_ETH_SPI_CLOCK_MHZ * 1000 * 1000;
        devcfg.spics_io_num   = cand.cs;
        devcfg.queue_size     = 1;

        spi_device_handle_t dev = nullptr;
        uint8_t ver = 0;
        bool found = false;
        if (spi_bus_add_device(eth_spi_host(), &devcfg, &dev) == ESP_OK) {
            found = w5500_read_common(dev, kW5500VersionReg, &ver) && ver == kW5500VersionVal;
            spi_bus_remove_device(dev);   // the driver adds its own device with its own config
        }

        if (found) {
            s_spi_bus_up = true;
            s_latched_eth_pins = cand;
            s_eth_present.store(true);
            ESP_LOGI(TAG, "W5500 found (VERSIONR=0x%02x) on %s: SCLK%d/CS%d/MISO%d/MOSI%d @ %d MHz",
                     ver, cand.name, cand.sclk, cand.cs, cand.miso, cand.mosi,
                     CONFIG_TESLA_ETH_SPI_CLOCK_MHZ);
            return true;
        }

        ESP_LOGI(TAG, "no W5500 on %s (SCLK%d/CS%d/MISO%d/MOSI%d, VERSIONR=0x%02x, expected 0x%02x)",
                 cand.name, cand.sclk, cand.cs, cand.miso, cand.mosi, ver, kW5500VersionVal);

        const esp_err_t err = spi_bus_free(eth_spi_host());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "W5500 probe bus cleanup failed for %s: %s", cand.name, esp_err_to_name(err));
            boot_fatal("W5500 probe cleanup");
        }
    }

    ESP_LOGI(TAG, "no W5500 on SPI across %zu candidate(s) — WiFi only", num_candidates);
    return false;
}

static const int ETH_GOT_IP_BIT = BIT0;

// Does the PHY report a negotiated link? Distinct from holding a LEASE, and the distinction is
// what keeps the WiFi stack switched off on a wired board: "no cable" and "cable in, DHCP still
// thinking" call for opposite answers at the boot deadline.
static void eth_event_handler(void*, esp_event_base_t base, int32_t event_id, void* data) {
    try {
      if (base == ETH_EVENT && event_id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
        s_eth_link.store(true);
    } else if (base == ETH_EVENT && event_id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Ethernet link down");
        s_eth_link.store(false);
        link_down(NetLink::Eth);
    } else if (base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "IP (eth): " IPSTR, IP2STR(&ev->ip_info.ip));
        link_up(NetLink::Eth);
        xEventGroupSetBits(s_eth_events, ETH_GOT_IP_BIT);
      }
    } catch (...) {
        ESP_LOGE(TAG, "Ethernet event callback threw; event dropped");
    }
}

bool net_start_eth() {
    if (!net_eth_probe()) return false;

    // The probe ran before esp-netif by design and retained SPI ownership.  Observe substrate
    // failure here so that ownership can still be unwound before boot_fatal parks a valid image.
    const char* substrate_component = nullptr;
    const esp_err_t substrate_err = net_init_substrate(&substrate_component);
    if (substrate_err != ESP_OK) {
        if (!eth_release_spi_bus()) boot_fatal("Ethernet startup cleanup");
        net_boot_require(substrate_err, substrate_component);
    }

    EthStartupResources r{};
    r.events = xEventGroupCreate();
    if (!r.events) eth_startup_fatal(r, "Ethernet event group");

    // Give the Ethernet netif a HIGHER route priority than the WiFi station, because ESP-IDF
    // defaults the other way round: esp_netif_defaults.h ships WIFI_STA_DEF at route_prio 100
    // and ETH_DEF at 50. Left alone, a board that came up on WiFi and later had a cable plugged
    // in would report NetLink::Eth while lwIP kept routing every outgoing packet — MQTT, syslog,
    // NTP, OTA — over the radio, and /status.ip would name an address nothing dialled out from.
    // Worse, the connectivity watchdog would ICMP the ETHERNET gateway while the traffic path
    // was WiFi, and bounce a perfectly good MAC every ~60 s if that segment does not answer echo.
    //
    // The whole point of this transport is that the wire wins, so make lwIP agree rather than
    // asserting it in a comment. `static` because esp_netif keeps a pointer to this config.
    static esp_netif_inherent_config_t eth_base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    eth_base.route_prio = kEthRoutePrio;
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_cfg.base = &eth_base;
    r.netif = esp_netif_new(&netif_cfg);
    if (!r.netif) eth_startup_fatal(r, "Ethernet netif");
    if (esp_netif_set_hostname(r.netif, kNetHostname) != ESP_OK)
        ESP_LOGW(TAG, "could not set DHCP hostname '%s'", kNetHostname);

    spi_device_interface_config_t devcfg = {};
    devcfg.mode           = 0;
    devcfg.clock_speed_hz = CONFIG_TESLA_ETH_SPI_CLOCK_MHZ * 1000 * 1000;
    devcfg.spics_io_num   = s_latched_eth_pins.cs;
    devcfg.queue_size     = 20;

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(eth_spi_host(), &devcfg);
    // POLLING mode. The ATOMIC PoE Base routes only SCLK/CS/MISO/MOSI + power, so there is no
    // interrupt line to wire; -1 selects polling and poll_period_ms sets the cadence. This is a
    // supported configuration, not a workaround — ESP-IDF ships a CI config for exactly it
    // (components/esp_eth/test_apps/sdkconfig.ci.poll_w5500, also at 10 ms). It bounds RX
    // LATENCY, not throughput: each poll drains everything queued in the W5500's 16 KB buffer.
    w5500_cfg.int_gpio_num   = -1;
    w5500_cfg.poll_period_ms = CONFIG_TESLA_ETH_POLL_MS;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    // The base routes no RESET line either, so the driver must not try to strobe one; the W5500
    // is reset over SPI (its MR register) instead, which esp_eth_phy_w5500 does at init.
    phy_cfg.reset_gpio_num = -1;

    r.mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    r.phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!r.mac || !r.phy) {
        ESP_LOGE(TAG, "W5500 mac/phy alloc failed");
        return eth_startup_fallback(r);
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(r.mac, r.phy);
    esp_err_t err = esp_eth_driver_install(&eth_cfg, &r.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "W5500 driver install failed: %s", esp_err_to_name(err));
        return eth_startup_fallback(r);
    }

    // The W5500 has no MAC address of its own (no EEPROM), so one must be supplied. ESP_MAC_ETH
    // is the chip's eFuse-derived Ethernet address — stable across reboots and distinct from the
    // WiFi STA MAC, so the two interfaces can never collide on the same LAN.
    uint8_t mac_addr[6] = {0};
    if (esp_read_mac(mac_addr, ESP_MAC_ETH) == ESP_OK)
        esp_eth_ioctl(r.handle, ETH_CMD_S_MAC_ADDR, mac_addr);

    r.glue = esp_eth_new_netif_glue(r.handle);
    if (!r.glue) {
        ESP_LOGE(TAG, "W5500 netif glue allocation failed");
        return eth_startup_fallback(r);
    }
    err = esp_netif_attach(r.netif, r.glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "W5500 netif attach failed: %s", esp_err_to_name(err));
        return eth_startup_fallback(r);
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                     eth_event_handler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet event handler registration failed: %s",
                 esp_err_to_name(err));
        eth_startup_fatal(r, "Ethernet event handler registration");
    }
    r.eth_handler_registered = true;

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                     eth_event_handler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet IP handler registration failed: %s", esp_err_to_name(err));
        eth_startup_fatal(r, "Ethernet IP handler registration");
    }
    r.ip_handler_registered = true;

    // Construction is complete.  Publish one coherent set immediately before activation: the
    // event-loop task may run a link/IP callback before esp_eth_start() returns, and that callback
    // must already find its matching netif, event group and driver handle.  A start failure
    // retracts this set before destroying any resource.
    s_eth_events = r.events;
    s_eth_netif = r.netif;
    s_eth_handle = r.handle;
    s_eth_glue = r.glue;
    r.published = true;
    s_eth_link.store(false);
    link_down(NetLink::Eth);

    r.start_attempted = true;
    err = esp_eth_start(r.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver start failed: %s", esp_err_to_name(err));
        eth_startup_fatal(r, "Ethernet driver start");
    }

    // Ownership is now process-lifetime state.  It remains active even when the boot-time link or
    // DHCP windows below return false, so later cable insertion can claim the wired lease.

    // TWO questions, two very different deadlines. Answering them with one timer is what made a
    // credential-less board sit dark for the whole lease window before its setup AP appeared.
    //
    // ── phase 1: is a cable connected at all? ──
    // Seconds, not tens of seconds — the PHY reports link as soon as auto-negotiation completes.
    // No link by the grace window ⇒ no cable or a dead port; nothing is coming, so hand over to
    // WiFi (or the setup portal) NOW instead of spending a lease deadline on it.
    EventBits_t bits = 0;
    for (int waited = 0; waited < kEthLinkGraceMs && !s_eth_link.load(); waited += kEthLinkPollMs) {
        bits = xEventGroupWaitBits(s_eth_events, ETH_GOT_IP_BIT, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(kEthLinkPollMs));
        if (bits & ETH_GOT_IP_BIT) break;   // a very fast DHCP server can beat the link event here
    }
    if (!(bits & ETH_GOT_IP_BIT) && !s_eth_link.load()) {
        ESP_LOGW(TAG, "no Ethernet link within %d ms — no cable or a dead switch port; falling "
                      "back without spending the %d s lease deadline",
                 kEthLinkGraceMs, CONFIG_TESLA_ETH_WAIT_S);
        return false;
    }

    // ── phase 2: the cable IS there — wait for the lease ──
    // Falling back here would run the WiFi stack for the rest of the boot, paying the BLE
    // radio-coexistence cost and ~57 KB of largest-block on a board that is about to be wired
    // anyway. So wait properly — up to a cap, so a segment with no DHCP server at all still ends
    // up somewhere. A cable pulled mid-DHCP drops back into the no-link case and gives up.
    const int base_ms = CONFIG_TESLA_ETH_WAIT_S * 1000;
    const int cap_ms  = base_ms * kEthLeaseLinkedCapFactor;
    for (int waited = 0; !(bits & ETH_GOT_IP_BIT) && waited < cap_ms; waited += base_ms) {
        bits = xEventGroupWaitBits(s_eth_events, ETH_GOT_IP_BIT, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(base_ms));
        if (bits & ETH_GOT_IP_BIT) break;
        if (!s_eth_link.load()) {
            ESP_LOGW(TAG, "Ethernet link went away while waiting for DHCP (%d s) — falling back",
                     (waited + base_ms) / 1000);
            break;
        }
        ESP_LOGW(TAG, "Ethernet link is UP but no DHCP lease after %d s — still waiting (a wired "
                      "board should not have to start WiFi for a slow DHCP server)",
                 (waited + base_ms) / 1000);
    }
    if (bits & ETH_GOT_IP_BIT) {
        ESP_LOGI(TAG, "Ethernet up — WiFi will not be started (no radio coexistence, "
                      "and its heap stays free)");
        return true;
    }
    if (s_eth_link.load())
        ESP_LOGW(TAG, "Ethernet link up but still no DHCP lease after %d s — falling back to "
                      "WiFi", cap_ms / 1000);
    // The driver stays running on purpose: a cable plugged in later still brings the link up and
    // the event handler still claims the lease. Leaving WiFi to run alongside is the cost of
    // that, and it is the right trade — an unreachable board is worse than a shared radio.
    return false;
}

bool net_eth_phy(int* speed_mbps, bool* full_duplex) {
    if (!s_eth_lease.load() || !s_eth_handle) return false;
    eth_speed_t   sp = ETH_SPEED_10M;
    eth_duplex_t  dx = ETH_DUPLEX_HALF;
    if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED,  &sp) == ESP_OK && speed_mbps)
        *speed_mbps = (sp == ETH_SPEED_100M) ? 100 : 10;
    if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &dx) == ESP_OK && full_duplex)
        *full_duplex = (dx == ETH_DUPLEX_FULL);
    return true;
}

#else   // !CONFIG_TESLA_ETH_ENABLED

static esp_netif_t* s_eth_netif_ptr() { return nullptr; }
bool net_eth_probe() { return false; }
bool net_start_eth() { return false; }
bool net_eth_phy(int* speed_mbps, bool* full_duplex) {
    (void)speed_mbps; (void)full_duplex;
    return false;
}

#endif  // CONFIG_TESLA_ETH_ENABLED

// ── connectivity watchdog ─────────────────────────────────────────────────────
// The decision logic is logic/net_link.hpp's watch_step(); this half only supplies samples and
// carries out the verdict. See that header for why a never-answering gateway must not count.

static const int kWdPeriodS       = 30;   // connectivity-check cadence
static const int kWdPingTimeoutMs = 1000; // per-echo timeout
static const int kWdPingCount     = 3;    // echoes per check; healthy if ≥1 replies

// Persistent across probes. The shared generation owner retains a timed-out session until its
// exact on_ping_end acknowledgement, so neither callback storage nor semaphore can be reused by a
// later probe while the old ping task is still alive.
static PingProbeControl s_wd{};

// Set true the first time THIS TRANSPORT's gateway answers ICMP — the baseline watch_step()
// requires before it will act. Indexed by NetLink, and that indexing is the point: a single
// global flag let a freshly plugged-in Ethernet segment inherit "this gateway has answered
// before" from the WiFi gateway, which is exactly the false evidence the latch exists to
// refuse. The guard has to be per transport or it evaporates at the moment it is needed.
static std::atomic<bool> s_gw_ever_reachable[3] = {};
static_assert(static_cast<int>(NetLink::None) == 0 && static_cast<int>(NetLink::Wifi) == 1 &&
              static_cast<int>(NetLink::Eth)  == 2,
              "s_gw_ever_reachable is indexed by NetLink — keep the enum contiguous from 0");

static std::atomic<bool>& gw_baseline(NetLink k) {
    return s_gw_ever_reachable[static_cast<int>(k)];
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

    const PingProbeResult result = ping_probe_run(
        s_wd, cfg,
        pdMS_TO_TICKS(kWdPingCount * (kWdPingTimeoutMs + 250) + 2000),
        pdMS_TO_TICKS(2000));
    // Only an exact completed generation with zero replies is evidence of failure. Setup failure
    // or a quarantined late callback remains "unknown", so the watchdog cannot false-alarm.
    const bool ok = result == PingProbeResult::Reply;
    if (ok) gw_baseline(s_kind.load()).store(true);
    return result == PingProbeResult::NoReply ? false : true;
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
#if CONFIG_TESLA_ETH_ENABLED
        case NetLink::Eth:
            // The wired equivalent of a ghost association: the PHY still reports link (the
            // switch port is powered and the pair is intact) but nothing is forwarded, so no
            // ETHERNET_EVENT_DISCONNECTED ever fires. Stop/start re-runs auto-negotiation and
            // re-requests DHCP; the event handler re-claims the lease exactly as it does at boot.
            ESP_LOGW(TAG, "watchdog: wired link forwards nothing — restarting the Ethernet MAC");
            if (s_eth_handle) {
                esp_eth_stop(s_eth_handle);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_eth_start(s_eth_handle);
            }
            break;
#endif
        default:
            // NetLink::None cannot reach here (watch_step returns Idle on a down link), and
            // a transport with no recovery action simply keeps its lease; the next probe
            // re-evaluates.
            break;
    }
}

static void net_watchdog_task(void*) {
    try {
      tk::LinkWatch watch{};
      for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));

        // When the link already knows it is down, the transport's own reconnect path owns
        // recovery — there is nothing to detect (the ghost case is link=up by definition) and
        // logging every period would fill the 16 KB /diag ring across a long router outage.
        const bool up = net_is_up();
        const bool gw = up && gateway_reachable();

        // The baseline belongs to the transport being probed, not to the boot.
        const bool gw_ever = gw_baseline(net_kind()).load();

        switch (tk::watch_step(watch, up, gw, gw_ever)) {
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
    } catch (...) {
        ESP_LOGE(TAG, "network watchdog task threw; stopping watchdog task");
        vTaskDelete(nullptr);
    }
}

bool net_watchdog_start() {
    s_wd.done = xSemaphoreCreateBinary();
    if (!s_wd.done) return false;
    if (xTaskCreate(net_watchdog_task, "net_wd", 3072, nullptr,
                    kPrioWifiWatchdog, nullptr) != pdPASS) {
        vSemaphoreDelete(s_wd.done);
        s_wd.done = nullptr;
        return false;
    }
    return true;
}

}  // namespace tk
