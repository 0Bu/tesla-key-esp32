#pragma once

// Internal header for the HTTP server implementation files ONLY (http_server.cpp,
// http_api.cpp, http_status.cpp, http_ota.cpp, http_config.cpp, mcp_server.cpp — split
// by route group; see .claude/CLAUDE.md "Architecture"). The public API stays
// http_server.hpp.
//
// Memory-model invariant (CLAUDE.md): EVERY handler declared here is invoked exclusively
// through handle_all's try/catch in http_server.cpp (503 on OOM). This is enforced
// structurally: handlers take GuardedReq — NOT the raw esp_http_server signature — so
// registering one directly with httpd_register_uri_handler (which would bypass the
// guard) is a compile error, not a comment violation.

#include "http_server.hpp"
#include <esp_http_server.h>
#include <cJSON.h>

// Global vehicle reference (set once in http_server_start).
extern VehicleController* g_vehicle;

// Global runtime-config store (tesla_cfg NVS namespace; set once in http_server_start,
// same idiom as g_vehicle). The persisted-config handlers (/set_vin, /set_mqtt, /set_time)
// read/write it directly. Keys must be ≤15 chars (NVS limit); an empty value disables the
// feature it gates.
extern NvsStorageAdapter* g_config;

// Proof-of-guard wrapper: constructed only inside handle_all's try/catch dispatch in
// http_server.cpp. Because every handler takes this instead of httpd_req_t*, its
// signature cannot match httpd_uri_t::handler, making a guard-bypassing direct
// registration impossible by construction.
struct GuardedReq { httpd_req_t* req; };

// Defined in main.cpp: true once SNTP has synced this boot. The browser /set_time
// fallback only applies the client clock while this is false (NTP is authoritative).
bool clock_synced_via_ntp();

// Defined in main.cpp: true only while the STA holds an IP. Gate esp_wifi_sta_get_ap_info()
// so it's never read during association churn (concurrent read of the half-built AP
// record faults — LoadProhibited/EXCVADDR=0x1).
bool wifi_is_connected();

// ─── Shared helpers (http_common.cpp) ─────────────────────────────────────────

// Serialize `root` (consumed) as the response with the given status. Degrades to a
// 503 when cJSON_PrintUnformatted returns NULL on a fragmented heap (that path returns
// NULL rather than throwing, so it bypasses the handle_all try/catch).
esp_err_t send_json(httpd_req_t* req, int status, cJSON* root);

cJSON* make_response(bool result, const char* command, const char* vin, const char* reason);

// Read POST body into a string (caller must free). NULL on empty/oversized/failed read.
char* read_body(httpd_req_t* req);

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

// ─── Route handlers ───────────────────────────────────────────────────────────

// http_api.cpp — evcc-facing TeslaBleHttpProxy-compatible API
esp_err_t handle_command(GuardedReq rq);          // POST /api/1/vehicles/{VIN}/command/{CMD}
esp_err_t handle_vehicle_data(GuardedReq rq);     // GET  /api/1/vehicles/{VIN}/vehicle_data
esp_err_t handle_body_controller(GuardedReq rq);  // GET  /api/1/vehicles/{VIN}/body_controller_state
esp_err_t handle_version(GuardedReq rq);          // GET  /api/proxy/1/version

// http_status.cpp — web UI + device status/diagnostics
esp_err_t handle_index(GuardedReq rq);            // GET  /  (embedded, pre-gzipped web UI)
esp_err_t handle_status(GuardedReq rq);           // GET  /status
esp_err_t handle_diag(GuardedReq rq);             // GET  /diag
esp_err_t handle_scan(GuardedReq rq);             // POST /scan

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

// mcp_server.cpp — MCP endpoint (Streamable HTTP, stateless JSON-RPC 2.0; docs/MCP.md)
esp_err_t mcp_handle_post(GuardedReq rq);         // POST /mcp
esp_err_t mcp_handle_get(GuardedReq rq);          // GET  /mcp → 405 (no SSE stream)
