// Host-side mock build — exercises the pure, hardware-free logic in main/logic/
// without ESP-IDF. Dependency-free (no gtest): a tiny CHECK macro tallies failures
// and the process exit code is the verdict, so it runs anywhere in seconds. See
// test/README.md.
//
// Scope: the IDF-free decision/conversion cores the firmware delegates to —
//   * VIN plausibility               (logic/vin.hpp        <- VehicleController::vin_is_plausible)
//   * imperial -> metric conversion  (logic/units.hpp      <- MQTT bridge / telemetry)
//   * link_state() four-state machine + its web/MQTT string mappings
//                                    (logic/link_state.hpp <- /status "link", MQTT sleep_status)
//   * per-target platform / OTA-suffix mapping
//                                    (logic/target.hpp     <- platform.hpp, ota_update.cpp)
//   * the shared command registry — names/kinds/arg bounds for BOTH surfaces
//                                    (logic/command_registry.hpp <- http_api.cpp + mcp_server.cpp)
//   * the /status field contract, pinned by golden emissions
//                                    (logic/status_model.hpp <- http_status.cpp)
//   * Syslog target parsing + send-failure classification
//                                    (logic/syslog_policy.hpp <- syslog.cpp, /set_syslog)
// The cJSON serializers stay IDF/cJSON-coupled, but they are one-to-one visitors now:
// every which-field/when/what-value decision they render is host-tested here.

#include "logic/vin.hpp"
#include "logic/units.hpp"
#include "logic/link_state.hpp"
#include "logic/target.hpp"
#include "logic/mcp.hpp"
#include "logic/command_registry.hpp"
#include "logic/status_model.hpp"
#include "logic/command_result.hpp"
#include "logic/ui_state.hpp"
#include "logic/display_model.hpp"
#include "logic/soc_gradient.hpp"
#include "logic/led_status.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/ws_policy.hpp"
#include "logic/active_window.hpp"
#include "logic/ha_templates.hpp"
#include "logic/http_body.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

// Compare two C strings allowing nullptr on either side.
static bool streq(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return a == b;
    return std::strcmp(a, b) == 0;
}
#define CHECK_STR(got, want) CHECK(streq((got), (want)))

// Floating compare with a small tolerance (conversions are inexact by nature).
static bool approx(double got, double want) { return std::fabs(got - want) < 1e-6; }
#define CHECK_NEAR(got, want) CHECK(approx((got), (want)))

// ─── VIN plausibility ──────────────────────────────────────────────────────────
static void test_vin() {
    // Canonical 17-char Tesla VIN.
    CHECK(tk::vin_is_plausible("5YJ3E1EA7KF000316"));
    CHECK(tk::vin_is_plausible("LRWYGCEK9PC000001"));
    CHECK(tk::vin_is_plausible("00000000000000000"));  // all digits, 17 chars

    // Wrong length.
    CHECK(!tk::vin_is_plausible(""));
    CHECK(!tk::vin_is_plausible("UNKNOWN"));                 // 7-char boot placeholder
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF00031"));        // 16
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003166"));      // 18

    // Lowercase is not accepted (the standard / our /set_vin are uppercase-only).
    CHECK(!tk::vin_is_plausible("5yj3e1ea7kf000316"));

    // I, O, Q are reserved by the VIN standard and must be rejected.
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003I6"));
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003O6"));
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003Q6"));

    // Stray punctuation / spaces.
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF00031-"));
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF 00316"));
}

// ─── Syslog target parsing + send-failure classification ────────────────────────
static void test_syslog_policy() {
    std::string host; int port = 0;

    // host:port.
    CHECK(tk::syslog_target_parse("192.168.1.22:514", host, port));
    CHECK(host == "192.168.1.22" && port == 514);
    CHECK(tk::syslog_target_parse("syslog.lan:1514", host, port));
    CHECK(host == "syslog.lan" && port == 1514);

    // Bare host defaults to port 514.
    CHECK(tk::syslog_target_parse("192.168.1.22", host, port));
    CHECK(host == "192.168.1.22" && port == 514);

    // Rejected: empty, empty host/port either side of ':', a non-numeric or
    // out-of-range port.
    CHECK(!tk::syslog_target_parse("", host, port));
    CHECK(!tk::syslog_target_parse(":514", host, port));
    CHECK(!tk::syslog_target_parse("192.168.1.22:", host, port));
    CHECK(!tk::syslog_target_parse("192.168.1.22:abc", host, port));
    CHECK(!tk::syslog_target_parse("192.168.1.22:0", host, port));
    CHECK(!tk::syslog_target_parse("192.168.1.22:65536", host, port));

    // /set_syslog validation: empty always disables; a valid target passes; a
    // malformed one, or one carrying whitespace, is rejected.
    CHECK(tk::syslog_target_is_plausible(""));
    CHECK(tk::syslog_target_is_plausible("192.168.1.22:514"));
    CHECK(!tk::syslog_target_is_plausible("192.168.1.22:abc"));
    CHECK(!tk::syslog_target_is_plausible("192.168.1.22: 514"));
    CHECK(!tk::syslog_target_is_plausible(std::string(200, 'a') + ":514"));

    // Send-failure classification: routing/host errors are HARD (re-resolve now);
    // resource/blocking errors are TRANSIENT (hold the destination, let the
    // ordinary cadence re-check).
    CHECK(tk::syslog_error_is_hard(ENETUNREACH));
    CHECK(tk::syslog_error_is_hard(EHOSTUNREACH));
    CHECK(tk::syslog_error_is_hard(ENETDOWN));
    CHECK(tk::syslog_error_is_hard(EHOSTDOWN));
    CHECK(tk::syslog_error_is_hard(EADDRNOTAVAIL));
    CHECK(!tk::syslog_error_is_hard(ENOMEM));
    CHECK(!tk::syslog_error_is_hard(ENOBUFS));
    CHECK(!tk::syslog_error_is_hard(EAGAIN));
    CHECK(!tk::syslog_error_is_hard(EINTR));
    CHECK(!tk::syslog_error_is_hard(0));

    // Send-failure actions: the once-per-outage re-probe (the probe-storm fix). The FIRST hard
    // failure (not yet failing) pauses forwarding AND triggers one immediate re-resolve+re-probe;
    // a LATER hard failure in the same outage (already failing) still pauses forwarding but must
    // NOT re-probe — else getaddrinfo()+ping fires per queued line.
    CHECK(tk::syslog_send_failure_actions(/*hard*/true,  /*already_failing*/false).stop_forwarding);
    CHECK(tk::syslog_send_failure_actions(true,  false).reprobe_once);
    CHECK(tk::syslog_send_failure_actions(true,  true).stop_forwarding);
    CHECK(!tk::syslog_send_failure_actions(true, true).reprobe_once);   // the fix: no storm
    // Transient errors never pause forwarding or touch the throttle, whatever the latch state.
    CHECK(!tk::syslog_send_failure_actions(false, false).stop_forwarding);
    CHECK(!tk::syslog_send_failure_actions(false, false).reprobe_once);
    CHECK(!tk::syslog_send_failure_actions(false, true).stop_forwarding);
    CHECK(!tk::syslog_send_failure_actions(false, true).reprobe_once);
}

// ─── Unit conversion ────────────────────────────────────────────────────────────
static void test_units() {
    CHECK_NEAR(tk::kMilesToKm, 1.609344);

    CHECK_NEAR(tk::mi_to_km(0.0),   0.0);
    CHECK_NEAR(tk::mi_to_km(1.0),   1.609344);
    CHECK_NEAR(tk::mi_to_km(100.0), 160.9344);    // ~100 mi range
    CHECK_NEAR(tk::mph_to_kmh(60.0), 96.56064);   // 60 mph charge rate

    // Odometer arrives in hundredths of a mile: 1,234,567 -> 12,345.67 mi -> km.
    CHECK_NEAR(tk::odo_hundredths_mi_to_km(1234567.0), 12345.67 * 1.609344);
    CHECK_NEAR(tk::odo_hundredths_mi_to_km(0.0), 0.0);
}

