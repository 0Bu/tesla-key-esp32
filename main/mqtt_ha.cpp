#include "mqtt_ha.hpp"
#include "config_blob.hpp"
#include "net.hpp"
#include "vehicle_ctrl.hpp"
#include "nvs_storage.hpp"
#include "platform.hpp"
#include "logic/ha_identity.hpp"
#include "logic/mqtt_discovery_registry.hpp"
#include "logic/units.hpp"
#include "logic/link_state.hpp"
#include "task_config.hpp"
#include "logic/mqtt_uri.hpp"
#include "diag_crash.hpp"
#include "mqtt_json_publish.hpp"
#include "mqtt_payloads.hpp"
#include "mqtt_publish_sequence.hpp"
#include "safe_mode.hpp"
#include "stack_watch.hpp"
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <atomic>
#include <array>
#include <string>
#include <ctime>
#include <exception>

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
static std::string s_node;                     // vehicle-stable node id (teslakey_<vin>)
static std::string s_base;                     // state-topic base (<base_prefix>/<node>)
static std::string s_avail;                    // availability (LWT) topic
static std::string s_devname;                  // HA device display name
static std::string s_cfgurl;                   // device configuration_url (http://<ip>)
static std::string s_broker_disp;              // "host:port" for the web UI
static int         s_interval_s = 15;
static bool        s_client_started = false;

// State topics are indexed by the hardware-free production registry's domain enum.
static std::array<std::string, tk::mqtt::kStateDomainCount> s_topic;

static const std::string& state_topic(tk::mqtt::StateDomain domain) {
    return s_topic[tk::mqtt::state_domain_index(domain)];
}

// ─── Publish helpers ──────────────────────────────────────────────────────────
static bool pub(const char* topic, const char* payload, bool retain = true) {
    if (!s_client || !topic || !payload) return false;
    const int message_id =
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain ? 1 : 0);
    // Every caller runs on mqtt_pub. Feeding only AFTER the blocking call returns preserves the
    // watchdog's ability to catch a wedged socket while proving progress through a long discovery
    // burst, whose many individually-completed publishes may legitimately outlive one 60 s cycle.
    esp_task_wdt_reset();
    return message_id >= 0;
}

// Print the complete object before touching a retained topic. The tested seam owns and frees the
// tree and print buffer on every failure; a nullptr tree or print OOM is therefore a hard failure,
// never a partial retained overwrite.
static bool pub_json(const std::string& topic, tk::JsonOwner root) {
    return tk::mqtt_publish_json(
        topic.c_str(), std::move(root), true,
        [](const char* publish_topic, const char* payload, bool retain) {
            return pub(publish_topic, payload, retain);
        });
}

// ─── HA discovery ─────────────────────────────────────────────────────────────
static bool publish_discovery() {
    for (const tk::mqtt::DiscoveryEntry& entry : tk::mqtt::kDiscoveryEntries) {
        const std::string uid = tk::mqtt::discovery_unique_id(s_node, entry);
        const std::string value_template = tk::mqtt::discovery_value_template(entry);
        const std::string config_topic =
            tk::mqtt::discovery_config_topic(s_prefix, s_node, entry);

        // HA's "lock" device_class is inverted vs every other binary here: it renders ON as
        // "Unlocked" and OFF as "Locked". The host-tested template already applies that inversion
        // and its presence guard; the shared emitter below is the exact production serializer.
        const tk::mqtt::DiscoveryPayload payload{
            entry.name.data(),
            uid.c_str(),
            state_topic(entry.domain).c_str(),
            s_avail.c_str(),
            value_template.c_str(),
            entry.device_class.empty() ? nullptr : entry.device_class.data(),
            entry.unit.empty() ? nullptr : entry.unit.data(),
            entry.state_class.empty() ? nullptr : entry.state_class.data(),
            entry.entity_category.empty() ? nullptr : entry.entity_category.data(),
            tk::mqtt::discovery_is_binary(entry),
            s_node.c_str(),
            s_devname.c_str(),
            TK_PLATFORM,
            esp_app_get_description()->version,
            s_cfgurl.empty() ? nullptr : s_cfgurl.c_str(),
        };

        // Retained so HA recreates entities after a restart. A failed builder, print, or broker
        // enqueue stops the round before availability and is retried as a complete discovery pass.
        if (!pub_json(config_topic, tk::mqtt::build_discovery_payload(payload))) return false;
    }
    ESP_LOGI(TAG, "published %d HA-discovery configs under %s/",
             static_cast<int>(tk::mqtt::kDiscoveryEntries.size()), s_prefix.c_str());
    return true;
}

