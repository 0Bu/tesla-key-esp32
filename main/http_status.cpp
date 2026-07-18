// Web-UI-facing routes: the embedded page itself and the endpoints that drive it:
//   GET  /        (embedded, pre-gzipped web UI)
//   GET  /status  (device + pairing state — a request/response snapshot of the live state)
//   GET  /diag    (in-memory diagnostic log)
//   POST /scan    (time-limited BLE discovery scan)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).
//
// The live web UI no longer polls /status on a timer: it holds one WebSocket to /events and the
// device pushes this same JSON (built by build_status_object() below) on a fixed cadence — see
// http_events.cpp. /status stays as the request/response form for curl/diagnostics and the
// post-OTA reboot probe (app.js waitReboot()); build_status_object() is the ONE builder both paths
// share, so the pushed frame and a manual GET can never drift.

#include "http_handlers.hpp"
#include "diag_log.hpp"
#include "mqtt_ha.hpp"
#include "syslog.hpp"
#include <esp_netif.h>
#include <esp_app_desc.h>
#include <esp_wifi.h>
#include <ctime>
#include <string>

// ─── POST /scan — start a time-limited BLE discovery scan ─────────────────────

esp_err_t handle_scan(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    g_vehicle->ble_scan();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "result", true);
    cJSON_AddStringToObject(root, "reason", "scanning for nearby Teslas (~12s)");
    return send_json(req, 200, root);
}

// ─── GET /status — device + pairing state (drives the web UI) ──────────────────

static void current_ip(char* out, size_t sz) {
    out[0] = '\0';
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, out, sz);
    }
}

// Whether the car is "live" (awake) vs merely sleeping vs unreachable is decided centrally
// in VehicleController::link_state(), shared with the MQTT bridge so the two never drift.