// Build a LinkInputs snapshot tersely.
static tk::LinkInputs mk(bool hc, uint32_t ca, bool hr, uint32_t ra, bool asleep) {
    tk::LinkInputs in;
    in.have_contact        = hc;
    in.contact_age_s       = ca;
    in.have_reachable      = hr;
    in.reachable_age_s     = ra;
    in.vcsec_stably_asleep = asleep;
    return in;
}

// ─── link_state() four-state machine ─────────────────────────────────────────────
static void test_link_state() {
    using LS = tk::LinkState;

    // Never heard anything ⇒ Unknown.
    CHECK(tk::compute_link_state(mk(false, 0, false, 0, false)) == LS::Unknown);

    // Fresh infotainment data ⇒ Awake (boundary: < kAwakeMaxAgeS).
    CHECK(tk::compute_link_state(mk(true, 0,  true, 0, false)) == LS::Awake);
    CHECK(tk::compute_link_state(mk(true, tk::kAwakeMaxAgeS - 1, true, 0, false)) == LS::Awake);
    // At exactly the boundary it is no longer Awake (>= is stale).
    CHECK(tk::compute_link_state(mk(true, tk::kAwakeMaxAgeS, true, 10, false)) != LS::Awake);

    // Reachable, no fresh data, NOT provably asleep ⇒ Idle (never a sleep claim).
    CHECK(tk::compute_link_state(mk(false, 0, true, 0, false)) == LS::Idle);
    CHECK(tk::compute_link_state(mk(true, tk::kAwakeMaxAgeS, true, 30, false)) == LS::Idle);

    // Reachable + debounced VCSEC ASLEEP ⇒ Asleep.
    CHECK(tk::compute_link_state(mk(false, 0, true, 30, true)) == LS::Asleep);

    // Reachable boundary: at/over kReachableMaxAgeS it stops being reachable.
    CHECK(tk::compute_link_state(mk(false, 0, true, tk::kReachableMaxAgeS - 1, false)) == LS::Idle);
    CHECK(tk::compute_link_state(mk(false, 0, true, tk::kReachableMaxAgeS, false)) == LS::Unreachable);

    // ASYMMETRY 1: fresh contact wins over a VCSEC ASLEEP flag — Awake, never Asleep.
    CHECK(tk::compute_link_state(mk(true, 5, true, 5, true)) == LS::Awake);

    // ASYMMETRY 2: VCSEC AWAKE (asleep=false) can never assert Awake without fresh
    // telemetry — a reachable car with stale data stays Idle, not Awake.
    CHECK(tk::compute_link_state(mk(false, 0, true, 10, false)) == LS::Idle);

    // Heard once but everything stale now ⇒ Unreachable (contact-only and reachable-only).
    CHECK(tk::compute_link_state(mk(true, 999, false, 0, false)) == LS::Unreachable);
    CHECK(tk::compute_link_state(mk(false, 0, true, 999, false)) == LS::Unreachable);

    // A stuck "stably asleep" flag must NOT resurrect Asleep once unreachable.
    CHECK(tk::compute_link_state(mk(false, 0, true, 999, true)) == LS::Unreachable);
}

// ─── link_state -> string mappings (web hero + MQTT sleep_status) ─────────────────
static void test_link_state_strings() {
    using LS = tk::LinkState;

    CHECK_STR(tk::link_state_web_str(LS::Awake),       "awake");
    CHECK_STR(tk::link_state_web_str(LS::Asleep),      "asleep");
    CHECK_STR(tk::link_state_web_str(LS::Idle),        "idle");
    CHECK_STR(tk::link_state_web_str(LS::Unreachable), "unreachable");
    CHECK_STR(tk::link_state_web_str(LS::Unknown),     "unknown");

    CHECK_STR(tk::link_state_mqtt_str(LS::Awake),       "AWAKE");
    CHECK_STR(tk::link_state_mqtt_str(LS::Asleep),      "ASLEEP");
    CHECK_STR(tk::link_state_mqtt_str(LS::Idle),        "IDLE");
    CHECK_STR(tk::link_state_mqtt_str(LS::Unreachable), "UNREACHABLE");
    // Unknown is omitted on the MQTT side (HA shows "unknown").
    CHECK(tk::link_state_mqtt_str(LS::Unknown) == nullptr);
}

// ─── per-target platform / OTA-suffix mapping ─────────────────────────────────────
static void test_target() {
    CHECK_STR(tk::platform_name(tk::Target::Esp32),   "ESP32");
    CHECK_STR(tk::platform_name(tk::Target::Esp32S3), "ESP32-S3");
    CHECK_STR(tk::platform_name(tk::Target::Esp32C3), "ESP32-C3");
    CHECK_STR(tk::platform_name(tk::Target::Esp32C6), "ESP32-C6");
    CHECK_STR(tk::platform_name(tk::Target::Esp32C5), "ESP32-C5");

    // esp32 has no suffix (tesla-key-esp32.bin); the rest get -s3/-c3/-c6/-c5.
    CHECK_STR(tk::image_suffix(tk::Target::Esp32),   "");
    CHECK_STR(tk::image_suffix(tk::Target::Esp32S3), "-s3");
    CHECK_STR(tk::image_suffix(tk::Target::Esp32C3), "-c3");
    CHECK_STR(tk::image_suffix(tk::Target::Esp32C6), "-c6");
    CHECK_STR(tk::image_suffix(tk::Target::Esp32C5), "-c5");

    // The full OTA filename the device pulls, assembled the same way ota_update.cpp does.
    const std::string base = "tesla-key-esp32";
    CHECK(base + tk::image_suffix(tk::Target::Esp32)   + ".bin" == "tesla-key-esp32.bin");
    CHECK(base + tk::image_suffix(tk::Target::Esp32S3) + ".bin" == "tesla-key-esp32-s3.bin");
    CHECK(base + tk::image_suffix(tk::Target::Esp32C5) + ".bin" == "tesla-key-esp32-c5.bin");
}

