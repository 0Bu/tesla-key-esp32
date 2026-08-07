#include "mqtt_ha.hpp"
#include "net.hpp"
#include "vehicle_ctrl.hpp"
#include "nvs_storage.hpp"
#include "platform.hpp"
#include "logic/units.hpp"
#include "logic/link_state.hpp"
#include "task_config.hpp"
#include "logic/ha_templates.hpp"
#include "logic/mqtt_uri.hpp"
#include "diag_crash.hpp"
#include "safe_mode.hpp"
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <atomic>
#include <string>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include <cJSON.h>

static const char* TAG = "mqtt_ha";


// Cumulative broker RE-connects since boot (the first connect of a boot is not counted). Written
// from the esp-mqtt event task, read by the publisher task — an atomic, since neither may take a
// lock on those paths.
static std::atomic<unsigned> s_reconnects{0};

// ─── Module state ─────────────────────────────────────────────────────────────
static esp_mqtt_client_handle_t s_client = nullptr;
static VehicleController*        s_vehicle = nullptr;
static std::atomic<bool>         s_connected{false};
static std::atomic<bool>         s_need_discovery{false};
// atomic: written once in mqtt_ha_start (app_main task), read from the HTTP /status task and
// the MQTT event callback — a cross-task scalar, so atomic gives it a defined value.
static std::atomic<bool>         s_configured{false};
static std::atomic<bool>         s_tls{false};           // connection uses mqtts:// (TLS)

// Last connection error, surfaced in /status so the web UI can explain a stuck connection
// (especially a TLS handshake failure, since we never silently fall back to plaintext mqtt).
// An atomic code (mapped to text in mqtt_ha_last_error) avoids locking a string across tasks.
enum MqttErr { ME_NONE = 0, ME_TRANSPORT, ME_REFUSED, ME_OTHER };
static std::atomic<int>          s_last_err{ME_NONE};

// Resolved config + derived topics (built once in mqtt_ha_start).
static std::string s_uri, s_user, s_pass;     // broker connection
static std::string s_prefix;                   // HA discovery prefix (e.g. "homeassistant")
static std::string s_node;                     // unique node id (teslakey_<mac3>)
static std::string s_base;                     // state-topic base (<base_prefix>/<node>)
static std::string s_avail;                    // availability (LWT) topic
static std::string s_devname;                  // HA device display name
static std::string s_cfgurl;                   // device configuration_url (http://<ip>)
static std::string s_broker_disp;              // "host:port" for the web UI
static int         s_interval_s = 15;
static bool        s_client_started = false;

struct CJsonDeleter {
    void operator()(cJSON* value) const noexcept { cJSON_Delete(value); }
};
struct CJsonStringDeleter {
    void operator()(char* value) const noexcept { free(value); }
};
using CJsonPtr = std::unique_ptr<cJSON, CJsonDeleter>;
using CJsonStringPtr = std::unique_ptr<char, CJsonStringDeleter>;

// State topics, indexed by Domain.
enum Domain { D_CHARGE, D_CLIMATE, D_DRIVE, D_TIRES, D_CLOSURES, D_VEHICLE, D_DEVICE, D_COUNT };
static std::string s_topic[D_COUNT];

// ─── HA-Discovery entity table ────────────────────────────────────────────────
// One row per HA entity. `field` is the JSON key inside the domain's state topic;
// the value_template pulls it out. Binary entities render ON/OFF from a JSON bool.
struct Entry {
    Domain      dom;
    const char* comp;      // "sensor" | "binary_sensor"
    const char* obj;       // object_id (unique within node) → unique_id + config topic
    const char* name;      // friendly name
    const char* field;     // JSON field in the domain state topic
    const char* dev_cla;   // device_class or nullptr
    const char* unit;      // unit_of_measurement or nullptr
    const char* stat_cla;  // state_class or nullptr
    const char* ent_cat;   // entity_category ("diagnostic") or nullptr
    bool        is_binary; // true → binary_sensor (ON/OFF template)
};