// Build the device + pairing + vehicle status object (caller owns the returned cJSON). This is the
// single source for BOTH the GET /status response and the /events WebSocket push (http_events.cpp),
// so the two can never drift. Pure state read — safe to call from the httpd task or the broadcast
// task (it only reads caches + the esp_wifi/mqtt/syslog getters, the same reads GET /status already
// did concurrently with the telemetry writer). May return nullptr under OOM (cJSON alloc failure);
// every caller guards for it.
cJSON* build_status_object() {
    char ip[16];
    current_ip(ip, sizeof(ip));

    cJSON* root = cJSON_CreateObject();
    // The by-value getters below (vin(), mqtt_ha_broker(), syslog_status(), the cached-telemetry
    // structs …) each allocate a std::string/std::vector and can throw std::bad_alloc mid-build.
    // cJSON has no RAII, so free the partial tree if that happens before it propagates — the
    // broadcast task (http_events.cpp) calls this every ~2 s with no guard above it, and precisely
    // under the memory pressure that makes these throw. On the success path we hand ownership back.
    struct RootGuard { cJSON* p; ~RootGuard() { if (p) cJSON_Delete(p); } } guard{root};
    cJSON_AddStringToObject(root, "vin",           g_vehicle->vin().c_str());
    cJSON_AddStringToObject(root, "ip",            ip);
    cJSON_AddStringToObject(root, "version",       esp_app_get_description()->version);
    cJSON_AddBoolToObject(root,   "key_present",   g_vehicle->has_key());
    cJSON_AddStringToObject(root, "key_fingerprint", g_vehicle->key_fingerprint().c_str());
    // Key creation date (epoch seconds), shown under the fingerprint in the UI.
    // Only emit a plausible wall-clock value (post-2020); a near-zero stamp means
    // the clock hadn't synced when the key was generated, so report it as unknown.
    time_t key_created = g_vehicle->key_created_at();
    if (key_created > 1600000000) cJSON_AddNumberToObject(root, "key_created", (double)key_created);
    cJSON_AddBoolToObject(root,   "paired",        g_vehicle->has_session());
    // Pairing date (epoch seconds), shown under "Paired" in the UI. Same post-2020
    // plausibility guard as key_created.
    time_t paired_at = g_vehicle->paired_at();
    if (paired_at > 1600000000) cJSON_AddNumberToObject(root, "paired_at", (double)paired_at);
    // True when the previous pairing was lost (key removed on the car side) and a
    // re-pair is pending — lets the UI explain why it's prompting to pair again.
    cJSON_AddBoolToObject(root,   "reauth",        g_vehicle->reauth_required());

    // WiFi: SSID + live signal strength (dBm) of the station link.
    cJSON* wifi = cJSON_CreateObject();
    wifi_ap_record_t ap{};
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(wifi, "ssid", (const char*)ap.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
        // Highest 802.11 generation the AP advertises → friendly Wi-Fi name. The
        // ESP32-S3 radio itself tops out at 802.11n, but the flags reflect the AP's
        // capability, so a Wi-Fi 6 router still reads "Wi-Fi 6".
        const char* std_ = ap.phy_11ax ? "Wi-Fi 6"
                         : ap.phy_11ac ? "Wi-Fi 5"
                         : ap.phy_11n  ? "Wi-Fi 4"
                         : ap.phy_11g  ? "802.11g"
                         : ap.phy_11b  ? "802.11b" : nullptr;
        if (std_) cJSON_AddStringToObject(wifi, "std", std_);
    }
    cJSON_AddItemToObject(root, "wifi", wifi);

    // MQTT (Home Assistant bridge): whether a broker is configured, whether the
    // session is live, and the host:port. Drives the web-UI Connection block.
    cJSON* mqtt = cJSON_CreateObject();
    cJSON_AddBoolToObject(mqtt, "configured", mqtt_ha_configured());
    cJSON_AddBoolToObject(mqtt, "connected",  mqtt_ha_connected());
    cJSON_AddBoolToObject(mqtt, "tls",        mqtt_ha_tls());
    std::string broker = mqtt_ha_broker();
    if (!broker.empty()) cJSON_AddStringToObject(mqtt, "broker", broker.c_str());
    std::string mqtt_err = mqtt_ha_last_error();
    if (!mqtt_err.empty()) cJSON_AddStringToObject(mqtt, "error", mqtt_err.c_str());
    cJSON_AddItemToObject(root, "mqtt", mqtt);

    // Syslog: whether a server is configured, whether DNS has resolved a destination
    // (delivery gate), and the advisory ARP/ICMP reachability hint. Drives the
    // web-UI Connections card.
    cJSON* syslog = cJSON_CreateObject();
    SyslogStatus sy = syslog_status();
    cJSON_AddBoolToObject(syslog, "configured", sy.configured);
    cJSON_AddBoolToObject(syslog, "resolved",   sy.resolved);
    cJSON_AddBoolToObject(syslog, "reachable",  sy.reachable);
    if (!sy.host.empty()) {
        cJSON_AddStringToObject(syslog, "host", sy.host.c_str());
        cJSON_AddNumberToObject(syslog, "port", sy.port);
    }
    if (!sy.error.empty()) cJSON_AddStringToObject(syslog, "error", sy.error.c_str());
    cJSON_AddItemToObject(root, "syslog", syslog);

    // BLE: when connected report the live signal strength; otherwise list nearby
    // Teslas seen while scanning, with their RSSI.
    cJSON* ble = cJSON_CreateObject();
    bool connected = g_vehicle->ble_connected();
    cJSON_AddBoolToObject(ble, "connected", connected);
    cJSON_AddBoolToObject(ble, "scanning",  g_vehicle->ble_scanning());
    if (connected) {
        int8_t rssi = 0;
        if (g_vehicle->ble_rssi(rssi)) cJSON_AddNumberToObject(ble, "rssi", rssi);
        cJSON_AddStringToObject(ble, "addr", g_vehicle->ble_peer().c_str());

        // Read-only telemetry caches, refreshed by the rotating background poll. Each
        // numeric field is emitted only when the car actually reported it (presence
        // flag) so the UI can show "—" for anything not yet seen. Grouped under "tele".
        cJSON* tele = cJSON_CreateObject();
        ClimateStateResult cl = g_vehicle->get_cached_climate();
        if (cl.valid) {
            cJSON* o = cJSON_CreateObject();
            if (cl.has_inside)   cJSON_AddNumberToObject(o, "inside",   cl.inside_temp);
            if (cl.has_outside)  cJSON_AddNumberToObject(o, "outside",  cl.outside_temp);
            if (cl.has_setpoint) cJSON_AddNumberToObject(o, "setpoint", cl.driver_setpoint);
            if (cl.has_climate_on)      cJSON_AddBoolToObject(o, "on",              cl.is_climate_on);
            if (cl.has_preconditioning) cJSON_AddBoolToObject(o, "preconditioning", cl.is_preconditioning);
            // Cabin Overheat Protection (separate subsystem; emitted only when the
            // car actually reported each field, so an absent one means "not sent").
            if (cl.has_cop)         cJSON_AddStringToObject(o, "cop",         cl.cop.c_str());
            if (cl.has_cop_cooling) cJSON_AddBoolToObject(o,   "cop_cooling", cl.cop_cooling);
            if (cl.has_cop_temp)    cJSON_AddStringToObject(o, "cop_temp",    cl.cop_temp.c_str());
            if (cl.has_cop_reason)  cJSON_AddStringToObject(o, "cop_reason",  cl.cop_reason.c_str());
            // Defrost (front/rear defroster + Max-defrost mode), emitted only when reported.
            if (cl.has_front_defrost) cJSON_AddBoolToObject(o,   "front_defrost", cl.front_defrost);
            if (cl.has_rear_defrost)  cJSON_AddBoolToObject(o,   "rear_defrost",  cl.rear_defrost);
            if (cl.has_defrost_mode)  cJSON_AddStringToObject(o, "defrost_mode",  cl.defrost_mode.c_str());
            cJSON_AddItemToObject(tele, "climate", o);
        }
        DriveStateResult dr = g_vehicle->get_cached_drive();
        if (dr.valid) {
            cJSON* o = cJSON_CreateObject();
            if (!dr.shift_state.empty()) cJSON_AddStringToObject(o, "shift", dr.shift_state.c_str());
            if (dr.has_odometer)         cJSON_AddNumberToObject(o, "odometer_km", dr.odometer_km);
            cJSON_AddItemToObject(tele, "drive", o);
        }
        TirePressureResult tp = g_vehicle->get_cached_tires();
        if (tp.valid) {
            cJSON* o = cJSON_CreateObject();
            if (tp.has_fl) cJSON_AddNumberToObject(o, "fl", tp.fl);
            if (tp.has_fr) cJSON_AddNumberToObject(o, "fr", tp.fr);
            if (tp.has_rl) cJSON_AddNumberToObject(o, "rl", tp.rl);
            if (tp.has_rr) cJSON_AddNumberToObject(o, "rr", tp.rr);
            cJSON_AddBoolToObject(o, "warn", tp.warn);
            cJSON_AddItemToObject(tele, "tires", o);
        }
        ClosuresStateResult cz = g_vehicle->get_cached_closures();
        if (cz.valid) {
            cJSON* o = cJSON_CreateObject();
            if (cz.has_locked)       cJSON_AddBoolToObject(o, "locked", cz.locked);
            cJSON_AddBoolToObject(o, "door",   cz.any_door_open);
            cJSON_AddBoolToObject(o, "frunk",  cz.frunk_open);
            cJSON_AddBoolToObject(o, "trunk",  cz.trunk_open);
            cJSON_AddBoolToObject(o, "window", cz.any_window_open);
            if (cz.has_user_present) cJSON_AddBoolToObject(o, "user", cz.user_present);
            cJSON_AddItemToObject(tele, "closures", o);
        }
        cJSON_AddItemToObject(root, "tele", tele);
    } else {
        cJSON* devices = cJSON_CreateArray();
        for (const auto& d : g_vehicle->ble_nearby()) {
            cJSON* dev = cJSON_CreateObject();
            cJSON_AddStringToObject(dev, "addr", d.addr.c_str());
            cJSON_AddStringToObject(dev, "name", d.name.c_str());
            cJSON_AddNumberToObject(dev, "rssi", d.rssi);
            cJSON_AddBoolToObject(dev, "connectable", d.connectable);
            cJSON_AddItemToArray(devices, dev);
        }
        cJSON_AddItemToObject(ble, "devices", devices);
        // The target car was found (advert heard, VIN-name matched) but the link keeps
        // failing to come up — e.g. another device is holding the car's single BLE
        // connection. Emitted only while actively failing so the UI can say "found but
        // can't connect" instead of blaming Bluetooth range. Absent ⇒ nothing to report.
        uint32_t cf = g_vehicle->ble_connect_fail();
        if (cf > 0) {
            cJSON_AddNumberToObject(ble, "connect_fail", cf);
            // Last-seen advert RSSI so the UI shows real bars + dBm in the "can't connect"
            // state (the devices[] list goes empty during the 10 s connect attempt, so it's
            // not a reliable source there). Same `rssi` key the connected branch uses.
            int8_t srssi;
            if (g_vehicle->ble_seen_rssi(srssi)) cJSON_AddNumberToObject(ble, "rssi", srssi);
            // Why it's failing, mirroring vehicle-command's ErrMaxConnectionsExceeded: the
            // target's advert connectability. false ⇒ the car is non-connectable (≈ at its
            // ~3-device BLE limit); true ⇒ it accepts connections but the link still fails
            // (weak signal / another proxy contending). Omitted when not yet known.
            int tc = g_vehicle->ble_target_connectable();
            if (tc >= 0) cJSON_AddBoolToObject(ble, "car_connectable", tc == 1);
        }
    }
    cJSON_AddItemToObject(root, "ble", ble);

    // Overall connectivity, the single source of truth the UI keys the hero off (and the
    // same enum the MQTT bridge publishes) — so "asleep", "idle" and "unreachable" can never
    // be confused. awake ⇒ live SOC card; asleep ⇒ "Vehicle asleep" card (proven, debounced);
    // idle ⇒ reachable but not provably asleep ⇒ neutral "Parked" card (last-known
    // SOC + wake), never a sleep claim; unreachable ⇒ the car drove off / out of range ⇒ a neutral
    // grey "Unreachable" hero (last-known SOC); unknown ⇒ nothing heard yet ⇒ a grey "Connecting…"
    // hero. Decoupled from the momentary BLE link.
    // Mapping lives in logic/link_state.hpp (host-tested) — the MQTT bridge shares it so
    // the hero and the published sleep_status can never drift.
    cJSON_AddStringToObject(root, "link", tk::link_state_web_str(g_vehicle->link_state()));
    // Raw VCSEC sleep flag (un-debounced) for transparency/diagnostics — lets the UI/operator
    // see what the car actually reports vs. the debounced `link` above. Not used for the hero.
    cJSON_AddStringToObject(root, "vcsec_sleep", g_vehicle->vcsec_sleep_raw());

    // Live "vehicle" object — drives the UI's awake/SOC view. Emitted only when the car is
    // AWAKE (fresh infotainment telemetry per link_state()), independent of the momentary
    // BLE link: the link is dropped between polls so the car can sleep, but data inside the
    // freshness window is still live. When not awake the UI falls through to the asleep/"Parked"
    // card (reachable) or a neutral grey "Unreachable"/"Connecting…" hero (unreachable/unknown),
    // using `link` above.
    if (g_vehicle->link_state() == VehicleController::LinkState::Awake) {
        ChargeStateResult cs = g_vehicle->get_cached_charge();
        if (cs.valid) {
            cJSON* veh = cJSON_CreateObject();
            if (cs.has_battery_level)
                cJSON_AddNumberToObject(veh, "soc", cs.battery_level);
            cJSON_AddStringToObject(veh, "status", cs.charging_state.c_str());
            // Charge target — lets the UI mark "charge complete" (SOC >= limit) and gate the
            // start-charge tap, which the car would only reject as "complete" anyway. Emitted
            // only when the car reported it (proto3 optional); a missing limit means the UI
            // keeps the button live rather than blocking on unknown data.
            if (cs.has_charge_limit_soc)
                cJSON_AddNumberToObject(veh, "charge_limit", cs.charge_limit_soc);
            // Charging detail for the web UI: live power (kW) and current (A). Emitted only
            // when the car actually reported them (proto3 optional) so the UI omits the chip
            // rather than rendering a phantom 0. Power is a whole number (no decimals).
            if (cs.has_charger_power)
                cJSON_AddNumberToObject(veh, "power",  (int)(cs.charger_power + 0.5f));
            if (cs.has_charging_amps)
                cJSON_AddNumberToObject(veh, "amps",   cs.charging_amps);
            // Actual AC draw — the UI computes current × voltage × phases (see liveKw). current
            // and voltage are PER PHASE and phases is the count, so all three are required: an EU
            // 3-phase car reports phases=2 and omitting it halves the kW. Distinct from
            // charger_power (DC to the battery, 0 when "Complete"). Lets us see what the car pulls
            // from the wall while e.g. cabin-overheat-protection runs with a full battery. Emitted
            // only if reported (phases falls back to 1 in the UI when the car didn't send it).
            if (cs.has_actual_current)
                cJSON_AddNumberToObject(veh, "actual_amps", cs.charger_actual_current);
            if (cs.has_voltage)
                cJSON_AddNumberToObject(veh, "volts", cs.charger_voltage);
            if (cs.has_charger_phases)
                cJSON_AddNumberToObject(veh, "phases", cs.charger_phases);
            cJSON_AddItemToObject(root, "vehicle", veh);
        }
    }

    // Last-known vehicle snapshot for the "Vehicle asleep" card. The charge cache is
    // retained in RAM across BLE disconnects, so the UI can show the last battery level
    // (a sleeping car barely drains, so it stays a good estimate) and — via last_seen_s —
    // how long the car has been asleep, all without waking it. Emitted regardless of the
    // BLE link state; when awake the UI uses the live "vehicle" object instead.
    {
        ChargeStateResult last = g_vehicle->get_cached_charge();
        if (last.valid) {
            cJSON* lo = cJSON_CreateObject();
            if (last.has_battery_level)
                cJSON_AddNumberToObject(lo, "soc", last.battery_level);
            cJSON_AddStringToObject(lo, "status", last.charging_state.c_str());
            cJSON_AddItemToObject(root, "last", lo);
        }
        uint32_t ago = 0;
        if (g_vehicle->seconds_since_contact(ago))
            cJSON_AddNumberToObject(root, "last_seen_s", (double)ago);
    }
    guard.p = nullptr;   // built successfully — release ownership to the caller (send_json / WS push)
    return root;
}