// ─── MCP endpoint core (version negotiation, routing, tool registry, clamp) ──────
static void test_mcp() {
    using MM = tk::McpMethod;

    // Version negotiation: echo a supported revision; anything else (or absent) answers
    // with our latest supported version, per the MCP lifecycle spec.
    CHECK_STR(tk::mcp_negotiate_version("2025-06-18"), "2025-06-18");
    CHECK_STR(tk::mcp_negotiate_version("2025-03-26"), "2025-03-26");
    CHECK_STR(tk::mcp_negotiate_version("2024-11-05"), "2025-06-18");
    CHECK_STR(tk::mcp_negotiate_version("bogus"),      "2025-06-18");
    CHECK_STR(tk::mcp_negotiate_version(nullptr),      "2025-06-18");

    // JSON-RPC method routing.
    CHECK(tk::mcp_method_from("initialize") == MM::Initialize);
    CHECK(tk::mcp_method_from("tools/list") == MM::ToolsList);
    CHECK(tk::mcp_method_from("tools/call") == MM::ToolsCall);
    CHECK(tk::mcp_method_from("ping")       == MM::Ping);
    // Any notifications/* is a notification (202, no body) — never "method not found".
    CHECK(tk::mcp_method_from("notifications/initialized") == MM::Notification);
    CHECK(tk::mcp_method_from("notifications/cancelled")   == MM::Notification);
    // Unimplemented capabilities and garbage stay Unknown (→ -32601).
    CHECK(tk::mcp_method_from("resources/list") == MM::Unknown);
    CHECK(tk::mcp_method_from("Initialize")     == MM::Unknown);  // case-sensitive
    CHECK(tk::mcp_method_from(nullptr)          == MM::Unknown);

    // Tool registry — now the MCP-visible half of the ONE shared command table
    // (logic/command_registry.hpp): every MCP row round-trips by name and by kind;
    // role-refused commands (which the Charging-Manager key can't execute) carry no
    // mcp_name and must not resolve as tools.
    for (const auto& t : tk::kCommands) {
        if (!t.mcp_name) continue;
        CHECK(tk::cmd_from_mcp_name(t.mcp_name) == &t);
        CHECK(t.mcp_desc != nullptr);
        CHECK(tk::cmd_info(t.kind) == &t);
    }
    CHECK(tk::cmd_from_mcp_name("door_unlock")     == nullptr);  // REST-only, never a tool
    CHECK(tk::cmd_from_mcp_name("set_sentry_mode") == nullptr);
    CHECK(tk::cmd_from_mcp_name(nullptr)           == nullptr);
    CHECK(tk::cmd_info(tk::CmdKind::Unknown)       == nullptr);

    // Arg-spec table — the single source of truth the advertised schema, the MCP
    // executor clamp AND the REST /command clamp all read (drift between them is
    // impossible by construction; this pins the values themselves). Every non-None arg
    // has sane shared bounds; an absent OPTIONAL Int defaults to 0 on the MCP side and
    // to api_default on the REST side, so both must lie inside the advertised bounds.
    for (const auto& t : tk::kCommands) {
        for (const auto& a : t.args) {
            if (a.type == tk::CmdArgType::None) continue;
            CHECK(a.api_key != nullptr || a.mcp_key != nullptr);
            if (a.type == tk::CmdArgType::Int) {
                CHECK(a.lo <= a.hi);
                if (a.mcp_key && !a.mcp_required) CHECK(a.lo <= 0 && 0 <= a.hi);
                if (a.api_key) CHECK(a.lo <= a.api_default && a.api_default <= a.hi);
            }
        }
    }
    const tk::CmdInfo* amps = tk::cmd_info(tk::CmdKind::SetChargingAmps);
    CHECK_STR(amps->args[0].mcp_key, "amps");
    CHECK_STR(amps->args[0].api_key, "charging_amps");   // TeslaBleHttpProxy compat name
    CHECK(amps->args[0].type == tk::CmdArgType::Int && amps->args[0].mcp_required);
    CHECK(amps->args[0].lo == 0 && amps->args[0].hi == 48);
    CHECK(amps->args[0].api_default == 0);
    const tk::CmdInfo* lim = tk::cmd_info(tk::CmdKind::SetChargeLimit);
    CHECK_STR(lim->args[0].mcp_key, "percent");
    CHECK_STR(lim->args[0].api_key, "percent");
    CHECK(lim->args[0].mcp_required && lim->args[0].lo == 50 && lim->args[0].hi == 100);
    CHECK(lim->args[0].api_default == 80);               // REST: absent body still means 80%
    const tk::CmdInfo* sched = tk::cmd_info(tk::CmdKind::SetScheduledCharging);
    CHECK_STR(sched->args[0].mcp_key, "enable");
    CHECK_STR(sched->args[0].api_key, "enable");
    CHECK(sched->args[0].type == tk::CmdArgType::Bool && sched->args[0].mcp_required);
    CHECK_STR(sched->args[1].mcp_key, "start_minutes");
    CHECK(sched->args[1].type == tk::CmdArgType::Int && !sched->args[1].mcp_required);
    CHECK(sched->args[1].lo == 0 && sched->args[1].hi == 1439);
    // Read-only + no-arg tools carry no args.
    CHECK(tk::cmd_info(tk::CmdKind::GetVehicleState)->args[0].type == tk::CmdArgType::None);
    CHECK(tk::cmd_info(tk::CmdKind::WakeUp)->args[0].type          == tk::CmdArgType::None);

    // REST surface of the shared table: exactly the 15 TeslaBleHttpProxy commands
    // resolve by API name, each to its own kind; get_vehicle_state is NOT one of them.
    const char* api_names[] = {
        "wake_up", "charge_start", "charge_stop", "charge_port_door_open",
        "charge_port_door_close", "door_lock", "door_unlock", "flash_lights",
        "honk_horn", "auto_conditioning_start", "auto_conditioning_stop",
        "set_charging_amps", "set_charge_limit", "set_sentry_mode",
        "set_scheduled_charging",
    };
    int api_count = 0;
    for (const auto& t : tk::kCommands) if (t.api_name) ++api_count;
    CHECK(api_count == 15);
    for (const char* n : api_names) {
        const tk::CmdInfo* c = tk::cmd_from_api_name(n);
        CHECK(c != nullptr);
        CHECK_STR(c->api_name, n);
    }
    CHECK(tk::cmd_from_api_name("get_vehicle_state") == nullptr);
    CHECK(tk::cmd_from_api_name("bogus")             == nullptr);
    CHECK(tk::cmd_from_api_name(nullptr)             == nullptr);
    // MCP name mapping of the charge-port pair differs from the REST name on purpose.
    CHECK_STR(tk::cmd_from_api_name("charge_port_door_open")->mcp_name, "charge_port_open");

    // tools/list wire order is table order — pin the 9 MCP rows exactly (a reorder
    // would change the serialized tools/list even with identical content).
    const char* mcp_order[] = {
        "get_vehicle_state", "wake_up", "charge_start", "charge_stop",
        "charge_port_open", "charge_port_close", "set_charging_amps",
        "set_charge_limit", "set_scheduled_charging",
    };
    int mi = 0;
    for (const auto& t : tk::kCommands) {
        if (!t.mcp_name) continue;
        CHECK(mi < 9);
        if (mi < 9) CHECK_STR(t.mcp_name, mcp_order[mi]);
        ++mi;
    }
    CHECK(mi == 9);

    // Clamp: an out-of-range double must never reach the int cast (UB guard).
    CHECK(tk::clamped_int(16.0,   0, 48)    == 16);
    CHECK(tk::clamped_int(-5.0,   0, 48)    == 0);
    CHECK(tk::clamped_int(1e300,  0, 48)    == 48);
    CHECK(tk::clamped_int(-1e300, 50, 100)  == 50);
    CHECK(tk::clamped_int(99.9,   50, 100)  == 99);
    CHECK(tk::clamped_int(1440.0, 0, 1439)  == 1439);
    // NaN compares false against BOTH bounds, so without its explicit check it would
    // fall through to the (int) cast — the exact UB this guard exists to prevent
    // (reachable via the string-argument path: strtod accepts "nan").
    CHECK(tk::clamped_int(std::nan(""), 0, 48) == 0);
    CHECK(tk::clamped_int(std::nan(""), 50, 100) == 50);

    // Command outcome text — shared by the REST /command reason and the MCP tools/call
    // result, so both paths report identical outcomes.
    const std::string tesla_reason = "complete";
    const std::string no_reason;
    CHECK_STR(tk::command_result_text(true,  no_reason),    "command executed successfully");
    CHECK_STR(tk::command_result_text(true,  tesla_reason), "command executed successfully");
    CHECK_STR(tk::command_result_text(false, tesla_reason), "complete");
    CHECK_STR(tk::command_result_text(false, no_reason),    "vehicle not reachable");
}

