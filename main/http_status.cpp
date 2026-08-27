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
#include "mqtt_ha.hpp"
#include "logic/status_model.hpp"
#include "logic/redact.hpp"
#include "logic/heap_json_stream.hpp"
#include "config_blob.hpp"
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
    httpd_req_t* req = rq.req;
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
    in.paired_at       = (long long)g_vehicle->paired_at();
    in.reauth          = g_vehicle->reauth_required();

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
    // Read from the CONFIG, not from any live state: the marker outlives the reboot the rollback
    // performed, which is the whole reason it is persisted rather than kept in RAM.
    //
    // Read ONCE per boot, not per request. The marker cannot change while this image runs — both
    // writers (main.cpp's rollback, POST /set_wifi) reboot immediately after saving — while
    // /status is the web UI's 4 s poll, so a read here is an NVS blob read plus its std::vector on
    // a request path, forever, for a value that is fixed at boot. This device's binding limit is
    // the largest CONTIGUOUS free block and the rule in AGENTS.md is not to allocate on a request
    // path without needing to.
    static const bool s_rolled_back = [] {
        tk::ConfigBlob cb;
        tk::cfg_load(*g_config, cb);
        return cb.wifi_rolled_back;
    }();
    in.wifi_rolled_back = s_rolled_back;

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
    in.charge      = g_vehicle->get_cached_charge();

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
    in.httpd_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::Httpd);
    in.vehicle_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::Vehicle);
    in.auto_pair_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::AutoPair);
    in.mqtt_stack_min_free_bytes =
        tk::stack_watch_min_free_bytes(tk::StackWatch::Mqtt);

    // ── last_crash ────────────────────────────────────────────────────────────────────────────
    // diag_crash_info_live() re-reads only the "is a dump still in flash" flag (a 4-byte read), not
    // the summary parse — GET /coredump?clear=1 can erase the image mid-session, and a cached flag
    // would leave the UI on a crash banner offering a download that 404s.
    const tk::CrashInfo ci = tk::diag_crash_info_live();
    in.reset_reason = tk::reset_reason_slug(ci.reset_code);
    if (tk::crash_is_notable(ci)) {
        in.have_crash        = true;
        in.crash_reason      = tk::reset_reason_slug(ci.reset_code);
        in.crash_reason_code = ci.reset_code;
        in.crash_fault       = ci.fault;
        in.crash_coredump    = ci.coredump;
        in.crash_corrupted   = ci.corrupted;
        in.crash_task        = ci.task;
        in.crash_pc          = ci.pc;
        in.crash_backtrace   = ci.backtrace;
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
    httpd_req_t* req = rq.req;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // ?redact=1 is the BUG-REPORT form: the six reporter-identifying values read "<redacted>"
    // (logic/status_model.hpp). Opt-in per request, never the default — the dashboard legitimately
    // shows the SSID, the broker and the VIN, and this is the same payload it polls.
    const bool redact = query_param_is(req, "redact", "1");
    return send_json(req, 200, build_status_object(redact));  // send_json degrades a nullptr to 503
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

// ─── GET /coredump — stream the raw crash image for offline symbolisation ─────
//
// The one artifact a panic leaves behind. Streamed in fixed chunks straight from flash: the image
// is tens of KB and this device's binding limit is the largest CONTIGUOUS free block, so reading it
// into one buffer is precisely the allocation that would fail on the fragmented heap a crash tends
// to leave. Decode it offline against the .elf of the SAME build (CI archives one per build);
// /status.last_crash.elf_sha256 is what identifies which.
//
// ?clear=1 erases the partition, freeing the slot for the next panic. Deliberately separate from
// POST /crash/dismiss: this frees flash and leaves the fault reset on record, while a dismissal
// says the crash has been dealt with.
esp_err_t handle_coredump(GuardedReq rq) {
    httpd_req_t* req = rq.req;

#if !defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    // This build writes no dumps (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH off — no current target
    // does, but see diag_crash.cpp). The route still EXISTS and answers with a reason
    // rather than 404-ing as "not found": a tool that just read `coredump:false` off /status needs
    // to be able to tell "this board never captures dumps" from "no crash has happened yet", and
    // silence cannot carry that difference. The esp_core_dump_image_* symbols are not linked here
    // at all, so everything below has to be compiled out rather than merely skipped.
    tk::JsonBuilder json;
    json.string(json.root(), "error", "core dumps are not enabled on this target");
    return send_json(req, 404, json.release());
#else
    if (query_param_is(req, "clear", "1")) {
        esp_err_t err = esp_core_dump_image_erase();
        tk::JsonBuilder json;
        json.boolean(json.root(), "ok", err == ESP_OK);
        if (err != ESP_OK) json.string(json.root(), "error", esp_err_to_name(err));
        return send_json(req, err == ESP_OK ? 200 : 500, json.release());
    }

    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        tk::JsonBuilder json;
        // 404 with a reason rather than an empty body: on a device flashed before the coredump
        // partition existed this is the PERMANENT answer, and "no coredump partition" is a very
        // different thing for the reader to know than "no crash has happened".
        json.string(json.root(), "error", "no core dump available");
        return send_json(req, 404, json.release());
    }

    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                                           nullptr);
    if (!part) {
        tk::JsonBuilder json;
        json.string(json.root(), "error", "no coredump partition");
        return send_json(req, 404, json.release());
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // 1 KB of stack, not heap: the read buffer must not compete for the contiguous block, and this
    // handler runs on the httpd task whose stack budget is hand-counted (see AGENTS.md).
    char   buf[1024];
    size_t off = 0;
    while (off < size) {
        const size_t n = (size - off) > sizeof(buf) ? sizeof(buf) : (size - off);
        const esp_err_t read_err = esp_partition_read(part, off, buf, n);
        if (read_err != ESP_OK) {
            // Once chunking starts, a terminating zero chunk would make a truncated dump look
            // complete to the downloader. Return failure without that terminator so httpd closes
            // the response; the byte offset is the durable clue for flash/read diagnostics.
            ESP_LOGE(TAG, "coredump read failed at offset %u/%u: %s",
                     static_cast<unsigned>(off), static_cast<unsigned>(size),
                     esp_err_to_name(read_err));
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;  // client went away
        off += n;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
#endif
}

// ─── POST /crash/dismiss — acknowledge and DELETE this boot's crash report ────
//
// POST rather than a GET beside /coredump: it destroys the one artifact a bug report needs, so it
// must not be reachable by a link, a prefetch or a crawler.
esp_err_t handle_crash_dismiss(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    const bool ok = tk::diag_crash_dismiss();
    tk::JsonBuilder json;
    json.boolean(json.root(), "ok", ok);
    if (!ok) json.string(json.root(), "error", "core dump erase failed");
    return send_json(req, ok ? 200 : 500, json.release());
}

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
    httpd_req_t* req = rq.req;

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
