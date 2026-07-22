// VehicleController core: wiring (init — BLE callbacks, revocation observer, task
// startup), the VIN plausibility gate, and the link_state() glue into the host-tested
// four-state machine. The other concerns live in vehicle_commands.cpp (command
// dispatch), vehicle_telemetry.cpp (parsers/caches/background poll) and
// vehicle_pairing.cpp (pairing lifecycle/keys); shared RAII in vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "logic/vin.hpp"
#include "logic/heap_watchdog.hpp"
#include "task_config.hpp"
#include <cmath>
#include <esp_log.h>

// No protobuf includes needed here: the only generated types this TU touches are the
// UniversalMessage_* ones, which vehicle_ctrl.hpp already provides via <vehicle.h>.

static const char* TAG = "vehicle_ctrl";

// ─── Boot reboot-reason (set once from app_main, read by /status) ────────────

// Function-local static so it is constructed on first use, before app_main writes it — no
// static-init-order dependency on the translation units that read it.
static std::string& boot_reason_slot() {
    static std::string why;
    return why;
}
void               VehicleController::set_boot_reboot_reason(const std::string& w) { boot_reason_slot() = w; }
const std::string& VehicleController::boot_reboot_reason() { return boot_reason_slot(); }
uint8_t            VehicleController::boot_heap_restarts() {
    return tk::heap_reason_parse(boot_reason_slot().c_str());
}

// ─── Custom no-op shared_ptr deleters ────────────────────────────────────────
// Vehicle needs shared_ptr<BleAdapter> and shared_ptr<StorageAdapter>.
// We own the objects externally, so we provide deleters that do nothing.
struct NoDelete {
    void operator()(TeslaBLE::BleAdapter*)    const {}
    void operator()(TeslaBLE::StorageAdapter*)const {}
};

// ─── init ────────────────────────────────────────────────────────────────────

