#pragma once

#include <client.h>
#include "logic/command_result.hpp"
#include "logic/command_deadline.hpp"
#include "logic/session_availability.hpp"
#include "logic/command_runner.hpp"
#include "logic/auto_enrollment.hpp"
#include "logic/key_rotation.hpp"
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <array>
#include <cstdint>
#include <ctime>
#include <esp_log.h>
#include "ble_client.hpp"
#include "nvs_storage.hpp"
#include "rtos_guard.hpp"
#include "logic/ble_phase.hpp"
#include "logic/connect_outcome.hpp"
#include "logic/charge_control.hpp"
#include "logic/link_state.hpp"
#include "logic/ui_state.hpp"
#include "logic/vin_transition.hpp"
#include "logic/task_start_gate.hpp"
#include "logic/ble_deferred_event.hpp"
#include "logic/ble_deferred_queue.hpp"
#include "logic/rx_framing.hpp"
#include "reboot_reason.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "logic/vehicle_data.hpp"

namespace tk { struct TelemetryPollState; }

class VehicleController {
public:
    VehicleController() = default;

    // All references must remain alive for the lifetime of this object.
    // known_mac: if empty, MAC is discovered via scan and saved to config_store.
    // start_tasks == false wires EVERYTHING (callbacks, mutexes, caches, the VIN gate) but creates
    // neither background task, so the controller is fully safe to READ — /status, the web UI and
    // the MQTT snapshot all work — while nothing of ours touches the car. That is safe mode
    // (safe_mode.cpp): after enough consecutive crash boots the vehicle path is exactly what must
    // not run, and skipping init() outright is not the alternative — it would hand the HTTP server
    // a half-constructed controller to read from.
    bool init(const std::string& vin, BleClient& ble, NvsStorageAdapter& storage,
              NvsStorageAdapter& config_store, std::string& known_mac,
              bool start_tasks = false);
    // Inert minimal initialization for safe mode: sets VIN, storage, and config_store, allocates
    // essential mutexes for safe UI/status accessors, but initializes no BLE client
    // and starts no mutating background tasks.
    bool init_safe_mode(const std::string& vin, NvsStorageAdapter& storage,
                        NvsStorageAdapter& config_store);
    // Idempotently starts the two mutating background tasks after the caller has completed boot
    // recovery and all essential initialization. Neither task can leave its start barrier until
    // both creates succeed; a partially-created task acknowledges cancellation and self-deletes.
    bool start_tasks();

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
    // Cache-only climate snapshot for evcc's climater: never polls, connects or wakes. Inside the
    // active window the climate must be at most tk::kActiveClimateStateMaxAgeS old. Outside it
    // (including after the charging arm lapses about 60 s after the last live contact, when no
    // command was sent in the last 5 min) the last-known snapshot is served, mirroring the
    // charge semantics.
    bool get_vehicle_climate(tk::VehicleClimateData& out);
    // Origin is mandatory: HTTP/manual callers pass Foreground; the auto-pair supervisor
    // passes Background so an absent car cannot turn unattended probes into an error stream.
    bool get_vehicle_status(VehicleStatusResult& out, tk::ConnectOrigin origin,
                            int timeout_ms = 20000);

    // Non-blocking accessors for cached state (refreshed by vehicle_loop; copied under
    // cache_mutex_ because HTTP/MQTT readers run concurrently — see cache_mutex_).
    ChargeStateResult   get_cached_charge()   { return copy_locked_(last_known_charge_); }
    // Same cache plus its coherent, monotonic ChargeState age; never connects or polls.
    ChargeStateResult get_cached_charge_with_age(bool& have_age, uint32_t& age_s);
    VehicleStatusResult get_cached_status()   { return copy_locked_(last_known_status_); }
    ClimateStateResult  get_cached_climate()  { return copy_locked_(last_known_climate_); }
    DriveStateResult    get_cached_drive()    { return copy_locked_(last_known_drive_); }
    tk::VehicleDriveData get_cached_vehicle_drive() {
        tk::SemGuard g(cache_mutex_);
        if (!g) return {};
        return tk::vehicle_drive_data(last_known_drive_);
    }
    TirePressureResult  get_cached_tires()    { return copy_locked_(last_known_tires_); }
    ClosuresStateResult get_cached_closures() { return copy_locked_(last_known_closures_); }

    // Vehicle-owned half of the on-device status-indicator input (the ST7735 display and the
    // APA102 LED) — see logic/ui_state.hpp. Collects link_state / ble_* / cached charge in ONE
    // pass so a presenter never mixes state from different instants across a frame. The caller
    // fills WiFi fields (esp_wifi) and `paired` from a ≤1 Hz durable-record or usable-RAM sample.
    tk::UiSnapshot ui_snapshot();

    // Seconds since the last *live* infotainment data (charge/climate/drive/tires/
    // closures) was received, written to `out`. Returns false if nothing has been
    // received since boot / re-pair (nothing to show yet). Monotonic (uptime-based,
    // so independent of wall-clock sync). The background infotainment polls are all
    // NO_WAKE_SKIP, so a sleeping car stops answering them and this value freezes at
    // the moment the car last responded — i.e. it reads as how long the car has been
    // asleep (more precisely: unreachable for live data). Drives the "asleep" card.
    bool seconds_since_contact(uint32_t& out) const {
        uint32_t t = last_contact_ticks_.load();
        if (t == 0) return false;
        out = (xTaskGetTickCount() - t) / configTICK_RATE_HZ;  // ticks → seconds; uint wrap is fine
        return true;
    }