esp_err_t handle_status(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    // A snapshot of live device state. Never let a browser/proxy serve a cached copy, or a manual
    // reload sticks on a stale state. Matches the no-store on "/" and "/diag". (The live UI reads
    // /events, not this — but waitReboot() and curl still hit /status directly.)
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return send_json(req, 200, build_status_object());   // send_json degrades a nullptr to 503
}

// ─── GET /diag — in-memory diagnostic log (for on-demand analysis) ────────────

esp_err_t handle_diag(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    if (query_param_is(req, "clear", "1"))        diag_log_clear();
    if (query_param_is(req, "verbose", "1"))      diag_set_verbose(true);
    else if (query_param_is(req, "verbose", "0")) diag_set_verbose(false);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Diag-Verbose", diag_verbose() ? "1" : "0");
    // Stream the log straight from its static buffer in (at most) two chunks. Building one
    // big std::string here used to throw std::bad_alloc when the whole buffer exceeded the
    // largest contiguous free block on a fragmented heap → uncaught in the httpd task →
    // abort() → reboot. Chunked send needs no large contiguous allocation, so /diag is safe
    // regardless of buffer size or fragmentation.
    diag_log_dump_chunks([req](const char* p, size_t n) {
        return n == 0 || httpd_resp_send_chunk(req, p, n) == ESP_OK;
    });
    return httpd_resp_send_chunk(req, nullptr, 0);  // terminate the chunked response
}

// ─── GET / — web UI (embedded from main/www/, inlined + gzipped at build time) ──

// The web UI is embedded pre-gzipped (see main/CMakeLists.txt: www/index.html +
// www/style.css + www/app.js are spliced into one page, then gzipped — ~13 KB vs 41 KB
// raw), the biggest first-paint win over a high-latency WiFi link. Browsers always accept
// gzip; the only consumer of "/" is a browser (evcc/curl hit /api and /status), so the
// encoding is sent unconditionally. Length is end-start (binary blob, not a C string).
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

esp_err_t handle_index(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    httpd_resp_set_type(req, "text/html");
    // The UI is embedded in the firmware and changes with every flash/OTA. Without
    // this, browsers cache index.html and keep rendering the OLD layout (with live
    // /status data) after an update — so tell them never to cache the page.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    const size_t len = index_html_gz_end - index_html_gz_start;
    return httpd_resp_send(req, (const char*)index_html_gz_start, len);
}
