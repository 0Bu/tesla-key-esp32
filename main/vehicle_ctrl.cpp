// VehicleController core: wiring (init — BLE callbacks, revocation observer, task
// startup), the VIN plausibility gate, and the link_state() glue into the host-tested
// four-state machine. The other concerns live in vehicle_commands.cpp (command
// dispatch), vehicle_telemetry.cpp (parsers/caches/background poll) and
// vehicle_pairing.cpp (pairing lifecycle/keys); shared RAII in vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "runtime_admission.hpp"
#include "logic/vin.hpp"
#include "logic/heap_watchdog.hpp"
#include "task_config.hpp"
#include <cmath>
#include <cstring>
#include <esp_log.h>

// No protobuf includes needed here: the only generated types this TU touches are the
// UniversalMessage_* ones, which vehicle_ctrl.hpp already provides via <vehicle.h>.

static const char* TAG = "vehicle_ctrl";

// ─── Boot reboot-reason (set once from app_main, read by /status) ────────────

// Function-local static so it is constructed on first use, before app_main writes it — no
// static-init-order dependency on the translation units that read it.
static tk::RebootReasonRecord& boot_reason_slot() {
    static tk::RebootReasonRecord reason;
    return reason;
}
void VehicleController::set_boot_reboot_reason(const tk::RebootReasonRecord& reason) {
    boot_reason_slot() = reason;
}
const char* VehicleController::boot_reboot_reason() { return boot_reason_slot().text; }
uint8_t VehicleController::boot_heap_restarts() { return boot_reason_slot().heap_restarts; }

bool VehicleController::ble_scan(int ms) {
    if (!tk::runtime_admission_vehicle_ready() || !ble_) return false;
    ble_->start_discovery(ms);
    return true;
}

// ─── Custom no-op shared_ptr deleters ────────────────────────────────────────
// Vehicle needs shared_ptr<BleAdapter> and shared_ptr<StorageAdapter>.
// We own the objects externally, so we provide deleters that do nothing.
struct NoDelete {
    void operator()(TeslaBLE::BleAdapter*)    const {}
    void operator()(TeslaBLE::StorageAdapter*)const {}
};

bool VehicleController::ble_link_event_cb_(void* context, bool connected,
                                           uint16_t conn_handle,
                                           uint32_t generation) noexcept {
    auto* self = static_cast<VehicleController*>(context);
    return self && self->enqueue_ble_link_event_(connected, conn_handle, generation);
}

bool VehicleController::ble_rx_event_cb_(void* context, const uint8_t* data, size_t size,
                                         uint32_t generation) noexcept {
    auto* self = static_cast<VehicleController*>(context);
    return self && self->enqueue_ble_rx_event_(data, size, generation);
}

bool VehicleController::enqueue_ble_link_event_(bool connected, uint16_t conn_handle,
                                                uint32_t generation) noexcept {
    if (!connected) {
        // Invalidate the active waiter before deferred Vehicle cleanup: a physical link loss can
        // never complete or be adopted by a later request while the FIFO is waiting for its task.
        command_generation_.fetch_add(1, std::memory_order_acq_rel);
        auth_fail_streak_.store(0, std::memory_order_release);
    }
    BleHostEvent event{};
    event.kind = connected ? tk::BleDeferredEventKind::LinkUp
                           : tk::BleDeferredEventKind::LinkDown;
    event.conn_handle = conn_handle;
    event.generation = generation;
    const bool queued = ble_event_queue_ &&
                        xQueueSend(ble_event_queue_, &event, 0) == pdTRUE;
    if (!queued) ble_event_overflow_.store(true, std::memory_order_release);
    return queued;
}