// ─── /status field contract (logic/status_model.hpp <- http_status.cpp) ──────────
// Flattening emitter for the goldens: containers print as "path{" / "path[", leaves as
// "path=value" — one line per emitter call, so the golden pins field ORDER, key NAMES,
// PRESENCE rules and VALUE shaping all at once (that is the /status contract app.js
// and any LAN script consume).
struct CollectEmitter {
    std::string out;
    std::vector<std::string> path;   // open containers
    std::vector<int> arr_next;       // per-container: next array index, or -1 for objects

    std::string joined(const std::string& leaf) {
        std::string p;
        for (const auto& s : path) { p += s; p += '.'; }
        return p + leaf;
    }
    std::string elem_name(const char* key) {
        if (key) return key;
        return std::to_string(arr_next.back()++);   // array element: enclosing counter
    }
    void obj_begin(const char* key) {
        std::string nm = elem_name(key);
        out += joined(nm) + "{\n";
        path.push_back(nm); arr_next.push_back(-1);
    }
    void obj_end() { path.pop_back(); arr_next.pop_back(); }
    void arr_begin(const char* key) {
        std::string nm = elem_name(key);
        out += joined(nm) + "[\n";
        path.push_back(nm); arr_next.push_back(0);
    }
    void arr_end() { obj_end(); }
    void str(const char* k, const char* v) { out += joined(elem_name(k)) + "=\"" + v + "\"\n"; }
    void num(const char* k, double v) {
        char b[40];
        if (v == (double)(long long)v) std::snprintf(b, sizeof b, "%lld", (long long)v);
        else                           std::snprintf(b, sizeof b, "%.10g", v);
        out += joined(elem_name(k)) + "=" + b + "\n";
    }
    void boolean(const char* k, bool v) { out += joined(elem_name(k)) + (v ? "=true\n" : "=false\n"); }
};

// Golden compare that prints the diff on mismatch (a bare CHECK only names the line).
static bool golden_eq(const std::string& got, const char* want) {
    if (got == want) return true;
    std::printf("---- golden mismatch: got ----\n%s---- want ----\n%s----\n", got.c_str(), want);
    return false;
}