static const Entry ENTRIES[] = {
    // ── Charge (charge_state cache) ──────────────────────────────────────────
    { D_CHARGE,  "sensor",        "soc",            "Battery",             "soc",            "battery",   "%",   "measurement", nullptr,      false },
    { D_CHARGE,  "sensor",        "charge_limit",   "Charge limit",        "charge_limit",   "battery",   "%",   "measurement", nullptr,      false },
    { D_CHARGE,  "sensor",        "charger_power",  "Charger power",        "power",          "power",     "kW",  "measurement", nullptr,      false },
    { D_CHARGE,  "sensor",        "charging_amps",  "Charging current",    "amps",           "current",   "A",   "measurement", nullptr,      false },
    { D_CHARGE,  "sensor",        "range",          "Range",               "range",          "distance",  "km",  "measurement", nullptr,      false },
    { D_CHARGE,  "sensor",        "charge_rate",    "Charge rate",         "rate",           nullptr,     "km/h","measurement",nullptr,      false },
    { D_CHARGE,  "sensor",        "charging_state", "Charging state",      "charging_state", nullptr,     nullptr,nullptr,      nullptr,      false },
    { D_CHARGE,  "sensor",        "actual_current", "Charging current (actual)",   "actual_current", "current",  "A",   "measurement",      nullptr,      false },
    { D_CHARGE,  "sensor",        "current_request","Charging current (requested)","current_request","current",  "A",   "measurement",      nullptr,      false },
    { D_CHARGE,  "sensor",        "charger_voltage","Charger voltage",     "volts",          "voltage",   "V",   "measurement",      nullptr,      false },
    { D_CHARGE,  "sensor",        "phases",         "Charger phases",      "phases",         nullptr,     nullptr,"measurement",       nullptr,      false },
    { D_CHARGE,  "sensor",        "energy_added",   "Energy added",        "energy_added",   "energy",    "kWh", "total_increasing",  nullptr,      false },
    { D_CHARGE,  "sensor",        "minutes_to_full","Time to full",        "minutes_to_full","duration",  "min", "measurement",       nullptr,      false },
    { D_CHARGE,  "sensor",        "limit_reason",   "Charge limit reason", "limit_reason",   nullptr,     nullptr,nullptr,            "diagnostic", false },

    // ── Climate (climate_state cache) ────────────────────────────────────────
    { D_CLIMATE, "sensor",        "inside_temp",    "Inside temperature",  "inside",         "temperature","°C", "measurement", nullptr,      false },
    { D_CLIMATE, "sensor",        "outside_temp",   "Outside temperature", "outside",        "temperature","°C", "measurement", nullptr,      false },
    { D_CLIMATE, "sensor",        "setpoint",       "Climate setpoint",    "setpoint",       "temperature","°C", "measurement", nullptr,      false },
    { D_CLIMATE, "binary_sensor", "climate_on",     "Climate",             "on",             "running",   nullptr,nullptr,      nullptr,      true  },
    { D_CLIMATE, "binary_sensor", "preconditioning","Preconditioning",     "preconditioning","running",   nullptr,nullptr,      nullptr,      true  },
    // Cabin Overheat Protection — separate from is_climate_on; runs while parked.
    { D_CLIMATE, "binary_sensor", "cop_cooling",    "Overheat protection cooling","cop_cooling","running",nullptr,nullptr,      nullptr,      true  },
    { D_CLIMATE, "sensor",        "cop",            "Overheat protection", "cop",            nullptr,     nullptr,nullptr,      "diagnostic", false },
    { D_CLIMATE, "sensor",        "cop_temp",       "Overheat threshold",  "cop_temp",       nullptr,     nullptr,nullptr,      "diagnostic", false },
    { D_CLIMATE, "sensor",        "cop_reason",     "Overheat protection reason","cop_reason",nullptr,    nullptr,nullptr,      "diagnostic", false },
    // Defrost — front/rear defroster + Max-defrost mode.
    { D_CLIMATE, "binary_sensor", "front_defrost",  "Front defroster",     "front_defrost",  "running",   nullptr,nullptr,      nullptr,      true  },
    { D_CLIMATE, "binary_sensor", "rear_defrost",   "Rear defroster",      "rear_defrost",   "running",   nullptr,nullptr,      nullptr,      true  },
    { D_CLIMATE, "sensor",        "defrost_mode",   "Defrost mode",        "defrost_mode",   nullptr,     nullptr,nullptr,      "diagnostic", false },

    // ── Drive (drive_state cache) ────────────────────────────────────────────
    { D_DRIVE,   "sensor",        "shift_state",    "Shift state",         "shift",          nullptr,     nullptr,nullptr,      nullptr,      false },
    { D_DRIVE,   "sensor",        "odometer",       "Odometer",            "odometer",       "distance",  "km",  "total_increasing", nullptr, false },

    // ── Tires (tire_pressure cache) ──────────────────────────────────────────
    { D_TIRES,   "sensor",        "tire_fl",        "Tire front left",     "fl",             "pressure",  "bar", "measurement", nullptr,      false },
    { D_TIRES,   "sensor",        "tire_fr",        "Tire front right",    "fr",             "pressure",  "bar", "measurement", nullptr,      false },
    { D_TIRES,   "sensor",        "tire_rl",        "Tire rear left",      "rl",             "pressure",  "bar", "measurement", nullptr,      false },
    { D_TIRES,   "sensor",        "tire_rr",        "Tire rear right",     "rr",             "pressure",  "bar", "measurement", nullptr,      false },
    { D_TIRES,   "binary_sensor", "tire_warn",      "Tire pressure warning","warn",          "problem",   nullptr,nullptr,      nullptr,      true  },

    // ── Closures (closures_state cache) ──────────────────────────────────────
    { D_CLOSURES,"binary_sensor", "locked",         "Locked",              "locked",         "lock",      nullptr,nullptr,      nullptr,      true  },
    { D_CLOSURES,"binary_sensor", "door_open",      "Doors",               "door",           "door",      nullptr,nullptr,      nullptr,      true  },
    { D_CLOSURES,"binary_sensor", "frunk_open",     "Frunk",               "frunk",          "opening",   nullptr,nullptr,      nullptr,      true  },
    { D_CLOSURES,"binary_sensor", "trunk_open",     "Trunk",               "trunk",          "opening",   nullptr,nullptr,      nullptr,      true  },
    { D_CLOSURES,"binary_sensor", "window_open",    "Windows",             "window",         "window",    nullptr,nullptr,      nullptr,      true  },
    { D_CLOSURES,"binary_sensor", "occupant",       "Occupant",            "user",           "occupancy", nullptr,nullptr,      nullptr,      true  },

    // ── Vehicle status (vcsec cache, best-effort) ────────────────────────────
    { D_VEHICLE, "sensor",        "sleep_state",    "Sleep state",         "sleep_status",   nullptr,     nullptr,nullptr,      "diagnostic", false },

    // ── Device diagnostics ───────────────────────────────────────────────────
    { D_DEVICE,  "sensor",        "wifi_rssi",      "WiFi signal",         "wifi_rssi",      "signal_strength","dBm","measurement","diagnostic", false },
    { D_DEVICE,  "sensor",        "ble_rssi",       "BLE signal",          "ble_rssi",       "signal_strength","dBm","measurement","diagnostic", false },
    { D_DEVICE,  "binary_sensor", "ble_link",       "BLE link",            "ble_connected",  "connectivity",nullptr,nullptr,    "diagnostic", true  },
    { D_DEVICE,  "binary_sensor", "paired",         "Paired",              "paired",         "connectivity",nullptr,nullptr,    "diagnostic", true  },
    { D_DEVICE,  "sensor",        "uptime",         "Last boot",           "boot_time",      "timestamp", nullptr,nullptr,      "diagnostic", false },
    { D_DEVICE,  "sensor",        "free_heap",      "Free heap",           "free_heap",      "data_size", "B",   "measurement", "diagnostic", false },
    { D_DEVICE,  "sensor",        "firmware",       "Firmware",            "version",        nullptr,     nullptr,nullptr,      "diagnostic", false },

    // ── Why the device last (re)booted, and whether it is still healthy ──────────────────────
    // These exist because a reboot used to be completely unattributable from outside the device:
    // esp_reset_reason() was read at boot and printed once, so a board that restarts weekly was
    // indistinguishable from a board that restarts weekly for a REASON.
    // The reason rides as BOTH a readable slug and a NUMBER on purpose: a metrics consumer keeps
    // numeric fields and drops strings, so the slug alone is invisible in exactly the store where
    // "how many panics this week" is the question worth asking.
    { D_DEVICE,  "sensor",        "reset_reason",   "Reset reason",        "reset_reason",   nullptr,     nullptr,nullptr,      "diagnostic", false },
    { D_DEVICE,  "sensor",        "reset_code",     "Reset reason code",   "reset_reason_code",nullptr,   nullptr,"measurement","diagnostic", false },
    { D_DEVICE,  "binary_sensor", "safe_mode",      "Safe mode",           "safe_mode",      "problem",   nullptr,nullptr,      "diagnostic", true  },
    { D_DEVICE,  "binary_sensor", "crash_dump",     "Crash dump waiting",  "crash_dump",     "problem",   nullptr,nullptr,      "diagnostic", true  },

    // ── The two heap numbers the firmware itself acts on ─────────────────────────────────────
    // free_heap above is the friendly one and the least useful: the binding limit on this chip is
    // the largest CONTIGUOUS block, which is what logic/heap_watchdog.hpp gates its restart on. The
    // two together are also what distinguishes a LEAK (both fall) from FRAGMENTATION (they
    // separate) — a distinction no single number can carry.
    { D_DEVICE,  "sensor",        "largest_block",  "Largest free block",  "largest_block",  "data_size", "B",   "measurement", "diagnostic", false },
    { D_DEVICE,  "sensor",        "min_free_heap",  "Min free heap",       "min_free_heap",  "data_size", "B",   "measurement", "diagnostic", false },

    // ── Link churn ───────────────────────────────────────────────────────────────────────────
    // Cumulative RE-connects since boot. A link that drops and recovers looks identical to a
    // healthy one in any instantaneous reading — connected is connected — so without a counter a
    // flapping AP or broker is invisible until someone happens to watch at the wrong second.
    { D_DEVICE,  "sensor",        "wifi_reconn",    "WiFi reconnects",     "wifi_reconnects","data_size", nullptr,"total_increasing","diagnostic", false },
    { D_DEVICE,  "sensor",        "mqtt_reconn",    "MQTT reconnects",     "mqtt_reconnects","data_size", nullptr,"total_increasing","diagnostic", false },
};