    // Seconds since the last ChargeState specifically was received, written to `out`. Returns
    // false if none has arrived since boot / re-pair. seconds_since_contact also advances for
    // climate/drive/tires/closures, so only this clock can prove the SOC reading itself is
    // current — which is what the stale-cache bootstrap in logic/wake_poll.hpp keys on.
    bool seconds_since_charge(uint32_t& out) const {
        uint32_t t = last_charge_ticks_.load();
        if (t == 0) return false;
        out = (xTaskGetTickCount() - t) / configTICK_RATE_HZ;  // ticks → seconds; uint wrap is fine
        return true;
    }

    // Seconds since the car was last REACHABLE over BLE (any successful command/status observation,
    // incl. the idle VCSEC health poll). Returns false if never reached since boot / re-pair.
    // Unlike seconds_since_contact this keeps refreshing while the car only sleeps nearby
    // (the health poll still answers), so a stale value means no recent reachability evidence.
    bool seconds_since_reachable(uint32_t& out) const {
        uint32_t t = last_reachable_ticks_.load();
        if (t == 0) return false;
        out = (xTaskGetTickCount() - t) / configTICK_RATE_HZ;
        return true;
    }

    // Single source of truth for the car's high-level connectivity, shared by the web UI
    // and the MQTT/HA bridge so the two never drift:
    //   Awake       — fresh live infotainment telemetry (we have current data).
    //   Asleep      — no live data AND positive, debounced proof the car is sleeping: its
    //                 own VCSEC sleep flag has held ASLEEP for kAsleepDebounceS while the
    //                 car stays reachable over BLE (parked & sleeping nearby).
    //   Idle        — reachable over BLE but no live data and NOT provably asleep yet (we
    //                 stopped polling the infotainment domain to let the car sleep, and the
    //                 VCSEC flag has not confirmed sleep). We honestly do not know whether
    //                 it is awake or asleep, so the UI must not claim either — it shows a
    //                 neutral "Parked" card (last-known SOC + a wake button), never
    //                 the confident "Vehicle asleep" hero. This is the state that fixes the bug
    //                 where an awake-but-idle car was mislabelled asleep the instant polling
    //                 stopped.
    //   Unreachable — the car answers nothing over BLE → driven off / out of range /
    //                 deep sleep. We genuinely do not know its state.
    //   Unknown     — nothing heard at all since boot / re-pair (nothing to show yet).
    // NOTE on VCSEC asymmetry: we trust the VCSEC flag's ASLEEP reading (debounced) as
    // POSITIVE proof of sleep, but we never trust its AWAKE reading to claim Awake — that
    // still requires live infotainment telemetry (a parked car reports VCSEC "AWAKE" while
    // its infotainment sleeps; see wake_up()). So a wrong VCSEC AWAKE can only ever leave us
    // in Idle, never falsely Awake.
    // The four-state machine lives in logic/link_state.hpp (host-tested, IDF-free);
    // link_state() snapshots the atomic member state into tk::LinkInputs and calls
    // tk::compute_link_state(). Alias keeps the existing VehicleController::LinkState::*
    // call sites (web UI, MQTT bridge) working unchanged.
    using LinkState = tk::LinkState;
    using SleepState = tk::SleepState;
    using WakePolicy = tk::WakePolicy;
    LinkState link_state() const;

    // Current RAW VCSEC sleep belief from the library (updated on every VCSEC poll, incl. the
    // idle health probe): "AWAKE" / "ASLEEP" / "UNKNOWN". Diagnostic/transparency only — the
    // hero uses link_state(), which DEBOUNCES this (a single ASLEEP blip is not yet "asleep").
    const char* vcsec_sleep_raw() const {
        switch (static_cast<tk::SleepState>(vcsec_sleep_state_.load())) {
            case tk::SleepState::Asleep: return "ASLEEP";
            case tk::SleepState::Awake:  return "AWAKE";
            default:                     return "UNKNOWN";
        }
    }

    struct KeyGenerationResult {
        tk::KeyRotationResult rotation{tk::KeyRotationResult::NotCommitted};
        bool existing_key_refused{false};
        bool key_probe_failed{false};
        bool transition_blocked{false};
    };
    // Request-local result is produced while command_mutex_ still owns the whole rotation; HTTP
    // uses it directly instead of re-sampling a fingerprint after unlock. The overwrite guard is
    // checked under that same lock, so auto-pair cannot create a key between a preflight check and
    // mutation. The bool wrapper is retained for trusted first-boot/auto-pair callers.
    KeyGenerationResult generate_key_result(bool allow_replace);
    // Supervisor-only wrapper: re-samples the recovery hint, durable-key presence and runtime
    // safety under command_mutex_; generation is allowed only for verified first-boot absence.
    bool generate_key();

    struct NewVehicleResetResult {
        tk::VinTransitionApply state{tk::VinTransitionApply::IdentityUnverified};
        std::string previous_key_id;
    };
    using VinTransitionStager = std::function<bool(const std::string& previous_key_id)>;

    // Re-point the device at a different vehicle as one command-FIFO transaction. The staging
    // callback persists the VIN journal + complete ConfigBlob while command_mutex_ still binds
    // `previous_key_id`; auto-rekey therefore cannot change the fingerprint between staging,
    // rotation and result classification. Any staged attempt gates all signing until the caller
    // reboots, because this VehicleController instance still owns the old in-memory VIN.
    NewVehicleResetResult reset_for_new_vehicle(const VinTransitionStager& stage);

    const std::string& vin() const { return vin_; }
    // A plausible Tesla VIN is exactly 17 chars, uppercase alphanumeric with I/O/Q excluded
    // (the VIN standard reserves them). Mirrors the client-side check in www/app.js and the
    // /set_vin validation. Pairing is gated on this: the device never connects/enrols on a
    // vehicle without a real configured VIN (the boot placeholder "UNKNOWN" is not plausible).
    static bool vin_is_plausible(const std::string& vin);
    bool has_plausible_vin() const { return vin_is_plausible(vin_); }
    bool key_rotation_recovered_at_boot() const {
        return key_rotation_recovered_at_boot_;
    }