static void test_status_model() {
    using tk::status::Inputs;

    // Scenario 1 — awake + charging, BLE link up, full telemetry with a deliberate mix
    // of present/absent fields (float-exact values so the golden is bit-stable).
    Inputs in;
    in.vin = "5YJ3E1EA7KF000316"; in.ip = "192.168.1.50"; in.version = "1.4.2";
    in.key_present = true; in.key_fingerprint = "AB:CD:EF:01";
    in.key_created = 1750000000; in.paired = true; in.paired_at = 1750500000;
    in.wifi_connected = true; in.wifi_ssid = "HomeNet"; in.wifi_rssi = -55; in.wifi_std = "Wi-Fi 6";
    in.mqtt_configured = true; in.mqtt_connected = true; in.mqtt_tls = true;
    in.mqtt_broker = "mqtt.local:8883";
    in.syslog_configured = true; in.syslog_resolved = true; in.syslog_reachable = true;
    in.syslog_host = "syslog.lan"; in.syslog_port = 514;   // error absent → omitted
    in.ble_connected = true; in.have_ble_rssi = true; in.ble_rssi = -60;
    in.ble_addr = "aa:bb:cc:dd:ee:ff";
    in.climate.valid = true;
    in.climate.has_inside = true;   in.climate.inside_temp = 21.5f;
    in.climate.has_outside = true;  in.climate.outside_temp = 18.0f;
    in.climate.has_setpoint = true; in.climate.driver_setpoint = 20.5f;
    in.climate.has_climate_on = true;      in.climate.is_climate_on = true;
    in.climate.has_preconditioning = true; in.climate.is_preconditioning = false;
    in.climate.has_cop = true;         in.climate.cop = "On";
    in.climate.has_cop_cooling = true; in.climate.cop_cooling = true;
    in.climate.has_cop_temp = true;    in.climate.cop_temp = "Medium";
    // cop_reason absent on purpose (presence rule)
    in.climate.has_rear_defrost = true; in.climate.rear_defrost = false;
    in.climate.has_defrost_mode = true; in.climate.defrost_mode = "Off";
    in.drive.valid = true; in.drive.shift_state = "P";
    in.drive.has_odometer = true; in.drive.odometer_km = 12345.5f;
    in.tires.valid = true;
    in.tires.has_fl = true; in.tires.fl = 2.75f;
    in.tires.has_fr = true; in.tires.fr = 2.75f;
    in.tires.has_rl = true; in.tires.rl = 3.0f;
    // rr absent on purpose; warn always emitted
    in.closures.valid = true;
    in.closures.has_locked = true; in.closures.locked = true;
    in.closures.trunk_open = true;   // door/frunk/window false, always emitted
    // user absent on purpose
    in.link = tk::LinkState::Awake; in.vcsec_sleep = "AWAKE";
    in.charge.valid = true;
    in.charge.has_battery_level = true;    in.charge.battery_level = 72.5f;
    in.charge.charging_state = "Charging";
    in.charge.has_charge_limit_soc = true; in.charge.charge_limit_soc = 80.0f;
    in.charge.has_charger_power = true;    in.charge.charger_power = 11.4f;  // → rounds to 11
    in.charge.has_charging_amps = true;    in.charge.charging_amps = 16;
    in.charge.has_actual_current = true;   in.charge.charger_actual_current = 16;
    in.charge.has_voltage = true;          in.charge.charger_voltage = 231;
    in.charge.has_charger_phases = true;   in.charge.charger_phases = 2;
    in.have_last_seen = true; in.last_seen_s = 5;

    CollectEmitter e1;
    tk::status::emit_status(in, e1);
    CHECK(golden_eq(e1.out, 
        "vin=\"5YJ3E1EA7KF000316\"\n"
        "ip=\"192.168.1.50\"\n"
        "version=\"1.4.2\"\n"
        "key_present=true\n"
        "key_fingerprint=\"AB:CD:EF:01\"\n"
        "key_created=1750000000\n"
        "paired=true\n"
        "paired_at=1750500000\n"
        "reauth=false\n"
        "wifi{\n"
        "wifi.ssid=\"HomeNet\"\n"
        "wifi.rssi=-55\n"
        "wifi.std=\"Wi-Fi 6\"\n"
        "mqtt{\n"
        "mqtt.configured=true\n"
        "mqtt.connected=true\n"
        "mqtt.tls=true\n"
        "mqtt.broker=\"mqtt.local:8883\"\n"
        "syslog{\n"
        "syslog.configured=true\n"
        "syslog.resolved=true\n"
        "syslog.reachable=true\n"
        "syslog.host=\"syslog.lan\"\n"
        "syslog.port=514\n"
        "tele{\n"
        "tele.climate{\n"
        "tele.climate.inside=21.5\n"
        "tele.climate.outside=18\n"
        "tele.climate.setpoint=20.5\n"
        "tele.climate.on=true\n"
        "tele.climate.preconditioning=false\n"
        "tele.climate.cop=\"On\"\n"
        "tele.climate.cop_cooling=true\n"
        "tele.climate.cop_temp=\"Medium\"\n"
        "tele.climate.rear_defrost=false\n"
        "tele.climate.defrost_mode=\"Off\"\n"
        "tele.drive{\n"
        "tele.drive.shift=\"P\"\n"
        "tele.drive.odometer_km=12345.5\n"
        "tele.tires{\n"
        "tele.tires.fl=2.75\n"
        "tele.tires.fr=2.75\n"
        "tele.tires.rl=3\n"
        "tele.tires.warn=false\n"
        "tele.closures{\n"
        "tele.closures.locked=true\n"
        "tele.closures.door=false\n"
        "tele.closures.frunk=false\n"
        "tele.closures.trunk=true\n"
        "tele.closures.window=false\n"
        "ble{\n"
        "ble.connected=true\n"
        "ble.scanning=false\n"
        "ble.rssi=-60\n"
        "ble.addr=\"aa:bb:cc:dd:ee:ff\"\n"
        "link=\"awake\"\n"
        "vcsec_sleep=\"AWAKE\"\n"
        "vehicle{\n"
        "vehicle.soc=72.5\n"
        "vehicle.status=\"Charging\"\n"
        "vehicle.charge_limit=80\n"
        "vehicle.power=11\n"
        "vehicle.amps=16\n"
        "vehicle.actual_amps=16\n"
        "vehicle.volts=231\n"
        "vehicle.phases=2\n"
        "last{\n"
        "last.soc=72.5\n"
        "last.status=\"Charging\"\n"
        "last_seen_s=5\n"));

    // Scenario 2 — asleep: BLE link down (dropped between polls), charge cache retained,
    // debounced VCSEC sleep proven; no devices seen, no connect failures.
    Inputs s2;
    s2.vin = "5YJ3E1EA7KF000316"; s2.ip = "192.168.1.50"; s2.version = "1.4.2";
    s2.key_present = true; s2.key_fingerprint = "AB:CD:EF:01";
    s2.paired = true;   // stamps below the plausibility floor stay omitted
    s2.wifi_connected = true; s2.wifi_ssid = "HomeNet"; s2.wifi_rssi = -58;
    s2.mqtt_configured = true; s2.mqtt_connected = false; s2.mqtt_tls = false;
    s2.mqtt_broker = "mqtt.local:1883"; s2.mqtt_error = "connection refused";
    s2.link = tk::LinkState::Asleep; s2.vcsec_sleep = "ASLEEP";
    s2.charge.valid = true;
    s2.charge.has_battery_level = true; s2.charge.battery_level = 68.0f;
    s2.charge.charging_state = "Stopped";
    s2.have_last_seen = true; s2.last_seen_s = 3600;

    CollectEmitter e2;
    tk::status::emit_status(s2, e2);
    CHECK(golden_eq(e2.out, 
        "vin=\"5YJ3E1EA7KF000316\"\n"
        "ip=\"192.168.1.50\"\n"
        "version=\"1.4.2\"\n"
        "key_present=true\n"
        "key_fingerprint=\"AB:CD:EF:01\"\n"
        "paired=true\n"
        "reauth=false\n"
        "wifi{\n"
        "wifi.ssid=\"HomeNet\"\n"
        "wifi.rssi=-58\n"
        "mqtt{\n"
        "mqtt.configured=true\n"
        "mqtt.connected=false\n"
        "mqtt.tls=false\n"
        "mqtt.broker=\"mqtt.local:1883\"\n"
        "mqtt.error=\"connection refused\"\n"
        "syslog{\n"
        "syslog.configured=false\n"
        "syslog.resolved=false\n"
        "syslog.reachable=false\n"
        "ble{\n"
        "ble.connected=false\n"
        "ble.scanning=false\n"
        "ble.devices[\n"
        "link=\"asleep\"\n"
        "vcsec_sleep=\"ASLEEP\"\n"
        "last{\n"
        "last.soc=68\n"
        "last.status=\"Stopped\"\n"
        "last_seen_s=3600\n"));

    // Scenario 3 — unreachable, scanning, found-but-can't-connect: devices listed, a
    // connect-fail streak with last-seen advert RSSI and a non-connectable target (the
    // car at its ~3-device BLE limit).
    Inputs s3;
    s3.vin = "5YJ3E1EA7KF000316"; s3.ip = "10.0.0.7"; s3.version = "1.4.2";
    s3.key_present = true; s3.key_fingerprint = "AB:CD:EF:01"; s3.paired = true;
    s3.wifi_connected = true; s3.wifi_ssid = "Garage"; s3.wifi_rssi = -71; s3.wifi_std = "Wi-Fi 4";
    s3.ble_scanning = true;
    s3.devices.push_back({ "de:ad:be:ef:00:01", "S1a87029XXXXXXXXC", -77, true });
    s3.devices.push_back({ "de:ad:be:ef:00:02", "Sxxxxxxxxxxxxxxxx", -90, false });
    s3.connect_fail = 4;
    s3.have_seen_rssi = true; s3.seen_rssi = -77;
    s3.target_connectable = 0;
    s3.link = tk::LinkState::Unreachable; s3.vcsec_sleep = "UNKNOWN";
    s3.charge.valid = true;
    s3.charge.has_battery_level = true; s3.charge.battery_level = 54.0f;
    s3.charge.charging_state = "Disconnected";
    s3.have_last_seen = true; s3.last_seen_s = 90000;

    CollectEmitter e3;
    tk::status::emit_status(s3, e3);
    CHECK(golden_eq(e3.out, 
        "vin=\"5YJ3E1EA7KF000316\"\n"
        "ip=\"10.0.0.7\"\n"
        "version=\"1.4.2\"\n"
        "key_present=true\n"
        "key_fingerprint=\"AB:CD:EF:01\"\n"
        "paired=true\n"
        "reauth=false\n"
        "wifi{\n"
        "wifi.ssid=\"Garage\"\n"
        "wifi.rssi=-71\n"
        "wifi.std=\"Wi-Fi 4\"\n"
        "mqtt{\n"
        "mqtt.configured=false\n"
        "mqtt.connected=false\n"
        "mqtt.tls=false\n"
        "syslog{\n"
        "syslog.configured=false\n"
        "syslog.resolved=false\n"
        "syslog.reachable=false\n"
        "ble{\n"
        "ble.connected=false\n"
        "ble.scanning=true\n"
        "ble.devices[\n"
        "ble.devices.0{\n"
        "ble.devices.0.addr=\"de:ad:be:ef:00:01\"\n"
        "ble.devices.0.name=\"S1a87029XXXXXXXXC\"\n"
        "ble.devices.0.rssi=-77\n"
        "ble.devices.0.connectable=true\n"
        "ble.devices.1{\n"
        "ble.devices.1.addr=\"de:ad:be:ef:00:02\"\n"
        "ble.devices.1.name=\"Sxxxxxxxxxxxxxxxx\"\n"
        "ble.devices.1.rssi=-90\n"
        "ble.devices.1.connectable=false\n"
        "ble.connect_fail=4\n"
        "ble.rssi=-77\n"
        "ble.car_connectable=false\n"
        "link=\"unreachable\"\n"
        "vcsec_sleep=\"UNKNOWN\"\n"
        "last{\n"
        "last.soc=54\n"
        "last.status=\"Disconnected\"\n"
        "last_seen_s=90000\n"));

    // Scenario 4 — factory-fresh: no key, unpaired, no WiFi yet (portal case aside),
    // nothing cached, nothing heard. The contract still emits the empty wifi/mqtt/ble
    // containers and the devices[] array (app.js reads them unconditionally).
    Inputs s4;
    s4.vin = "UNKNOWN"; s4.version = "1.4.2";
    CollectEmitter e4;
    tk::status::emit_status(s4, e4);
    CHECK(golden_eq(e4.out, 
        "vin=\"UNKNOWN\"\n"
        "ip=\"\"\n"
        "version=\"1.4.2\"\n"
        "key_present=false\n"
        "key_fingerprint=\"\"\n"
        "paired=false\n"
        "reauth=false\n"
        "wifi{\n"
        "mqtt{\n"
        "mqtt.configured=false\n"
        "mqtt.connected=false\n"
        "mqtt.tls=false\n"
        "syslog{\n"
        "syslog.configured=false\n"
        "syslog.resolved=false\n"
        "syslog.reachable=false\n"
        "ble{\n"
        "ble.connected=false\n"
        "ble.scanning=false\n"
        "ble.devices[\n"
        "link=\"unknown\"\n"
        "vcsec_sleep=\"UNKNOWN\"\n"));
}

