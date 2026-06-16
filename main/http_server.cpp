#include "http_server.hpp"
#include <esp_http_server.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <cstdlib>

static const char* TAG = "http_server";

// Global vehicle reference (set once at start)
static VehicleController* g_vehicle = nullptr;

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t* req, int status, cJSON* root) {
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (status != 200) {
        char code[4];
        snprintf(code, sizeof(code), "%d", status);
        httpd_resp_set_status(req, status == 400 ? "400 Bad Request"
                                : status == 404 ? "404 Not Found"
                                : "500 Internal Server Error");
    }
    esp_err_t ret = httpd_resp_sendstr(req, body);
    free(body);
    return ret;
}

static cJSON* make_response(bool result, const char* command,
                             const char* vin, const char* reason) {
    cJSON* root     = cJSON_CreateObject();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "response", response);
    cJSON_AddBoolToObject(response, "result", result);
    cJSON_AddStringToObject(response, "command", command);
    cJSON_AddStringToObject(response, "vin",     vin);
    cJSON_AddStringToObject(response, "reason",  reason);
    return root;
}

// Read POST body into a string (caller must free)
static char* read_body(httpd_req_t* req) {
    if (req->content_len == 0) return nullptr;
    size_t len = req->content_len;
    char* buf  = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) { free(buf); return nullptr; }
    buf[received] = '\0';
    return buf;
}

// Extract last path segment from URI like /api/1/vehicles/VIN/command/CMD
static bool parse_uri(const char* uri, char* vin_out, size_t vin_sz,
                       char* cmd_out, size_t cmd_sz) {
    // URI pattern: /api/1/vehicles/{VIN}/command/{CMD}
    const char* vehicles = strstr(uri, "/vehicles/");
    if (!vehicles) return false;
    const char* vin_start = vehicles + strlen("/vehicles/");
    const char* command   = strstr(vin_start, "/command/");
    if (!command) return false;
    size_t vin_len = command - vin_start;
    if (vin_len == 0 || vin_len >= vin_sz) return false;
    strncpy(vin_out, vin_start, vin_len);
    vin_out[vin_len] = '\0';
    const char* cmd_start = command + strlen("/command/");
    // strip query string
    const char* q = strchr(cmd_start, '?');
    size_t cmd_len = q ? (size_t)(q - cmd_start) : strlen(cmd_start);
    if (cmd_len == 0 || cmd_len >= cmd_sz) return false;
    strncpy(cmd_out, cmd_start, cmd_len);
    cmd_out[cmd_len] = '\0';
    return true;
}

// Extract VIN from /api/1/vehicles/{VIN}/...
static bool parse_vin_only(const char* uri, char* vin_out, size_t vin_sz) {
    const char* vehicles = strstr(uri, "/vehicles/");
    if (!vehicles) return false;
    const char* vin_start = vehicles + strlen("/vehicles/");
    const char* slash = strchr(vin_start, '/');
    size_t vin_len = slash ? (size_t)(slash - vin_start) : strlen(vin_start);
    if (vin_len == 0 || vin_len >= vin_sz) return false;
    strncpy(vin_out, vin_start, vin_len);
    vin_out[vin_len] = '\0';
    return true;
}

// ─── POST /api/1/vehicles/{VIN}/command/{CMD} ─────────────────────────────────

static esp_err_t handle_command(httpd_req_t* req) {
    char vin[64], cmd[64];
    if (!parse_uri(req->uri, vin, sizeof(vin), cmd, sizeof(cmd))) {
        return send_json(req, 400, make_response(false, "unknown", "?", "invalid URI"));
    }
    ESP_LOGI(TAG, "CMD %s on VIN %s", cmd, vin);

    // Parse optional JSON body
    char* body = read_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);

    bool ok = false;

    if      (strcmp(cmd, "wake_up")           == 0) ok = g_vehicle->wake_up();
    else if (strcmp(cmd, "charge_start")      == 0) ok = g_vehicle->charge_start();
    else if (strcmp(cmd, "charge_stop")       == 0) ok = g_vehicle->charge_stop();
    else if (strcmp(cmd, "charge_port_door_open")  == 0) ok = g_vehicle->charge_port_open();
    else if (strcmp(cmd, "charge_port_door_close") == 0) ok = g_vehicle->charge_port_close();
    else if (strcmp(cmd, "door_lock")         == 0) ok = g_vehicle->door_lock();
    else if (strcmp(cmd, "door_unlock")       == 0) ok = g_vehicle->door_unlock();
    else if (strcmp(cmd, "flash_lights")      == 0) ok = g_vehicle->flash_lights();
    else if (strcmp(cmd, "honk_horn")         == 0) ok = g_vehicle->honk_horn();
    else if (strcmp(cmd, "auto_conditioning_start") == 0) ok = g_vehicle->climate_start();
    else if (strcmp(cmd, "auto_conditioning_stop")  == 0) ok = g_vehicle->climate_stop();
    else if (strcmp(cmd, "set_charging_amps") == 0) {
        int amps = 0;
        if (json) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "charging_amps");
            if (j) amps = (int)(cJSON_IsNumber(j) ? j->valuedouble
                                                   : atof(j->valuestring));
        }
        ok = g_vehicle->set_charging_amps(amps);
    }
    else if (strcmp(cmd, "set_charge_limit")  == 0) {
        int pct = 80;
        if (json) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "percent");
            if (j) pct = (int)(cJSON_IsNumber(j) ? j->valuedouble
                                                  : atof(j->valuestring));
        }
        ok = g_vehicle->set_charge_limit(pct);
    }
    else if (strcmp(cmd, "set_sentry_mode")   == 0) {
        bool on = false;
        if (json) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "on");
            if (j) on = cJSON_IsTrue(j);
        }
        ok = g_vehicle->set_sentry_mode(on);
    }
    else {
        cJSON_Delete(json);
        return send_json(req, 404, make_response(false, cmd, vin, "unknown command"));
    }

    cJSON_Delete(json);
    return send_json(req, 200,
        make_response(ok, cmd, vin, ok ? "command executed successfully"
                                       : "command failed or timed out"));
}