    // Status accessors (for /status and the web UI)
    bool ble_connected() const { return ble_ && ble_->is_connected(); }
    // Nearby Teslas seen while scanning (when not connected); RSSI of the live link.
    std::vector<TeslaScan> ble_nearby() const {
        return ble_ ? ble_->nearby() : std::vector<TeslaScan>{};
    }
    bool ble_rssi(int8_t& out) const { return ble_ && ble_->connected_rssi(out); }
    // Last-seen target advert RSSI, valid even while not connected (for the "can't connect"
    // signal-strength display). false if nothing seen.
    bool ble_seen_rssi(int8_t& out) const { return ble_ && ble_->last_advert_rssi(out); }
    std::string ble_peer() const { return ble_ ? ble_->peer_addr_str() : std::string{}; }
    bool ble_scan(int ms = 12000);
    bool ble_scanning() const { return ble_ && ble_->is_scanning(); }
    // Consecutive recent connect failures to the target car (0 = none / out of range). Lets
    // the web UI show "found the car but can't connect" instead of blaming BLE range.
    uint32_t ble_connect_fail() const { return ble_ ? ble_->connect_fail_recent() : 0; }
    // Target car's advert connectability: -1 unknown, 0 non-connectable (≈ at its BLE
    // connection limit), 1 connectable. Tells "car at its ~3-device limit" from "link failing".
    int ble_target_connectable() const { return ble_ ? ble_->target_connectable() : -1; }
    bool has_key();      // a private key exists in NVS
    // Cache-only current RAM state; `paired`/has_session retain durable record semantics.
    tk::SessionAvailability session_availability() const noexcept;
    bool session_available() const noexcept;
    bool has_session();  // a VCSEC session record exists in NVS; this does not prove current reachability
    // True when the vehicle connection needs attention. It is a hint, not proof of key deletion;
    // the hero shows automatic reconnection/pairing while an NVS session may still exist.
    bool reauth_required() const { return repair_notice_; }
    const char* pairing_block_reason() const noexcept {
        return tk::pairing_block_reason(key_reload_required_.load(), pairing_cleanup_pending_.load(),
                                        vin_transition_pending_.load(), automatic_key_action_.load());
    }

    // Tesla public-key id of the stored key (SHA-1(pubkey)[:4], "AB:CD:EF:01"),
    // matching the key list shown on the vehicle. Empty if no key is present.
    std::string key_fingerprint();
    // Epoch seconds when the stored key was generated, or 0 if unknown (no key,
    // or the clock had not yet synced at generation time).
    time_t key_created_at();
    // First status observation of the stored VCSEC session with NTP time; epoch seconds,
    // or 0 if no stored session / unknown. Not the Tesla pairing event timestamp.
    time_t paired_at();
    // Which BLE phase the web UI's Bluetooth row counts down, and how long is left in it.
    // Rules + rationale live in logic/ble_phase.hpp (host-tested); this only supplies the
    // two deadlines and the clock.
    tk::ble::PhaseView ble_phase() const {
        return tk::ble::phase(connect_deadline_.load(), retry_deadline_.load(),
                              xTaskGetTickCount(), configTICK_RATE_HZ);
    }
    // Reason the most recent command failed (e.g. "complete", "not_charging"), or empty
    // if it succeeded or got no response at all (car unreachable / timed out). Lets the
    // UI tell "the car rejected this" apart from "the car couldn't be reached".
    std::string last_command_error() const;
    void set_last_command_error(const std::string& err);

    // NOTE: generic runtime-config persistence deliberately does NOT live here. The HTTP
    // layer talks to the tesla_cfg store directly (g_config in http_handlers.hpp) — the
    // controller's config_store_ is only for facts the controller itself owns (the
    // discovered BLE MAC, reset_for_new_vehicle()'s cleanup of it, and the reboot reason
    // below, which loop_task is the only writer of).

private:
    using BleHostEvent = tk::BleDeferredQueue::Event;
    struct BleHostMailbox {
        tk::BleDeferredQueue queue;
        portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        bool ready{false};
        std::atomic<bool> overflow{false};
    };
    // Preserve at least32B of the old4188B queue span for the separate
    // callback-containment work. Actual whole-target budgets remain authoritative.
    static_assert(sizeof(BleHostMailbox) <= 4188 - 32);

    // Exact nonthrowing adapters registered with BleClient. They may only copy POD into the
    // statically backed queue and publish atomic fault evidence; vehicle_loop owns every library,
    // allocation, cache and NVS operation.
    static bool ble_link_event_cb_(void* context, bool connected, uint16_t conn_handle,
                                   uint32_t generation) noexcept;
    static bool ble_rx_event_cb_(void* context, const uint8_t* data, size_t size,
                                 uint32_t generation) noexcept;
    bool enqueue_ble_link_event_(bool connected, uint16_t conn_handle,
                                 uint32_t generation) noexcept;
    bool enqueue_ble_rx_event_(const uint8_t* data, size_t size,
                               uint32_t generation) noexcept;
    bool ble_host_events_ready_() noexcept;
    bool pop_ble_host_event_(BleHostEvent& event, uint32_t* empty_ms = nullptr) noexcept;
    void clear_ble_host_events_() noexcept;
    void process_ble_host_events_();
    bool apply_ble_link_state_(bool connected);
    void persist_discovered_mac_after_ready_();
    void on_charge_state_(const CarServer_ChargeState& state) noexcept;
    void on_climate_state_(const CarServer_ClimateState& state) noexcept;
    void on_drive_state_(const CarServer_DriveState& state) noexcept;
    void on_tire_pressure_state_(const CarServer_TirePressureState& state) noexcept;
    void on_closures_state_(const CarServer_ClosuresState& state) noexcept;
    void on_vehicle_message_(const UniversalMessage_RoutableMessage& message) noexcept;
    void process_pending_telemetry_();
    struct ChargingAmpsFeedback {
        uint32_t generation{0};
        bool has_charging_amps{false};
        int charging_amps{0};
        bool has_current_request{false};
        int current_request{0};
        bool has_actual_current{false};
        int actual_current{0};
    };
    ChargingAmpsFeedback charging_amps_feedback_snapshot_() noexcept;

