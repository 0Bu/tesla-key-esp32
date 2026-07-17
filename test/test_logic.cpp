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
//   * Syslog target parsing + send-failure classification
//                                    (logic/syslog_policy.hpp <- syslog.cpp, /set_syslog)
// The cJSON envelope builders themselves stay IDF/cJSON-coupled; their pure inputs
// (the conversions and link strings that feed /status and the MQTT payloads) are
// what regress silently, and those are covered here.

#include "logic/vin.hpp"
#include "logic/units.hpp"
#include "logic/link_state.hpp"
#include "logic/target.hpp"
#include "logic/mcp.hpp"
#include "logic/command_result.hpp"
#include "logic/ui_state.hpp"
#include "logic/display_model.hpp"
#include "logic/soc_gradient.hpp"
#include "logic/led_status.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/ws_policy.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

    // Tool registry: every entry round-trips by name and by enum; role-refused commands
    // (which the Charging-Manager key can't execute) are deliberately not tools.
    for (const auto& t : tk::kMcpTools) {
        CHECK(tk::mcp_tool_from(t.name) == t.tool);
        CHECK(tk::mcp_tool_info(t.tool) == &t);
    }
    CHECK(tk::mcp_tool_from("door_unlock")     == tk::McpTool::Unknown);
    CHECK(tk::mcp_tool_from("set_sentry_mode") == tk::McpTool::Unknown);
    CHECK(tk::mcp_tool_from(nullptr)           == tk::McpTool::Unknown);
    CHECK(tk::mcp_tool_info(tk::McpTool::Unknown) == nullptr);

    // Arg-spec table — the single source of truth the advertised schema AND the executor
    // clamp both read (schema-vs-clamp drift is impossible by construction; this pins the
    // values themselves). Every non-None arg has a key and sane bounds; an absent
    // OPTIONAL Int defaults to 0 in the executor, so an optional spec's range must
    // contain 0.
    for (const auto& t : tk::kMcpTools) {
        for (const auto& a : t.args) {
            if (a.type == tk::McpArgType::None) continue;
            CHECK(a.key != nullptr);
            if (a.type == tk::McpArgType::Int) {
                CHECK(a.lo <= a.hi);
                // Optional Int args default to 0 when absent — the spec range must
                // contain 0 or the default would be out of the advertised bounds.
                if (!a.required) CHECK(a.lo <= 0 && 0 <= a.hi);
            }
        }
    }
    const tk::McpToolInfo* amps = tk::mcp_tool_info(tk::McpTool::SetChargingAmps);
    CHECK_STR(amps->args[0].key, "amps");
    CHECK(amps->args[0].type == tk::McpArgType::Int && amps->args[0].required);
    CHECK(amps->args[0].lo == 0 && amps->args[0].hi == 48);
    const tk::McpToolInfo* lim = tk::mcp_tool_info(tk::McpTool::SetChargeLimit);
    CHECK_STR(lim->args[0].key, "percent");
    CHECK(lim->args[0].required && lim->args[0].lo == 50 && lim->args[0].hi == 100);
    const tk::McpToolInfo* sched = tk::mcp_tool_info(tk::McpTool::SetScheduledCharging);
    CHECK_STR(sched->args[0].key, "enable");
    CHECK(sched->args[0].type == tk::McpArgType::Bool && sched->args[0].required);
    CHECK_STR(sched->args[1].key, "start_minutes");
    CHECK(sched->args[1].type == tk::McpArgType::Int && !sched->args[1].required);
    CHECK(sched->args[1].lo == 0 && sched->args[1].hi == 1439);
    // Read-only + no-arg tools carry no args.
    CHECK(tk::mcp_tool_info(tk::McpTool::GetVehicleState)->args[0].type == tk::McpArgType::None);
    CHECK(tk::mcp_tool_info(tk::McpTool::WakeUp)->args[0].type          == tk::McpArgType::None);

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

int main() {
    test_vin();
    test_syslog_policy();
    test_units();
    test_link_state();
    test_link_state_strings();
    test_target();
    test_mcp();
    test_display_helpers();
    test_display_model();
    test_led();
    test_ws_policy();

    if (g_failures == 0) {
        std::printf("OK  %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