// ─── on-device display presenter (logic/display_model.hpp <- display.cpp compose()) ──────
namespace dm = tk::display;

// Build a UiSnapshot tersely for the presenter tests.
static tk::UiSnapshot us(tk::LinkState ls, bool wifi_on, bool ble_conn, bool paired,
                         bool have_soc, int soc, bool charging) {
    tk::UiSnapshot s;
    s.link_state     = ls;
    s.wifi_on        = wifi_on;
    s.ble_connected  = ble_conn;
    s.paired         = paired;
    s.have_soc       = have_soc;
    s.soc            = soc;
    s.charging       = charging;
    return s;
}

static void test_display_helpers() {
    // RSSI → 0..4 bars, at and around every threshold.
    CHECK(dm::rssi_bars(-50) == 4);  CHECK(dm::rssi_bars(-55) == 4);  CHECK(dm::rssi_bars(-56) == 3);
    CHECK(dm::rssi_bars(-65) == 3);  CHECK(dm::rssi_bars(-66) == 2);
    CHECK(dm::rssi_bars(-75) == 2);  CHECK(dm::rssi_bars(-76) == 1);
    CHECK(dm::rssi_bars(-85) == 1);  CHECK(dm::rssi_bars(-86) == 0);  CHECK(dm::rssi_bars(-99) == 0);

    // Text width: 5px glyph + 1px gap = 6 px/char, times the scale.
    CHECK(dm::text_w("",     2) == 0);
    CHECK(dm::text_w("A",    2) == 12);
    CHECK(dm::text_w("WiFi", 2) == 48);

    // SoC gradient: the five stops land on exact colours (mirrors soc_rgb()).
    int r, g, b;
    tk::soc_rgb(0,   r, g, b); CHECK(r == 231 && g ==  76 && b == 60);
    tk::soc_rgb(18,  r, g, b); CHECK(r == 240 && g == 190 && b == 40);
    tk::soc_rgb(45,  r, g, b); CHECK(r == 120 && g == 200 && b == 90);
    tk::soc_rgb(80,  r, g, b); CHECK(r ==  60 && g == 175 && b == 80);
    tk::soc_rgb(100, r, g, b); CHECK(r ==  30 && g == 140 && b == 60);
    // Out-of-range clamps to the endpoints, not past them.
    tk::soc_rgb(-5,  r, g, b); CHECK(r == 231 && g ==  76 && b == 60);
    tk::soc_rgb(150, r, g, b); CHECK(r ==  30 && g == 140 && b == 60);

    // SSID marquee ping-pong (span 10, pause 8, speed 2 → travel 5, period 26).
    CHECK(dm::scroll_offset(0,  10) == 0);    // paused at the start
    CHECK(dm::scroll_offset(7,  10) == 0);    // still in the start pause
    CHECK(dm::scroll_offset(10, 10) == 4);    // scrolling out
    CHECK(dm::scroll_offset(15, 10) == 10);   // paused at the end (fully revealed)
    CHECK(dm::scroll_offset(200, 0) == 0);    // nothing to scroll
}

static void test_display_model() {
    using LS = tk::LinkState;

    // ── priority ladder: WiFi search > pairing > BLE search > battery ──
    // WiFi down ⇒ WifiSearch, whatever else is true.
    CHECK(dm::compose(us(LS::Awake, /*wifi*/false, /*ble*/true, /*paired*/true,
                         /*soc?*/true, 50, false), 0).hero == dm::Hero::WifiSearch);
    // WiFi up, BLE link up but not yet paired ⇒ Pairing.
    CHECK(dm::compose(us(LS::Idle, true, /*ble*/true, /*paired*/false,
                         false, 0, false), 0).hero == dm::Hero::Pairing);
    // WiFi up, no BLE link, no usable SoC ⇒ BLE search.
    CHECK(dm::compose(us(LS::Unknown, true, /*ble*/false, false,
                         /*soc?*/false, 0, false), 0).hero == dm::Hero::BleSearch);
    // Unreachable overrides a stale cached SoC ⇒ still BLE search, never battery.
    CHECK(dm::compose(us(LS::Unreachable, true, false, true,
                         /*soc?*/true, 50, false), 0).hero == dm::Hero::BleSearch);
    // WiFi up, reachable, have SoC ⇒ Battery.
    CHECK(dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0).hero == dm::Hero::Battery);

    // ── header indicator visibility (a search hero hides its own small indicator) ──
    dm::Model wifi_s = dm::compose(us(LS::Unknown, false, false, false, false, 0, false), 0);
    CHECK(!wifi_s.show_wifi && wifi_s.show_ble);          // WiFi hero: wifi hidden, ble shown
    dm::Model ble_s = dm::compose(us(LS::Unknown, true, false, false, false, 0, false), 0);
    CHECK(ble_s.show_wifi && !ble_s.show_ble);            // BLE hero: ble hidden, wifi shown
    dm::Model batt = dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0);
    CHECK(batt.show_wifi && batt.show_ble);               // battery: both shown

    // ── SSID available width: wider when the BLE header is absent (BLE-search hero) ──
    CHECK(ble_s.ssid_avail == 130);                       // 158 - 28
    CHECK(batt.ssid_avail  ==  98);                       // 158 - 28 - 32

    // ── BLE header bars: 0 without a live RSSI, mapped when present ──
    tk::UiSnapshot rssi_snap = us(LS::Idle, true, true, true, true, 50, false);
    rssi_snap.ble_rssi_valid = true; rssi_snap.ble_rssi = -60;
    dm::Model rssi_m = dm::compose(rssi_snap, 0);
    CHECK(rssi_m.ble_bars == 3 && rssi_m.ble_glyph_on);
    CHECK(dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0).ble_bars == 0);  // no RSSI

    // ── battery fill colour, SoC clamp, and the charging bolt ──
    dm::Model full = dm::compose(us(LS::Idle, true, true, true, true, 150, /*charging*/true), 0);
    CHECK(full.soc == 100);                               // clamped
    CHECK(!full.show_bolt);                               // no bolt at 100%
    dm::Model chg = dm::compose(us(LS::Idle, true, true, true, true, 80, /*charging*/true), 0);
    CHECK(chg.show_bolt);                                 // charging below 100% ⇒ bolt
    CHECK(chg.fill_r == 60 && chg.fill_g == 175 && chg.fill_b == 80);   // soc 80 → the 0.80 gradient stop

    // Asleep dims the fill toward the panel colour AND suppresses the bolt even when charging.
    dm::Model slp = dm::compose(us(LS::Asleep, true, true, true, true, 80, /*charging*/true), 0);
    CHECK(slp.asleep && !slp.show_bolt);
    int br, bg, bb; tk::soc_rgb(80, br, bg, bb);
    CHECK(slp.fill_r == dm::lerp8(br, dm::kAsleepDimR, 0.5f));
    CHECK(slp.fill_g == dm::lerp8(bg, dm::kAsleepDimG, 0.5f));
    CHECK(slp.fill_b == dm::lerp8(bb, dm::kAsleepDimB, 0.5f));

    // ── animating: search/pairing heroes always; battery only while the SSID scrolls ──
    CHECK(wifi_s.animating);
    CHECK(dm::compose(us(LS::Idle, true, true, false, false, 0, false), 0).animating);  // pairing
    CHECK(ble_s.animating);
    CHECK(!batt.animating);                               // short SSID ("") ⇒ static battery
    tk::UiSnapshot long_ssid = us(LS::Idle, true, true, true, true, 50, false);
    std::strcpy(long_ssid.ssid, "AVeryLongNetworkNameXYZ");   // > 98 px at scale 2 ⇒ marquee
    dm::Model scr = dm::compose(long_ssid, 10);
    CHECK(scr.ssid_scrolling && scr.animating);

    // ── portrait orientation (BOOT-rotated 80x160) — only the SSID geometry differs ──
    // The SSID gets its own header row, full width (72px) at scale 1, independent of the BLE cluster.
    dm::Model p_batt = dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0, dm::Orient::Portrait);
    dm::Model p_ble  = dm::compose(us(LS::Unknown, true, false, false, false, 0, false), 0, dm::Orient::Portrait);
    CHECK(p_batt.ssid_avail == 72 && p_ble.ssid_avail == 72);   // same avail with or without BLE indicator
    // A 12-char SSID fits portrait's dedicated row (72px at scale 1) but scrolls landscape (144px > 98 at scale 2).
    tk::UiSnapshot mid_ssid = us(LS::Idle, true, true, true, true, 50, false);
    std::strcpy(mid_ssid.ssid, "TwelveCharSS");                // 12 ch → 72px (portrait fits) / 144px (landscape scrolls)
    CHECK(!dm::compose(mid_ssid, 10, dm::Orient::Portrait).ssid_scrolling);
    CHECK( dm::compose(mid_ssid, 10, dm::Orient::Landscape).ssid_scrolling);
    tk::UiSnapshot long_p = us(LS::Idle, true, true, true, true, 50, false);
    std::strcpy(long_p.ssid, "ThirteenChars");                 // 13 ch → 78px > 72 ⇒ portrait marquee too
    CHECK(dm::compose(long_p, 10, dm::Orient::Portrait).ssid_scrolling);
    // Orientation changes ONLY the SSID geometry — the hero/battery decisions are identical.
    CHECK(p_batt.hero == batt.hero && p_batt.soc == batt.soc);
    CHECK(p_batt.fill_r == batt.fill_r && p_batt.fill_g == batt.fill_g && p_batt.fill_b == batt.fill_b);
}