bool VehicleController::init(const std::string& vin,
                              BleClient& ble,
                              NvsStorageAdapter& storage,
                              NvsStorageAdapter& config_store,
                              std::string& known_mac) {
    ble_          = &ble;
    storage_      = &storage;
    config_store_ = &config_store;
    known_mac_    = &known_mac;
    vin_          = vin;

    cmd_sem_       = xSemaphoreCreateBinary();
    vehicle_mutex_ = xSemaphoreCreateMutex();
    command_mutex_ = xSemaphoreCreateMutex();
    cache_mutex_   = xSemaphoreCreateMutex();
    // VehicleController is an ESSENTIAL component: without these primitives the command
    // serialization and cache locking cannot hold, so refuse to report a healthy controller
    // rather than run one that could deadlock or race (issue #204, Scenario C). app_main halts
    // boot on a false return.
    if (!cmd_sem_ || !vehicle_mutex_ || !command_mutex_ || !cache_mutex_) {
        ESP_LOGE(TAG, "synchronization primitive allocation failed");
        return false;
    }

    auto ble_sp     = std::shared_ptr<TeslaBLE::BleAdapter>(&ble, NoDelete{});
    auto storage_sp = std::shared_ptr<TeslaBLE::StorageAdapter>(&storage, NoDelete{});
    vehicle_ = std::make_unique<TeslaBLE::Vehicle>(ble_sp, storage_sp);

    vehicle_->set_vin(vin);

    // Wire BLE → Vehicle callbacks
    ble_->set_connected_cb([this](bool connected) {
        {
            // RAII give — this runs on the NimBLE host task and the callers (on_gap_event /
            // on_dsc_disc) catch instead of rethrowing, so a throw out of the library here must
            // not skip the give: a silently-held vehicle_mutex_ would wedge every later
            // command/poll until power-cycle. The catch also flags a link reset (loop_task).
            tk::SemGuard g(vehicle_mutex_);
            try {
                vehicle_->set_connected(connected);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "set_connected threw (%s) — resetting link", e.what());
                ble_fault_.store(true);
            } catch (...) {
                ESP_LOGE(TAG, "set_connected threw (unknown) — resetting link");
                ble_fault_.store(true);
            }
        }

        if (!connected) {
            // The BLE link just dropped. The "auth response authentication failed" →
            // pairing_lost_ heuristic in make_result_cb_ (now fed only by the signed health
            // probe) requires TWO such replies in a row,
            // on the premise that a genuinely de-whitelisted key keeps failing on a healthy,
            // continuously-connected link. A lossy/recovering link, by contrast, emits the
            // same message as transient corruption and then drops — so two failures that
            // straddle a disconnect are NOT evidence of a deleted key. Reset the streak here
            // so a reconnect starts clean and a flaky link can't be mistaken for a revocation
            // (which would clear the session and wrongly prompt "approve on the touchscreen"
            // on an already-paired car). The definitive signals — a "whitelist" message and
            // the ERROR_UNKNOWN_KEY_ID/INACTIVE_KEY/INVALID_KEY_HANDLE faults — are immediate
            // and unaffected, so a real key deletion is still caught.
            auth_fail_streak_.store(0);
        }

        // Persist discovered MAC on first connection so we skip scanning next boot
        if (connected && known_mac_ && known_mac_->empty() && config_store_) {
            std::string addr = ble_client_instance()
                               ? ble_client_instance()->peer_addr_str() : "";
            if (!addr.empty()) {
                *known_mac_ = addr;
                config_store_->save_str("ble_mac", addr);
                ESP_LOGI(TAG, "Tesla MAC saved: %s", addr.c_str());
            }
        }
    });
    ble_->set_rx_data_cb([this](const std::vector<uint8_t>& data) {
        // on_rx_data parses Tesla's length-prefixed frames out of these bytes synchronously.
        // A weak/lossy BLE link desyncs the framing ("Invalid message length …") and some
        // corrupt inputs make the parser throw (out_of_range / bad_alloc). This callback runs
        // in NimBLE's host task, so an escaping throw unwinds through C dispatch frames →
        // std::terminate → abort() → reboot. Catch it at this nearest C++ boundary and flag a
        // link reset (handled in loop_task). RAII give — the mutex releases on unwind too.
        tk::SemGuard g(vehicle_mutex_);
        try {
            vehicle_->on_rx_data(data);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "on_rx_data threw (%s) — corrupt BLE RX; resetting link", e.what());
            ble_fault_.store(true);
        } catch (...) {
            ESP_LOGE(TAG, "on_rx_data threw (unknown) — corrupt BLE RX; resetting link");
            ble_fault_.store(true);
        }
    });

    // Persistent charge-state + read-only telemetry cache callbacks (installed once,
    // never cleared) — defined in vehicle_telemetry.cpp next to the parsers they use.
    install_state_callbacks_();

    // Reliable key-revocation detector. When the key is deleted on the car side, the
    // VCSEC health poll keeps succeeding from its cached session (the whitelist is not
    // re-checked per command), so it can miss the deletion entirely. But the car rejects
    // every signed command on the *infotainment* domain immediately with a signed-message
    // fault naming the key (ERROR_UNKNOWN_KEY_ID) — the background charge poll triggers
    // exactly that. Observe every incoming message and, while we believe we're paired,
    // treat such a fault as a lost pairing. Runs in the BLE RX task; only cheap atomic
    // ops here. Gated on believed_paired_ so enrolment-time rejections are ignored.
    vehicle_->set_message_callback([this](const UniversalMessage_RoutableMessage& msg) {
        if (!believed_paired_ || !msg.has_signedMessageStatus) return;
        switch (msg.signedMessageStatus.signed_message_fault) {
            case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_UNKNOWN_KEY_ID:
            case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INACTIVE_KEY:
            case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_KEY_HANDLE:
                if (!pairing_lost_.exchange(true)) {
                    ESP_LOGW(TAG, "auto-pair: car rejected our key (fault %d) — key deleted on the car side, pairing lost",
                             (int)msg.signedMessageStatus.signed_message_fault);
                }
                break;
            default:
                break;
        }
    });

    // Seed the active window open at boot so evcc gets a warm cache for the first few
    // minutes after start; it then backs off if the car stays idle (no command, not charging).
    //
    // NOT after a heap-watchdog restart. That seeding is what makes a restart LOOP expensive: the
    // window re-opens on every boot, so a device restarting every ~10 minutes would poll
    // infotainment forever and a parked car next to it would never sleep. A device that just
    // self-healed has no user waiting on a warm cache anyway — the first evcc command re-opens
    // the window in the normal way.
    if (boot_heap_restarts() == 0) {
        last_cmd_ticks_.store(xTaskGetTickCount());
    } else {
        ESP_LOGW(TAG, "boot after %u heap-watchdog restart(s) — leaving the active window CLOSED "
                      "so a parked car can still sleep", (unsigned) boot_heap_restarts());
    }

    if (xTaskCreate(loop_task_fn_, "vehicle_loop", 8192, this,
                    tk::kPrioVehicleLoop, &loop_task_) != pdPASS) {
        ESP_LOGE(TAG, "vehicle_loop task creation failed");
        loop_task_ = nullptr;
        return false;
    }
    if (xTaskCreate(auto_pair_task_fn_, "auto_pair", 8192, this,
                    tk::kPrioAutoPair, &auto_pair_task_) != pdPASS) {
        ESP_LOGE(TAG, "auto_pair task creation failed");
        // Unwind the loop task we already started so no half-initialised controller is left running.
        if (loop_task_) { vTaskDelete(loop_task_); loop_task_ = nullptr; }
        auto_pair_task_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "VehicleController ready for VIN %s", vin.c_str());
    return true;
}

