// Web-UI-facing routes: the embedded page itself and the endpoints that drive it:
//   GET  /        (embedded, pre-gzipped web UI)
//   GET  /status  (device + pairing state — the UI polls this every 4 s)
//   GET  /diag    (in-memory diagnostic log; ?redact=1 for a bug report)
//   POST /scan    (time-limited BLE discovery scan)
//   GET  /coredump (stream the raw crash image; ?clear=1 erases it)
//   POST /crash/dismiss (acknowledge + delete this boot's crash report)
//   GET  /heap    (the board's 24-hour free/largest-block trend)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "net.hpp"
#include "diag_log.hpp"
#include "diag_crash.hpp"
#include "heap_trend.hpp"
#include "safe_mode.hpp"
#include "runtime_admission.hpp"
#include "mqtt_ha.hpp"
#include "logic/status_model.hpp"
#include "logic/redact.hpp"
#include "logic/heap_json_stream.hpp"
#include "config_blob.hpp"
#include "ota_update.hpp"
#include "syslog.hpp"
#include "stack_watch.hpp"
#include "status_json_emitter.hpp"
#include <esp_netif.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

static constexpr const char* TAG = "http-status";

// ─── POST /scan — start a time-limited BLE discovery scan ─────────────────────

esp_err_t handle_scan(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (!g_vehicle->ble_scan()) {
        tk::JsonBuilder unavailable;
        unavailable.boolean(unavailable.root(), "result", false);
        unavailable.string(unavailable.root(), "reason", "vehicle runtime unavailable");
        return send_json(req, 503, unavailable.release());
    }
    tk::JsonBuilder json;
    json.boolean(json.root(), "result", true);
    json.string(json.root(), "reason", "scanning for nearby Teslas (~12s)");
    return send_json(req, 200, json.release());
}

// ─── GET /status — device + pairing state (drives the web UI) ──────────────────

static void current_ip(char* out, size_t sz) {
    out[0] = '\0';
    esp_netif_t* netif = tk::net_active_netif();
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, out, sz);
    }
}

// Whether the car is "live" (awake) vs merely sleeping vs unreachable is decided centrally
// in VehicleController::link_state(), shared with the MQTT bridge so the two never drift.