    BleHostMailbox ble_events_;

    // Native telemetry dispatch runs synchronously while serialized by
    // vehicle_mutex_. The handlers therefore copy only nanopb POD into these latest-value slots;
    // vehicle_loop parses strings and updates the public caches after releasing vehicle_mutex_.
    enum PendingTelemetry : uint32_t {
        PendingCharge   = 1u << 0,
        PendingClimate  = 1u << 1,
        PendingDrive    = 1u << 2,
        PendingTires    = 1u << 3,
        PendingClosures = 1u << 4,
    };
    portMUX_TYPE telemetry_pending_mux_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t telemetry_pending_mask_{0};
    std::atomic<uint32_t> identity_epoch_{0};
    uint32_t telemetry_pending_charge_epoch_{0};
    uint32_t telemetry_pending_climate_epoch_{0};
    uint32_t telemetry_pending_drive_epoch_{0};
    uint32_t telemetry_pending_tires_epoch_{0};
    uint32_t telemetry_pending_closures_epoch_{0};
    CarServer_ChargeState telemetry_pending_charge_{};
    ChargingAmpsFeedback charging_amps_feedback_{};
    CarServer_ClimateState telemetry_pending_climate_{};
    CarServer_DriveState telemetry_pending_drive_{};
    CarServer_TirePressureState telemetry_pending_tires_{};
    CarServer_ClosuresState telemetry_pending_closures_{};


public:
    // Why the PREVIOUS boot ended, if it ended by our own hand ("heap:<n>"); empty after an
    // ordinary power/crash/OTA boot. A `heap:nvs-*` value instead reports that the breadcrumb was
    // unreadable, invalid or could not be consumed; that boot is conservatively counted at the cap.
    // Read once at startup (main.cpp) and consumed, so it always describes the boot just made.
    static tk::RebootReasonRecord take_reboot_reason(NvsStorageAdapter& cfg) {
        return tk::take_reboot_reason(cfg);
    }

    // The value take_reboot_reason() returned at boot, held for the life of the process so
    // /status can report it. Set once from app_main before any handler runs; read-only after.
    static void        set_boot_reboot_reason(const tk::RebootReasonRecord& why);
    static const char* boot_reboot_reason();

    // How many consecutive heap-watchdog restarts led to THIS boot (0 on any ordinary boot).
    // Classified while consuming the breadcrumb; an NVS read/erase error sets this to the cap,
    // closing both the automatic-restart ladder and the post-restart vehicle activity window.
    static uint8_t boot_heap_restarts();

private:
    // Builder function type used by send_vcsec_ and send_infotainment_
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    using ResultCb = std::function<void(tk::OperationResult)>;

    // Sub-handlers for processing received BLE frames, decomposed to bound vehicle_loop stack frame.
    bool response_matches_current_(const UniversalMessage_RoutableMessage& msg);
    bool authenticate_fault_reply_(UniversalMessage_RoutableMessage& msg);
    void handle_signed_message_fault_(const UniversalMessage_RoutableMessage& msg, bool authenticated);
    void handle_session_info_frame_(const UniversalMessage_RoutableMessage& msg);
    void handle_vcsec_frame_(const UniversalMessage_RoutableMessage& msg);
    void handle_carserver_frame_(const UniversalMessage_RoutableMessage& msg);

    // A command result belongs to exactly one caller. The completion object is retained by
    // both the waiting task and the command completion callback, so a timeout can return without
    // leaving a callback that refers to stack storage or a semaphore reused by the next call.
    struct CommandCompletion {
        static constexpr size_t kErrorCapacity = 192;
        SemaphoreHandle_t sem{xSemaphoreCreateBinary()};
        bool completed{false};
        bool success{false};
        tk::CommandFailureInfo failure{};
        uint32_t command_id{0};
        bool drop_link{false};
        bool callback_fault{false};
        std::array<char, kErrorCapacity> error{};

        ~CommandCompletion() { if (sem) vSemaphoreDelete(sem); }
        CommandCompletion() = default;
        CommandCompletion(const CommandCompletion&) = delete;
        CommandCompletion& operator=(const CommandCompletion&) = delete;
    };

    struct CommandOutcome {
        bool completed{false};
        bool success{false};
        tk::CommandFailureInfo failure{};
        std::string error;
    };

    bool ensure_connected_until_(uint32_t deadline, tk::ConnectOrigin origin);

    // Drop the BLE link, reset the in-memory peer sessions, erase the
    // persisted VCSEC/Infotainment sessions, and clear cached vehicle readings.
    // After this has_session() is false until a fresh handshake re-pairs, so the UI
    // and evcc stop serving stale "paired"/SOC data from a defunct pairing.
    // Returns false when the in-memory reset or any persisted-session erase failed. A caller
    // that has already committed a new private key must then leave its cross-namespace journal
    // armed: the new identity is durable, but boot recovery still has cleanup work to finish.
    bool clear_session_and_cache_();
    bool initialize_routing_address_();
    int build_vcsec_attempt_(tk::VcsecReplyKind kind, uint8_t* buffer, size_t* length);
    bool vcsec_response_matches_(const UniversalMessage_RoutableMessage& msg, tk::VcsecReplyKind kind);
    bool deliver_vcsec_response_(const UniversalMessage_RoutableMessage& msg,
                                 bool success, const char* error = "", bool authenticated_rejection = false);
    void handle_empty_vcsec_reply_(const UniversalMessage_RoutableMessage& msg);
    void retire_vcsec_contexts_() noexcept;
    // Every key rotation first commits a marker in tesla_ble. The locked runner removes it only
    // after old sessions are durably erased; boot recovery performs the same cleanup before the
    // vehicle controller can initialize and load/sign with persisted peer state.
    tk::KeyRotationResult generate_key_locked_();
    bool finish_key_rotation_cleanup_();
    bool recover_pending_key_rotation_at_boot_();
    std::string compute_key_fingerprint_();
    std::string key_fingerprint_cache_{};

