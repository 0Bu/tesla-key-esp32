#pragma once

// Internal header for the HTTP server implementation files ONLY (http_server.cpp,
// http_api.cpp, http_status.cpp, http_ota.cpp, http_config.cpp, mcp_server.cpp — split
// by route group; see docs/ARCHITECTURE.md). The public API stays
// http_server.hpp.
//
// Memory-model invariant (.claude/CLAUDE.md): EVERY handler declared here is invoked exclusively
// through handle_all's try/catch in http_server.cpp (503 on OOM). This is enforced
// structurally: handlers take GuardedReq — NOT the raw esp_http_server signature — so
// registering one directly with httpd_register_uri_handler (which would bypass the
// guard) is a compile error, not a comment violation.

#include "http_server.hpp"
#include "json_builder.hpp"
#include "json_http_reply.hpp"
#include "mcp_json_payloads.hpp"
#include "logic/command_registry.hpp"
#include "logic/http_body.hpp"
#include <esp_http_server.h>

// Global vehicle reference (set once in http_server_start).
extern VehicleController* g_vehicle;

// Global runtime-config store (tesla_cfg NVS namespace; set once in http_server_start,
// same idiom as g_vehicle). The configuration handlers read/write it directly: /set_vin,
// /set_mqtt, /set_syslog and /set_wifi update the atomic `cfg` blob, while /set_time updates the
// separately owned `last_time` key. Keys must be ≤15 chars (NVS limit).
extern NvsStorageAdapter* g_config;

// Proof-of-guard wrapper: constructed only inside handle_all's try/catch dispatch in
// http_server.cpp. Because every handler takes this instead of httpd_req_t*, its
// signature cannot match httpd_uri_t::handler, making a guard-bypassing direct
// registration impossible by construction.
struct GuardedReq { httpd_req_t* req; };

// Defined in main.cpp: true once SNTP has synced this boot. The browser /set_time
// fallback only applies the client clock while this is false (NTP is authoritative).
bool clock_synced_via_ntp();

// Link state, the active netif and the WiFi-only readings all come from the transport seam
// (net.hpp): tk::net_is_up(), tk::net_active_netif(), tk::net_wifi_signal(). Handlers must not
// call esp_wifi_sta_get_ap_info() themselves — the station record has transiently-null fields
// mid-association and a concurrent read there faults (LoadProhibited/EXCVADDR=0x1).

// ─── Shared helpers (http_common.cpp) ─────────────────────────────────────────

// Serialize `root` (consumed) as the response with the given status. Degrades to a
// 503 when cJSON_PrintUnformatted returns NULL on a fragmented heap (that path returns
// NULL rather than throwing, so it bypasses the handle_all try/catch).
esp_err_t send_json(httpd_req_t* req, int status, cJSON* root);

cJSON* make_response(bool result, const char* command, const char* vin, const char* reason);

// Read a POST body while preserving the exact failure class.  Persisted configuration handlers
// MUST use this form so empty/failed/OOM input can never be mistaken for an explicit JSON "" that
// disables a service.  `data` is malloc-owned on Ok and must be freed by the caller.
tk::BodyReadResult read_body_result(httpd_req_t* req);

// True only if query parameter `key` is present AND equals `want` exactly. Replaces
// strstr(uri,"force=1")-style checks, which also fire on "force=10", "xforce=1", or the
// same string buried in an unrelated parameter value — a real hazard for /gen_keys?force=1,
// whose whole job is to gate the destructive key-overwrite that un-pairs the car.
bool query_param_is(httpd_req_t* req, const char* key, const char* want);

// True if a browser epoch (ms) is inside the floor..ceiling plausibility window
// (floor ~2023-11, ceiling build year + 10). The clock gates OTA TLS certificate
// validation, so an unauthenticated LAN client must not push it far in either direction.
bool browser_time_plausible(double epoch_ms);

// Apply an (already plausibility-checked) browser epoch as the wall clock and persist
// it for the next offline boot. The ONE implementation both NTP-fallback entry points
// (/set_time and /ota/check?ms=) share, so the security-sensitive clock-set path can
// never drift between them. Returns the applied epoch seconds (for logging).
long long apply_browser_clock(double epoch_ms);

// The ONE kind → VehicleController dispatch both command surfaces execute through
// (command_exec.cpp). ival/bval are the positional value arrays each surface filled
// against the shared arg specs in logic/command_registry.hpp (tk::kCmdMaxArgs slots).
bool execute_vehicle_command(VehicleController& v, tk::CmdKind kind,
                             const int* ival, const bool* bval);

// ─── Route handlers ───────────────────────────────────────────────────────────

// http_api.cpp — evcc-facing TeslaBleHttpProxy-compatible API
// Exact, query-tolerant predicate owned by the same parser as handle_command().
bool is_command_route(const char* uri);
esp_err_t handle_command(GuardedReq rq);          // POST /api/1/vehicles/{VIN}/command/{CMD}
esp_err_t handle_vehicle_data(GuardedReq rq);     // GET  /api/1/vehicles/{VIN}/vehicle_data
esp_err_t handle_body_controller(GuardedReq rq);  // GET  /api/1/vehicles/{VIN}/body_controller_state
esp_err_t handle_version(GuardedReq rq);          // GET  /api/proxy/1/version

// http_status.cpp — web UI + device status/diagnostics
esp_err_t handle_index(GuardedReq rq);            // GET  /  (embedded, pre-gzipped web UI)
esp_err_t handle_status(GuardedReq rq);           // GET  /status
esp_err_t handle_diag(GuardedReq rq);             // GET  /diag[?redact=1]
esp_err_t handle_scan(GuardedReq rq);             // POST /scan
esp_err_t handle_coredump(GuardedReq rq);         // GET  /coredump[?clear=1]
esp_err_t handle_crash_dismiss(GuardedReq rq);    // POST /crash/dismiss
esp_err_t handle_heap(GuardedReq rq);             // GET  /heap

// http_ota.cpp — OTA self-update endpoints
esp_err_t handle_ota_check(GuardedReq rq);        // GET  /ota/check[?ms=<epoch>]
esp_err_t handle_ota_update(GuardedReq rq);       // POST /ota/update
esp_err_t handle_ota_status(GuardedReq rq);       // GET  /ota/status

// http_config.cpp — setup / pairing / persisted-config endpoints
esp_err_t handle_gen_keys(GuardedReq rq);         // POST /gen_keys[?force=1]
esp_err_t handle_send_key(GuardedReq rq);         // POST /send_key
esp_err_t handle_set_time(GuardedReq rq);         // POST /set_time
esp_err_t handle_set_vin(GuardedReq rq);          // POST /set_vin
esp_err_t handle_set_mqtt(GuardedReq rq);         // POST /set_mqtt
esp_err_t handle_set_syslog(GuardedReq rq);       // POST /set_syslog
esp_err_t handle_set_wifi(GuardedReq rq);         // POST /set_wifi

// mcp_server.cpp — MCP endpoint (Streamable HTTP, stateless JSON-RPC 2.0; docs/MCP.md)
esp_err_t mcp_handle_post(GuardedReq rq);         // POST /mcp
esp_err_t mcp_handle_get(GuardedReq rq);          // GET  /mcp → 405 (no SSE stream)
