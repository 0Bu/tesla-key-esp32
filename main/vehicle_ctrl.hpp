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

    bool get_charge_state(ChargeStateResult& out, int timeout_ms = 20000);
    bool get_vehicle_status(VehicleStatusResult& out, int timeout_ms = 20000);

    // Non-blocking accessors for cached state (refreshed in background)
    ChargeStateResult   get_cached_charge() { return last_known_charge_; }
    VehicleStatusResult get_cached_status() { return last_known_status_; }

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

    // Persist a new VIN to the config store (takes effect after reboot).
    bool save_config_vin(const std::string& vin) {
        return config_store_ ? config_store_->save_str("vin", vin) : false;
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
                     TeslaBLE::WakePolicy wp, int timeout_ms);
    bool send_infotainment_(const std::string& name, Builder builder, int timeout_ms,
                            TeslaBLE::WakePolicy wp = TeslaBLE::WakePolicy::WAKE_IF_NEEDED);

    // RAII helper: takes cmd_sem_ when done
    ResultCb make_result_cb_();

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
    bool              last_result_{false};

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

    // Sticky "re-pairing needed" notice for the UI: set when a lost pairing is handled,
    // cleared when a new session is established. Distinguishes a re-pair-after-revocation
    // from a never-paired device. atomic: written/read across tasks.
    std::atomic<bool> repair_notice_{false};

    // Cached results for non-blocking UI access
    ChargeStateResult   last_known_charge_{};
    VehicleStatusResult last_known_status_{};

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
