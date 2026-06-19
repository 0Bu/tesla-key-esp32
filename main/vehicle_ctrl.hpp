#pragma once

#include <vehicle.h>
#include <client.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <ctime>
#include "ble_client.hpp"
#include "nvs_storage.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct ChargeStateResult {
    bool valid{false};
    float       battery_level{0};
    float       charge_limit_soc{0};
    std::string charging_state;
    float       charger_power{0};
    float       charge_rate{0};
    int         charging_amps{0};
    float       battery_range{0};
};

struct VehicleStatusResult {
    bool valid{false};
    std::string lock_state;
    std::string sleep_status;
    std::string user_presence;
};

// ─── Read-only telemetry (refreshed in the background, shown in the web UI) ──────
// Each carries presence flags for the numeric fields because the car omits values
// it has no reading for (proto3 optional); a missing field must render as "—",
// not as 0.
struct ClimateStateResult {
    bool valid{false};
    bool is_climate_on{false};
    bool is_preconditioning{false};
    bool  has_inside{false};   float inside_temp{0};      // °C
    bool  has_outside{false};  float outside_temp{0};     // °C
    bool  has_setpoint{false}; float driver_setpoint{0};  // °C
};

struct DriveStateResult {
    bool valid{false};
    std::string shift_state;          // "P"/"R"/"N"/"D" or "" if unknown
    bool  has_odometer{false}; float odometer_km{0};
};

struct TirePressureResult {
    bool valid{false};
    bool  has_fl{false}; float fl{0};   // bar
    bool  has_fr{false}; float fr{0};
    bool  has_rl{false}; float rl{0};
    bool  has_rr{false}; float rr{0};
    bool  warn{false};                  // any soft/hard TPMS warning set
};

struct ClosuresStateResult {
    bool valid{false};
    bool has_locked{false};       bool locked{false};
    bool any_door_open{false};
    bool frunk_open{false};
    bool trunk_open{false};
    bool any_window_open{false};
    bool has_user_present{false}; bool user_present{false};
};

class VehicleController {
public:
    VehicleController() = default;

    // All references must remain alive for the lifetime of this object.
    // known_mac: if empty, MAC is discovered via scan and saved to config_store.
    bool init(const std::string& vin, BleClient& ble, NvsStorageAdapter& storage,
              NvsStorageAdapter& config_store, std::string& known_mac);

    bool wake_up(int timeout_ms = 20000);
    bool charge_start(int timeout_ms = 20000);
    bool charge_stop(int timeout_ms = 20000);
    bool set_charging_amps(int amps, int timeout_ms = 20000);
    bool set_charge_limit(int percent, int timeout_ms = 20000);
    bool charge_port_open(int timeout_ms = 20000);
    bool charge_port_close(int timeout_ms = 20000);
    bool door_lock(int timeout_ms = 20000);
    bool door_unlock(int timeout_ms = 20000);
    bool flash_lights(int timeout_ms = 20000);
    bool honk_horn(int timeout_ms = 20000);
    bool set_sentry_mode(bool enable, int timeout_ms = 20000);
    bool climate_start(int timeout_ms = 20000);
    bool climate_stop(int timeout_ms = 20000);
    // Scheduled charging: enable/disable a daily start time. start_minutes is minutes
    // after local midnight (0–1439; e.g. 23:00 → 1380), ignored when enable is false.
    // (Scheduled *departure* is not exposed: the tesla-ble version in use registers no
    // builder for scheduledDepartureAction, so it cannot be sent.)
    bool set_scheduled_charging(bool enable, int start_minutes, int timeout_ms = 20000);

    bool get_charge_state(ChargeStateResult& out, int timeout_ms = 20000);
    bool get_vehicle_status(VehicleStatusResult& out, int timeout_ms = 20000);

    // Non-blocking accessors for cached state (refreshed in background; copied under
    // cache_mutex_ because the BLE RX task writes these concurrently — see cache_mutex_).
    ChargeStateResult   get_cached_charge()   { return copy_locked_(last_known_charge_); }
    VehicleStatusResult get_cached_status()   { return copy_locked_(last_known_status_); }
    ClimateStateResult  get_cached_climate()  { return copy_locked_(last_known_climate_); }
    DriveStateResult    get_cached_drive()    { return copy_locked_(last_known_drive_); }
    TirePressureResult  get_cached_tires()    { return copy_locked_(last_known_tires_); }
    ClosuresStateResult get_cached_closures() { return copy_locked_(last_known_closures_); }

    bool generate_key();
    // Always enrolls a Charging Manager key (charging + wake only); never an owner key.
    bool pair(int timeout_ms = 30000);

    // Re-point the device at a different vehicle (VIN change): regenerate the key,
    // drop the now-orphaned session + cached data, and forget the discovered BLE MAC
    // (it belongs to the old car). The caller is expected to reboot afterwards so the
    // new VIN takes effect. Safe to call regardless of current pairing state.
    bool reset_for_new_vehicle();

    const std::string& vin() const { return vin_; }
    TeslaBLE::Vehicle* vehicle() { return vehicle_.get(); }