// A plausible Tesla VIN: exactly 17 chars, uppercase alphanumeric excluding I/O/Q (reserved by
// the VIN standard). Mirrors the web UI's client-side check (www/app.js) and /set_vin's server validation.
// Used to gate pairing so the device never connects/enrols without a real VIN — the boot
// placeholder "UNKNOWN" (7 chars) is not plausible, so it can never reach the matching path.
bool VehicleController::vin_is_plausible(const std::string& vin) {
    return tk::vin_is_plausible(vin);  // single source of truth (logic/vin.hpp, host-tested)
}

// ─── link_state — single source of truth for the car's connectivity ──────────

// Derived connectivity state — see the enum doc in vehicle_ctrl.hpp and the pure decision in
// logic/link_state.hpp. Centralised so the web UI (/status) and the MQTT/HA bridge consume one
// consistent answer. The k* thresholds below are defined in logic/link_state.hpp; their
// rationale:
//   kAwakeMaxAgeS     mirrors the old per-file thresholds (charge polls refresh contact
//                     every ~10 s while the window is open, so 60 s won't flap).
//   kReachableMaxAgeS must span TWO full idle health-probe cycles incl. one missed probe so a
//                     transient miss never flaps a sleeping-NEARBY car to Unreachable (which
//                     would wrongly show the web-UI "Unreachable" hero / publish a phantom "UNREACHABLE").
//                     The idle reachability stamp comes only from auto_pair_task's health
//                     probe, whose cycle is its 30 s post-probe wait + a VCSEC scan/connect
//                     (≤10 s, ensure_connected_) + round-trip (≤8 s, health_probe_) ≈ 40-48 s;
//                     a failed probe on the flaky link to a sleeping car adds another ~30 s
//                     wait + timeout. 150 s clears two such cycles with margin while a
//                     genuinely-gone car still flips to Unreachable in ~2.5 min.
//   kAsleepDebounceS  must outlast the COP-driven VCSEC AWAKE↔ASLEEP flap (~60 s observed) so
//                     a momentary ASLEEP blip can't flip the UI to "Vehicle asleep"; 120 s
//                     needs the flag to stay ASLEEP across at least two idle health probes.
VehicleController::LinkState VehicleController::link_state() const {
    // Snapshot the atomic member state and hand it to the pure, host-tested decision in
    // logic/link_state.hpp. The rationale for each state (and the Asleep-debounce that
    // stops an awake-but-idle car being mislabelled "asleep" the instant polling stops,
    // surviving the ~60 s Cabin-Overheat-Protection AWAKE↔ASLEEP flap) lives there and on
    // the member declaration in vehicle_ctrl.hpp.
    tk::LinkInputs in;
    in.have_contact        = seconds_since_contact(in.contact_age_s);
    in.have_reachable      = seconds_since_reachable(in.reachable_age_s);
    in.vcsec_stably_asleep = vcsec_stably_asleep_(tk::kAsleepDebounceS);
    return tk::compute_link_state(in);
}

// Assemble the vehicle-owned half of the on-device indicators' input snapshot (see
// logic/ui_state.hpp). One pass over link_state / ble_* / the cached charge — the charge
// copy is taken under cache_mutex_ by get_cached_charge() — so a presenter reads one
// consistent view instead of racing five separate accessors across a frame. `soc` is left
// RAW (the presenter clamps); `paired` and the wifi_* fields are filled by the caller.
tk::UiSnapshot VehicleController::ui_snapshot() {
    tk::UiSnapshot s;
    ChargeStateResult cs = get_cached_charge();
    s.have_soc  = cs.valid && cs.has_battery_level;
    s.soc       = static_cast<int>(lroundf(cs.battery_level));
    s.charging  = (cs.charging_state == "Charging");
    s.link_state    = link_state();
    s.ble_connected = ble_connected();
    int8_t rssi = 0;
    s.ble_rssi_valid = s.ble_connected && ble_rssi(rssi);
    s.ble_rssi       = rssi;
    return s;
}