// cJSON visitor for tk::status::emit_status() — builds the response tree one-to-one as
// the model walks it (no intermediate field list; the contract layer adds zero heap).
// The shared builder is sticky: any Create/Add failure invalidates the whole tree; release() then
// destroys it and returns nullptr, so send_json emits 503 instead of a structurally valid but
// dangerously partial status document with an HTTP 200.
// Build the device + pairing + vehicle status object (caller owns the returned cJSON) — the body of
// GET /status, which the web UI polls every 4 s. GATHER ONLY here: every which-field/when/what-value
// decision lives in the host-tested model (logic/status_model.hpp, golden CHECKs in
// test/test_logic.cpp); this collects the inputs under the existing locks, then emits. The throwing
// by-value getters (vin(), broker, syslog host, cached structs) all run in the GATHER below — BEFORE
// any cJSON is allocated — so a std::bad_alloc can't leak a partial tree; the emit that follows only
// does cJSON allocs, which return NULL under pressure rather than throw. May return nullptr under
// total OOM; the caller guards for it.
static cJSON* build_status_object(bool redact) {
    tk::status::Inputs in;
    in.redact = redact;

    char ip[16];
    current_ip(ip, sizeof(ip));
    in.ip              = ip;
    in.vin             = g_vehicle->vin();
    in.version         = esp_app_get_description()->version;
    in.key_present     = g_vehicle->has_key();
    in.key_fingerprint = g_vehicle->key_fingerprint();
    in.key_created     = (long long)g_vehicle->key_created_at();
    in.paired          = g_vehicle->has_session();
    in.sessions        = g_vehicle->session_availability();
    in.paired_at       = (long long)g_vehicle->paired_at();
    in.reauth          = g_vehicle->reauth_required();
    in.pairing_block_reason = g_vehicle->pairing_block_reason();

    // WiFi: SSID + live signal strength (dBm) of the station link, both from the transport
    // seam, which owns the mid-association guard. All three stay unset on a wired device, so
    // the `wifi` object comes out empty exactly as it does while the radio is down.
    char ssid[33] = {0};
    int  rssi     = 0;
    if (tk::net_wifi_signal(&rssi, ssid)) {
        in.wifi_connected = true;
        in.wifi_ssid      = ssid;
        in.wifi_rssi      = rssi;
        if (const char* std_ = tk::net_wifi_standard()) in.wifi_std = std_;
    }

    // Ethernet: emitted only while the wire carries the lease (logic/status_model.hpp explains
    // why its presence — not a false-valued object — is the signal).
    in.eth_link = (tk::net_kind() == tk::NetLink::Eth);
    if (in.eth_link) tk::net_eth_phy(&in.eth_speed_mbps, &in.eth_full_duplex);
    // Boot seeds this cache from verified configuration; confirmed commits refresh it before
    // fallible mirroring or reply work. Polling never reads or allocates an NVS blob.
    in.wifi_rolled_back = tk::cfg_wifi_rolled_back();
    in.ota_channel = tk::ota_channel_name(ota_get_channel());
    in.ota_verification = ota_cached_verification_state();

    in.mqtt_configured = mqtt_ha_configured();
    in.mqtt_connected  = mqtt_ha_connected();
    in.mqtt_tls        = mqtt_ha_tls();
    in.mqtt_broker     = mqtt_ha_broker();
    in.mqtt_error      = mqtt_ha_last_error();

    // Syslog: configured / DNS-resolved (the delivery gate) / advisory ARP-ICMP
    // reachability hint. Gathered into the model like every other field.
    {
        SyslogStatus sy = syslog_status();
        in.syslog_configured = sy.configured;
        in.syslog_resolved   = sy.resolved;
        in.syslog_reachable  = sy.reachable;
        in.syslog_dropped_lines = sy.dropped_lines;
        in.syslog_host       = sy.host;
        in.syslog_port       = sy.port;
        in.syslog_error      = sy.error;
    }

    in.ble_connected = g_vehicle->ble_connected();
    in.ble_scanning  = g_vehicle->ble_scanning();
    if (in.ble_connected) {
        int8_t rssi = 0;
        if (g_vehicle->ble_rssi(rssi)) { in.have_ble_rssi = true; in.ble_rssi = rssi; }
        in.ble_addr = g_vehicle->ble_peer();
        // Telemetry caches ride along only while the link is up (model rule).
        in.climate  = g_vehicle->get_cached_climate();
        in.drive    = g_vehicle->get_cached_drive();
        in.tires    = g_vehicle->get_cached_tires();
        in.closures = g_vehicle->get_cached_closures();
    } else {
        for (const auto& d : g_vehicle->ble_nearby())
            in.devices.push_back({ d.addr, d.name, d.rssi, d.connectable });
        in.connect_fail = g_vehicle->ble_connect_fail();
        int8_t srssi;
        if (g_vehicle->ble_seen_rssi(srssi)) { in.have_seen_rssi = true; in.seen_rssi = srssi; }
        in.target_connectable = g_vehicle->ble_target_connectable();
    }

    {
        tk::ble::PhaseView ph = g_vehicle->ble_phase();
        in.ble_phase   = tk::ble::phase_name(ph.kind);   // "" ⇒ model omits the block
        in.ble_phase_s = ph.secs;
    }
    in.link        = g_vehicle->link_state();
    in.vcsec_sleep = g_vehicle->vcsec_sleep_raw();
    in.charge = g_vehicle->get_cached_charge_with_age(in.have_charge_age, in.charge_age_s);

    uint32_t ago = 0;
    if (g_vehicle->seconds_since_contact(ago)) { in.have_last_seen = true; in.last_seen_s = ago; }
    in.last_reboot = VehicleController::boot_reboot_reason();

    // ── sys — always gathered, never conditional ──────────────────────────────────────────────
    // INTERNAL caps on all three, matching logic/heap_watchdog.hpp exactly: plain 8BIT reports the
    // max across every heap carrying the cap, and a board that registers PSRAM there would report
    // megabytes free while internal DRAM sat in the exact wedge this device restarts itself for.
    // The eFuse-derived WiFi STA MAC is the physical BOARD identity even when Ethernet is active.
    // Cache its canonical spelling once: it cannot change during this or any later boot.
    static const std::string s_board_mac = [] {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return std::string("unavailable");
        char text[18];
        std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
                      (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                      (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
        return std::string(text);
    }();
    in.board_mac       = s_board_mac;
    in.free_heap       = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    in.min_free_heap   = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    in.largest_block   = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    in.uptime_s        = (uint32_t)(esp_timer_get_time() / 1000000);
    in.wifi_reconnects = tk::net_reconnect_count();
    in.safe_mode       = tk::safe_mode_active();
    in.runtime_state   = tk::runtime_admission_state();
    in.diag_dropped_lines = diag_log_dropped_lines();
    in.httpd_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::Httpd);
    in.vehicle_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::Vehicle);
    in.auto_pair_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::AutoPair);
    in.mqtt_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::Mqtt);

    // ── last_crash ────────────────────────────────────────────────────────────────────────────
    // The live snapshot queries bounded header metadata and rechecks admission after borrowing
    // immutable boot data. It never reparses the summary or restores invalidated authority.
    const auto ci = tk::diag_crash_info_live();
    in.reset_reason = tk::reset_reason_slug(ci.reset_code);
    if (tk::crash_is_notable(ci)) {
        in.have_crash        = true;
        in.crash_reason      = tk::reset_reason_slug(ci.reset_code);
        in.crash_reason_code = ci.reset_code;
        in.crash_fault       = ci.fault;
        in.crash_coredump    = ci.coredump;
        in.crash_artifact_present = ci.artifact_present;
        in.crash_raw_available = ci.raw_available;
        in.crash_query_error = ci.query_error;
        in.crash_foreign_dump = ci.foreign_dump;
        in.crash_checksum_verified = ci.checksum_verified;
        in.crash_dump_identity = ci.dump_identity;
        in.crash_dump_elf_sha256 = ci.dump_elf_sha256;
        in.crash_summary_available = ci.summary_available;
        in.crash_truncated = ci.truncated;
        in.crash_isr = ci.isr;
        in.crash_corrupted   = ci.corrupted;
        in.crash_task        = ci.task;
        in.crash_pc          = ci.pc;
        in.crash_backtrace.assign(ci.backtrace.begin(), ci.backtrace.end());
        in.crash_elf_sha256  = ci.elf_sha256;
    }

    // The last-known charge snapshot ("last" / "last_seen_s", shown on the asleep card
    // regardless of link state) is emitted by the model from in.charge + in.last_seen.
    tk::JsonBuilder json;
    tk::StatusJsonEmitter e(json);
    tk::status::emit_status(in, e);
    return e.release();
}