    // Status accessors (for /status and the web UI)
    bool ble_connected() const { return ble_ && ble_->is_connected(); }
    // Nearby Teslas seen while scanning (when not connected); RSSI of the live link.
    std::vector<TeslaScan> ble_nearby() const {
        return ble_ ? ble_->nearby() : std::vector<TeslaScan>{};
    }
    bool ble_rssi(int8_t& out) const { return ble_ && ble_->connected_rssi(out); }
    std::string ble_peer() const { return ble_ ? ble_->peer_addr_str() : std::string{}; }
    void ble_scan(int ms = 12000) { if (ble_) ble_->start_discovery(ms); }
    bool ble_scanning() const { return ble_ && ble_->is_scanning(); }
    bool has_key();      // a private key exists in NVS
    bool has_session();  // a VCSEC session exists in NVS (i.e. paired & handshaked)
    // True after a pairing was *lost* (key deleted on the car side) and a re-pair is
    // pending. Lets the UI explain why it's asking to pair again rather than showing
    // the generic first-time prompt. Cleared once a fresh session is established.
    bool reauth_required() const { return repair_notice_; }
    // Tesla public-key id of the stored key (SHA-1(pubkey)[:4], "AB:CD:EF:01"),
    // matching the key list shown on the vehicle. Empty if no key is present.
    std::string key_fingerprint();
    // Epoch seconds when the stored key was generated, or 0 if unknown (no key,
    // or the clock had not yet synced at generation time).
    time_t key_created_at();
    // Epoch seconds when the current pairing (VCSEC session) was first established,
    // or 0 if not paired / unknown (paired before this was tracked, or clock unsynced).
    // Lazily stamped the first time we hold a session and the wall clock is valid.
    time_t paired_at();
    // Reason the most recent command failed (e.g. "complete", "not_charging"), or empty
    // if it succeeded or got no response at all (car unreachable / timed out). Lets the
    // UI tell "the car rejected this" apart from "the car couldn't be reached".
    std::string last_command_error() const { return last_error_; }

    // Persist a new VIN to the config store (takes effect after reboot).
    bool save_config_vin(const std::string& vin) {
        return config_store_ ? config_store_->save_str("vin", vin) : false;
    }

    // Persist an arbitrary runtime-config string (tesla_cfg namespace). Used by the
    // web UI to store the MQTT broker ("mqtt_uri"); applied on the next boot. Keys
    // must be ≤15 chars (NVS limit). An empty value disables the feature it gates.
    bool save_config_str(const char* key, const std::string& value) {
        return config_store_ ? config_store_->save_str(key, value) : false;
    }

    // Cache the last-known wall clock (epoch seconds). The device has no
    // battery-backed RTC and deliberately makes no NTP call; the browser sets the
    // clock via POST /set_time and we persist it so a headless reboot (evcc only,
    // no browser visit) still comes up with a plausible time for TLS certificate
    // validation (OTA) and tesla-ble session-freshness checks. main.cpp restores
    // it on boot by reading the same "last_time" key from the config store.
    bool save_config_time(long long epoch) {
        return config_store_ ? config_store_->save_str("last_time", std::to_string(epoch)) : false;
    }

private:
    // Builder function type used by send_command_result
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    using ResultCb = TeslaBLE::Command::OperationResultCallback;

    bool ensure_connected_(int timeout_ms = 10000);

    // Drop the BLE link, reset the library's in-memory peer sessions, erase the
    // persisted VCSEC/Infotainment sessions, and clear cached vehicle readings.
    // After this has_session() is false until a fresh handshake re-pairs, so the UI
    // and evcc stop serving stale "paired"/SOC data from a defunct pairing.
    void clear_session_and_cache_();

    // Signed VCSEC GET_STATUS poll used purely to detect that our key was deleted on
    // the car side (the response then carries KEY_NOT_ON_WHITELIST, which trips
    // pairing_lost_ via make_result_cb_). Benign/read-only; does not wake the car.
    bool health_probe_(int timeout_ms = 8000);

    // Enqueue an Infotainment or VCSEC command and wait for the callback.
    bool send_vcsec_(const std::string& name, Builder builder,
                     TeslaBLE::WakePolicy wp, int timeout_ms,
                     bool count_as_activity = true);
    bool send_infotainment_(const std::string& name, Builder builder, int timeout_ms,
                            TeslaBLE::WakePolicy wp = TeslaBLE::WakePolicy::WAKE_IF_NEEDED);

    // RAII helper: takes cmd_sem_ when done
    ResultCb make_result_cb_();

    // Copy a background-refreshed cache under cache_mutex_ (see cache_mutex_ below). The
    // caches hold std::string members written from the BLE RX task; an unlocked by-value
    // read races the writer (torn string → UB), so all reads/writes take this mutex.
    template <typename T> T copy_locked_(const T& src) {
        if (!cache_mutex_) return src;
        xSemaphoreTake(cache_mutex_, portMAX_DELAY);
        T copy = src;
        xSemaphoreGive(cache_mutex_);
        return copy;
    }