bool VehicleController::enqueue_ble_rx_event_(const uint8_t* data, size_t size,
                                              uint32_t generation) noexcept {
    if (!data || size == 0 || size > tk::kBleMaxWritePayload) {
        ble_event_overflow_.store(true, std::memory_order_release);
        return false;
    }
    BleHostEvent event{};
    event.kind = tk::BleDeferredEventKind::Rx;
    event.size = static_cast<uint16_t>(size);
    event.generation = generation;
    std::memcpy(event.data.data(), data, size);
    const bool queued = ble_event_queue_ &&
                        xQueueSend(ble_event_queue_, &event, 0) == pdTRUE;
    if (!queued) ble_event_overflow_.store(true, std::memory_order_release);
    return queued;
}

bool VehicleController::recover_pending_key_rotation_at_boot_() {
    key_rotation_recovered_at_boot_ = false;
    bool marker_present = false;
    const bool probe_ok = storage_ &&
                          storage_->probe_blob(tk::kKeyRotationMarker, marker_present);
    const tk::KeyRotationMarkerProbe probe =
        tk::classify_key_rotation_marker_probe(probe_ok, marker_present);
    if (probe == tk::KeyRotationMarkerProbe::Error) {
        ESP_LOGE(TAG, "key-rotation marker could not be read — refusing vehicle construction");
        return false;
    }
    if (tk::decide_key_rotation_boot(probe == tk::KeyRotationMarkerProbe::Present,
                                     false, false, false) ==
        tk::KeyRotationBootState::Ready) {
        return true;
    }

    // Do not construct TeslaBLE::Vehicle while this marker exists: its constructor loads the
    // private key and persisted peers, and a torn rotation makes that combination ambiguous.
    key_runtime_safe_.store(false);
    pairing_cleanup_pending_.store(true);
    ESP_LOGW(TAG, "interrupted key rotation detected — cleaning persisted peer state before key load");

    const bool vcsec_removed = storage_->remove(tk::nvs_contract::kSessionVcsec);
    const bool info_removed = storage_->remove(tk::nvs_contract::kSessionInfotainment);
    const bool paired_removed = storage_->remove(tk::nvs_contract::kPairedAt);
    const bool cleanup_ok = vcsec_removed && info_removed && paired_removed;
    // Short-circuit deliberately: the journal is the retry authority and must remain durable
    // after any failed peer erase. Each remove commits independently, so power loss at every
    // boundary simply re-enters this idempotent branch on the next boot.
    const bool marker_removed = cleanup_ok && storage_->remove(tk::kKeyRotationMarker);
    const tk::KeyRotationBootState final_state =
        tk::decide_key_rotation_boot(true, true, cleanup_ok, marker_removed);
    if (final_state != tk::KeyRotationBootState::Ready) {
        ESP_LOGE(TAG, "key-rotation boot cleanup incomplete (vcsec=%d info=%d paired_at=%d marker=%d)",
                 static_cast<int>(vcsec_removed), static_cast<int>(info_removed),
                 static_cast<int>(paired_removed), static_cast<int>(marker_removed));
        return false;
    }

    pairing_cleanup_pending_.store(false);
    key_rotation_recovered_at_boot_ = true;
    ESP_LOGI(TAG, "interrupted key-rotation cleanup completed before vehicle construction");
    return true;
}

// ─── init ────────────────────────────────────────────────────────────────────

