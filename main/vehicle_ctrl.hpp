#pragma once

#include <vehicle.h>
#include <client.h>
#include <string>
#include <functional>
#include <memory>
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

    bool generate_key();
    bool pair(bool owner_role = false, int timeout_ms = 30000);

    const std::string& vin() const { return vin_; }
    TeslaBLE::Vehicle* vehicle() { return vehicle_.get(); }

private:
    // Builder function type used by send_command_result
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    using ResultCb = TeslaBLE::Command::OperationResultCallback;

    bool ensure_connected_(int timeout_ms = 10000);

    // Enqueue an Infotainment or VCSEC command and wait for the callback.
    bool send_vcsec_(const std::string& name, Builder builder,
                     TeslaBLE::WakePolicy wp, int timeout_ms);
    bool send_infotainment_(const std::string& name, Builder builder, int timeout_ms);

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
    bool              last_result_{false};

    // Pending query results stored as members to avoid lambda capturing stack refs
    ChargeStateResult   pending_charge_{};
    VehicleStatusResult pending_status_{};

    TaskHandle_t loop_task_{nullptr};
    static void loop_task_fn_(void* arg);
};