// ─── Publish helpers ──────────────────────────────────────────────────────────
static void pub(const std::string& topic, const char* payload, bool retain = true) {
    if (s_client) esp_mqtt_client_publish(s_client, topic.c_str(), payload, 0, 1, retain ? 1 : 0);
}

// Print + publish a cJSON object to a topic, then delete it (takes ownership).
static void pub_json(const std::string& topic, cJSON* obj) {
    CJsonPtr root(obj);
    if (!root) return;
    CJsonStringPtr json(cJSON_PrintUnformatted(root.get()));
    if (json) pub(topic, json.get());
}

// ─── HA discovery ─────────────────────────────────────────────────────────────
// Origin info: identifies the integration behind these entities. HA treats it as
// mandatory whenever a `device` block is present (a config that carries `dev` but no
// `o` is dropped — silently, at debug level — by HA 2026.x), so every entity gets it.
static void add_origin_block(cJSON* root) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", "tesla-key-esp32");
    cJSON_AddStringToObject(o, "sw",   esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "url",  "https://github.com/0Bu/tesla-key-esp32");
    cJSON_AddItemToObject(root, "o", o);
}

static void add_device_block(cJSON* root) {
    cJSON* dev = cJSON_CreateObject();
    cJSON* ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_node.c_str()));
    cJSON_AddItemToObject(dev, "ids", ids);
    cJSON_AddStringToObject(dev, "name", s_devname.c_str());
    cJSON_AddStringToObject(dev, "mf",   "tesla-key-esp32");
    cJSON_AddStringToObject(dev, "mdl",  TK_PLATFORM);
    cJSON_AddStringToObject(dev, "sw",   esp_app_get_description()->version);
    if (!s_cfgurl.empty()) cJSON_AddStringToObject(dev, "cu", s_cfgurl.c_str());
    cJSON_AddItemToObject(root, "dev", dev);
}