// GET /status — the live snapshot. The web UI's feed: app.js polls this every 4 s, and it also
// serves curl/diagnostics and the post-OTA reboot probe (app.js waitReboot()). Never cache: a stale
// copy sticks the hero on a transient state until a manual reload (matches "/" and "/diag") — the
// poll cache-busts the URL for the same reason.
esp_err_t handle_status(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (validate_query_string(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query string");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // ?redact=1 is the BUG-REPORT form: the six reporter-identifying values read "<redacted>"
    // (logic/status_model.hpp). Opt-in per request, never the default — the dashboard legitimately
    // shows the SSID, the broker and the VIN, and this is the same payload it polls.
    const bool redact = query_param_is(req, "redact", "1");
    return send_json(req, 200, build_status_object(redact));  // send_json degrades a nullptr to 503
}

// ─── GET /diag — in-memory diagnostic log (for on-demand analysis) ────────────

esp_err_t handle_diag(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (validate_query_string(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query string");
    }
    if (query_param_is(req, "clear", "1"))        diag_log_clear();
    if (query_param_is(req, "verbose", "1"))      diag_set_verbose(true);
    else if (query_param_is(req, "verbose", "0")) diag_set_verbose(false);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Diag-Verbose", diag_verbose() ? "1" : "0");
    // Stream the log in fixed-size chunks. Each chunk is copied under the ring mutex and sent only
    // after that mutex is released, so slow/throwing HTTP I/O cannot stall log producers. Building
    // one big std::string here used to throw std::bad_alloc when the whole buffer exceeded the
    // largest contiguous free block on a fragmented heap → uncaught in the httpd task →
    // abort() → reboot. Chunked send needs no large contiguous allocation, so /diag is safe
    // regardless of buffer size or fragmentation.
    if (!query_param_is(req, "redact", "1")) {
        const DiagDumpResult dump = diag_log_dump_chunks([req](const char* p, size_t n) {
            return n == 0 || httpd_resp_send_chunk(req, p, n) == ESP_OK;
        });
        if (dump != DiagDumpResult::Complete) return ESP_FAIL;
        return httpd_resp_send_chunk(req, nullptr, 0);  // terminate the chunked response
    }

    // ?redact=1 — the bug-report form. /diag leaks by LINE (unlike /status, which leaks by field):
    // a handful of log statements interpolate the VIN, an SSID, an IP, a vehicle BLE MAC, the
    // broker or the syslog host. The board MAC deliberately survives as hardware diagnosis. The
    // rules are the host-tested logic/redact.hpp, and they FAIL CLOSED — a line
    // the ring truncated mid-value redacts to the end of the line rather than giving up.
    //
    // A wrapped byte ring may begin inside a secret after its marker was overwritten. The dump
    // seam therefore suppresses that first untrusted fragment through its newline; all retained
    // complete lines are reassembled in a fixed stack buffer. Bounded on purpose:
    // building the redacted dump as one std::string is exactly the allocation /diag streams to
    // avoid, and a redaction REPLACEMENT is usually longer than the value it replaces, so the
    // redacted text can be bigger than the buffer it came from.
    static constexpr size_t kLineMax = 288;   // diag lines are capped at 256; slack for a long one
    static constexpr size_t kRedactedLineMax = tk::diag_redacted_capacity(kLineMax);
    char   line[kLineMax];
    char   redacted[kRedactedLineMax];
    bool   ok  = true;
    tk::DiagLineFrame frame;

    auto flush_line = [&](size_t len, bool had_newline) noexcept {
        if (!ok) return;
        if (len == 0 && !had_newline) return;
        const tk::FixedDiagRedaction result =
            tk::redact_diag_line_fixed(std::string_view(line, len),
                                       redacted, sizeof(redacted));
        const char* out = result.safe ? redacted : tk::kRedacted;
        const size_t out_len = result.safe ? result.size : tk::kRedactedLength;
        if (out_len != 0 && httpd_resp_send_chunk(req, out, out_len) != ESP_OK) ok = false;
        if (ok && had_newline && httpd_resp_send_chunk(req, "\n", 1) != ESP_OK) ok = false;
    };
    auto flush_overlong = [&](bool had_newline) noexcept {
        if (!ok) return;
        if (httpd_resp_send_chunk(req, tk::kRedacted, tk::kRedactedLength) != ESP_OK) ok = false;
        if (ok && had_newline && httpd_resp_send_chunk(req, "\n", 1) != ESP_OK) ok = false;
    };

    const DiagDumpResult dump = diag_log_dump_chunks([&](const char* p, size_t n) noexcept {
        for (size_t i = 0; i < n && ok; i++) {
            const tk::DiagLineStep step = tk::diag_line_step(frame, p[i], kLineMax);
            switch (step.action) {
                case tk::DiagLineAction::Append:
                    line[step.size] = p[i];
                    break;
                case tk::DiagLineAction::EmitLine:
                    flush_line(step.size, true);
                    break;
                case tk::DiagLineAction::IgnoreOverlong:
                    break;
                case tk::DiagLineAction::EmitOverlong:
                    flush_overlong(true);
                    break;
            }
        }
        return ok;
    }, DiagDumpStart::AfterWrappedLineBoundary);
    // Do not make a truncated snapshot look like a complete 200 response. In particular, never
    // flush the partly accumulated line after a sink failure or a concurrent clear/overwrite,
    // and never send the success terminator after either failure class.
    if (dump != DiagDumpResult::Complete || !ok) return ESP_FAIL;
    if (frame.overlong) flush_overlong(false);
    else                flush_line(frame.size, false);  // unterminated final line
    if (!ok) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// Coredump access is request-owned. Current-build downloads require boot verification;
// raw=1 is an explicit bounded forensic download, never a whole-partition fallback.
static esp_err_t coredump_error(GuardedReq rq, int status, const char* reason) {
    tk::JsonBuilder json;
    json.string(json.root(), "error", reason);
    rq.reply()->state.terminal = true;
    return send_json(rq.req(), status, json.release());
}
static esp_err_t coredump_erase_reply(GuardedReq rq, bool dismiss) {
    rq.reply()->state.shape = tk::ReplyShape::FlatResult;
    const auto result = tk::diag_crash_erase(dismiss, rq.reply()->state);
    const bool ok = tk::crash_erase_permits_dismiss(result);
    tk::JsonBuilder json;
    json.boolean(json.root(), "ok", ok);
    json.boolean(json.root(), "result", ok);
    if (result == tk::CrashEraseResult::Busy) json.string(json.root(), "error", "crash operation busy");
    else if (!ok) {
        json.string(json.root(), "outcome", "unknown");
        json.string(json.root(), "reason", "core dump erase was not confirmed; do not retry automatically");
    }
    if (!rq.reply()->state.needs_effect_reply()) rq.reply()->state.terminal = true;
    return send_effect_json(rq, result == tk::CrashEraseResult::Busy ? 409 : 200, json.release());
}

esp_err_t handle_coredump(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    const size_t length = httpd_req_get_url_query_len(req);
    if (length > 7) return coredump_error(rq, 400, "invalid core dump query");
    char query[8]{};
    if (length && httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return coredump_error(rq, 400, "invalid core dump query");
    const auto mode = tk::coredump_request_mode(std::string_view(query, length));
    if (mode == tk::CoredumpRequestMode::Invalid)
        return coredump_error(rq, 400, "invalid core dump query");
    if (mode == tk::CoredumpRequestMode::Clear) return coredump_erase_reply(rq, false);

    tk::CoredumpLease lease;
    tk::CoredumpDownload source;
    const auto opened = tk::diag_crash_open_dump(lease, mode == tk::CoredumpRequestMode::Raw, source);
    if (opened == tk::CoredumpOpenResult::Busy) return coredump_error(rq, 409, "crash operation busy");
    if (opened == tk::CoredumpOpenResult::Error) return coredump_error(rq, 503, "core dump query failed");
    if (opened == tk::CoredumpOpenResult::Absent) return coredump_error(rq, 404, "no core dump available");
    if (opened != tk::CoredumpOpenResult::Ready)
        return coredump_error(rq, 409, "core dump is not verified for this firmware");

    // A header/send/read failure is terminal. No JSON fallback or clean terminator follows it.
    rq.reply()->state.terminal = true;
    if (httpd_resp_set_type(req, "application/octet-stream") != ESP_OK ||
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"") != ESP_OK ||
        httpd_resp_set_hdr(req, "Cache-Control", "no-store") != ESP_OK) return ESP_FAIL;
    tk::CoredumpReadBuffer buffer;
    for (uint32_t offset = 0; offset < source.bytes;) {
        const uint32_t remaining = source.bytes - offset;
        const size_t bytes = remaining < sizeof(buffer.bytes) ? remaining : sizeof(buffer.bytes);
        const esp_err_t read_err = esp_partition_read(source.partition, offset, buffer.bytes, bytes);
        if (read_err != ESP_OK) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, buffer.bytes, bytes) != ESP_OK) return ESP_FAIL;
        offset += bytes;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// Acknowledgement is published only after confirmed absence or successful erase.
esp_err_t handle_crash_dismiss(GuardedReq rq) { return coredump_erase_reply(rq, true); }

// ─── GET /heap — the board's own 24-hour memory trend ─────────────────────────
//
// Two series, oldest sample first, in TENTHS OF A KiB (the ring's storage unit — exact, no floats
// on the wire, and the consumer scales by 10). `null` marks a bucket with no sample.
//
// What this answers that /status.sys cannot: whether the heap is DRIFTING. A leak is a slope;
// fragmentation is `free` holding steady while `largest` sinks toward the 4 KB floor at which the
// heap watchdog restarts the device. Neither is visible in any single reading, and both are what
// precedes the failure this firmware has a whole watchdog for.
esp_err_t handle_heap(GuardedReq rq) {
    httpd_req_t* req = rq.req();

    // Caller-owned buffers, filled under the ring's lock without allocating there. 2 × 288 ×
    // int16_t = 1152 B of stack, against the httpd task's 8192 (http_server.cpp) — ~14 %, and
    // cheaper than the heap it reports on. Growing kHeapHistorySamples grows this linearly, so
    // that constant and this budget move together.
    static constexpr size_t kMax = tk::kHeapHistorySamples;
    tk::HeapTrendSample free_s[kMax];
    tk::HeapTrendSample large_s[kMax];
    uint32_t bucket0 = 0, boot_bucket = 0;
    const size_t n = tk::heap_trend_snapshot(free_s, large_s, kMax, &bucket0, &boot_bucket);

    // Stream bounded chunks directly from the caller-owned snapshot. The full 24-hour history is
    // 576 values; materialising it as cJSON nodes plus one contiguous Print buffer makes the heap
    // diagnostic disappear precisely under the fragmentation it is meant to explain.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    const tk::HeapJsonStreamView view{
        tk::heap_trend_dt_s(), bucket0, boot_bucket,
        free_s, large_s, n, tk::kHeapTrendNoSample,
    };
    const bool sent = tk::stream_heap_json(view, [req](const char* data, size_t size) {
        return httpd_resp_send_chunk(req, data, size) == ESP_OK;
    });
    if (!sent) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
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
    httpd_req_t* req = rq.req();
    if (httpd_resp_set_hdr(req, "X-Frame-Options", "DENY") != ESP_OK ||
        httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'") != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    // The UI is embedded in the firmware and changes with every flash/OTA. Without
    // this, browsers cache index.html and keep rendering the OLD layout (with live
    // /status data) after an update — so tell them never to cache the page.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    const size_t len = index_html_gz_end - index_html_gz_start;
    return httpd_resp_send(req, (const char*)index_html_gz_start, len);
}