    // Non-waking VCSEC GET_STATUS health poll. Rejection/error responses are only volatile hints;
    // they never rotate the key or erase sessions. The supervisor may retry the same key later. This probe
    // does not wake the car.
    bool health_probe_(int timeout_ms = 8000);
    // Idle until the next health poll, arming the "next attempt in…" countdown for it.
    void idle_until_next_health_poll_();

    // Enqueue an Infotainment or VCSEC command and wait for the callback. auth_fail_is_health_probe
    // lets the health poll count repeated authentication errors as a volatile UI hint; the
    // hint never mutates the key or stored sessions.
    bool send_vcsec_(const std::string& name, Builder builder,
                     WakePolicy wp, int timeout_ms,
                     tk::ConnectOrigin origin = tk::ConnectOrigin::Foreground,
                     bool auth_fail_is_health_probe = false,
                     tk::CompletionTimeoutPolicy timeout_policy =
                         tk::CompletionTimeoutPolicy::ForegroundWarn,
                     bool completes_on_transmit = false,
                     tk::CommandEffect effect = tk::CommandEffect::Mutating,
                     tk::VcsecReplyKind kind = tk::VcsecReplyKind::Action);
    CommandOutcome send_vcsec_outcome_(const std::string& name, Builder builder,
                     WakePolicy wp, uint32_t deadline, tk::ConnectOrigin origin,
                     bool auth_fail_is_health_probe, tk::CompletionTimeoutPolicy timeout_policy,
                     bool completes_on_transmit, tk::CommandEffect effect, tk::VcsecReplyKind kind);
    bool send_infotainment_(const std::string& name, Builder builder, int timeout_ms,
                            WakePolicy wp = WakePolicy::WakeIfNeeded,
                            tk::CommandOutcomePolicy policy = tk::CommandOutcomePolicy::AlreadySetOnly);
    // Same runner with command_mutex_ + cmd_in_flight_ already held. The absolute deadline
    // includes connection setup and all preceding work in the transaction. Used by
    // set_charging_amps to keep the action and its independent ChargeState readback
    // in one serialized transaction, with no background poll inserted between them.
    CommandOutcome send_infotainment_locked_(const std::string& name, Builder builder,
                                              uint32_t deadline, WakePolicy wp,
                                              tk::CommandEffect effect = tk::CommandEffect::Mutating,
                                              tk::CommandOutcomePolicy policy = tk::CommandOutcomePolicy::AlreadySetOnly);

    CommandOutcome send_vcsec_locked_(const std::string& name, Builder builder,
                                      WakePolicy wp, uint32_t deadline,
                                      tk::ConnectOrigin origin, bool auth_fail_is_health_probe,
                                      tk::CompletionTimeoutPolicy timeout_policy,
                                      bool completes_on_transmit = false,
                                      uint32_t max_retries = 3,
                                      tk::CommandEffect effect = tk::CommandEffect::Mutating,
                                      tk::VcsecReplyKind kind = tk::VcsecReplyKind::Action);

    // Build the per-command result callback. auth_fail_is_health_probe gates whether an
    // "authentication failed" reply contributes to the volatile recovery hint. Neither that
    // hint nor an explicit "whitelist" fault authorizes identity or session mutation.
    ResultCb make_result_cb_(const std::shared_ptr<CommandCompletion>& completion,
                             uint32_t generation,
                             bool auth_fail_is_health_probe = false,
                             bool response_expected = true);
    std::shared_ptr<CommandCompletion> begin_completion_(uint32_t& generation);
    CommandOutcome await_completion_(const std::shared_ptr<CommandCompletion>& completion,
                                     uint32_t generation, uint32_t deadline,
                                     const char* name,
                                     tk::CompletionTimeoutPolicy timeout_policy);
    void note_completion_timeout_(const char* name,
                                  tk::CompletionTimeoutPolicy timeout_policy);
    void invalidate_and_flush_(uint32_t generation);
    void publish_command_outcome_(const CommandOutcome& outcome);
    bool command_identity_ready_() const {
        return key_runtime_safe_.load() && !pairing_cleanup_pending_.load() &&
               !vin_transition_pending_.load() && !key_reload_required_.load();
    }

    // Copy a background-refreshed cache under cache_mutex_ (see cache_mutex_ below). The
    // caches hold std::string members written by vehicle_loop; an unlocked by-value
    // read races the writer (torn string → UB), so all reads/writes take this mutex.
    template <typename T> T copy_locked_(const T& src) {
        if (!cache_mutex_) return src;
        // RAII give — `T copy = src` copies structs with std::string members and can throw
        // std::bad_alloc; a hand-rolled give would then be skipped, wedging cache_mutex_ and
        // freezing every later cache read (issue #204, Scenario B).
        tk::SemGuard g(cache_mutex_);
        return src;
    }

    BleClient*         ble_{nullptr};
    NvsStorageAdapter* storage_{nullptr};
    NvsStorageAdapter* config_store_{nullptr};
    // Set at init when no durable BLE MAC exists. vehicle_loop atomically claims the one
    // best-effort persistence attempt only after it applies LinkUp and publishes readiness;
    // it never writes the main-task-owned std::string passed to init().
    std::atomic<bool>  persist_discovered_mac_{false};
    std::string        vin_;