    BleClient*         ble_{nullptr};
    NvsStorageAdapter* storage_{nullptr};
    NvsStorageAdapter* config_store_{nullptr};
    std::string*       known_mac_{nullptr};
    std::string        vin_;

    std::unique_ptr<TeslaBLE::Vehicle> vehicle_;

    SemaphoreHandle_t cmd_sem_{nullptr};
    SemaphoreHandle_t vehicle_mutex_{nullptr};
    // Serializes a whole command/query cycle so concurrent HTTP requests
    // (e.g. evcc poll + a manual command) cannot share cmd_sem_/last_result_.
    SemaphoreHandle_t command_mutex_{nullptr};
    // Guards the last_known_* caches below: they hold std::string members written from
    // the BLE RX task (parse_* callbacks) and read by the HTTP task, so an unlocked
    // by-value copy would race the writer (torn string → UB).
    SemaphoreHandle_t cache_mutex_{nullptr};
    bool              last_result_{false};
    // Failure text from the most recent signed command (the Tesla "...action failed:
    // <reason>" message), or empty after a success / when no response came back. Lets
    // the HTTP layer report the real reason (e.g. "complete") instead of a generic one.
    // Written in make_result_cb_ (BLE RX task) before cmd_sem_ is given; read by the
    // HTTP task only after it takes cmd_sem_, so the semaphore orders the access.
    std::string       last_error_;

    // Set from a command callback (possibly the BLE RX task) when the vehicle reports
    // KEY_NOT_ON_WHITELIST — i.e. our key was removed on the car side. The auto-pair
    // supervisor consumes it to re-key and restart pairing. atomic: cross-task flag.
    std::atomic<bool> pairing_lost_{false};

    // Consecutive "authentication failed" responses from signed commands. A car whose
    // whitelist no longer holds our key replies to a signed command with an untagged
    // session-info → "auth response authentication failed" (not KEY_NOT_ON_WHITELIST).
    // Since that message is also used for one-off glitches, two in a row are required
    // before pairing_lost_ is set. Reset on any successful response. atomic: cross-task.
    std::atomic<int> auth_fail_streak_{0};

    // True only while the supervisor believes we are paired (in the health-check phase).
    // Gates the message observer so a key-rejection fault seen during *enrolment* (when
    // rejections are expected) can't trip a re-key, while one seen while paired does.
    std::atomic<bool> believed_paired_{false};

    // Sticky "re-pairing needed" notice for the UI: set when a lost pairing is handled,
    // cleared when a new session is established. Distinguishes a re-pair-after-revocation
    // from a never-paired device. atomic: written/read across tasks.
    std::atomic<bool> repair_notice_{false};

    // ── Sleep / active-window gating ────────────────────────────────────────────────
    // The background infotainment polls (charge + telemetry) and the warm-up connect open
    // an infotainment session, which keeps the car's main computer awake. To let a parked
    // car sleep, loop_task runs them ONLY while the "active window" is open: a real command
    // in the last kActiveWindowMs (last_cmd_ticks_), OR the car is charging. (We do NOT open
    // the window merely because the car is awake — that would be self-perpetuating; the car
    // could never finish its idle→sleep transition. See loop_task_fn_.)
    std::atomic<uint32_t> last_cmd_ticks_{0};  // ticks of the last real command (0 = never)
    static constexpr uint32_t kActiveWindowMs = 300000;  // 5 min command-recency window

    // Set when a library call (BLE rx parse or loop()) throws an uncaught C++ exception on
    // corrupt RX. The tesla-ble framer parses Tesla's length-prefixed messages out of the
    // BLE stream; a lossy link desyncs the framing and some corrupt inputs make it throw
    // (out_of_range / bad_alloc). Exceptions are enabled but the library never catches, so an
    // escaping throw → std::terminate → abort() → reboot (observed on a parked, awake car).
    // We catch at our call boundary and set this; loop_task then drops the BLE link once to
    // clear the library's rx_buffer and re-sync, turning the reboot into a brief reconnect.
    std::atomic<bool> ble_fault_{false};

    // Cached results for non-blocking UI access
    ChargeStateResult   last_known_charge_{};
    VehicleStatusResult last_known_status_{};
    ClimateStateResult  last_known_climate_{};
    DriveStateResult    last_known_drive_{};
    TirePressureResult  last_known_tires_{};
    ClosuresStateResult last_known_closures_{};

    // Pending status result stored as a member to avoid lambda capturing stack refs
    VehicleStatusResult pending_status_{};

    TaskHandle_t loop_task_{nullptr};
    static void loop_task_fn_(void* arg);

    // Automatic pairing: while not paired, periodically connect to the configured
    // vehicle and (re)send the whitelist-add so the car keeps prompting "Add key?".
    // Once the user taps the key card, the VCSEC handshake establishes a session and
    // the loop goes idle. No manual scan/pair needed.
    TaskHandle_t auto_pair_task_{nullptr};
    static void auto_pair_task_fn_(void* arg);
};
