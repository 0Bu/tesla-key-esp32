#pragma once

#include <vehicle.h>
#include <client.h>
#include <string>
#include <functional>
#include <memory>
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

private:
    // Builder function type used by send_command_result
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    using ResultCb = TeslaBLE::Command::OperationResultCallback;

    bool ensure_connected_(int timeout_ms = 10000);

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