static void publish_discovery() {
    for (const Entry& e : ENTRIES) {
        std::string uid = s_node + "_" + e.obj;
        std::string value_template;
        if (e.is_binary) {
            const bool invert = e.dev_cla && strcmp(e.dev_cla, "lock") == 0;
            value_template = tk::ha_binary_value_template(e.field, invert);
        } else {
            value_template = std::string("{{ value_json.") + e.field + " }}";
        }
        std::string ct = s_prefix + "/" + e.comp + "/" + s_node + "/" + e.obj + "/config";

        cJSON* c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "name",    e.name);
        cJSON_AddStringToObject(c, "uniq_id", uid.c_str());
        cJSON_AddStringToObject(c, "stat_t",  s_topic[e.dom].c_str());
        cJSON_AddStringToObject(c, "avty_t",  s_avail.c_str());
        if (e.is_binary) {
            // HA's "lock" device_class is inverted vs every other binary here: it
            // renders ON as "Unlocked" and OFF as "Locked". Our `locked` field is
            // true=locked, so flip the template for that one class — a locked car
            // must read "Locked", not "Unlocked".
            // Presence-aware: an unreported optional field renders empty → HA "unknown", not a
            // phantom OFF/"Unlocked". Template built + host-tested in logic/ha_templates.hpp.
            cJSON_AddStringToObject(c, "val_tpl", value_template.c_str());
            cJSON_AddStringToObject(c, "pl_on",  "ON");
            cJSON_AddStringToObject(c, "pl_off", "OFF");
        } else {
            cJSON_AddStringToObject(c, "val_tpl", value_template.c_str());
        }
        if (e.dev_cla)  cJSON_AddStringToObject(c, "dev_cla",      e.dev_cla);
        if (e.unit)     cJSON_AddStringToObject(c, "unit_of_meas", e.unit);
        if (e.stat_cla) cJSON_AddStringToObject(c, "stat_cla",     e.stat_cla);
        if (e.ent_cat)  cJSON_AddStringToObject(c, "ent_cat",      e.ent_cat);
        add_origin_block(c);
        add_device_block(c);

        pub_json(ct, c);  // retained so HA recreates entities after a restart
    }
    ESP_LOGI(TAG, "published %d HA-discovery configs under %s/", (int)(sizeof(ENTRIES)/sizeof(ENTRIES[0])), s_prefix.c_str());
}

