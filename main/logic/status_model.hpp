#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "link_state.hpp"
#include "vehicle_data.hpp"

// Pure, hardware-free shaping of GET /status — the de-facto contract the web UI
// (www/app.js, which polls it every 4 s) and any LAN script consume. http_status.cpp only GATHERS the
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
    // The last POST /set_wifi was UNDONE by the credential rollback. Sticky until the next
    // /set_wifi, and the ONLY trace of it: the rollback reboots, so the SSID on screen afterwards
    // is simply the old one again and nothing else would say the new one was ever tried.
    bool        wifi_rolled_back{false};

    // Ethernet (W5500 over SPI — the M5Stack ATOMIC PoE Base). eth_link false ⇒ the whole
    // `eth` object is OMITTED, so its PRESENCE is the signal that this device is on a wire
    // rather than a radio. That is deliberately not symmetric with `wifi`, which is always
    // emitted (empty when down): a WiFi board that suddenly stopped reporting `wifi` would
    // read as an older build, whereas no board has ever reported `eth`, so absence there
    // cannot be misread. Carries no MAC — nothing here identifies the reporter, which keeps
    // it out of the ?redact=1 list entirely.
    bool        eth_link{false};
    int         eth_speed_mbps{0};      // 10 or 100; 0 = not reported
    bool        eth_full_duplex{false};

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
    // What the Bluetooth row counts down (VehicleController::ble_phase()). Empty = no
    // phase is running, both fields omitted. "connecting" = an attempt is running and
    // gives up in phase_s; "waiting" = the next attempt starts in phase_s. phase_s is
    // emitted WITH phase even at 0 ("right now"), so a countdown never vanishes on its
    // last second and leave the row's label bare.
    std::string            ble_phase;
    uint32_t               ble_phase_s{0};

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

    // ── sys — board facts that are ALWAYS present ─────────────────────────────────────────────
    // Unlike everything above, none of this depends on a link, a pairing or a broker: it is what
    // the device can always say about itself, which is exactly what a remote triage needs first.
    // The heap figures were previously absent from /status ENTIRELY, on a device whose dominant
    // failure mode is heap exhaustion — so the primary API could not report the number that causes
    // its reboots. largest_block is the one that matters (it is what the heap watchdog gates on);
    // free_heap beside it is what distinguishes a leak from fragmentation.
    // WiFi-STA eFuse MAC: the physical board identity, independent of the active transport.
    // Deliberately retained in ?redact=1 diagnostics so two replacement boards can be told apart
    // while their HA/MQTT identity correctly stays attached to the same VIN.
    std::string board_mac;
    uint32_t    free_heap{0};
    uint32_t    min_free_heap{0};
    uint32_t    largest_block{0};
    uint32_t    uptime_s{0};
    uint32_t    wifi_reconnects{0};
    std::string reset_reason;          // logic/reset_reason.hpp slug for THIS boot
    bool        safe_mode{false};      // the latched boot-loop recovery state (safe_mode.cpp)
    // Historical minimum free stack since boot, sampled by each owning task. nullopt means never
    // sampled and is omitted: safe mode intentionally never starts three of these tasks. An
    // engaged zero is a valid, critical measurement and must remain distinguishable from absence.
    std::optional<uint32_t> httpd_stack_min_free_bytes;
    std::optional<uint32_t> vehicle_stack_min_free_bytes;
    std::optional<uint32_t> auto_pair_stack_min_free_bytes;
    std::optional<uint32_t> mqtt_stack_min_free_bytes;

    // ── last_crash — emitted only when the boot is NOTABLE ────────────────────────────────────
    // have_crash is crash_is_notable(): a real fault reset, or a dump for this build still sitting
    // in flash, and not dismissed. An ordinary boot emits nothing at all rather than a block full
    // of zeroes that reads like a crash with no information in it.
    bool                  have_crash{false};
    std::string           crash_reason;      // slug, e.g. "panic" / "task_wdt" / "brownout"
    int                   crash_reason_code{0};
    bool                  crash_fault{false};
    bool                  crash_coredump{false};   // a dump for THIS build is downloadable now
    bool                  crash_corrupted{false};  // the unwinder flagged the backtrace unreliable
    std::string           crash_task;
    uint32_t              crash_pc{0};
    std::vector<uint32_t> crash_backtrace;
    std::string           crash_elf_sha256;        // the RUNNING build — matches a dump to its .elf

    // ── redaction ─────────────────────────────────────────────────────────────────────────────
    // GET /status?redact=1 — the bug-report form of this payload. Six values identify the reporter
    // or their car and are substituted with "<redacted>": the VIN (the single most identifying
    // value this device holds — it names one specific car and is an input to Tesla's own APIs), the
    // device IP, the WiFi SSID, the vehicle's BLE MAC (and every scanned neighbour's, which are
    // other people's devices in the reporter's home), the MQTT broker and the syslog host.
    // `sys.board_mac` deliberately remains visible: it identifies the replaceable hardware and
    // is the evidence that lets a board-swap diagnosis distinguish old board from new.
    //
    // The KEY is always still emitted with a placeholder VALUE. Dropping the field instead would
    // forge an "older build that never had it" signal, and "which build produced this?" is the
    // first question anyone reading a frozen report has to answer.
    //
    // Applied HERE rather than at the gather (which is where the sibling firmware does it) because
    // here it is inside the golden-pinned contract, so the test proves which fields are covered.
    // The sibling's reason not to — that a post-processing pass over the finished JSON needs a
    // second full-size buffer — is about the pass, not about substituting at the point of emission.
    bool redact{false};
};