bool VehicleController::init(const std::string& vin,
                              BleClient& ble,
                              NvsStorageAdapter& storage,
                              NvsStorageAdapter& config_store,
                              std::string& known_mac,
                              bool start_tasks) {
    ble_          = &ble;
    storage_      = &storage;
    config_store_ = &config_store;
    persist_discovered_mac_.store(known_mac.empty());
    vin_          = vin;

    if (!recover_pending_key_rotation_at_boot_()) {
        // app_main treats init failure as boot-fatal. No background task and, critically, no
        // TeslaBLE::Vehicle exists on this path, so a pending journal can never be reported as
        // paired or used to sign/enrol until a later boot finishes its cleanup.
        return false;
    }

    bool stored_private_key = false;
    if (!storage_->probe_blob(tk::nvs_contract::kPrivateKey, stored_private_key)) {
        // Missing is a valid first-boot state; unreadable is not. Do not construct Vehicle or
        // start auto-pair with an NVS error misclassified as permission to generate a new key.
        ESP_LOGE(TAG, "private-key storage probe failed — refusing vehicle construction");
        return false;
    }

    vehicle_mutex_ = xSemaphoreCreateMutex();
    command_mutex_ = xSemaphoreCreateMutex();
    cache_mutex_   = xSemaphoreCreateMutex();
    result_mutex_  = xSemaphoreCreateMutex();
    ble_event_queue_ = xQueueCreateStatic(
        kBleHostEventDepth, sizeof(BleHostEvent), ble_event_queue_storage_.data(),
        &ble_event_queue_control_);
    // VehicleController is an ESSENTIAL component: without these primitives the command
    // serialization and cache locking cannot hold, so refuse to report a healthy controller
    // rather than run one that could deadlock or race (issue #204, Scenario C). app_main halts
    // boot on a false return.
    if (!vehicle_mutex_ || !command_mutex_ || !cache_mutex_ || !result_mutex_ ||
        !ble_event_queue_) {
        ESP_LOGE(TAG, "synchronization primitive allocation failed");
        return false;
    }

    auto ble_sp     = std::shared_ptr<TeslaBLE::BleAdapter>(&ble, NoDelete{});
    auto storage_sp = std::shared_ptr<TeslaBLE::StorageAdapter>(&storage, NoDelete{});
    vehicle_ = std::make_unique<TeslaBLE::Vehicle>(ble_sp, storage_sp);

    vehicle_->set_vin(vin);
    // A blob's mere presence does not prove that the library parsed it. Commands may use the
    // identity only when storage contains a key AND this Vehicle instance successfully loaded it.
    // This also keeps a corrupt/truncated key from being treated as enrolment-safe after reboot.
    key_runtime_safe_.store(stored_private_key && vehicle_->has_private_key());

    // Exact named adapters are mechanically audited as fixed POD/atomic/queue-only callbacks.
    // The NimBLE host never calls Vehicle, allocates, waits on a shared mutex, logs or touches NVS.
    ble_->set_connected_cb(&VehicleController::ble_link_event_cb_, this);
    ble_->set_rx_data_cb(&VehicleController::ble_rx_event_cb_, this);

    // Persistent charge-state + read-only telemetry cache callbacks (installed once,
    // never cleared) — defined in vehicle_telemetry.cpp next to the parsers they use.
    install_state_callbacks_();

    // Reliable key-revocation detector. When the key is deleted on the car side, the
    // VCSEC health poll keeps succeeding from its cached session (the whitelist is not
    // re-checked per command), so it can miss the deletion entirely. But the car rejects
    // every signed command on the *infotainment* domain immediately with a signed-message
    // fault naming the key (ERROR_UNKNOWN_KEY_ID) — the background charge poll triggers
    // exactly that. Observe every incoming message and, while we believe we're paired,
    // treat such a fault as a lost pairing. Runs inside serialized Vehicle dispatch; only atomic
    // ops here. Gated on believed_paired_ so enrolment-time rejections are ignored. Keep the
    // std::function adapter itself mechanically trivial: tesla-ble invokes it synchronously while
    // Vehicle owns its internal dispatch, so logging/allocation belongs in the normal task loop.
    vehicle_->set_message_callback([this](const UniversalMessage_RoutableMessage& msg) {
        on_vehicle_message_(msg);
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

    if (!start_tasks) {
        // The fully wired controller is safe to read, but the caller owns the lifecycle boundary:
        // normal boot defers these mutating tasks until recovery + essential services complete;
        // safe mode intentionally never starts them.
        ESP_LOGI(TAG, "vehicle_loop and auto_pair deferred");
        return true;
    }

    return this->start_tasks();
}

void VehicleController::on_vehicle_message_(
    const UniversalMessage_RoutableMessage& msg) noexcept {
    if (!believed_paired_ || !msg.has_signedMessageStatus) return;
    switch (msg.signedMessageStatus.signed_message_fault) {
        case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_UNKNOWN_KEY_ID:
        case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INACTIVE_KEY:
        case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_KEY_HANDLE:
            pairing_lost_.store(true, std::memory_order_release);
            break;
        default:
            break;
    }
}

bool VehicleController::start_tasks() {
    if (loop_task_ && auto_pair_task_ &&
        task_start_gate_.state() == tk::TaskStartState::Running) return true;
    if (loop_task_ || auto_pair_task_) {
        // Never externally delete a task: it may be running on the other core while registered
        // with TWDT or holding a mutex. A failed start remains fail-closed until its entry task
        // has observed Cancelled, acknowledged and self-deleted.
        ESP_LOGE(TAG, "inconsistent vehicle task lifecycle — refusing external task deletion");
        return false;
    }
    if (!vehicle_ || !vehicle_mutex_ || !command_mutex_ || !cache_mutex_ || !result_mutex_) {
        ESP_LOGE(TAG, "vehicle tasks cannot start before controller initialization completes");
        return false;
    }

    if (!task_start_gate_.begin()) {
        ESP_LOGE(TAG, "vehicle task start gate is not idle");
        return false;
    }

    TaskHandle_t loop_task = nullptr;
    if (xTaskCreate(loop_task_fn_, "vehicle_loop", 8192, this,
                    tk::kPrioVehicleLoop, &loop_task) != pdPASS) {
        ESP_LOGE(TAG, "vehicle_loop task creation failed");
        task_start_gate_.cancel();
        task_start_gate_.reset_cancelled(0);
        return false;
    }
    loop_task_ = loop_task;

    TaskHandle_t auto_pair_task = nullptr;
    if (xTaskCreate(auto_pair_task_fn_, "auto_pair", 8192, this,
                    tk::kPrioAutoPair, &auto_pair_task) != pdPASS) {
        ESP_LOGE(TAG, "auto_pair task creation failed");
        task_start_gate_.cancel();

        // The first task has done nothing except wait at await_task_start_(). Give it a bounded
        // chance to observe cancellation and self-delete. No external vTaskDelete is safe here:
        // on a dual-core target the task could otherwise be killed while using TWDT or a mutex.
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (!task_start_gate_.cancelled_tasks_acknowledged(1) &&
               static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) {
            vTaskDelay(1);
        }
        if (!task_start_gate_.cancelled_tasks_acknowledged(1)) {
            ESP_LOGE(TAG, "vehicle_loop did not acknowledge cancelled start");
            return false;
        }
        loop_task_ = nullptr;
        if (!task_start_gate_.reset_cancelled(1)) {
            ESP_LOGE(TAG, "vehicle task start gate could not reset after cancellation");
        }
        return false;
    }
    auto_pair_task_ = auto_pair_task;
    if (!task_start_gate_.release()) {
        // Both tasks still sit before every runtime resource. Cancel them cooperatively; boot will
        // fail closed rather than run a lifecycle whose release state cannot be proven.
        task_start_gate_.cancel();
        ESP_LOGE(TAG, "vehicle task start gate could not release both tasks");
        return false;
    }
    ESP_LOGI(TAG, "VehicleController ready for VIN %s", vin_.c_str());
    return true;
}

bool VehicleController::await_task_start_() {
    for (;;) {
        switch (task_start_gate_.entry_action()) {
            case tk::TaskEntryAction::Run:
                // Both allocations succeeding is necessary but not sufficient: app_main has
                // additional essential services to admit as one boot.  Wait until its global
                // fail-closed gate says the entire runtime is Ready.
                switch (tk::runtime_admission_action()) {
                    case tk::RuntimeAdmissionAction::Run:  return true;
                    case tk::RuntimeAdmissionAction::Stop: return false;
                    case tk::RuntimeAdmissionAction::Wait:
                        vTaskDelay(1);
                        break;
                }
                break;
            case tk::TaskEntryAction::Cancel:
                task_start_gate_.acknowledge_cancel();
                return false;
            case tk::TaskEntryAction::Wait:
                // This is the only operation permitted before both task allocations succeed.
                vTaskDelay(1);
                break;
        }
    }
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