// ─── status LED priority ladder (logic/led_status.hpp <- shared UiSnapshot + LedAlerts) ──
// The LED reads the SAME tk::UiSnapshot the display presenter does (one input contract), plus
// a tiny LED-only LedAlerts for its latched error/warn/OTA tiers. Base = healthy, parked
// (Idle), paired, 55 % SoC, WiFi up, no alerts; each case flips one field to prove the ladder
// picks the right colour/animation and that a higher tier wins.
static tk::UiSnapshot led_snap() {
    tk::UiSnapshot s;
    s.wifi_on    = true;
    s.link_state = tk::LinkState::Idle;
    s.paired     = true;
    s.have_soc   = true;
    s.soc        = 55;
    return s;
}
static void test_led() {
    using C = tk::LedColor;
    using A = tk::LedAnim;
    const tk::LedAlerts none;   // no latched alerts

    // Parked with a known SoC → dimmed gradient, steady.
    CHECK(tk::led_pattern(led_snap(), none) == (tk::LedPattern{C::SocGradient, A::Solid, true}));
    // Full charge reads as steady green (still dim/resting).
    { auto s = led_snap(); s.soc = 100; CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Green, A::Solid, true})); }
    // Awake is also a "have live reading" state → gradient, not search.
    { auto s = led_snap(); s.link_state = tk::LinkState::Awake; CHECK(tk::led_pattern(s, none).color == C::SocGradient); }

    // Charging outranks the static SoC display (green swell).
    { auto s = led_snap(); s.charging = true; CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Green, A::Breathe, false})); }

    // Asleep → LED off entirely.
    { auto s = led_snap(); s.link_state = tk::LinkState::Asleep; s.have_soc = false;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Off, A::Off, false})); }

    // Pairing: BLE up but no session → magenta pulse. Outranks charging/SoC.
    { auto s = led_snap(); s.paired = false; s.ble_connected = true; s.charging = true;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Magenta, A::Pulse, false})); }

    // WiFi search: no LAN → blue breathe, and it outranks pairing/charging below it.
    { auto s = led_snap(); s.wifi_on = false; s.ble_connected = true; s.paired = false;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Blue, A::Breathe, false})); }

    // Searching for the car: reachable readings absent → teal breathe.
    { auto s = led_snap(); s.link_state = tk::LinkState::Unreachable;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Teal, A::Breathe, false})); }
    // A stale SoC while Unreachable must NOT show as a live gradient — still searching.
    { auto s = led_snap(); s.link_state = tk::LinkState::Unreachable; s.have_soc = true; s.soc = 80;
      CHECK(tk::led_pattern(s, none).color == C::Teal); }

    // ── LED-only latched alerts (top of the ladder), passed via LedAlerts ──
    // Warning (repeated connect failures) → amber blink, above WiFi/pairing.
    { auto s = led_snap(); s.wifi_on = false; tk::LedAlerts a; a.warn = true;
      CHECK(tk::led_pattern(s, a) == (tk::LedPattern{C::Amber, A::Blink, false})); }
    // OTA download → blue pulse, above the warning.
    { tk::LedAlerts a; a.ota_downloading = true; a.warn = true;
      CHECK(tk::led_pattern(led_snap(), a) == (tk::LedPattern{C::Blue, A::Pulse, false})); }
    // Error is the top of the ladder — beats OTA, warn, charging, everything.
    { auto s = led_snap(); s.charging = true; tk::LedAlerts a; a.error = true; a.ota_downloading = true; a.warn = true;
      CHECK(tk::led_pattern(s, a) == (tk::LedPattern{C::Red, A::Blink, false})); }
}

// ── /events WebSocket command policy (logic/ws_policy.hpp) ───────────────────────────────────
static void test_ws_policy() {
    using namespace tk;
    // An empty frame carries no command and leaves no body in the stream: ignore it, keep the
    // connection. Anything that fits the command buffer is safe to read.
    CHECK(ws_frame_plan(0) == WsPlan::Skip);
    CHECK(ws_frame_plan(3) == WsPlan::Read);

    // The boundary. A frame of exactly WS_CMD_MAX still fits; ONE byte more is undrainable on a
    // chip whose binding limit is the largest contiguous free block, so it must close the socket
    // rather than be read into a buffer sized from a length the client merely asserted.
    CHECK(ws_frame_plan(WS_CMD_MAX)     == WsPlan::Read);
    CHECK(ws_frame_plan(WS_CMD_MAX + 1) == WsPlan::Reject);
    CHECK(ws_frame_plan(1u << 20)       == WsPlan::Reject);
    CHECK(ws_frame_plan(SIZE_MAX)       == WsPlan::Reject);

    // The one command we speak — and a prefix still subscribes (a trailing newline / "subscribe").
    CHECK(ws_frame_action(true, "sub", 3)       == WsAction::Subscribe);
    CHECK(ws_frame_action(true, "sub\n", 4)     == WsAction::Subscribe);
    CHECK(ws_frame_action(true, "subscribe", 9) == WsAction::Subscribe);

    // Everything else earns nothing: no snapshot, and no slot in the broadcast list.
    CHECK(ws_frame_action(true, "nope", 4) == WsAction::Ignore);
    CHECK(ws_frame_action(true, "su",   2) == WsAction::Ignore);   // too short to be the command
    CHECK(ws_frame_action(true, "",     0) == WsAction::Ignore);
    CHECK(ws_frame_action(true, nullptr, 3) == WsAction::Ignore);  // never deref a null payload

    // A binary frame is not the text protocol, whatever bytes it carries.
    CHECK(ws_frame_action(false, "sub", 3) == WsAction::Ignore);
}

