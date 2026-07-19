#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "link_state.hpp"
#include "vehicle_data.hpp"

// Pure, hardware-free shaping of GET /status — the de-facto contract the web UI
// (www/app.js, 4 s poll) and any LAN script consume. http_status.cpp only GATHERS the
// Inputs under the existing locks and SERIALIZES what emit_status() decides; every
// which-field/when/what-value decision lives here so the whole field contract is pinned
// by golden CHECKs in the host mock build (test/test_logic.cpp) — a renamed field or a
// changed presence rule fails in seconds on the host instead of surfacing on hardware.
//
// emit_status() walks the document in EXACT wire order and calls the emitter visitor;
// the device's emitter builds cJSON one-to-one (no intermediate field list — the
// contract layer adds zero heap on this heap-tight device), the test emitter flattens
// to "path=value" lines for the goldens. Field order, key names, presence conditions
// and value shaping (e.g. the rounded "power") are all THE CONTRACT — change them here
// and the golden test together, knowing the web UI reads them by name.
//
// Emitter concept (duck-typed; see CjsonEmitter in http_status.cpp / the collector in
// test_logic.cpp):
//   void obj_begin(const char* key);  void obj_end();     // key nullptr = array element
//   void arr_begin(const char* key);  void arr_end();
//   void str(const char* key, const char* v);
//   void num(const char* key, double v);
//   void boolean(const char* key, bool v);

namespace tk {
namespace status {

// Post-2020 plausibility floor for the key_created / paired_at wall-clock stamps: a
// near-zero value means the clock had not synced when the stamp was taken, so the field
// is omitted and the UI shows "unknown" rather than 1970.
inline constexpr long long kEpochPlausibleFloor = 1600000000;

struct BleDevice {
    std::string addr, name;
    int  rssi{0};
    bool connectable{false};
};

// Everything /status is shaped from, gathered by http_status.cpp in one pass. Plain
// data only — no IDF types, no locks, no callbacks — so a golden test can construct
// any device state directly.
struct Inputs {
    // Device / pairing block.
    std::string vin, ip, version;
    bool        key_present{false};
    std::string key_fingerprint;
    long long   key_created{0};   // epoch s; emitted only above kEpochPlausibleFloor
    bool        paired{false};
    long long   paired_at{0};     // epoch s; same plausibility rule
    bool        reauth{false};

    // WiFi (STA link). wifi_connected false ⇒ the wifi object stays empty.
    bool        wifi_connected{false};
    std::string wifi_ssid;
    int         wifi_rssi{0};
    std::string wifi_std;         // friendly 802.11 generation; empty = omit

    // MQTT / HA bridge.
    bool        mqtt_configured{false}, mqtt_connected{false}, mqtt_tls{false};
    std::string mqtt_broker;      // empty = omit
    std::string mqtt_error;       // empty = omit

    // Syslog forwarder. host empty ⇒ host+port omitted; error empty ⇒ omit.
    bool        syslog_configured{false}, syslog_resolved{false}, syslog_reachable{false};
    std::string syslog_host;      // empty = omit host+port
    int         syslog_port{0};
    std::string syslog_error;     // empty = omit

    // BLE link / discovery.
    bool                   ble_connected{false}, ble_scanning{false};
    bool                   have_ble_rssi{false};
    int                    ble_rssi{0};        // live-link RSSI (connected)
    std::string            ble_addr;           // peer address (connected)
    std::vector<BleDevice> devices;            // nearby scan results (not connected)
    uint32_t               connect_fail{0};    // consecutive connect failures; 0 = omit block
    bool                   have_seen_rssi{false};
    int                    seen_rssi{0};       // last advert RSSI (the "can't connect" bars)
    int                    target_connectable{-1};  // -1 unknown / 0 no / 1 yes

    // Link-state machine + raw VCSEC flag (diagnostics).
    LinkState   link{LinkState::Unknown};
    std::string vcsec_sleep{"UNKNOWN"};

    // Cached vehicle data (copies of the last_known_* caches).
    ChargeStateResult   charge;    // drives "vehicle" (Awake only) and "last"
    ClimateStateResult  climate;   // "tele" — emitted only while BLE-connected
    DriveStateResult    drive;
    TirePressureResult  tires;
    ClosuresStateResult closures;