    std::unique_ptr<TeslaBLE::Client>  client_;
    tk::CommandRunner                  command_runner_;
    std::array<Builder, tk::CommandRunner::kMaxQueueSize> command_builders_{};
    std::function<void(const VCSEC_VehicleStatus&)> vehicle_status_callback_{nullptr};

    void drive_command_runner_();
    void process_rx_frame_(const uint8_t* frame, size_t len);
    bool load_nvs_sessions_();
    tk::SessionRuntimeState runtime_session_observation_locked_(UniversalMessage_Domain domain) noexcept;
    void publish_session_runtime_locked_(UniversalMessage_Domain domain, tk::SessionSaveState save,
                                         uint32_t identity_epoch) noexcept;
    void invalidate_session_runtime_locked_(UniversalMessage_Domain domain) noexcept;
    // Cache epoch is separate from command_generation_: ordinary commands do not change identity.
    std::atomic<uint32_t> session_runtime_bits_{0};
    std::atomic<uint32_t> session_runtime_epoch_{0};
    bool persist_session_(UniversalMessage_Domain domain, const UniversalMessage_RoutableMessage_session_info_t& info);
    bool save_session_runtime_locked_(UniversalMessage_Domain domain,
        const UniversalMessage_RoutableMessage_session_info_t& info, uint32_t identity_epoch) noexcept;
    void establish_verified_session_locked_(UniversalMessage_Domain domain,
        Signatures_SessionInfo& session_info,
        const UniversalMessage_RoutableMessage_session_info_t& payload);
    bool regenerate_key_native_();
    bool enqueue_background_poll_(const std::string& name, tk::BleDomain domain, Builder builder);
    void enqueue_telemetry_round_(tk::TelemetryPollState& telemetry_poll);

    SemaphoreHandle_t vehicle_mutex_{nullptr};
    // Serializes a whole command/query cycle so concurrent HTTP requests and the automatic
    // health/pairing task cannot interleave entries in the command FIFO.
    SemaphoreHandle_t command_mutex_{nullptr};
    // Guards the last_known_* caches below: they hold std::string members written by
    // vehicle_loop after deferred parsing and read by HTTP/MQTT tasks, so an unlocked
    // by-value copy would race the writer (torn string → UB).
    SemaphoreHandle_t cache_mutex_{nullptr};
    // Guards the externally visible result snapshot. Background health/pair operations never
    // publish here, so they cannot replace a foreground request's error between its bool return
    // and the HTTP/MCP layer reading last_command_error().
    SemaphoreHandle_t result_mutex_{nullptr};
    // Failure text from the most recent foreground command (the Tesla "...action failed:
    // <reason>" message), or empty after a success / when no response came back. Lets
    // the HTTP layer report the real reason (e.g. "complete") instead of a generic one.
    std::string       last_error_;

    // Monotonically identifies the one active command request. A timeout increments
    // this before clearing the command runner. Callbacks from
    // the invalidated generation therefore cannot signal or mutate a later request.
    std::atomic<uint32_t> command_generation_{0};

    // regenerate_key() can fail after mutating the in-memory key while the durable NVS key
    // remains old (or absent on first boot). Until a successful persisted generation or reboot
    // reloads the key from storage, no command may enrol that ambiguous runtime identity.
    std::atomic<bool> key_runtime_safe_{false};
    // A private-key write could not confirm its NVS commit. The durable
    // fingerprint is now unknowable from this controller instance (which may have restored the old
    // RAM key), so block signing AND all further rotations until boot reloads storage.
    std::atomic<bool> key_reload_required_{false};
    std::atomic<tk::AutomaticKeyAction> automatic_key_action_{tk::AutomaticKeyAction::Continue};
    // Set only when this boot consumed a persisted key_rotate marker. main uses it to prevent
    // an absent durable key from entering an automatic CommitUnknown reboot loop.
    bool key_rotation_recovered_at_boot_{false};
    // The new key is durable, but session/cache erasure was incomplete. Auto-pair retries only
    // that idempotent cleanup; it must not generate a different key on every retry.
    std::atomic<bool> pairing_cleanup_pending_{false};
    // The stored key_created date belongs to a previous key and could not be replaced or erased
    // when the key rotated; key_created_at() reports 0 (unknown) while this is set.
    std::atomic<bool> key_created_untrusted_{false};
    // Set before /set_vin stages its cross-namespace journal and held until the mandatory reboot.
    // It closes the otherwise small unlock→HTTP-rollback/reboot window in which auto-pair or a
    // telemetry task could sign/re-key using a request whose persisted VIN is still in flight.
    std::atomic<bool> vin_transition_pending_{false};

    // Set from status/result callbacks when a response suggests enrolment needs attention. It
    // is advisory, may be based on ambiguous remote text, and can never authorize key/session
    // mutation. A successful health response clears it. atomic: cross-task flag.
    std::atomic<bool> pairing_recovery_hint_{false};

    // Consecutive "authentication failed" responses from the VCSEC health probe ONLY.
    // The same response can also mean a role refusal, so it is only a volatile recovery hint,
    // never evidence for key/session mutation. Two consecutive failures are required; success
    // and BLE disconnect reset the streak. atomic: cross-task.
    std::atomic<int> auth_fail_streak_{0};

    // True only while a session is stored and the supervisor is in its health-check phase.
    // Gates the message observer so expected faults during automatic enrollment do not create
    // a recovery hint.
    std::atomic<bool> believed_paired_{false};