// ── /events per-subscriber send backpressure (logic/ws_policy.hpp) ────────────────────────────
// Regression cover for the 2026-07-18 wedge: one subscriber that stopped reading let the 2 s
// broadcast out-produce its 5 s send timeout until the queued payload copies exhausted the heap.
static void test_ws_backpressure() {
    using namespace tk;

    // A fresh subscriber may be sent to; the cap is what bounds the backlog.
    WsClientSend c;
    CHECK(ws_may_send(c));
    ws_note_queued(c);
    CHECK(ws_may_send(c));            // WS_MAX_INFLIGHT == 2, so one more is still allowed
    ws_note_queued(c);
    CHECK(!ws_may_send(c));           // at the cap: this is the tick that used to keep allocating

    // THE incident shape: a client that never completes a send is refused every subsequent tick,
    // no matter how long the stall lasts. The backlog therefore has a ceiling, not a slope.
    for (int tick = 0; tick < 1000; tick++) CHECK(!ws_may_send(c));

    // Completions free the slot again, so a healthy client keeps streaming.
    CHECK(!ws_note_completed(c, true));
    CHECK(ws_may_send(c));

    // A failure streak evicts — but only at WS_MAX_FAILS, so a single blip is tolerated.
    WsClientSend d;
    ws_note_queued(d);
    CHECK(!ws_note_completed(d, false));   // 1
    ws_note_queued(d);
    CHECK(!ws_note_completed(d, false));   // 2
    ws_note_queued(d);
    CHECK(ws_note_completed(d, false));    // 3 → close it

    // One good send clears the streak: a client that recovers is not punished for earlier failures.
    WsClientSend e;
    ws_note_queued(e); CHECK(!ws_note_completed(e, false));
    ws_note_queued(e); CHECK(!ws_note_completed(e, false));
    ws_note_queued(e); CHECK(!ws_note_completed(e, true));    // recovered → streak reset
    ws_note_queued(e); CHECK(!ws_note_completed(e, false));   // counting starts over, not at 3
    ws_note_queued(e); CHECK(!ws_note_completed(e, false));
    ws_note_queued(e); CHECK(ws_note_completed(e, false));

    // A completion that arrives with nothing in flight must not underflow the counter — an 8-bit
    // wrap to 255 would read as "permanently at the cap" and silence a live subscriber forever.
    WsClientSend f;
    CHECK(!ws_note_completed(f, true));
    CHECK(f.in_flight == 0);
    CHECK(ws_may_send(f));

    // Symmetrically, the queued counter saturates instead of wrapping to 0 (which would re-open
    // the floodgate the cap exists to hold shut).
    WsClientSend g;
    for (int i = 0; i < 1000; i++) ws_note_queued(g);
    CHECK(g.in_flight == UINT8_MAX);
    CHECK(!ws_may_send(g));
}

// ── request-body reassembly (logic/http_body.hpp) — the multi-segment truncation fix ──────────
static void test_http_body() {
    using namespace tk;
    char buf[64];

    // Whole body in one recv.
    {
        std::string body = "{\"amps\":16}";
        int r = http_body_read(buf, sizeof(buf), body.size(),
            [&](char* dst, size_t len) -> BodyChunk { std::memcpy(dst, body.data(), len); return { BodyRecv::Data, len }; });
        CHECK(r == (int)body.size());
        CHECK(std::string(buf) == body);
    }
    // Delivered ONE byte at a time (the segmentation the old single-recv truncated).
    {
        std::string body = "{\"broker\":\"host:1883\"}";
        size_t off = 0;
        int r = http_body_read(buf, sizeof(buf), body.size(),
            [&](char* dst, size_t) -> BodyChunk { dst[0] = body[off++]; return { BodyRecv::Data, 1 }; });
        CHECK(r == (int)body.size());
        CHECK(std::string(buf) == body);
    }
    // A mid-body peer error / close → fail (no truncated result handed back).
    {
        int r = http_body_read(buf, sizeof(buf), 10,
            [&](char* dst, size_t) -> BodyChunk { dst[0] = 'x'; return { BodyRecv::Error, 0 }; });
        CHECK(r == -1);
    }
    // Bounded timeouts: BODY_MAX_IDLE consecutive timeouts are tolerated, one more abandons it.
    {
        int calls = 0;
        int r = http_body_read(buf, sizeof(buf), 4,
            [&](char*, size_t) -> BodyChunk { ++calls; return { BodyRecv::Timeout, 0 }; });
        CHECK(r == -1);
        CHECK(calls == BODY_MAX_IDLE + 1);   // gives up after one too many
    }
    // Body too large for the buffer (no room for the terminator) → fail, never overflow.
    CHECK(http_body_read(buf, sizeof(buf), sizeof(buf), [&](char*, size_t) -> BodyChunk { return { BodyRecv::Data, 0 }; }) == -1);
    // Empty / null guards.
    CHECK(http_body_read(buf, sizeof(buf), 0, [&](char*, size_t) -> BodyChunk { return { BodyRecv::Data, 0 }; }) == -1);
    CHECK(http_body_read(nullptr, 8, 4, [&](char*, size_t) -> BodyChunk { return { BodyRecv::Data, 0 }; }) == -1);
}

// ── HA MQTT-discovery binary value_template (logic/ha_templates.hpp) — the phantom-OFF fix ─────
static void test_ha_templates() {
    // Every binary template MUST guard on `is defined` so an unreported optional field renders empty
    // (HA → unknown) instead of a phantom OFF — the whole point of the fix.
    std::string door = tk::ha_binary_value_template("door", false);
    CHECK(door == "{% if value_json.door is defined %}{{ 'ON' if value_json.door else 'OFF' }}{% endif %}");
    CHECK(door.find("is defined") != std::string::npos);

    // The inverted `lock` class emits OFF-when-true so a locked car reads "Locked", still guarded.
    std::string locked = tk::ha_binary_value_template("locked", true);
    CHECK(locked == "{% if value_json.locked is defined %}{{ 'OFF' if value_json.locked else 'ON' }}{% endif %}");
    CHECK(locked.find("is defined") != std::string::npos);
}

// ── Active-window gate (logic/active_window.hpp) — the stale-charging-window fix ───────────────
static void test_active_window() {
    using namespace tk;
    // A recent command opens the window regardless of charging/contact.
    CHECK(active_window_open({/*recent_cmd*/true, false, false, 0}) == true);
    CHECK(active_window_open({true, false, true, 999999}) == true);

    // Charging + FRESH contact holds the window open.
    CHECK(active_window_open({false, /*charging*/true, /*have_contact*/true, 0}) == true);
    CHECK(active_window_open({false, true, true, kAwakeMaxAgeS - 1}) == true);

    // The bug: charging cached true but contact STALE must NOT hold the window open (car departed
    // while "Charging" → otherwise perpetual scanning). Boundary is exactly kAwakeMaxAgeS.
    CHECK(active_window_open({false, true, true, kAwakeMaxAgeS})     == false);
    CHECK(active_window_open({false, true, true, kAwakeMaxAgeS + 1}) == false);
    CHECK(active_window_open({false, true, true, 999999})           == false);

    // Charging cached true but we have NO contact at all (never heard) → not fresh → closed.
    CHECK(active_window_open({false, true, /*have_contact*/false, 0}) == false);

    // Not charging and no recent command → closed, whatever the contact age.
    CHECK(active_window_open({false, false, true, 0}) == false);
    CHECK(active_window_open({false, false, false, 0}) == false);
}

int main() {
    test_vin();
    test_syslog_policy();
    test_ha_templates();
    test_units();
    test_link_state();
    test_link_state_strings();
    test_target();
    test_mcp();
    test_status_model();
    test_display_helpers();
    test_display_model();
    test_led();
    test_ws_policy();
    test_ws_backpressure();
    test_active_window();
    test_http_body();

    if (g_failures == 0) {
        std::printf("OK  %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