    bool     have_last_seen{false};
    uint32_t last_seen_s{0};

    // Why the PREVIOUS boot ended, when it ended by our own hand ("heap:<n>" = the heap watchdog
    // restarted us; n = how many consecutive such restarts). Empty for every ordinary boot —
    // power-on, crash, OTA — so the field is emitted only when there is something to report.
    std::string last_reboot;
};

template <typename E>
inline void emit_status(const Inputs& in, E& e) {
    // ── Device / pairing scalars ──────────────────────────────────────────────
    e.str("vin",     in.vin.c_str());
    e.str("ip",      in.ip.c_str());
    e.str("version", in.version.c_str());
    e.boolean("key_present", in.key_present);
    e.str("key_fingerprint", in.key_fingerprint.c_str());
    if (in.key_created > kEpochPlausibleFloor) e.num("key_created", (double)in.key_created);
    e.boolean("paired", in.paired);
    if (in.paired_at > kEpochPlausibleFloor) e.num("paired_at", (double)in.paired_at);
    e.boolean("reauth", in.reauth);

    // ── wifi ──────────────────────────────────────────────────────────────────
    e.obj_begin("wifi");
    if (in.wifi_connected) {
        e.str("ssid", in.wifi_ssid.c_str());
        e.num("rssi", in.wifi_rssi);
        if (!in.wifi_std.empty()) e.str("std", in.wifi_std.c_str());
    }
    e.obj_end();

    // ── mqtt ──────────────────────────────────────────────────────────────────
    e.obj_begin("mqtt");
    e.boolean("configured", in.mqtt_configured);
    e.boolean("connected",  in.mqtt_connected);
    e.boolean("tls",        in.mqtt_tls);
    if (!in.mqtt_broker.empty()) e.str("broker", in.mqtt_broker.c_str());
    if (!in.mqtt_error.empty())  e.str("error",  in.mqtt_error.c_str());
    e.obj_end();

    // ── syslog ────────────────────────────────────────────────────────────────
    e.obj_begin("syslog");
    e.boolean("configured", in.syslog_configured);
    e.boolean("resolved",   in.syslog_resolved);
    e.boolean("reachable",  in.syslog_reachable);
    if (!in.syslog_host.empty()) {
        e.str("host", in.syslog_host.c_str());
        e.num("port", in.syslog_port);
    }
    if (!in.syslog_error.empty()) e.str("error", in.syslog_error.c_str());
    e.obj_end();

    // ── tele — read-only telemetry caches, emitted only while the BLE link is up
    // (root-level sibling BEFORE "ble", mirroring the historical insertion order). ──
    if (in.ble_connected) {
        e.obj_begin("tele");
        if (in.climate.valid) {
            const ClimateStateResult& cl = in.climate;
            e.obj_begin("climate");
            if (cl.has_inside)   e.num("inside",   cl.inside_temp);
            if (cl.has_outside)  e.num("outside",  cl.outside_temp);
            if (cl.has_setpoint) e.num("setpoint", cl.driver_setpoint);
            if (cl.has_climate_on)      e.boolean("on",              cl.is_climate_on);
            if (cl.has_preconditioning) e.boolean("preconditioning", cl.is_preconditioning);
            if (cl.has_cop)         e.str("cop",             cl.cop.c_str());
            if (cl.has_cop_cooling) e.boolean("cop_cooling", cl.cop_cooling);
            if (cl.has_cop_temp)    e.str("cop_temp",        cl.cop_temp.c_str());
            if (cl.has_cop_reason)  e.str("cop_reason",      cl.cop_reason.c_str());
            if (cl.has_front_defrost) e.boolean("front_defrost", cl.front_defrost);
            if (cl.has_rear_defrost)  e.boolean("rear_defrost",  cl.rear_defrost);
            if (cl.has_defrost_mode)  e.str("defrost_mode",      cl.defrost_mode.c_str());
            e.obj_end();
        }
        if (in.drive.valid) {
            e.obj_begin("drive");
            if (!in.drive.shift_state.empty()) e.str("shift", in.drive.shift_state.c_str());
            if (in.drive.has_odometer)         e.num("odometer_km", in.drive.odometer_km);
            e.obj_end();
        }
        if (in.tires.valid) {
            e.obj_begin("tires");
            if (in.tires.has_fl) e.num("fl", in.tires.fl);
            if (in.tires.has_fr) e.num("fr", in.tires.fr);
            if (in.tires.has_rl) e.num("rl", in.tires.rl);
            if (in.tires.has_rr) e.num("rr", in.tires.rr);
            e.boolean("warn", in.tires.warn);
            e.obj_end();
        }
        if (in.closures.valid) {
            const ClosuresStateResult& cz = in.closures;
            e.obj_begin("closures");
            if (cz.has_locked) e.boolean("locked", cz.locked);
            e.boolean("door",   cz.any_door_open);
            e.boolean("frunk",  cz.frunk_open);
            e.boolean("trunk",  cz.trunk_open);
            e.boolean("window", cz.any_window_open);
            if (cz.has_user_present) e.boolean("user", cz.user_present);
            e.obj_end();
        }
        e.obj_end();
    }

    // ── ble ───────────────────────────────────────────────────────────────────
    e.obj_begin("ble");
    e.boolean("connected", in.ble_connected);
    e.boolean("scanning",  in.ble_scanning);
    if (in.ble_connected) {
        if (in.have_ble_rssi) e.num("rssi", in.ble_rssi);
        e.str("addr", in.ble_addr.c_str());
    } else {
        e.arr_begin("devices");
        for (const BleDevice& d : in.devices) {
            e.obj_begin(nullptr);
            e.str("addr", d.addr.c_str());
            e.str("name", d.name.c_str());
            e.num("rssi", d.rssi);
            e.boolean("connectable", d.connectable);
            e.obj_end();
        }
        e.arr_end();
        // "Found the car but can't connect": emitted only while actively failing.
        if (in.connect_fail > 0) {
            e.num("connect_fail", (double)in.connect_fail);
            if (in.have_seen_rssi) e.num("rssi", in.seen_rssi);
            if (in.target_connectable >= 0)
                e.boolean("car_connectable", in.target_connectable == 1);
        }
    }
    e.obj_end();

    // ── link (single source of truth) + raw VCSEC flag ────────────────────────
    e.str("link",        link_state_web_str(in.link));
    e.str("vcsec_sleep", in.vcsec_sleep.c_str());

    // ── vehicle — live awake/SOC view, only with fresh telemetry (link == Awake),
    // independent of the momentary BLE link. ──────────────────────────────────
    if (in.link == LinkState::Awake && in.charge.valid) {
        const ChargeStateResult& cs = in.charge;
        e.obj_begin("vehicle");
        if (cs.has_battery_level)    e.num("soc", cs.battery_level);
        e.str("status", cs.charging_state.c_str());
        if (cs.has_charge_limit_soc) e.num("charge_limit", cs.charge_limit_soc);
        // Whole-number kW: the rounding IS part of the contract (the UI shows it raw).
        if (cs.has_charger_power)    e.num("power", (int)(cs.charger_power + 0.5f));
        if (cs.has_charging_amps)    e.num("amps", cs.charging_amps);
        if (cs.has_actual_current)   e.num("actual_amps", cs.charger_actual_current);
        if (cs.has_voltage)          e.num("volts", cs.charger_voltage);
        if (cs.has_charger_phases)   e.num("phases", cs.charger_phases);
        e.obj_end();
    }

    // ── last — last-known snapshot for the asleep/"Parked" cards, link-independent. ──
    if (in.charge.valid) {
        e.obj_begin("last");
        if (in.charge.has_battery_level) e.num("soc", in.charge.battery_level);
        e.str("status", in.charge.charging_state.c_str());
        e.obj_end();
    }
    if (in.have_last_seen) e.num("last_seen_s", (double)in.last_seen_s);
    if (!in.last_reboot.empty()) e.str("last_reboot", in.last_reboot.c_str());
}

}  // namespace status
}  // namespace tk