    // Sticky "pairing needs attention" notice for the UI. A stored session alone does not
    // clear it; a successful health response does. atomic: written/read across tasks.
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
    // The ONE active-window decision (command recency OR charging with FRESH contact, see
    // logic/active_window.hpp). The background poll and get_charge_state() must both use it: if the
    // getter kept a window open that the poll had already closed, it would demand a fresh
    // ChargeState nothing refreshes any more and answer evcc with a permanent 503.
    bool active_window_now_(uint32_t now_ticks, bool charging_state) const;

    // Set when an unexpected error occurs during BLE rx framing or processing.
    // Exceptions are caught at our boundary and set this; loop_task then drops the BLE link once to
    // reset RX framing and re-sync sessions, turning an error into a brief reconnect.
    std::atomic<bool> ble_fault_{false};

    // True while a serialized command/query (including the VCSEC health probe) is enqueued and
    // awaiting its result. loop_task reads it and SKIPS injecting background telemetry polls
    // (charge/climate/drive/tires/closures) for the duration, so a command isn't stuck behind
    // freshly-queued, 7-s-failing polls in the command FIFO (the cause of 15-19 s command
    // latency on an awake, busy link). This is FIFO arbitration only: it must never be reused to
    // decide whether the attempt was foreground or background.
    // Atomic: set by the HTTP task, read by loop_task. Managed by an RAII guard so it always
    // clears, even if the library call throws.
    std::atomic<bool> cmd_in_flight_{false};

    // Why the last ensure_connected_() attempt failed and how many times in a row, so a car
    // that is simply parked elsewhere states that once and then on a slow heartbeat instead of
    // an ERROR line every 40 s forever (logic/connect_outcome.hpp holds the rule and the
    // measurement that motivated it). NOT atomic and deliberately so: every ensure_connected_
    // caller (send_vcsec_, send_infotainment_, get_vehicle_status, enroll_existing_key_) holds command_mutex_
    // for the whole attempt, so this is single-writer under that lock — the same lock that
    // already serializes connect attempts themselves. Adding an atomic here would imply a
    // concurrency that does not exist and hide that invariant.
    tk::ConnectFailState connect_fail_{};
    tk::CompletionTimeoutState completion_timeout_{};

    // Consecutive command failures reported through make_result_cb_ (foreground commands +
    // the VCSEC health poll). On an awake, busy link an unrecoverable desync or repeated auth
    // failure can cause consecutive timeouts. After kCmdFailDropStreak
    // failures in a row, while paired, we proactively raise ble_fault_ to drop the link once:
    // a clean link and session resync, driven by soft failures instead of a
    // throw. Reset on any success. Paired-gated so it can't disturb the enrolment handshake
    // (where command failures are expected). Background telemetry-poll failures do NOT reach
    // make_result_cb_, so this counts commands + health poll only — a deliberate backstop.
    std::atomic<int> cmd_fail_streak_{0};
    static constexpr int kCmdFailDropStreak = 3;

    // Uptime tick of the last live infotainment data received (see seconds_since_contact).
    // Stamped after vehicle_loop publishes deferred caches; read by HTTP/MQTT tasks. atomic so no
    // lock is needed. 0 = nothing received yet. Cleared on a pairing reset.
    std::atomic<uint32_t> last_contact_ticks_{0};
    // ChargeState-specific freshness + generation. last_contact_ticks_ also advances for
    // climate/drive/etc., so it cannot prove that the current-limit readback is fresh.
    // Generation changes on every published public ChargeState and gates active-window freshness.
    // set_charging_amps uses the earlier fixed ChargingAmpsFeedback generation instead, so command
    // completion cannot outrun deferred string-cache publication.
    std::atomic<uint32_t> last_charge_ticks_{0};
    std::atomic<uint32_t> charge_state_generation_{0};
    // ClimateState freshness stamp + generation (same pattern as the charge pair): bound the age of
    // the climate cache that evcc's climater reads inside the active window. Generation != 0 is the
    // "have sample" bit; both are cleared on a pairing reset.
    std::atomic<uint32_t> last_climate_ticks_{0};
    std::atomic<uint32_t> climate_state_generation_{0};
    std::atomic<bool>     charge_cache_stale_reported_{false};
    // Uptime tick of the last time the car was confirmed REACHABLE over BLE — any successful
    // command response or accepted status observation, including the idle VCSEC health poll while the
    // car merely sleeps nearby (the body controller is always on). This is what tells a
    // parked, sleeping car (reachable) apart from one that has driven off / is out of range
    // (unreachable) — telemetry freshness alone cannot, since both stop the live polls.
    // See link_state(). 0 = never reached yet; cleared on a pairing reset.
    std::atomic<uint32_t> last_reachable_ticks_{0};
    // Live data implies the car is reachable, so stamp both clocks together.
    void note_contact_()   { uint32_t t = xTaskGetTickCount(); last_contact_ticks_.store(t);
                                                               last_reachable_ticks_.store(t); }
    void note_reachable_() { last_reachable_ticks_.store(xTaskGetTickCount()); }

    // VCSEC sleep-flag debounce (see link_state()). The car's body controller reports a
    // vehicleSleepStatus on every VCSEC poll — including the idle health probe. The RX path
    // updates vcsec_sleep_state_ directly. We sample it in loop_task and mark
    // here the START tick of an uninterrupted ASLEEP run (0 = not currently ASLEEP). The flag
    // can flap AWAKE↔ASLEEP (~60 s) while Cabin-Overheat-Protection cycles the A/C, so a
    // single ASLEEP reading is NOT proof of sleep; link_state() only treats it as asleep once
    // the run has held for kAsleepDebounceS, which filters those blips. UNKNOWN also breaks
    // proof. Cleared on a pairing reset (clear_session_and_cache_). The clock rules live in
    // tk::next_asleep_since() / tk::vcsec_asleep_proven() (logic/command_runner.hpp), which
    // the host tests cover; the members below only hold the atomics those functions read.
    std::atomic<uint32_t> vcsec_asleep_since_ticks_{0};
    // vcsec_sleep_state_ is updated from incoming VCSEC frames under vehicle_mutex_; status
    // and HTTP tasks read this atomic mirror so there is no data race.
    std::atomic<int> vcsec_sleep_state_{static_cast<int>(tk::SleepState::Unknown)};

