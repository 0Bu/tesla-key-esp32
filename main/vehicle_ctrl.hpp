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
#include "logic/ble_phase.hpp"
#include "logic/link_state.hpp"
#include "logic/ui_state.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "logic/vehicle_data.hpp"

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
        if (!vehicle_) return "UNKNOWN";
        switch (vehicle_->sleep_state()) {
            case TeslaBLE::SleepState::ASLEEP: return "ASLEEP";
            case TeslaBLE::SleepState::AWAKE:  return "AWAKE";
            default:                           return "UNKNOWN";
        }
    }

    bool generate_key();
    // Always enrolls a Charging Manager key (charging + wake only); never an owner key.
    bool pair(int timeout_ms = 30000);

    // Re-point the device at a different vehicle (VIN change): regenerate the key,
    // drop the now-orphaned session + cached data, and forget the discovered BLE MAC
    // (it belongs to the old car). The caller is expected to reboot afterwards so the
    // new VIN takes effect. Safe to call regardless of current pairing state.
    bool reset_for_new_vehicle();

    const std::string& vin() const { return vin_; }
    // A plausible Tesla VIN is exactly 17 chars, uppercase alphanumeric with I/O/Q excluded
    // (the VIN standard reserves them). Mirrors the client-side check in www/app.js and the
    // /set_vin validation. Pairing is gated on this: the device never connects/enrols on a
    // vehicle without a real configured VIN (the boot placeholder "UNKNOWN" is not plausible).
    static bool vin_is_plausible(const std::string& vin);
    bool has_plausible_vin() const { return vin_is_plausible(vin_); }
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
    void ble_scan(int ms = 12000) { if (ble_) ble_->start_discovery(ms); }
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
        return tk::ble::phase({connect_deadline_.load(), connect_total_s_.load()},
                              {retry_deadline_.load(),   retry_total_s_.load()},
                              xTaskGetTickCount(), configTICK_RATE_HZ);
    }
    // Reason the most recent command failed (e.g. "complete", "not_charging"), or empty
    // if it succeeded or got no response at all (car unreachable / timed out). Lets the
    // UI tell "the car rejected this" apart from "the car couldn't be reached".
    std::string last_command_error() const { return last_error_; }

    // NOTE: generic runtime-config persistence deliberately does NOT live here. The HTTP
    // layer talks to the tesla_cfg store directly (g_config in http_handlers.hpp) — the
    // controller's config_store_ is only for facts the controller itself owns (the
    // discovered BLE MAC, and reset_for_new_vehicle()'s cleanup of it).

private:
    // Builder function type used by send_command_result
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    using ResultCb = TeslaBLE::Command::OperationResultCallback;

    // Install the persistent set_*_state_callback hooks that keep the last_known_*
    // caches fresh (charge + the read-only telemetry domains). Called once from init();
    // lives in vehicle_telemetry.cpp next to the protobuf→struct parsers it uses.
    void install_state_callbacks_();

    bool ensure_connected_(int timeout_ms = 10000);

    // Drop the BLE link, reset the library's in-memory peer sessions, erase the
    // persisted VCSEC/Infotainment sessions, and clear cached vehicle readings.
    // After this has_session() is false until a fresh handshake re-pairs, so the UI
    // and evcc stop serving stale "paired"/SOC data from a defunct pairing.
    void clear_session_and_cache_();

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
                     bool count_as_activity = true, bool auth_fail_is_revocation = false);
    bool send_infotainment_(const std::string& name, Builder builder, int timeout_ms,
                            TeslaBLE::WakePolicy wp = TeslaBLE::WakePolicy::WAKE_IF_NEEDED);

    // Build the per-command result callback. auth_fail_is_revocation gates whether an
    // "authentication failed" reply may count toward the two-strike pairing_lost_ heuristic
    // (true only for the authorised health probe); an explicit "whitelist" fault always trips.
    ResultCb make_result_cb_(bool auth_fail_is_revocation = false);

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

    // True while a foreground command (evcc/manual, via send_vcsec_/send_infotainment_) is
    // enqueued and awaiting its result. loop_task reads it and SKIPS injecting background
    // telemetry polls (charge/climate/drive/tires/closures) for the duration, so a command
    // isn't stuck behind freshly-queued, 7-s-failing polls in the library's single FIFO
    // (the cause of 15-19 s command latency on an awake, busy link). The ~30 s VCSEC health
    // poll is already serialized via command_mutex_ and can't pile up, so it needs no gating.
    // Atomic: set by the HTTP task, read by loop_task. Managed by an RAII guard so it always
    // clears, even if the library call throws.
    std::atomic<bool> cmd_in_flight_{false};

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

    // Pending status result stored as a member to avoid lambda capturing stack refs
    VehicleStatusResult pending_status_{};

    // ── BLE phase deadlines (see ble_phase()) ─────────────────────────────────────
    // Tick counts, 0 = not armed. Each is written only by the code that owns that
    // phase — ensure_connected_ owns the connect attempt, auto_pair_task_fn_ owns the
    // idle wait — so neither can clear the other's countdown. That matters when both
    // run at once: a command's connect attempt inside auto-pair's idle wait shows its
    // own "gives up in" countdown, and when it ends the still-running idle wait's
    // "next attempt in" countdown reappears instead of the row going bare.
    // Each deadline carries the phase's FULL length alongside it, so the UI can draw how
    // much of the phase is spent without guessing at the span from the values it happens
    // to observe.
    std::atomic<uint32_t> connect_deadline_{0}, connect_total_s_{0};
    std::atomic<uint32_t> retry_deadline_{0},   retry_total_s_{0};
    // Arm a deadline `ms` from now. Tick 0 is the "not armed" sentinel, so a deadline
    // that lands exactly on a tick-counter wrap is nudged by one tick.
    static uint32_t deadline_in_(uint32_t ms) {
        uint32_t d = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
        return d ? d : 1;
    }

    TaskHandle_t loop_task_{nullptr};
    static void loop_task_fn_(void* arg);

    // Automatic pairing: while not paired, periodically connect to the configured
    // vehicle and (re)send the whitelist-add so the car keeps prompting "Add key?".
    // Once the user taps the key card, the VCSEC handshake establishes a session and
    // the loop goes idle. No manual scan/pair needed.
    TaskHandle_t auto_pair_task_{nullptr};
    static void auto_pair_task_fn_(void* arg);
};
