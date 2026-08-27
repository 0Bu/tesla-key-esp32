#pragma once

#include <vehicle.h>
#include <client.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
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
#include "reboot_reason.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "logic/vehicle_data.hpp"

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
    // Origin is mandatory: HTTP/manual callers pass Foreground; the auto-pair supervisor
    // passes Background so an absent car cannot turn unattended probes into an error stream.
    bool get_vehicle_status(VehicleStatusResult& out, tk::ConnectOrigin origin,
                            int timeout_ms = 20000);

    // Non-blocking accessors for cached state (refreshed in background; copied under
    // cache_mutex_ because the BLE RX task writes these concurrently — see cache_mutex_).
    ChargeStateResult   get_cached_charge()   { return copy_locked_(last_known_charge_); }
    VehicleStatusResult get_cached_status()   { return copy_locked_(last_known_status_); }
    ClimateStateResult  get_cached_climate()  { return copy_locked_(last_known_climate_); }
    DriveStateResult    get_cached_drive()    { return copy_locked_(last_known_drive_); }
    TirePressureResult  get_cached_tires()    { return copy_locked_(last_known_tires_); }
    ClosuresStateResult get_cached_closures() { return copy_locked_(last_known_closures_); }

    // Vehicle-owned half of the on-device status-indicator input (the ST7735 display and the
    // APA102 LED) — see logic/ui_state.hpp. Collects link_state / ble_* / cached charge in ONE
    // pass so a presenter never mixes state from different instants across a frame. The caller
    // fills the WiFi fields (esp_wifi) and `paired` (its own ≤1 Hz has_session() sample — NVS).
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

    // Seconds since the car was last REACHABLE over BLE (any successful signed round-trip,
    // incl. the idle VCSEC health poll). Returns false if never reached since boot / re-pair.
    // Unlike seconds_since_contact this keeps refreshing while the car only sleeps nearby
    // (the health poll still answers), so a stale value means genuinely unreachable.
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
    LinkState link_state() const;

    // Current RAW VCSEC sleep belief from the library (updated on every VCSEC poll, incl. the
    // idle health probe): "AWAKE" / "ASLEEP" / "UNKNOWN". Diagnostic/transparency only — the
    // hero uses link_state(), which DEBOUNCES this (a single ASLEEP blip is not yet "asleep").
    const char* vcsec_sleep_raw() const {
        switch (static_cast<TeslaBLE::SleepState>(vcsec_sleep_state_.load())) {
            case TeslaBLE::SleepState::ASLEEP: return "ASLEEP";
            case TeslaBLE::SleepState::AWAKE:  return "AWAKE";
            default:                           return "UNKNOWN";
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
    // Auto-pair-only wrapper: re-samples revocation, durable-key presence and runtime safety
    // together under command_mutex_ before it authorizes mutation.
    bool generate_key();
    // Always enrolls a Charging Manager key (charging + wake only); never an owner key.
    bool pair(tk::ConnectOrigin origin, int timeout_ms = 30000);

    struct NewVehicleResetResult {
        tk::VinTransitionApply state{tk::VinTransitionApply::IdentityUnverified};
        std::string previous_key_id;
    };
    using VinTransitionStager = std::function<bool(const std::string& previous_key_id)>;

    // Re-point the device at a different vehicle as one command-FIFO transaction. The staging
    // callback persists the VIN journal + complete ConfigBlob while command_mutex_ still binds
    // `previous_key_id`; auto-rekey therefore cannot change the fingerprint between staging,
    // rotation and result classification. Any staged attempt gates all signing until the caller
    // reboots, because this Vehicle instance still owns the old in-memory VIN.
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
    TeslaBLE::Vehicle* vehicle() { return vehicle_.get(); }

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

    // NOTE: generic runtime-config persistence deliberately does NOT live here. The HTTP
    // layer talks to the tesla_cfg store directly (g_config in http_handlers.hpp) — the
    // controller's config_store_ is only for facts the controller itself owns (the
    // discovered BLE MAC, reset_for_new_vehicle()'s cleanup of it, and the reboot reason
    // below, which loop_task is the only writer of).

private:
    // Record WHY we are about to restart ourselves, so the reason survives into the next boot
    // (see take_reboot_reason()). Written from loop_task's heap watchdog on a heap that is by
    // definition failing. Restart authorization is fail-closed: without a durable next counter,
    // rebooting would erase the five-cycle cap and could reopen the BLE/radio window forever.
    bool persist_reboot_reason_(const char* why) noexcept {
        return tk::persist_reboot_reason(config_store_, why);
    }

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
    // Builder function type used by send_command_result
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    using ResultCb = TeslaBLE::Command::OperationResultCallback;

    // A command result belongs to exactly one caller. The completion object is retained by
    // both the waiting task and tesla-ble's queued callback, so a timeout can return without
    // leaving a callback that refers to stack storage or a semaphore reused by the next call.
    struct CommandCompletion {
        SemaphoreHandle_t sem{xSemaphoreCreateBinary()};
        bool completed{false};
        bool success{false};
        std::string error;

        ~CommandCompletion() { if (sem) vSemaphoreDelete(sem); }
        CommandCompletion() = default;
        CommandCompletion(const CommandCompletion&) = delete;
        CommandCompletion& operator=(const CommandCompletion&) = delete;
    };

    struct CommandOutcome {
        bool completed{false};
        bool success{false};
        std::string error;
    };

    // Install the persistent set_*_state_callback hooks that keep the last_known_*
    // caches fresh (charge + the read-only telemetry domains). Called once from init();
    // lives in vehicle_telemetry.cpp next to the protobuf→struct parsers it uses.
    void install_state_callbacks_();

    bool ensure_connected_until_(uint32_t deadline, tk::ConnectOrigin origin);

    // Drop the BLE link, reset the library's in-memory peer sessions, erase the
    // persisted VCSEC/Infotainment sessions, and clear cached vehicle readings.
    // After this has_session() is false until a fresh handshake re-pairs, so the UI
    // and evcc stop serving stale "paired"/SOC data from a defunct pairing.
    // Returns false when the in-memory reset or any persisted-session erase failed. A caller
    // that has already committed a new private key must then leave its cross-namespace journal
    // armed: the new identity is durable, but boot recovery still has cleanup work to finish.
    bool clear_session_and_cache_();
    // Every key rotation first commits a marker in tesla_ble. The locked runner removes it only
    // after old sessions are durably erased; boot recovery performs the same cleanup before a
    // TeslaBLE::Vehicle can be constructed and load/sign with persisted peer state.
    tk::KeyRotationResult generate_key_locked_();
    bool finish_key_rotation_cleanup_();
    bool recover_pending_key_rotation_at_boot_();

    // Signed VCSEC GET_STATUS poll used purely to detect that our key was deleted on the
    // car side (the response then carries KEY_NOT_ON_WHITELIST, or a tagless session-info →
    // "authentication failed", which trips pairing_lost_ via make_result_cb_). It is the
    // ONLY caller that passes auth_fail_is_revocation, because GET_STATUS is the one signed
    // command every key role may run — so an auth failure here cannot be a role refusal.
    // Benign/read-only; does not wake the car.
    bool health_probe_(int timeout_ms = 8000);
    // Idle until the next health poll, arming the "next attempt in…" countdown for it.
    void idle_until_next_health_poll_();

    // Enqueue an Infotainment or VCSEC command and wait for the callback. auth_fail_is_revocation
    // forwards to make_result_cb_: set ONLY for the health probe, so a role-denied user command
    // (door/flash/honk/climate/sentry on a Charging-Manager key — all answered "authentication
    // failed") cannot be mistaken for a revocation and destroy the pairing.
    bool send_vcsec_(const std::string& name, Builder builder,
                     TeslaBLE::WakePolicy wp, int timeout_ms,
                     tk::ConnectOrigin origin = tk::ConnectOrigin::Foreground,
                     bool auth_fail_is_revocation = false,
                     tk::CompletionTimeoutPolicy timeout_policy =
                         tk::CompletionTimeoutPolicy::ForegroundWarn);
    bool send_infotainment_(const std::string& name, Builder builder, int timeout_ms,
                            TeslaBLE::WakePolicy wp = TeslaBLE::WakePolicy::WAKE_IF_NEEDED);
    // Same runner with command_mutex_ + cmd_in_flight_ already held. The absolute deadline
    // includes connection setup and all preceding work in the transaction. Used by
    // set_charging_amps to keep the action and its independent ChargeState readback
    // in one serialized transaction, with no background poll inserted between them.
    CommandOutcome send_infotainment_locked_(const std::string& name, Builder builder,
                                              uint32_t deadline, TeslaBLE::WakePolicy wp);

    CommandOutcome send_vcsec_locked_(const std::string& name, Builder builder,
                                      TeslaBLE::WakePolicy wp, uint32_t deadline,
                                      tk::ConnectOrigin origin, bool auth_fail_is_revocation,
                                      tk::CompletionTimeoutPolicy timeout_policy);

    // Build the per-command result callback. auth_fail_is_revocation gates whether an
    // "authentication failed" reply may count toward the two-strike pairing_lost_ heuristic
    // (true only for the authorised health probe); an explicit "whitelist" fault always trips.
    ResultCb make_result_cb_(const std::shared_ptr<CommandCompletion>& completion,
                             uint32_t generation,
                             bool auth_fail_is_revocation = false);
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
    // caches hold std::string members written from the BLE RX task; an unlocked by-value
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
    // Set at init when no durable BLE MAC exists. The NimBLE host task atomically claims the
    // one best-effort persistence attempt after its first successful connection; it never
    // writes the main-task-owned std::string passed to init().
    std::atomic<bool>  persist_discovered_mac_{false};
    std::string        vin_;

    std::unique_ptr<TeslaBLE::Vehicle> vehicle_;

    SemaphoreHandle_t vehicle_mutex_{nullptr};
    // Serializes a whole command/query cycle so concurrent HTTP requests and the automatic
    // health/pairing task cannot interleave entries in tesla-ble's single FIFO.
    SemaphoreHandle_t command_mutex_{nullptr};
    // Guards the last_known_* caches below: they hold std::string members written from
    // the BLE RX task (parse_* callbacks) and read by the HTTP task, so an unlocked
    // by-value copy would race the writer (torn string → UB).
    SemaphoreHandle_t cache_mutex_{nullptr};
    // Guards the externally visible result snapshot. Background health/pair operations never
    // publish here, so they cannot replace a foreground request's error between its bool return
    // and the HTTP/MCP layer reading last_command_error().
    SemaphoreHandle_t result_mutex_{nullptr};
    // Failure text from the most recent signed command (the Tesla "...action failed:
    // <reason>" message), or empty after a success / when no response came back. Lets
    // the HTTP layer report the real reason (e.g. "complete") instead of a generic one.
    std::string       last_error_;

    // Monotonically identifies the one active tesla-ble FIFO request. A timeout increments
    // this BEFORE set_connected(false) synchronously finalises queued callbacks. Callbacks from
    // the invalidated generation therefore cannot signal or mutate a later request.
    std::atomic<uint32_t> command_generation_{0};

    // regenerate_key() can fail after mutating the in-memory key while the durable NVS key
    // remains old (or absent on first boot). Until a successful persisted generation or reboot
    // reconstructs Vehicle from storage, no command may enrol that ambiguous runtime identity.
    std::atomic<bool> key_runtime_safe_{false};
    // tesla-ble attempted a private-key write but could not confirm its NVS commit. The durable
    // fingerprint is now unknowable from this Vehicle instance (which may have restored the old
    // RAM key), so block signing AND all further rotations until boot reloads storage.
    std::atomic<bool> key_reload_required_{false};
    // Set only when this boot consumed a persisted key_rotate marker. main uses it to prevent
    // an absent durable key from entering an automatic CommitUnknown reboot loop.
    bool key_rotation_recovered_at_boot_{false};
    // The new key is durable, but session/cache erasure was incomplete. Auto-pair retries only
    // that idempotent cleanup; it must not generate a different key on every retry.
    std::atomic<bool> pairing_cleanup_pending_{false};
    // Set before /set_vin stages its cross-namespace journal and held until the mandatory reboot.
    // It closes the otherwise small unlock→HTTP-rollback/reboot window in which auto-pair or a
    // telemetry task could sign/re-key using a request whose persisted VIN is still in flight.
    std::atomic<bool> vin_transition_pending_{false};

    // Set from a command callback (possibly the BLE RX task) when the vehicle reports
    // KEY_NOT_ON_WHITELIST — i.e. our key was removed on the car side. The auto-pair
    // supervisor consumes it to re-key and restart pairing. atomic: cross-task flag.
    std::atomic<bool> pairing_lost_{false};

    // Consecutive "authentication failed" responses from the signed VCSEC health probe ONLY.
    // A car whose whitelist no longer holds our key replies to a signed command with an
    // untagged session-info → "auth response authentication failed" (not KEY_NOT_ON_WHITELIST).
    // That same message, however, is also the car's answer to a command the key's role is not
    // allowed to run, so it is counted ONLY for the health probe (auth_fail_is_revocation) — a
    // GET_STATUS every role may issue — never for role-deniable user commands. Two in a row are
    // required before pairing_lost_ is set (one-off glitch guard) — and they must be consecutive
    // on ONE continuous link, so the streak is reset on any successful response AND on a BLE
    // disconnect (a flaky link dropping between two glitches is not a revocation). atomic: cross-task.
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

    // True while a serialized command/query (including the VCSEC health probe) is enqueued and
    // awaiting its result. loop_task reads it and SKIPS injecting background telemetry polls
    // (charge/climate/drive/tires/closures) for the duration, so a command isn't stuck behind
    // freshly-queued, 7-s-failing polls in the library's single FIFO (the cause of 15-19 s command
    // latency on an awake, busy link). This is FIFO arbitration only: it must never be reused to
    // decide whether the attempt was foreground or background.
    // Atomic: set by the HTTP task, read by loop_task. Managed by an RAII guard so it always
    // clears, even if the library call throws.
    std::atomic<bool> cmd_in_flight_{false};

    // Why the last ensure_connected_() attempt failed and how many times in a row, so a car
    // that is simply parked elsewhere states that once and then on a slow heartbeat instead of
    // an ERROR line every 40 s forever (logic/connect_outcome.hpp holds the rule and the
    // measurement that motivated it). NOT atomic and deliberately so: every ensure_connected_
    // caller (send_vcsec_, send_infotainment_, get_vehicle_status, pair) holds command_mutex_
    // for the whole attempt, so this is single-writer under that lock — the same lock that
    // already serializes connect attempts themselves. Adding an atomic here would imply a
    // concurrency that does not exist and hide that invariant.
    tk::ConnectFailState connect_fail_{};
    tk::CompletionTimeoutState completion_timeout_{};

    // Consecutive failed signed round-trips seen in make_result_cb_ (foreground commands +
    // the VCSEC health poll). On an awake, busy link the tesla-ble framer's single rx buffer
    // can desync ("buffer recovery failed") and most ops time out, but the library RECOVERS
    // internally WITHOUT throwing — so ble_fault_ above never fires and the storm can persist
    // for minutes (stale telemetry + multi-second command latency). After kCmdFailDropStreak
    // failures in a row, while paired, we proactively raise ble_fault_ to drop the link once:
    // the same clean rx-buffer/session resync, just driven by soft failures instead of a
    // throw. Reset on any success. Paired-gated so it can't disturb the enrolment handshake
    // (where command failures are expected). Background telemetry-poll failures do NOT reach
    // make_result_cb_, so this counts commands + health poll only — a deliberate backstop.
    std::atomic<int> cmd_fail_streak_{0};
    static constexpr int kCmdFailDropStreak = 3;

    // Uptime tick of the last live infotainment data received (see seconds_since_contact).
    // Stamped from the cache callbacks (BLE RX task); read by the HTTP task. atomic so no
    // lock is needed. 0 = nothing received yet. Cleared on a pairing reset.
    std::atomic<uint32_t> last_contact_ticks_{0};
    // ChargeState-specific freshness + generation. last_contact_ticks_ also advances for
    // climate/drive/etc., so it cannot prove that the current-limit readback is fresh.
    // Generation changes on every decoded ChargeState and lets set_charging_amps distinguish
    // its explicit post-command poll from an older cached value.
    std::atomic<uint32_t> last_charge_ticks_{0};
    std::atomic<uint32_t> charge_state_generation_{0};
    std::atomic<bool>     charge_cache_stale_reported_{false};
    // Uptime tick of the last time the car was confirmed REACHABLE over BLE — any successful
    // signed round-trip, including the idle VCSEC health poll that keeps answering while the
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
    // vehicleSleepStatus on every VCSEC poll — including the idle health probe — which the
    // tesla-ble library tracks in Vehicle::sleep_state(). We sample it in loop_task and mark
    // here the START tick of an uninterrupted ASLEEP run (0 = not currently ASLEEP). The flag
    // can flap AWAKE↔ASLEEP (~60 s) while Cabin-Overheat-Protection cycles the A/C, so a
    // single ASLEEP reading is NOT proof of sleep; link_state() only treats it as asleep once
    // the run has held for kAsleepDebounceS, which filters those blips. Cleared on a pairing
    // reset (clear_session_and_cache_).
    std::atomic<uint32_t> vcsec_asleep_since_ticks_{0};
    // Vehicle::sleep_state() is written by tesla-ble from the BLE RX task. Sampling that field
    // directly from loop/status tasks is a C++ data race, so RX publishes this atomic mirror
    // while vehicle_mutex_ is held and every other task reads only the mirror.
    std::atomic<int> vcsec_sleep_state_{static_cast<int>(TeslaBLE::SleepState::UNKNOWN)};
    // Fold one sampled VCSEC sleep reading into the debounce clock. ASLEEP starts/continues
    // the run (keeping its original start tick); AWAKE breaks it. UNKNOWN is not passed here
    // (the caller leaves the clock untouched so a transient unknown can't reset a real run).
    void note_vcsec_sleep_(bool asleep) {
        if (asleep) { uint32_t z = 0; vcsec_asleep_since_ticks_.compare_exchange_strong(z, xTaskGetTickCount()); }
        else        { vcsec_asleep_since_ticks_.store(0); }
    }
    // True once the VCSEC ASLEEP run has held uninterrupted for at least debounce_s seconds.
    bool vcsec_stably_asleep_(uint32_t debounce_s) const {
        uint32_t t = vcsec_asleep_since_ticks_.load();
        if (t == 0) return false;
        return ((xTaskGetTickCount() - t) / configTICK_RATE_HZ) >= debounce_s;
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
        uint32_t cap = deadline_in_(max_ms);
        return ticks_until_(deadline) <= ticks_until_(cap) ? deadline : cap;
    }

    // xTaskCreate() can schedule the first task before the second allocation is attempted. This
    // gate keeps both entry functions quiescent until both handles exist and provides a
    // cooperative Cancel/Ack/Self-delete path for a second-create OOM.
    tk::DualTaskStartGate task_start_gate_{};
    bool await_task_start_();

    TaskHandle_t loop_task_{nullptr};
    static void loop_task_fn_(void* arg);

    // Automatic pairing: while not paired, periodically connect to the configured
    // vehicle and (re)send the whitelist-add so the car keeps prompting "Add key?".
    // Once the user taps the key card, the VCSEC handshake establishes a session and
    // the loop goes idle. No manual scan/pair needed.
    TaskHandle_t auto_pair_task_{nullptr};
    static void auto_pair_task_fn_(void* arg);
};