// ─── State publish ────────────────────────────────────────────────────────────
// Each domain is published only when its cache is valid, and each numeric field
// only when the car actually reported it (proto3-optional presence flags) — so a
// value the car never sent stays "unknown" in HA rather than reading a phantom 0.
static void publish_state() {
    if (!s_vehicle) return;

    // Charge
    {
        ChargeStateResult cs = s_vehicle->get_cached_charge();
        if (cs.valid) {
            cJSON* o = cJSON_CreateObject();
            // Emit each numeric field only when the car reported it (proto3 optional), so an
            // unseen value reads "unknown" in HA rather than a phantom 0.
            if (cs.has_battery_level)    cJSON_AddNumberToObject(o, "soc",          cs.battery_level);
            if (cs.has_charge_limit_soc) cJSON_AddNumberToObject(o, "charge_limit", cs.charge_limit_soc);
            if (cs.has_charger_power)    cJSON_AddNumberToObject(o, "power",        cs.charger_power);
            if (cs.has_charging_amps)    cJSON_AddNumberToObject(o, "amps",         cs.charging_amps);
            // Tesla reports these imperial; convert to metric for HA (the Tesla-compatible
            // /api path keeps miles for evcc). range: miles → km, rate: mph → km/h.
            if (cs.has_battery_range)    cJSON_AddNumberToObject(o, "range", tk::mi_to_km(cs.battery_range));
            if (cs.has_charge_rate)      cJSON_AddNumberToObject(o, "rate",  tk::mph_to_kmh(cs.charge_rate));
            if (!cs.charging_state.empty())
                cJSON_AddStringToObject(o, "charging_state", cs.charging_state.c_str());
            // Extended charge telemetry (read-only enrichment for HA; currents in A, energy in
            // kWh, time in minutes — all native units, no imperial conversion needed).
            if (cs.has_actual_current)   cJSON_AddNumberToObject(o, "actual_current",  cs.charger_actual_current);
            if (cs.has_voltage)          cJSON_AddNumberToObject(o, "volts",           cs.charger_voltage);
            if (cs.has_current_request)  cJSON_AddNumberToObject(o, "current_request", cs.charge_current_request);
            if (cs.has_charger_phases)   cJSON_AddNumberToObject(o, "phases",          cs.charger_phases);
            if (cs.has_energy_added)     cJSON_AddNumberToObject(o, "energy_added",     cs.charge_energy_added);
            if (cs.has_minutes_to_full)  cJSON_AddNumberToObject(o, "minutes_to_full",  cs.minutes_to_full_charge);
            if (!cs.charge_limit_reason.empty())
                cJSON_AddStringToObject(o, "limit_reason", cs.charge_limit_reason.c_str());
            pub_json(s_topic[D_CHARGE], o);
        }
    }
    // Climate
    {
        ClimateStateResult cl = s_vehicle->get_cached_climate();
        if (cl.valid) {
            cJSON* o = cJSON_CreateObject();
            if (cl.has_inside)   cJSON_AddNumberToObject(o, "inside",   cl.inside_temp);
            if (cl.has_outside)  cJSON_AddNumberToObject(o, "outside",  cl.outside_temp);
            if (cl.has_setpoint) cJSON_AddNumberToObject(o, "setpoint", cl.driver_setpoint);
            if (cl.has_climate_on)      cJSON_AddBoolToObject(o, "on",              cl.is_climate_on);
            if (cl.has_preconditioning) cJSON_AddBoolToObject(o, "preconditioning", cl.is_preconditioning);
            if (cl.has_cop)         cJSON_AddStringToObject(o, "cop",         cl.cop.c_str());
            if (cl.has_cop_cooling) cJSON_AddBoolToObject(o,   "cop_cooling", cl.cop_cooling);
            if (cl.has_cop_temp)    cJSON_AddStringToObject(o, "cop_temp",    cl.cop_temp.c_str());
            if (cl.has_cop_reason)  cJSON_AddStringToObject(o, "cop_reason",  cl.cop_reason.c_str());
            if (cl.has_front_defrost) cJSON_AddBoolToObject(o,   "front_defrost", cl.front_defrost);
            if (cl.has_rear_defrost)  cJSON_AddBoolToObject(o,   "rear_defrost",  cl.rear_defrost);
            if (cl.has_defrost_mode)  cJSON_AddStringToObject(o, "defrost_mode",  cl.defrost_mode.c_str());
            pub_json(s_topic[D_CLIMATE], o);
        }
    }
    // Drive
    {
        DriveStateResult dr = s_vehicle->get_cached_drive();
        if (dr.valid) {
            cJSON* o = cJSON_CreateObject();
            if (!dr.shift_state.empty()) cJSON_AddStringToObject(o, "shift", dr.shift_state.c_str());
            if (dr.has_odometer)         cJSON_AddNumberToObject(o, "odometer", dr.odometer_km);
            pub_json(s_topic[D_DRIVE], o);
        }
    }
    // Tires
    {
        TirePressureResult tp = s_vehicle->get_cached_tires();
        if (tp.valid) {
            cJSON* o = cJSON_CreateObject();
            if (tp.has_fl) cJSON_AddNumberToObject(o, "fl", tp.fl);
            if (tp.has_fr) cJSON_AddNumberToObject(o, "fr", tp.fr);
            if (tp.has_rl) cJSON_AddNumberToObject(o, "rl", tp.rl);
            if (tp.has_rr) cJSON_AddNumberToObject(o, "rr", tp.rr);
            cJSON_AddBoolToObject(o, "warn", tp.warn);
            pub_json(s_topic[D_TIRES], o);
        }
    }
    // Closures
    {
        ClosuresStateResult cz = s_vehicle->get_cached_closures();
        if (cz.valid) {
            cJSON* o = cJSON_CreateObject();
            if (cz.has_locked)       cJSON_AddBoolToObject(o, "locked", cz.locked);
            cJSON_AddBoolToObject(o, "door",   cz.any_door_open);
            cJSON_AddBoolToObject(o, "frunk",  cz.frunk_open);
            cJSON_AddBoolToObject(o, "trunk",  cz.trunk_open);
            cJSON_AddBoolToObject(o, "window", cz.any_window_open);
            if (cz.has_user_present) cJSON_AddBoolToObject(o, "user", cz.user_present);
            pub_json(s_topic[D_CLOSURES], o);
        }
    }
    // Vehicle reachability / sleep state — taken straight from VehicleController::link_state(),
    // the same source the web UI uses, so the two never drift. AWAKE = fresh live telemetry;
    // ASLEEP = no live data AND the car's VCSEC sleep flag has held ASLEEP long enough to be
    // proven (debounced past the COP flap); IDLE = reachable but not provably asleep yet (we
    // stopped polling to let the car sleep and the flag hasn't confirmed) — published as a
    // distinct value, never a phantom "ASLEEP"; UNREACHABLE = the car answers nothing over BLE
    // (driven off / out of range / deep sleep). Nothing heard since boot/re-pair ⇒ omit (HA
    // shows "unknown").
    {
        // Same mapping the web UI uses, from logic/link_state.hpp (host-tested) so the two
        // never drift. nullptr (Unknown) ⇒ omit the field (HA shows "unknown").
        const char* ss = tk::link_state_mqtt_str(s_vehicle->link_state());
        if (ss) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "sleep_status", ss);
            pub_json(s_topic[D_VEHICLE], o);
        }
    }
    // Device diagnostics
    {
        cJSON* o = cJSON_CreateObject();
        // WiFi signal — OMITTED, not zeroed, on a wired device: HA renders a missing value as
        // "unavailable", whereas a published 0 dBm would be read as an implausibly perfect link.
        int rssi = 0;
        if (tk::net_wifi_signal(&rssi, nullptr))
            cJSON_AddNumberToObject(o, "wifi_rssi", rssi);
        cJSON_AddBoolToObject(o, "ble_connected", s_vehicle->ble_connected());
        int8_t r = 0;
        if (s_vehicle->ble_rssi(r)) cJSON_AddNumberToObject(o, "ble_rssi", r);
        cJSON_AddBoolToObject(o,   "paired",    s_vehicle->has_session());
        // Boot time as an ISO-8601 timestamp → HA renders it as auto-scaling relative
        // time ("8 minutes ago" → "2 days ago"), so it's human-readable and each reboot
        // shows as a step change. Only emit once the wall clock is plausibly NTP-synced
        // (else the absolute time would be wrong); cached so it stays stable per boot.
        static time_t s_boot_epoch = 0;
        time_t now_ = time(nullptr);
        if (s_boot_epoch == 0 && now_ > 1600000000)
            s_boot_epoch = now_ - (time_t)(esp_timer_get_time() / 1000000);
        if (s_boot_epoch > 0) {
            struct tm tmv; gmtime_r(&s_boot_epoch, &tmv);
            char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
            cJSON_AddStringToObject(o, "boot_time", ts);
        }
        cJSON_AddNumberToObject(o, "free_heap", (double)esp_get_free_heap_size());
        cJSON_AddStringToObject(o, "version",   esp_app_get_description()->version);

        // INTERNAL caps for both, matching the heap watchdog and the /heap trend exactly: plain
        // 8BIT would report any PSRAM too and make such a board look permanently healthy.
        cJSON_AddNumberToObject(o, "largest_block",
                                (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(o, "min_free_heap",
                                (double)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

        const tk::CrashInfo ci = tk::diag_crash_info_live();
        cJSON_AddStringToObject(o, "reset_reason",      tk::reset_reason_slug(ci.reset_code));
        cJSON_AddNumberToObject(o, "reset_reason_code", (double)ci.reset_code);
        // "a dump for THIS build is downloadable right now" — re-read from flash per publish, since
        // GET /coredump?clear=1 can erase it mid-session and a latched `true` would leave HA
        // reporting a crash whose evidence is gone.
        cJSON_AddStringToObject(o, "crash_dump", ci.coredump ? "ON" : "OFF");
        cJSON_AddStringToObject(o, "safe_mode",  tk::safe_mode_active() ? "ON" : "OFF");

        cJSON_AddNumberToObject(o, "wifi_reconnects", (double)tk::net_reconnect_count());
        cJSON_AddNumberToObject(o, "mqtt_reconnects", (double)s_reconnects.load());
        pub_json(s_topic[D_DEVICE], o);
    }
}

// ─── MQTT event handler ───────────────────────────────────────────────────────
static void mqtt_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    // Runs on the esp-mqtt event-loop C task. Only atomic stores + logging here (no throwing
    // ops), but keep a final boundary so a future edit that allocates cannot unwind into the C
    // event loop → std::terminate → reboot (issue #204, C-callback boundary).
    try {
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        // Count RE-connects only: the first connect of a boot is not churn, and counting it would
        // put a permanent 1 on every healthy device and hide the difference from a real flap.
        static bool first = true;
        if (first) first = false; else s_reconnects.fetch_add(1);
        ESP_LOGI(TAG, "connected to broker%s", s_tls.load() ? " (TLS)" : "");
        s_connected = true;
        s_last_err  = ME_NONE;    // clear any prior failure now that we're up
        s_need_discovery = true;  // (re)announce discovery + state from the publisher task
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR: {
        // Record why the connection failed so /status can explain it. The common case for a
        // credentialed broker is a TLS handshake failure (untrusted cert / wrong port) — we do
        // NOT retry over plaintext, so the user needs to see the reason rather than a silent stall.
        auto* ev = static_cast<esp_mqtt_event_handle_t>(event_data);
        int code = ME_OTHER;
        if (ev && ev->error_handle) {
            switch (ev->error_handle->error_type) {
            case MQTT_ERROR_TYPE_TCP_TRANSPORT:     code = ME_TRANSPORT; break;
            case MQTT_ERROR_TYPE_CONNECTION_REFUSED: code = ME_REFUSED;  break;
            default: break;
            }
        }
        s_last_err = code;
        ESP_LOGE(TAG, "MQTT error (type %d)", ev && ev->error_handle ? ev->error_handle->error_type : -1);
        break;
    }
    default:
        break;
    }
    } catch (...) {
        ESP_LOGE(TAG, "mqtt event handler threw — ignored");
    }
}