// ─── GET /api/1/vehicles/{VIN}/vehicle_data ───────────────────────────────────

static esp_err_t handle_vehicle_data(httpd_req_t* req) {
    char vin[64];
    parse_vin_only(req->uri, vin, sizeof(vin));

    ChargeStateResult cs{};
    bool ok = g_vehicle->get_charge_state(cs);

    cJSON* root     = cJSON_CreateObject();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "response", response);
    cJSON_AddBoolToObject(response, "result", ok);
    cJSON_AddStringToObject(response, "vin", vin);

    if (ok) {
        cJSON* data  = cJSON_CreateObject();
        cJSON* state = cJSON_CreateObject();
        cJSON_AddStringToObject(state, "charging_state",   cs.charging_state.c_str());
        cJSON_AddNumberToObject(state, "battery_level",    cs.battery_level);
        cJSON_AddNumberToObject(state, "charge_limit_soc", cs.charge_limit_soc);
        cJSON_AddNumberToObject(state, "charger_power",    cs.charger_power);
        cJSON_AddNumberToObject(state, "charge_rate",      cs.charge_rate);
        cJSON_AddNumberToObject(state, "charging_amps",    cs.charging_amps);
        cJSON_AddNumberToObject(state, "battery_range",    cs.battery_range);
        cJSON_AddItemToObject(data, "charge_state", state);
        cJSON_AddItemToObject(response, "data", data);
        cJSON_AddStringToObject(response, "reason", "success");
    } else {
        cJSON_AddStringToObject(response, "reason", "failed to retrieve vehicle data");
    }

    return send_json(req, 200, root);
}

// ─── GET /api/1/vehicles/{VIN}/body_controller_state ─────────────────────────

static esp_err_t handle_body_controller(httpd_req_t* req) {
    char vin[64];
    parse_vin_only(req->uri, vin, sizeof(vin));

    VehicleStatusResult vs{};
    bool ok = g_vehicle->get_vehicle_status(vs);

    cJSON* root     = cJSON_CreateObject();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "response", response);
    cJSON_AddBoolToObject(response, "result", ok);
    cJSON_AddStringToObject(response, "vin", vin);

    if (ok) {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "vehicle_lock_state",   vs.lock_state.c_str());
        cJSON_AddStringToObject(data, "vehicle_sleep_status", vs.sleep_status.c_str());
        cJSON_AddStringToObject(data, "user_presence",        vs.user_presence.c_str());
        cJSON_AddItemToObject(response, "data", data);
        cJSON_AddStringToObject(response, "reason", "success");
    } else {
        cJSON_AddStringToObject(response, "reason", "failed to retrieve vehicle status");
    }

    return send_json(req, 200, root);
}

// ─── POST /gen_keys ───────────────────────────────────────────────────────────

static esp_err_t handle_gen_keys(httpd_req_t* req) {
    bool ok = g_vehicle->generate_key();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "result", ok);
    cJSON_AddStringToObject(root, "reason", ok ? "key generated — use /send_key to pair with vehicle"
                                               : "key generation failed");
    return send_json(req, 200, root);
}

// ─── POST /send_key ───────────────────────────────────────────────────────────

static esp_err_t handle_send_key(httpd_req_t* req) {
    // Optional: ?role=owner or ?role=charging_manager
    char role_param[32] = "charging_manager";
    if (req->uri && strstr(req->uri, "role=owner")) {
        strcpy(role_param, "owner");
    }
    bool owner = (strcmp(role_param, "owner") == 0);
    bool ok = g_vehicle->pair(owner);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "result", ok);
    cJSON_AddStringToObject(root, "role",   role_param);
    cJSON_AddStringToObject(root, "reason",
        ok ? "key sent — tap NFC card on Tesla center console to confirm"
           : "failed to send key (vehicle not reachable or timed out)");
    return send_json(req, 200, root);
}

// ─── GET /api/proxy/1/version ─────────────────────────────────────────────────

static esp_err_t handle_version(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", "1.0.0-esp32");
    cJSON_AddStringToObject(root, "platform", "ESP32-S3");
    return send_json(req, 200, root);
}

// ─── Wildcard handler dispatching ─────────────────────────────────────────────

// Single catch-all handler registered for /*
static esp_err_t handle_all(httpd_req_t* req) {
    const char* uri = req->uri;

    if (req->method == HTTP_POST && strstr(uri, "/command/")) {
        return handle_command(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/vehicle_data")) {
        return handle_vehicle_data(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/body_controller_state")) {
        return handle_body_controller(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/gen_keys")) {
        return handle_gen_keys(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/send_key")) {
        return handle_send_key(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/version")) {
        return handle_version(req);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "not found");
    return send_json(req, 404, root);
}

// ─── Start ────────────────────────────────────────────────────────────────────

bool http_server_start(VehicleController& vehicle) {
    g_vehicle = &vehicle;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 2;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return false;
    }

    // Wildcard GET handler
    httpd_uri_t get_handler = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = handle_all,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &get_handler);

    // Wildcard POST handler
    httpd_uri_t post_handler = {
        .uri      = "/*",
        .method   = HTTP_POST,
        .handler  = handle_all,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &post_handler);

    ESP_LOGI(TAG, "HTTP server started on :80");
    return true;
}