    // Thread-safe single-owner RxFramer reset request flag (consumed strictly by vehicle_loop)
    std::atomic<bool> rx_framer_reset_requested_{false};
    void request_framer_reset_() noexcept {
        rx_framer_reset_requested_.store(true, std::memory_order_release);
    }

    // Reusable single-allocation buffer for command transmission (builder wire format without double length)
    std::vector<uint8_t> tx_buffer_{};
    // Vehicle-lock-owned routing state. Ordinary builders retain one stable address; routable
    // VCSEC attempts temporarily select a fresh address and restore this one before unlock.
    tk::BleUuid ordinary_routing_address_{};
    tk::BleUuid previous_vcsec_reply_address_{};
    bool ordinary_routing_address_ready_{false};
    // One serialized request slot with distinct auth/payload/wake phase admission.
    tk::VcsecReplyContext vcsec_reply_context_{};
    uint32_t vcsec_attempt_counter_{0};  // Vehicle-lock-owned; never reset by retirement/flush.
    // Fold one VCSEC sleep reading into the debounce clock. Takes the SleepState itself, never a
    // pre-reduced bool, so no call site can skip UNKNOWN: tk::next_asleep_since() ends the run on
    // AWAKE and UNKNOWN alike. Lock-free read-modify-write on one 32-bit atomic, like
    // net.cpp's lease generations (a narrow atomic would pull in a runtime helper on C3).
    void note_vcsec_sleep_(tk::SleepState reported) {
        const uint32_t now = xTaskGetTickCount();
        uint32_t since = vcsec_asleep_since_ticks_.load();
        while (!vcsec_asleep_since_ticks_.compare_exchange_weak(
            since, tk::next_asleep_since(since, reported, now))) {}
    }
    void mark_vcsec_unknown_() {
        vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Unknown));
        note_vcsec_sleep_(tk::SleepState::Unknown);
    }
    // True once the car's current report is still ASLEEP and that run has held uninterrupted for
    // at least debounce_s seconds (tk::vcsec_asleep_proven).
    bool vcsec_stably_asleep_(uint32_t debounce_s) const {
        return tk::vcsec_asleep_proven(
            static_cast<tk::SleepState>(vcsec_sleep_state_.load()),
            vcsec_asleep_since_ticks_.load(), xTaskGetTickCount(), configTICK_RATE_HZ,
            debounce_s);
    }

    // Cached results for non-blocking UI access
    ChargeStateResult   last_known_charge_{};
    VehicleStatusResult last_known_status_{};
    ClimateStateResult  last_known_climate_{};
    DriveStateResult    last_known_drive_{};
    TirePressureResult  last_known_tires_{};
    ClosuresStateResult last_known_closures_{};

    // ── BLE phase deadlines (see ble_phase()) ─────────────────────────────────────
    // Tick counts, 0 = not armed. Each is written only by the code that owns that
    // phase — ensure_connected_ owns the connect attempt, auto_pair_task_fn_ owns the
    // idle wait — so neither can clear the other's countdown. That matters when both
    // run at once: a command's connect attempt inside auto-pair's idle wait shows its
    // own "gives up in" countdown, and when it ends the still-running idle wait's
    // "next attempt in" countdown reappears instead of the row going bare.
    std::atomic<uint32_t> connect_deadline_{0};
    std::atomic<uint32_t> retry_deadline_{0};
    // Arm a deadline `ms` from now. Tick 0 is the "not armed" sentinel, so a deadline
    // that lands exactly on a tick-counter wrap is nudged by one tick.
    static uint32_t deadline_in_(uint32_t ms) {
        uint32_t d = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
        return d ? d : 1;
    }
    static TickType_t ticks_until_(uint32_t deadline) {
        int32_t left = static_cast<int32_t>(deadline - xTaskGetTickCount());
        return left > 0 ? static_cast<TickType_t>(left) : 0;
    }
    static int remaining_ms_(uint32_t deadline) {
        TickType_t ticks = ticks_until_(deadline);
        return ticks > 0 ? static_cast<int>(ticks * portTICK_PERIOD_MS) : 0;
    }
    static uint32_t capped_deadline_(uint32_t deadline, uint32_t max_ms) {
        // Compare both ends against one snapshot: preemption between separate remaining-time
        // reads can otherwise select a cap later than an already expired outer deadline.
        return tk::capped_command_deadline(deadline, xTaskGetTickCount(), pdMS_TO_TICKS(max_ms));
    }

    // xTaskCreate() can schedule the first task before the second allocation is attempted. This
    // gate keeps both entry functions quiescent until both handles exist and provides a
    // cooperative Cancel/Ack/Self-delete path for a second-create OOM.
    tk::DualTaskStartGate task_start_gate_{};
    bool await_task_start_();

    TaskHandle_t loop_task_{nullptr};
    static void loop_task_fn_(void* arg);

    // Pairing supervisor: probe first, then automatically submit the existing Charging Manager
    // key with backoff when no durable/runtime session exists or a recovery hint persists.
    bool poll_existing_key_();
    tk::AutoEnrollmentSession enrollment_session_snapshot_(TickType_t wait_ticks);
    bool enroll_existing_key_(int timeout_ms = 6000);
    TaskHandle_t auto_pair_task_{nullptr};
    static void auto_pair_task_fn_(void* arg);
};