// ─── Publisher task ───────────────────────────────────────────────────────────
// Owns all publishing (off the MQTT event-loop task). On (re)connect it pushes the
// discovery configs, the "online" availability, then an immediate state snapshot;
// thereafter it republishes state every s_interval_s.
static void publisher_task(void*) {
    TickType_t last = 0;
    const TickType_t interval = pdMS_TO_TICKS(s_interval_s * 1000);

    // Subscribe to the task watchdog: this task performs real, blocking network I/O (a publish over
    // a TLS socket to a broker that may be gone), and a wedge here is silent — HA simply stops
    // receiving, which looks exactly like a device that is off.
    if (esp_task_wdt_add(nullptr) != ESP_OK) {
        ESP_LOGW(TAG, "mqtt_pub could not subscribe to the task watchdog — a wedged publish will no "
                      "longer reboot the device automatically");
    }

    while (true) {
        // UNCONDITIONAL, at the top: not gated on s_connected or on a successful publish, so a long
        // broker outage — during which this task correctly does nothing — can never be mistaken for
        // a hang. What must trip the watchdog is a publish that never returns, not a broker that
        // never answers.
        esp_task_wdt_reset();

        // Iteration-boundary containment (issue #204): publish_discovery()/publish_state()
        // build cJSON + std::string discovery payloads that can throw std::bad_alloc on a
        // fragmented heap. An escape would unwind into the FreeRTOS C task trampoline →
        // std::terminate → reboot; contain it, skip this round, and try again next tick.
        bool discovery_in_progress = false;
        try {
            if (s_connected) {
                if (s_need_discovery.exchange(false)) {
                    discovery_in_progress = true;
                    publish_discovery();
                    pub(s_avail, "online");      // retained; after configs so HA has the entities
                    publish_state();
                    last = xTaskGetTickCount();
                } else if ((xTaskGetTickCount() - last) >= interval) {
                    publish_state();
                    last = xTaskGetTickCount();
                }
            }
        } catch (const std::exception& e) {
            if (discovery_in_progress) s_need_discovery = true;
            ESP_LOGE(TAG, "mqtt publish iteration threw (%s) — skipping round", e.what());
            vTaskDelay(pdMS_TO_TICKS(5000));
        } catch (...) {
            if (discovery_in_progress) s_need_discovery = true;
            ESP_LOGE(TAG, "mqtt publish iteration threw (unknown) — skipping round");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ─── Config helpers ───────────────────────────────────────────────────────────
// Display form "host:port": strip any "scheme://" and trailing path.
static std::string broker_display(const std::string& uri) {
    std::string s = uri;
    size_t p = s.find("://");
    if (p != std::string::npos) s = s.substr(p + 3);
    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    return s;
}

// ─── Public API ───────────────────────────────────────────────────────────────
static void mqtt_ha_cleanup_start_failure() noexcept {
    if (s_client) {
        if (s_client_started) esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    s_client_started = false;
    s_connected = false;
    s_need_discovery = false;
    s_configured.store(false);
}

static bool mqtt_ha_start_impl(VehicleController& vehicle,
                               NvsStorageAdapter& config_store) {
    s_vehicle = &vehicle;

    // Resolve broker URI: NVS "mqtt_uri" (web UI) overrides the Kconfig default. An
    // empty value (incl. an explicit "" stored to disable) leaves MQTT off.
    s_uri = CONFIG_TESLA_MQTT_BROKER_URI;
    config_store.load_str("mqtt_uri", s_uri);
    s_uri = tk::mqtt_trim(s_uri);

    if (s_uri.empty()) {
        ESP_LOGI(TAG, "MQTT disabled (no broker configured)");
        s_configured = false;
        return true;   // nothing to start is a healthy outcome, not a failure
    }
    s_user   = CONFIG_TESLA_MQTT_USERNAME;
    s_pass   = CONFIG_TESLA_MQTT_PASSWORD;
    s_prefix = CONFIG_TESLA_MQTT_DISCOVERY_PREFIX;

    // Scheme (credential-aware TLS default) — the rule lives in logic/mqtt_uri.hpp, because
    // /set_mqtt's save-time pre-flight has to dial the SAME URI this does. A probe that succeeded
    // against a differently-derived URI would be a green check for a broker the bridge never
    // connects to, which is worse than no check at all.
    s_uri = tk::mqtt_effective_uri(s_uri, !s_user.empty());
    s_tls = tk::mqtt_uri_is_tls(s_uri);
    s_broker_disp = broker_display(s_uri);
    s_interval_s = CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S;
    if (s_interval_s < 5) s_interval_s = 5;

    // Unique node id from the WiFi STA MAC (stable across VIN changes / reboots).
    // DELIBERATELY the WiFi MAC even on a wired device, where no WiFi link exists: this id is
    // baked into every Home Assistant entity id under this device. Deriving it from whichever
    // transport happens to be active would RENAME every entity the first time a board changed
    // transport — and ESP_MAC_WIFI_STA is an eFuse-backed base address that is readable whether
    // or not the radio was ever started, so it stays a stable identity rather than a link fact.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char node[24];
    snprintf(node, sizeof(node), "teslakey_%02x%02x%02x", mac[3], mac[4], mac[5]);
    s_node = node;

    std::string base_prefix = CONFIG_TESLA_MQTT_BASE_TOPIC;
    if (base_prefix.empty()) base_prefix = "tesla-key";
    s_base  = base_prefix + "/" + s_node;
    s_avail = s_base + "/availability";
    s_topic[D_CHARGE]   = s_base + "/charge";
    s_topic[D_CLIMATE]  = s_base + "/climate";
    s_topic[D_DRIVE]    = s_base + "/drive";
    s_topic[D_TIRES]    = s_base + "/tires";
    s_topic[D_CLOSURES] = s_base + "/closures";
    s_topic[D_VEHICLE]  = s_base + "/vehicle";
    s_topic[D_DEVICE]   = s_base + "/device";

    // Device display name + a clickable link back to this device's web UI.
    const std::string& vin = vehicle.vin();
    s_devname = "Tesla Key";
    if (vin.size() == 17) s_devname += " (" + vin + ")";
    esp_netif_t* netif = tk::net_active_netif();
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        char ipbuf[16]; esp_ip4addr_ntoa(&ip.ip, ipbuf, sizeof(ipbuf));
        s_cfgurl = std::string("http://") + ipbuf;
    }

    // esp-mqtt v5 nested config struct. LWT marks us "offline" if the link drops.
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri          = s_uri.c_str();
    // For mqtts:// verify the broker certificate against the bundled CA roots (same trust store
    // as OTA). A broker presenting an untrusted/self-signed cert fails the handshake and the
    // bridge stays disconnected with an error in /status — we never downgrade to plaintext.
    if (s_tls) cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    if (!s_user.empty()) cfg.credentials.username = s_user.c_str();
    if (!s_pass.empty()) cfg.credentials.authentication.password = s_pass.c_str();
    cfg.session.last_will.topic     = s_avail.c_str();
    cfg.session.last_will.msg       = "offline";
    cfg.session.last_will.qos       = 1;
    cfg.session.last_will.retain    = 1;
    cfg.session.keepalive           = 30;

    // Check every resource; on any failure unwind the partially-created client and fall back to
    // the disabled state so the public status honestly reports not-configured/not-connected (an
    // OPTIONAL subsystem must degrade visibly, never half-run).
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed — MQTT bridge disabled (degraded)");
        s_configured.store(false);
        return false;
    }
    if (esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_register_event failed — MQTT bridge disabled (degraded)");
        mqtt_ha_cleanup_start_failure();
        return false;
    }
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed — MQTT bridge disabled (degraded)");
        mqtt_ha_cleanup_start_failure();
        return false;
    }
    s_client_started = true;
    if (xTaskCreate(publisher_task, "mqtt_pub", 6144, nullptr, tk::kPrioMqttPub, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "publisher task creation failed — MQTT bridge disabled (degraded)");
        mqtt_ha_cleanup_start_failure();
        return false;
    }

    // Release-publish the immutable topic/configuration strings only after the complete bridge
    // is live. Concurrent HTTP readers see disabled defaults until this point.
    s_configured.store(true);
    ESP_LOGI(TAG, "MQTT bridge started → %s (base topic %s, HA prefix %s)",
             s_broker_disp.c_str(), s_base.c_str(), s_prefix.c_str());
    return true;
}

bool mqtt_ha_start(VehicleController& vehicle, NvsStorageAdapter& config_store) {
    try {
        return mqtt_ha_start_impl(vehicle, config_store);
    } catch (const std::exception& e) {
        mqtt_ha_cleanup_start_failure();
        ESP_LOGE(TAG, "MQTT initialization threw (%s); bridge disabled", e.what());
    } catch (...) {
        mqtt_ha_cleanup_start_failure();
        ESP_LOGE(TAG, "MQTT initialization threw (unknown); bridge disabled");
    }
    return false;
}

bool mqtt_ha_configured() { return s_configured.load(); }
bool mqtt_ha_connected()  { return s_configured.load() && s_connected.load(); }
bool mqtt_ha_tls()        { return s_configured.load() && s_tls.load(); }
std::string mqtt_ha_broker() { return s_configured.load() ? s_broker_disp : std::string{}; }

// Human-readable last connection error ("" when none / connected). Surfaced in /status.
std::string mqtt_ha_last_error() {
    if (!s_configured.load() || s_connected.load()) return {};
    switch (s_last_err.load()) {
    case ME_TRANSPORT: return s_tls.load() ? "TLS handshake failed (untrusted cert or wrong port?)"
                                    : "transport error (host/port?)";
    case ME_REFUSED:   return "broker refused connection (credentials?)";
    case ME_OTHER:     return "connection error";
    default:           return {};
    }
}