// Substitute an identifying value when the caller asked for a redacted snapshot. One helper so
// every redacted field reads identically at the call site and none can be missed by using the raw
// value by accident.
inline const char* redacted_or(const std::string& v, bool redact) {
    return redact ? "<redacted>" : v.c_str();
}

template <typename E>
inline void emit_status(const Inputs& in, E& e) {
    // ── Device / pairing scalars ──────────────────────────────────────────────
    e.str("vin",     redacted_or(in.vin, in.redact));
    e.str("ip",      redacted_or(in.ip, in.redact));
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
        e.str("ssid", redacted_or(in.wifi_ssid, in.redact));
        e.num("rssi", in.wifi_rssi);
        if (!in.wifi_std.empty()) e.str("std", in.wifi_std.c_str());
    }
    // OUTSIDE the connected branch: a rollback means the device is back on its OLD network, so it
    // is reported precisely when wifi IS connected — but it must also survive the case where it is
    // not. Emitted only when true, so its presence is the signal.
    if (in.wifi_rolled_back) e.boolean("rolled_back", true);
    e.obj_end();

    // ── eth — present only while Ethernet carries the lease (see Inputs::eth_link) ──
    if (in.eth_link) {
        e.obj_begin("eth");
        e.boolean("link", true);
        // Omitted rather than reported as 0: the PHY may not have negotiated yet, and "0 Mbit"
        // is a claim about the link rather than an admission that we did not read it.
        if (in.eth_speed_mbps > 0) e.num("speed", in.eth_speed_mbps);
        e.boolean("full_duplex", in.eth_full_duplex);
        e.obj_end();
    }

    // ── mqtt ──────────────────────────────────────────────────────────────────
    e.obj_begin("mqtt");
    e.boolean("configured", in.mqtt_configured);
    e.boolean("connected",  in.mqtt_connected);
    e.boolean("tls",        in.mqtt_tls);
    // Substituted, never omitted: these two are emitted only when non-empty, so clearing them
    // would positively assert "not configured" — a different and wrong claim about the device.
    if (!in.mqtt_broker.empty()) e.str("broker", redacted_or(in.mqtt_broker, in.redact));
    if (!in.mqtt_error.empty())  e.str("error",  in.mqtt_error.c_str());
    e.obj_end();

    // ── syslog ────────────────────────────────────────────────────────────────
    e.obj_begin("syslog");
    e.boolean("configured", in.syslog_configured);
    e.boolean("resolved",   in.syslog_resolved);
    e.boolean("reachable",  in.syslog_reachable);
    if (!in.syslog_host.empty()) {
        e.str("host", redacted_or(in.syslog_host, in.redact));
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
    if (!in.ble_phase.empty()) {
        e.str("phase", in.ble_phase.c_str());
        e.num("phase_s", (double)in.ble_phase_s);
    }
    if (in.ble_connected) {
        if (in.have_ble_rssi) e.num("rssi", in.ble_rssi);
        e.str("addr", redacted_or(in.ble_addr, in.redact));
    } else {
        e.arr_begin("devices");
        for (const BleDevice& d : in.devices) {
            e.obj_begin(nullptr);
            // Scanned NEIGHBOURS, not just the car: these are other people's devices in the
            // reporter's home, so they are redacted for the same reason the car's own MAC is.
            e.str("addr", redacted_or(d.addr, in.redact));
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

    // ── sys — always present ──────────────────────────────────────────────────
    // Deliberately unconditional, unlike every block above it: a device with no pairing, no broker
    // and no link still has to be able to say how it is doing. This is the block a remote triage
    // reads first, and before it existed /status could not report the heap at all — on a device
    // whose watchdog restarts it for running out of exactly that.
    e.obj_begin("sys");
    e.str("board_mac",      in.board_mac.c_str());
    e.num("free_heap",       (double)in.free_heap);
    e.num("min_free_heap",   (double)in.min_free_heap);
    e.num("largest_block",   (double)in.largest_block);
    e.num("uptime_s",        (double)in.uptime_s);
    e.num("wifi_reconnects", (double)in.wifi_reconnects);
    e.str("reset_reason",    in.reset_reason.c_str());
    e.boolean("safe_mode",   in.safe_mode);
    if (in.httpd_stack_min_free_bytes || in.vehicle_stack_min_free_bytes ||
        in.auto_pair_stack_min_free_bytes || in.mqtt_stack_min_free_bytes) {
        e.obj_begin("stack_min_free_bytes");
        if (in.httpd_stack_min_free_bytes)
            e.num("httpd", (double)*in.httpd_stack_min_free_bytes);
        if (in.vehicle_stack_min_free_bytes)
            e.num("vehicle", (double)*in.vehicle_stack_min_free_bytes);
        if (in.auto_pair_stack_min_free_bytes)
            e.num("auto_pair", (double)*in.auto_pair_stack_min_free_bytes);
        if (in.mqtt_stack_min_free_bytes)
            e.num("mqtt", (double)*in.mqtt_stack_min_free_bytes);
        e.obj_end();
    }
    e.obj_end();

    // ── last_crash — only when this boot is NOTABLE ───────────────────────────
    // Absent on an ordinary boot, so its mere PRESENCE is the signal. `fault` separates the two
    // reasons it can appear: a real crash reset, versus a dump for this build still sitting in
    // flash from an earlier one — an orphan dump alone is not "restarted after a crash", and a UI
    // that said so would send the reader after the wrong event.
    if (in.have_crash) {
        e.obj_begin("last_crash");
        e.str("reason",      in.crash_reason.c_str());
        e.num("reason_code", (double)in.crash_reason_code);
        e.boolean("fault",     in.crash_fault);
        e.boolean("coredump",  in.crash_coredump);
        if (!in.crash_task.empty()) e.str("task", in.crash_task.c_str());
        if (in.crash_pc != 0)       e.num("pc", (double)in.crash_pc);
        if (!in.crash_backtrace.empty()) {
            // Hex STRINGS, not numbers: a PC is an address, it is read and pasted as 0x…, and a
            // JSON number would render it in decimal in every viewer that touches this payload.
            e.arr_begin("backtrace");
            for (uint32_t pc : in.crash_backtrace) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "0x%08x", (unsigned)pc);
                e.str(nullptr, buf);
            }
            e.arr_end();
            // Emitted only alongside a backtrace, because that is the only thing it qualifies: it
            // says the frames below are unreliable, and on its own it would look like a verdict
            // about the crash itself.
            if (in.crash_corrupted) e.boolean("corrupted", true);
        }
        if (!in.crash_elf_sha256.empty()) e.str("elf_sha256", in.crash_elf_sha256.c_str());
        e.obj_end();
    }
}

}  // namespace status
}  // namespace tk