// ─── State publish ────────────────────────────────────────────────────────────
// Each domain is published only when its cache is valid, and each numeric field
// only when the car actually reported it (proto3-optional presence flags) — so a
// value the car never sent stays "unknown" in HA rather than reading a phantom 0.
static bool publish_state() {
    if (!s_vehicle) return false;

    // Charge
    {
        ChargeStateResult cs = s_vehicle->get_cached_charge();
        if (cs.valid) {
            tk::mqtt::ChargePayload payload;
            payload.has_battery_level = cs.has_battery_level;
            payload.battery_level = cs.battery_level;
            payload.has_charge_limit = cs.has_charge_limit_soc;
            payload.charge_limit = cs.charge_limit_soc;
            payload.has_power = cs.has_charger_power;
            payload.power = cs.charger_power;
            payload.has_amps = cs.has_charging_amps;
            payload.amps = cs.charging_amps;
            // Tesla reports these imperial; convert to metric for HA (the Tesla-compatible
            // /api path keeps miles for evcc). range: miles → km, rate: mph → km/h.
            payload.has_range = cs.has_battery_range;
            payload.range_km = tk::mi_to_km(cs.battery_range);
            payload.has_rate = cs.has_charge_rate;
            payload.rate_kmh = tk::mph_to_kmh(cs.charge_rate);
            payload.charging_state =
                cs.charging_state.empty() ? nullptr : cs.charging_state.c_str();
            // Extended charge telemetry (read-only enrichment for HA; currents in A, energy in
            // kWh, time in minutes — all native units, no imperial conversion needed).
            payload.has_actual_current = cs.has_actual_current;
            payload.actual_current = cs.charger_actual_current;
            payload.has_voltage = cs.has_voltage;
            payload.voltage = cs.charger_voltage;
            payload.has_current_request = cs.has_current_request;
            payload.current_request = cs.charge_current_request;
            payload.has_phases = cs.has_charger_phases;
            payload.phases = cs.charger_phases;
            payload.has_energy_added = cs.has_energy_added;
            payload.energy_added = cs.charge_energy_added;
            payload.has_minutes_to_full = cs.has_minutes_to_full;
            payload.minutes_to_full = cs.minutes_to_full_charge;
            payload.limit_reason =
                cs.charge_limit_reason.empty() ? nullptr : cs.charge_limit_reason.c_str();
            if (!pub_json(state_topic(tk::mqtt::StateDomain::Charge),
                          tk::mqtt::build_charge_payload(payload)))
                return false;
        }
    }
    // Climate
    {
        ClimateStateResult cl = s_vehicle->get_cached_climate();
        if (cl.valid) {
            tk::mqtt::ClimatePayload payload;
            payload.has_inside = cl.has_inside;
            payload.inside = cl.inside_temp;
            payload.has_outside = cl.has_outside;
            payload.outside = cl.outside_temp;
            payload.has_setpoint = cl.has_setpoint;
            payload.setpoint = cl.driver_setpoint;
            payload.has_climate_on = cl.has_climate_on;
            payload.climate_on = cl.is_climate_on;
            payload.has_preconditioning = cl.has_preconditioning;
            payload.preconditioning = cl.is_preconditioning;
            payload.has_cop = cl.has_cop;
            payload.cop = cl.cop.c_str();
            payload.has_cop_cooling = cl.has_cop_cooling;
            payload.cop_cooling = cl.cop_cooling;
            payload.has_cop_temp = cl.has_cop_temp;
            payload.cop_temp = cl.cop_temp.c_str();
            payload.has_cop_reason = cl.has_cop_reason;
            payload.cop_reason = cl.cop_reason.c_str();
            payload.has_front_defrost = cl.has_front_defrost;
            payload.front_defrost = cl.front_defrost;
            payload.has_rear_defrost = cl.has_rear_defrost;
            payload.rear_defrost = cl.rear_defrost;
            payload.has_defrost_mode = cl.has_defrost_mode;
            payload.defrost_mode = cl.defrost_mode.c_str();
            if (!pub_json(state_topic(tk::mqtt::StateDomain::Climate),
                          tk::mqtt::build_climate_payload(payload)))
                return false;
        }
    }
    // Drive
    {
        DriveStateResult dr = s_vehicle->get_cached_drive();
        if (dr.valid) {
            const tk::mqtt::DrivePayload payload{
                dr.shift_state.empty() ? nullptr : dr.shift_state.c_str(),
                dr.has_odometer,
                dr.odometer_km,
            };
            if (!pub_json(state_topic(tk::mqtt::StateDomain::Drive),
                          tk::mqtt::build_drive_payload(payload)))
                return false;
        }
    }
    // Tires
    {
        TirePressureResult tp = s_vehicle->get_cached_tires();
        if (tp.valid) {
            const tk::mqtt::TiresPayload payload{
                tp.has_fl, tp.fl,
                tp.has_fr, tp.fr,
                tp.has_rl, tp.rl,
                tp.has_rr, tp.rr,
                tp.warn,
            };
            if (!pub_json(state_topic(tk::mqtt::StateDomain::Tires),
                          tk::mqtt::build_tires_payload(payload)))
                return false;
        }
    }
    // Closures
    {
        ClosuresStateResult cz = s_vehicle->get_cached_closures();
        if (cz.valid) {
            const tk::mqtt::ClosuresPayload payload{
                cz.has_locked,
                cz.locked,
                cz.any_door_open,
                cz.frunk_open,
                cz.trunk_open,
                cz.any_window_open,
                cz.has_user_present,
                cz.user_present,
            };
            if (!pub_json(state_topic(tk::mqtt::StateDomain::Closures),
                          tk::mqtt::build_closures_payload(payload)))
                return false;
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
        if (ss && !pub_json(state_topic(tk::mqtt::StateDomain::Vehicle),
                            tk::mqtt::build_vehicle_payload(ss)))
            return false;
    }
    // Device diagnostics
    {
        tk::mqtt::DevicePayload payload;
        // WiFi signal — OMITTED, not zeroed, on a wired device: HA renders a missing value as
        // "unavailable", whereas a published 0 dBm would be read as an implausibly perfect link.
        int rssi = 0;
        payload.has_wifi_rssi = tk::net_wifi_signal(&rssi, nullptr);
        payload.wifi_rssi = rssi;
        payload.ble_connected = s_vehicle->ble_connected();
        int8_t r = 0;
        payload.has_ble_rssi = s_vehicle->ble_rssi(r);
        payload.ble_rssi = r;
        payload.paired = s_vehicle->has_session();
        // Boot time as an ISO-8601 timestamp → HA renders it as auto-scaling relative
        // time ("8 minutes ago" → "2 days ago"), so it's human-readable and each reboot
        // shows as a step change. Only emit once the wall clock is plausibly NTP-synced
        // (else the absolute time would be wrong); cached so it stays stable per boot.
        static time_t s_boot_epoch = 0;
        time_t now_ = time(nullptr);
        if (s_boot_epoch == 0 && now_ > 1600000000)
            s_boot_epoch = now_ - (time_t)(esp_timer_get_time() / 1000000);
        char boot_time[32] = {};
        if (s_boot_epoch > 0) {
            struct tm tmv; gmtime_r(&s_boot_epoch, &tmv);
            strftime(boot_time, sizeof(boot_time), "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
            payload.boot_time = boot_time;
        }
        payload.free_heap = (double)esp_get_free_heap_size();
        payload.version = esp_app_get_description()->version;

        // INTERNAL caps for both, matching the heap watchdog and the /heap trend exactly: plain
        // 8BIT would report any PSRAM too and make such a board look permanently healthy.
        payload.largest_block =
            (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        payload.min_free_heap =
            (double)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

        const tk::CrashInfo ci = tk::diag_crash_info_live();
        payload.reset_reason = tk::reset_reason_slug(ci.reset_code);
        payload.reset_reason_code = (double)ci.reset_code;
        // "a dump for THIS build is downloadable right now" — re-read from flash per publish, since
        // GET /coredump?clear=1 can erase it mid-session and a latched `true` would leave HA
        // reporting a crash whose evidence is gone.
        payload.crash_dump = ci.coredump;
        payload.safe_mode = tk::safe_mode_active();

        payload.wifi_reconnects = (double)tk::net_reconnect_count();
        payload.mqtt_reconnects = (double)s_reconnects.load();
        // Payload-only fleet diagnostics (no extra HA entities). Omit an unsampled task rather than
        // publishing zero bytes free for a task that has not run or is absent in safe mode.
        const auto httpd_stack = tk::stack_watch_min_free_bytes(tk::StackWatch::Httpd);
        const auto vehicle_stack = tk::stack_watch_min_free_bytes(tk::StackWatch::Vehicle);
        const auto auto_pair_stack = tk::stack_watch_min_free_bytes(tk::StackWatch::AutoPair);
        const auto mqtt_stack = tk::stack_watch_min_free_bytes(tk::StackWatch::Mqtt);
        payload.has_httpd_stack = httpd_stack.has_value();
        if (httpd_stack) payload.httpd_stack = *httpd_stack;
        payload.has_vehicle_stack = vehicle_stack.has_value();
        if (vehicle_stack) payload.vehicle_stack = *vehicle_stack;
        payload.has_auto_pair_stack = auto_pair_stack.has_value();
        if (auto_pair_stack) payload.auto_pair_stack = *auto_pair_stack;
        payload.has_mqtt_stack = mqtt_stack.has_value();
        if (mqtt_stack) payload.mqtt_stack = *mqtt_stack;
        if (!pub_json(state_topic(tk::mqtt::StateDomain::Device),
                      tk::mqtt::build_device_payload(payload)))
            return false;
    }
    return true;
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
  try {
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
        // Same retrospective placement as vehicle_loop: every branch reaches it, including broker
        // outage and OOM recovery cycles, and the MQTT snapshot below reads this task's own mark.
        tk::stack_watch_sample(tk::StackWatch::Mqtt);

        // Iteration-boundary containment (issue #204): publish_discovery()/publish_state()
        // build cJSON + std::string discovery payloads that can throw std::bad_alloc on a
        // fragmented heap. An escape would unwind into the FreeRTOS C task trampoline →
        // std::terminate → reboot; contain it, skip this round, and try again next tick.
        try {
            if (s_connected) {
                if (s_need_discovery.exchange(false)) {
                    const bool success = tk::mqtt_run_discovery_round(
                        [] { return publish_discovery(); },
                        [] {
                            // Retained and strictly after every discovery config, so HA never sees
                            // an online bridge whose entity set failed to build or enqueue.
                            return pub(s_avail.c_str(), "online");
                        },
                        [] { return publish_state(); },
                        [] { s_need_discovery = true; });
                    if (!success) {
                        ESP_LOGE(TAG, "MQTT discovery publish failed — retrying complete round");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    } else {
                        last = xTaskGetTickCount();
                    }
                } else if ((xTaskGetTickCount() - last) >= interval) {
                    const bool success = tk::mqtt_run_state_round(
                        [] { return publish_state(); },
                        [] { s_need_discovery = true; });
                    if (!success) {
                        // Re-run discovery too: a state build/print/enqueue failure is evidence that
                        // the retained MQTT surface may be incomplete, not merely a skipped tick.
                        ESP_LOGE(TAG, "MQTT state publish failed — rearming discovery");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    } else {
                        last = xTaskGetTickCount();
                    }
                }
            }
        } catch (const std::exception& e) {
            s_need_discovery = true;
            ESP_LOGE(TAG, "mqtt publish iteration threw (%s) — skipping round", e.what());
            vTaskDelay(pdMS_TO_TICKS(5000));
        } catch (...) {
            s_need_discovery = true;
            ESP_LOGE(TAG, "mqtt publish iteration threw (unknown) — skipping round");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
  } catch (const std::exception& e) {
      s_need_discovery = true;
      ESP_LOGE(TAG, "mqtt publisher boundary threw (%s) — stopping task", e.what());
      vTaskDelete(nullptr);
  } catch (...) {
      s_need_discovery = true;
      ESP_LOGE(TAG, "mqtt publisher boundary threw (unknown) — stopping task");
      vTaskDelete(nullptr);
  }
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
    tk::ConfigBlob stored_cfg;
    tk::cfg_load(config_store, stored_cfg);
    s_uri = stored_cfg.mqtt_uri;
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
    // Status, UI defaults and logs receive only the credential-free authority. Keep s_uri intact
    // for the client itself: removing userinfo there would break authentication.
    s_broker_disp = tk::mqtt_broker_display(s_uri);
    s_interval_s = CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S;
    if (s_interval_s < 5) s_interval_s = 5;

    // Every HA identity surface must survive replacement of the ESP32 board: discovery topic,
    // state topic, unique_id and device identifier all derive from s_node. The VIN identifies the
    // vehicle those entities describe and is already strictly validated before pairing. Using the
    // board's eFuse MAC here created a second 55-entity device on every board replacement.
    const std::string& vin = vehicle.vin();
    s_node = tk::ha_node_id_from_vin(vin);
    if (s_node.empty()) {
        // Setup-mode fallback only. A configured vehicle always has a plausible VIN, but MQTT is
        // optional and may have been provisioned first. Do not collapse all such devices onto one
        // shared node while the VIN is absent; /set_vin reboots and selects the stable identity.
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char node[24];
        snprintf(node, sizeof(node), "teslakey_%02x%02x%02x", mac[3], mac[4], mac[5]);
        s_node = node;
        ESP_LOGW(TAG, "plausible VIN unavailable — HA identity temporarily uses this board");
    }

    std::string base_prefix = CONFIG_TESLA_MQTT_BASE_TOPIC;
    if (base_prefix.empty()) base_prefix = "tesla-key";
    s_base  = base_prefix + "/" + s_node;
    s_avail = s_base + "/availability";
    for (size_t i = 0; i < tk::mqtt::kStateDomainCount; ++i) {
        const auto domain = static_cast<tk::mqtt::StateDomain>(i);
        s_topic[i] = tk::mqtt::discovery_state_topic(s_base, domain);
    }

    // Device display name + a clickable link back to this device's web UI.
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
