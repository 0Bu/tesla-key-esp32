// Pairing lifecycle: the auto-pair supervisor task, key generation/fingerprinting,
// session/cache invalidation, the VIN-change reset, the signed VCSEC health probe
// (revocation canary) and the whitelist-add (pair). Part of the VehicleController
// implementation split — see vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "vehicle_ctrl_internal.hpp"
#include "ota_update.hpp"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>

// protobuf generated headers (from tesla-ble) — VCSEC only: this TU never builds
// infotainment (CarServer) messages; Keys_Role comes via <vehicle.h> → keys.pb.h.
#include <vcsec.pb.h>

// mbedtls for deriving the public-key fingerprint from the stored PEM key
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha1.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

static const char* TAG = "vehicle_ctrl";

// Automatic pairing supervisor. The hard constraint is the tesla-ble library's
// single FIFO command queue: an unsigned "Whitelist Add Key" lingers in that
// queue until the car confirms it (or ~180 s pass), so anything queued behind it
// is blocked. The earlier design queued a session probe *behind* the whitelist-add,
// so the probe never ran, commands piled up, and overlapping responses corrupted
// the RX buffer ("Invalid message length …"). The car accepted the key while the
// firmware never established a session.
//
// This version keeps the queue clean and runs ONE command at a time per round:
//   1. Probe with a signed VCSEC poll. If the key is already authorised this
//      establishes + persists the session (done). If not, it fails *cleanly* with
//      KEY_NOT_ON_WHITELIST and is popped — no clog.
//   2. Send the whitelist-add. The car whitelists the key when the user confirms on
//      screen but sends NO completing commandStatus, so this command can otherwise sit
//      at the FIFO head through the library retries. pair() owns command_mutex_ through
//      its absolute timeout and generation-aware flush, so nothing can queue behind it.
//   3. Probe once more on a clean link — now authorised, this establishes the session.
// Timed-out queued work is invalidated before set_connected(false) synchronously flushes
// callbacks, so the next round starts clean and no late completion reaches another request.
void VehicleController::auto_pair_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    vTaskDelay(pdMS_TO_TICKS(4000));  // let WiFi/BLE come up first
    bool warned_no_vin = false;
    tk::PeriodicLogState unpaired_notice;
    while (true) {
      // Iteration-boundary containment (issue #204): pair()/generate_key()/health_probe_() run
      // tesla-ble crypto + std::string work that can throw std::bad_alloc. An escape would unwind
      // into the FreeRTOS C task trampoline → std::terminate → reboot (and a reboot loop re-opens
      // the poll window). Contain it, pause, and start the next supervision round.
      try {
        // No vehicle to target: without a plausible 17-char VIN we must not connect or enrol —
        // that risks whitelisting our key onto an arbitrary nearby Tesla. Idle quietly instead
        // of spinning a connect→10 s-timeout loop; /scan still lists nearby cars. Logged once.
        // Re-checked each cycle so enrolment starts automatically once a VIN is saved (the web
        // UI's POST /set_vin reboots into a configured state, but this stays robust regardless).
        if (!self->has_plausible_vin()) {
            if (!warned_no_vin) {
                ESP_LOGW(TAG, "auto-pair: no VIN configured — pairing disabled. Set a VIN via the "
                              "setup AP or POST /set_vin, then enrolment starts automatically.");
                warned_no_vin = true;
            }
            // Keep a fresh, LISTING-ONLY view of nearby Teslas for the web UI (nearby() sorts
            // by RSSI). start_discovery never connects/enrols — want_connect_ stays false — so
            // this only populates /status ble.devices and can't whitelist our key onto an
            // arbitrary car. Re-armed each cycle once the ~12 s scan window lapses.
            if (self->ble_ && !self->ble_->is_scanning()) self->ble_scan(12000);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        warned_no_vin = false;  // a VIN is present again — re-arm the one-shot log

        // /set_vin owns a cross-namespace transaction whose controller half is complete before
        // the HTTP task rolls back/finalises its config journal and reboots. Never let this
        // supervisor slip into that short hand-off window: even a no-key retry here could commit
        // an unrelated identity and make the request-local VIN classification meaningless.
        if (self->vin_transition_pending_.load()) {
            self->believed_paired_.store(false);
            ESP_LOGW(TAG, "auto-pair: VIN transition awaiting reboot — pairing/signing disabled");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // regenerate_key() returned without confirming whether its NVS commit landed. Do not
        // let pairing_lost_ authorize another rotation: only a reboot can reconstruct Vehicle
        // from storage and classify the durable fingerprint.
        if (self->key_reload_required_.load()) {
            self->key_runtime_safe_.store(false);
            self->believed_paired_.store(false);
            ESP_LOGE(TAG, "auto-pair: key commit outcome is ambiguous — reboot required; pairing disabled");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        // A key commit succeeded but old sessions could not be erased. Retry ONLY the
        // idempotent cleanup under the FIFO transaction lock; generating another key here
        // would churn NVS and change the fingerprint every supervision cycle.
        if (self->pairing_cleanup_pending_.load()) {
            self->believed_paired_ = false;
            bool cleaned = false;
            {
                tk::SemGuard cmd_guard(self->command_mutex_);
                cleaned = self->finish_key_rotation_cleanup_();
            }
            if (!cleaned) {
                ESP_LOGE(TAG, "auto-pair: persisted new key, but pairing cleanup still fails — retrying in 30 s");
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
            ESP_LOGI(TAG, "auto-pair: deferred pairing cleanup completed");
            continue;
        }

        // Probe the durable key with a real tri-state result before ANY automatic mutation.
        // An NVS error is not first-boot absence: gate all signing and wait for reboot/recovery.
        bool stored_key_present = false;
        const bool key_probe_ok = self->storage_ &&
                                  self->storage_->probe_blob("private_key", stored_key_present);
        const bool pairing_lost = self->pairing_lost_.load();
        const tk::AutomaticKeyAction key_action = tk::decide_automatic_key_action(
            key_probe_ok, stored_key_present, self->key_runtime_safe_.load(), pairing_lost);
        if (key_action == tk::AutomaticKeyAction::StorageUnavailable) {
            self->key_runtime_safe_.store(false);
            self->believed_paired_.store(false);
            ESP_LOGE(TAG, "auto-pair: private-key storage probe failed — pairing/signing disabled; retrying in 30 s");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }
        if (key_action == tk::AutomaticKeyAction::RebootRequired) {
            self->key_runtime_safe_.store(false);
            self->believed_paired_.store(false);
            ESP_LOGE(TAG, "auto-pair: runtime and persisted key state disagree — reboot required; pairing disabled");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }
        if (key_action == tk::AutomaticKeyAction::Generate) {
            self->believed_paired_.store(false);  // stop the observer acting during re-enrol
            if (pairing_lost) {
                ESP_LOGW(TAG, "auto-pair: KEY DELETED on the car — clearing pairing, generating a new key, restarting enrolment");
                self->repair_notice_.store(true);
            } else {
                ESP_LOGW(TAG, "auto-pair: verified first-boot key absence — generating a storage-backed key");
            }
            if (!self->generate_key()) {
                // If the key committed but cleanup did not, the dedicated branch above retries
                // cleanup only; otherwise runtime identity stays fail-closed until retry/reboot.
                ESP_LOGE(TAG, "auto-pair: key generation/rotation failed or cleanup incomplete — pairing remains disabled");
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
            if (pairing_lost) {
                ESP_LOGI(TAG, "auto-pair: new key generated (%s) — re-enrol it on the car",
                         self->key_fingerprint().c_str());
            }
            continue;
        }

        if (self->has_session()) {
            // Re-entering enrolment after a real paired state is a state change and should
            // announce its instructions immediately rather than inherit the old hourly clock.
            tk::periodic_log_reset(unpaired_notice);
            // A live session means (re-)pairing succeeded; drop the re-auth notice.
            self->repair_notice_ = false;
            // We're paired: arm the message observer so a key-rejection fault on any
            // signed command (e.g. the background charge poll → ERROR_UNKNOWN_KEY_ID)
            // trips pairing_lost_ even while the cached VCSEC session keeps succeeding.
            self->believed_paired_ = true;
            // Paired — periodically run a signed VCSEC health poll (~30 s) so a key deleted
            // on the car side is noticed even with no evcc traffic. The poll hits the always-
            // on body controller (VCSEC), which does NOT wake the car's main computer (wake
            // sequences are infotainment-only), so it never keeps a parked car awake. Three
            // outcomes, distinguished so /diag clearly says what happened:
            //   • success            → key still valid
            //   • auth rejection     → car refused our key (likely deleted) — confirm now
            //   • neither (no reply) → car unreachable (asleep / out of range / weak link)
            // Deliberately do NOT clear retry_deadline_ here. health_probe_ runs through
            // send_vcsec_, which first takes command_mutex_ and therefore BLOCKS until any
            // in-flight evcc/manual command finishes — clearing first left that whole wait
            // with no phase armed at all, so the Bluetooth row dropped its countdown and
            // showed a bare label for as long as the mutex was held. Leaving the (by now
            // expired) deadline in place reads as "retrying… right now", which is exactly
            // what is happening, and ensure_connected_ overrides it with the attempt's own
            // countdown the moment the probe actually gets to run (Connecting outranks
            // Waiting). idle_until_next_health_poll_ re-arms it for the next cycle.
            int  streak_before = self->auth_fail_streak_;
            bool ok            = self->health_probe_();
            if (self->pairing_lost_) continue;  // 2nd strike already → revoked (top of loop)

            if (ok) {
                ESP_LOGD(TAG, "auto-pair: health check OK — key still valid");
                // Idle ~30 s, but bail out fast if the message observer flags a deletion
                // (a faulting charge poll mid-wait) so we re-key promptly, not 30 s later.
                self->idle_until_next_health_poll_();
            } else if (self->auth_fail_streak_ > streak_before) {
                // The car answered but REFUSED our key → almost certainly deleted on the
                // car side. Confirm immediately (don't wait a whole cycle) so we react in
                // ~1-2 s; a second auth rejection trips pairing_lost_ in make_result_cb_.
                ESP_LOGW(TAG, "auto-pair: car refused our key (auth fail %d/2) — re-checking to confirm…",
                         (int)self->auth_fail_streak_);
                self->health_probe_();
                if (self->pairing_lost_) continue;
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                // No auth answer at all → could not reach/talk to the car. NOT a revocation;
                // keep the pairing and retry. The connection layer already emits the first
                // failure and its hourly heartbeat; keep this per-probe summary at DEBUG so
                // the supervisor cannot recreate the warning storm above that rate limiter.
                ESP_LOGD(TAG, "auto-pair: car not reachable over BLE — can't verify key right now, will retry");
                self->idle_until_next_health_poll_();
            }
            continue;
        }

        // Not paired (enrolling): disarm the observer — key-rejection faults are expected
        // here and must not be mistaken for a revocation.
        self->believed_paired_ = false;
        // Leaving the paired steady state also retires its retry countdown. The idle wait
        // deliberately leaves its deadline armed-but-expired so the row reads "retrying…"
        // across the probe that follows (see idle_until_next_health_poll_), but enrolment has
        // no health-poll schedule at all — carrying that stale deadline in would pin the row
        // on "retrying…" for the entire time the user is being asked to tap an NFC keycard.
        self->retry_deadline_.store(0);

        // 1. Probe for an existing whitelist entry. Once the key is enrolled — by resting a
        //    Tesla NFC keycard on the center-console reader and confirming the "Add key"
        //    dialog the car then shows on its touchscreen (the dialog only appears while a
        //    card is present) — it shows up here as a usable session.
        VehicleStatusResult st;
        self->get_vehicle_status(st, tk::ConnectOrigin::Background, 6000);
        if (self->has_session()) {
            ESP_LOGI(TAG, "auto-pair: session established");
            continue;
        }

        // Keep setup guidance visible on the state transition and as an hourly reminder, but
        // not on every ~38 s automatic round. Per-round detail is available only in a build
        // compiled with maximum DEBUG; production INFO/syslog stays bounded.
        const bool announce_unpaired = tk::periodic_log_due(
            unpaired_notice, static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL,
            tk::kAutoPairNoticeRepeatMs);

        // 2. Send the whitelist-add ONCE. This is what makes the car
        //    show the "Add key" dialog on its touchscreen — but the car only shows it while
        //    a Tesla NFC keycard is resting on the center-console reader. We do NOT block
        //    waiting on it: the "Whitelist Add Key" never completes cleanly on this car (no
        //    completing commandStatus) — success is detected by probing (step 3), not by
        //    pair()'s return. The short wait just lets the message reach the car; pair()'s
        //    generation-aware timeout path invalidates and flushes the lingering request while
        //    it still owns command_mutex_, so the probes below run clean. Sending it only once per round
        //    (instead of every ~45 s block) also stops the car re-prompting after the key
        //    is already registered.
        if (announce_unpaired) {
            ESP_LOGI(TAG, "auto-pair: not paired — requesting key enrolment from the car…");
        } else {
            ESP_LOGD(TAG, "auto-pair: not paired — requesting key enrolment from the car…");
        }
        self->pair(tk::ConnectOrigin::Background, 5000);
        if (announce_unpaired) {
            ESP_LOGI(TAG, "auto-pair: enrolment request attempted — place a Tesla NFC keycard on the center-console reader, then confirm 'Add key' on the touchscreen; waiting for the key to register…");
        } else {
            ESP_LOGD(TAG, "auto-pair: enrolment request attempted — place a Tesla NFC keycard on the center-console reader, then confirm 'Add key' on the touchscreen; waiting for the key to register…");
        }

        // 3. Poll for the resulting session at a short cadence so an enrolment that lands
        //    mid-round — the instant a keycard is tapped — is noticed within a few seconds
        //    instead of after a full slow round. A failed probe (not yet enrolled) returns
        //    on its timeout; a successful one (now enrolled) returns in ~1 s and persists
        //    the session, so has_session() flips and we stop here.
        bool established = false;
        for (int i = 0; i < 8; i++) {
            self->get_vehicle_status(st, tk::ConnectOrigin::Background, 3000);
            if (self->has_session()) { established = true; break; }
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        if (established) {
            ESP_LOGI(TAG, "auto-pair: key registered on the car — session established, now PAIRED");
            continue;
        }
        if (announce_unpaired) {
            ESP_LOGI(TAG, "auto-pair: not registered yet — place a Tesla NFC keycard on the console reader and confirm 'Add key' on screen (or move closer if the car is out of BLE range)");
        } else {
            ESP_LOGD(TAG, "auto-pair: not registered yet — place a Tesla NFC keycard on the console reader and confirm 'Add key' on screen (or move closer if the car is out of BLE range)");
        }
      } catch (const std::exception& e) {
          ESP_LOGE(TAG, "auto-pair iteration threw (%s) — pausing, will retry", e.what());
          vTaskDelay(pdMS_TO_TICKS(2000));
      } catch (...) {
          ESP_LOGE(TAG, "auto-pair iteration threw (unknown) — pausing, will retry");
          vTaskDelay(pdMS_TO_TICKS(2000));
      }
    }
}

// ─── Key management ───────────────────────────────────────────────────────────

VehicleController::KeyGenerationResult VehicleController::generate_key_result(
        bool allow_replace) {
    KeyGenerationResult result;
    tk::MutexGuard cmd_guard(command_mutex_);
    if (vin_transition_pending_.load()) {
        ESP_LOGE(TAG, "key generation refused — VIN transaction is awaiting reboot");
        result.transition_blocked = true;
        return result;
    }
    if (key_reload_required_.load()) {
        ESP_LOGE(TAG, "key generation refused — previous key commit outcome requires reboot");
        result.rotation = tk::KeyRotationResult::CommitUnknown;
        return result;
    }
    bool key_exists = false;
    bool probe_ok = true;
    if (!allow_replace) {
        probe_ok = storage_ && storage_->probe_blob("private_key", key_exists);
    }
    switch (tk::decide_key_generation_preflight(allow_replace, probe_ok, key_exists)) {
        case tk::KeyGenerationPreflight::ProbeFailed:
            ESP_LOGE(TAG, "key generation refused — private-key storage could not be read");
            result.key_probe_failed = true;
            return result;
        case tk::KeyGenerationPreflight::ExistingKeyRefused:
            result.existing_key_refused = true;
            return result;
        case tk::KeyGenerationPreflight::Proceed:
            break;
    }
    result.rotation = generate_key_locked_();
    return result;
}

bool VehicleController::generate_key() {
    // Auto-pair must re-sample every authorizing fact AFTER taking the transaction lock. Reading
    // pairing_lost_ first and then blocking here allowed a concurrent HTTP rotation to clear the
    // flag while the stale true still replaced its freshly committed key a second time.
    // Acquire OTA exclusion first, matching the HTTP/first-boot lock order (OTA gate then
    // command_mutex_). An active OTA check/download returns immediately; once held, no OTA worker
    // can start and reboot while the key journal is in flight.
    OtaIdentityMutationGuard identity_guard(tk::IdentityMutationEntry::AutomaticKey);
    if (!identity_guard) {
        ESP_LOGW(TAG, "automatic key rotation blocked during OTA verification/update");
        return false;
    }
    tk::MutexGuard cmd_guard(command_mutex_);
    if (vin_transition_pending_.load() || key_reload_required_.load() ||
        pairing_cleanup_pending_.load()) {
        ESP_LOGE(TAG, "automatic key generation refused — identity recovery is pending");
        return false;
    }

    bool key_exists = false;
    const bool probe_ok = storage_ && storage_->probe_blob("private_key", key_exists);
    const bool pairing_lost = pairing_lost_.load();
    const tk::AutomaticKeyAction action = tk::decide_automatic_key_action(
        probe_ok, key_exists, key_runtime_safe_.load(), pairing_lost);
    if (action != tk::AutomaticKeyAction::Generate) {
        key_runtime_safe_.store(action == tk::AutomaticKeyAction::Continue);
        ESP_LOGE(TAG, "automatic key generation refused after locked identity recheck (action=%d)",
                 static_cast<int>(action));
        return false;
    }

    return tk::key_rotation_runtime_safe(generate_key_locked_());
}

tk::KeyRotationResult VehicleController::generate_key_locked_() {
    if (!vehicle_ || !storage_) {
        ESP_LOGE(TAG, "key generation unavailable — controller/storage not initialized");
        return tk::KeyRotationResult::NotCommitted;
    }
    if (key_reload_required_.load()) {
        ESP_LOGE(TAG, "key generation refused — durable identity must be reloaded first");
        return tk::KeyRotationResult::CommitUnknown;
    }
    if (pairing_cleanup_pending_.load()) {
        ESP_LOGE(TAG, "key generation refused — previous committed rotation still needs cleanup");
        return tk::KeyRotationResult::CleanupPending;
    }

    // Allocate the marker payload before changing any runtime gate. If allocation itself throws,
    // no journal/key state has changed and the caller's outer exception boundary can respond.
    const std::vector<uint8_t> marker_payload{1};
    const bool runtime_was_safe = key_runtime_safe_.load();
    // Block every signer before tesla-ble mutates its in-memory key. Background polling reads
    // this gate too; foreground runners are additionally serialized by command_mutex_. It is
    // restored only after both durable key commit and obsolete-session cleanup succeed.
    key_runtime_safe_.store(false);
    bool marker_saved = false;
    try {
        marker_saved = storage_->save(tk::kKeyRotationMarker, marker_payload);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "key rotation journal write threw (%s)", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "key rotation journal write threw (unknown)");
    }
    if (!marker_saved) {
        // The private key has not been touched, but a failed nvs_commit is ambiguous: the marker
        // may nevertheless be visible. Restore the previous runtime gate only after a tri-state
        // probe proves ABSENCE; present/unreadable stays fail-closed until retry/reboot.
        bool marker_exists = false;
        const bool probe_ok = storage_->probe_blob(tk::kKeyRotationMarker, marker_exists);
        if (probe_ok && !marker_exists) {
            key_runtime_safe_.store(runtime_was_safe);
            ESP_LOGE(TAG, "key rotation journal was not persisted — private key left unchanged");
        } else {
            ESP_LOGE(TAG, "key rotation journal write outcome is ambiguous — signing remains disabled");
        }
        return tk::KeyRotationResult::NotCommitted;
    }

    bool generated = false;
    try {
        tk::SemGuard g(vehicle_mutex_);   // RAII: regenerate_key() (crypto/NVS) can throw
        generated = vehicle_->regenerate_key();
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "key generation threw (%s) — durable commit not confirmed", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "key generation threw (unknown) — durable commit not confirmed");
    }
    // Key rotation is a transaction boundary.  The pinned tesla-ble implementation now
    // reports both crypto generation and the private-key NVS commit.  Do not destroy the
    // previous key's still-usable sessions unless that commit succeeded: on NVS-full the
    // previous firmware unpaired the device and returned HTTP success even though the new
    // key existed only in RAM.
    if (!generated) {
        // A failed nvs_commit is not proof that the new blob stayed uncommitted. tesla-ble may
        // restore the old RAM key while flash already contains the new one, so neither runtime
        // fingerprint can classify durable identity. Keep key_rotate (and, for /set_vin,
        // vin_txn) armed and require boot to reload the authoritative fingerprint.
        key_reload_required_.store(true);
        ESP_LOGE(TAG, "key generation/persistence outcome is ambiguous — runtime key is untrusted; reboot required");
        invalidate_and_flush_(command_generation_.load());
        // Keep key_rotate armed. A reboot erases every potentially mismatched session before it
        // constructs Vehicle, regardless of whether the failed commit left the old or new key.
        return tk::KeyRotationResult::CommitUnknown;
    }
    pairing_cleanup_pending_.store(true);
    // Record when the key was generated so the UI can show the key's creation
    // date next to its fingerprint. Wall-clock comes from the browser (POST
    // /set_time) or the NVS-cached time; if neither is set yet this stamps a
    // near-zero value, which the UI ignores.
    if (storage_) {
        try {
            time_t now = time(nullptr);
            if (!storage_->save_str("key_created", std::to_string((long long)now))) {
                ESP_LOGW(TAG, "key generated but its creation date was not persisted");
            }
        } catch (...) {
            ESP_LOGW(TAG, "key generated but creation-date metadata allocation failed");
        }
    }
    // A new key invalidates any existing pairing: the stored session belonged to the
    // previous key/whitelist entry, so a fresh enrolment + handshake is required.
    // Wipe the session and cached data so has_session() flips to false (the UI shows
    // "not paired" and hides the controls/SOC) and the auto-pair loop re-enrolls.
    bool cleanup_complete = false;
    try {
        cleanup_complete = finish_key_rotation_cleanup_();
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "key-rotation cleanup threw (%s) — journal remains armed", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "key-rotation cleanup threw (unknown) — journal remains armed");
    }
    if (!cleanup_complete) {
        // regenerate_key() has already committed the NEW key at this point. Returning false
        // reports only that the now-obsolete sessions were not durably erased; it must not be
        // interpreted as permission to restore/use the previous identity. /set_vin consumes
        // this request-local CleanupPending enum while still bound to its staged journal.
        ESP_LOGE(TAG, "new key persisted, but pairing cleanup is incomplete");
        return tk::KeyRotationResult::CleanupPending;
    }
    ESP_LOGI(TAG, "new key generated");
    return tk::KeyRotationResult::Complete;
}

bool VehicleController::finish_key_rotation_cleanup_() {
    if (!clear_session_and_cache_()) return false;
    // The marker is removed LAST. If this commit fails, pairing_cleanup_pending_ keeps every
    // signer gated and the supervisor/next boot repeats the idempotent erases.
    if (!storage_ || !storage_->remove(tk::kKeyRotationMarker)) {
        ESP_LOGE(TAG, "pairing sessions erased, but key-rotation journal is still pending");
        return false;
    }
    pairing_cleanup_pending_.store(false);
    key_reload_required_.store(false);
    key_runtime_safe_.store(true);
    pairing_lost_.store(false);     // re-keying + cleanup resolved the pending revocation
    auth_fail_streak_.store(0);     // and the streak that may have led here
    return true;
}

// Tear down the current pairing without touching the private key. Used by
// generate_key() (re-key) and reset_for_new_vehicle() (VIN change). Must NOT be
// called while holding vehicle_mutex_ (it takes it to reset the in-memory peers).
bool VehicleController::clear_session_and_cache_() {
    bool cleanup_ok = true;
    // Reset the library's in-memory peer sessions (and flush its command queue / RX
    // buffer) so a stale session key cannot be reused. set_connected(false) does this;
    // only bother when something is actually established to avoid a spurious log on a
    // first-boot key generation.
    bool had_link    = ble_ && ble_->is_connected();
    bool had_session = has_session();
    // is_connected() now means GATT command-ready, which is intentionally narrower than a
    // physical GAP link. Disconnect unconditionally so a rotation also aborts service/CCCD
    // discovery; otherwise its delayed ready callback could publish the just-invalidated link.
    if (ble_) ble_->disconnect();
    if ((had_link || had_session) && vehicle_) {
        // set_connected(false) synchronously flushes queued callbacks. Although key rotation
        // owns command_mutex_, invalidate defensively before the flush so no compatible SKIPPED
        // result can be observed as success by an older waiter.
        command_generation_.fetch_add(1);
        try {
            tk::SemGuard g(vehicle_mutex_);   // RAII: set_connected() can throw
            vehicle_->set_connected(false);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "pairing in-memory reset threw (%s)", e.what());
            cleanup_ok = false;
        } catch (...) {
            ESP_LOGE(TAG, "pairing in-memory reset threw (unknown)");
            cleanup_ok = false;
        }
    }

    // Erase the persisted sessions so has_session() is false until a fresh handshake.
    if (storage_) {
        // A failed erase here is the one worth shouting about: the in-RAM state says
        // "unpaired" while flash still holds a session blob, so the NEXT boot loads a session
        // that belongs to a key the car has already forgotten — and the symptom is a device
        // that looks paired and fails every command. has_session() reads flash, so this is a
        // real divergence rather than a cosmetic one.
        const bool v = storage_->remove("session_vcsec");
        const bool i = storage_->remove("session_infotainment");
        const bool p = storage_->remove("paired_at");   // re-pair re-stamps the pairing date
        if (!v || !i || !p) {
            ESP_LOGE(TAG, "session erase incomplete (vcsec=%d info=%d paired_at=%d) — "
                          "flash may still hold a stale session across the next boot",
                     (int)v, (int)i, (int)p);
            cleanup_ok = false;
        }
    } else if (!storage_) {
        ESP_LOGE(TAG, "session erase unavailable — no storage adapter");
        cleanup_ok = false;
    }

    // Drop cached readings so /status and vehicle_data never serve old SOC/charge data
    // (or stale telemetry) from a defunct pairing. Under cache_mutex_ since the HTTP task
    // may be copying these concurrently.
    {
        tk::MutexGuard cache_guard(cache_mutex_);
        last_known_charge_   = {};
        last_known_status_   = {};
        last_known_climate_  = {};
        last_known_drive_    = {};
        last_known_tires_    = {};
        last_known_closures_ = {};
    }
    last_contact_ticks_.store(0);    // no live data anymore → "asleep" card has nothing to show
    last_charge_ticks_.store(0);
    charge_state_generation_.store(0);
    charge_cache_stale_reported_.store(false);
    last_reachable_ticks_.store(0);  // and no proven reachability → link_state() back to Unknown
    vcsec_asleep_since_ticks_.store(0);  // forget any debounced sleep run from the old pairing
    vcsec_sleep_state_.store(static_cast<int>(TeslaBLE::SleepState::UNKNOWN));
    ESP_LOGI(TAG, "pairing/session cleanup %s", cleanup_ok ? "complete" : "incomplete");
    return cleanup_ok;
}

VehicleController::NewVehicleResetResult VehicleController::reset_for_new_vehicle(
        const VinTransitionStager& stage) {
    NewVehicleResetResult result;
    tk::MutexGuard cmd_guard(command_mutex_);

    if (tk::vin_transition_recovery_blocks_staging(
            key_reload_required_.load(), pairing_cleanup_pending_.load())) {
        // CleanupPending belongs to the PREVIOUS rotation. Returning it from
        // generate_key_locked_ after staging would falsely classify this VIN request as having
        // committed a new key. Reject before its callback can write vin_txn or ConfigBlob.
        result.state = tk::VinTransitionApply::IdentityRecoveryPending;
        ESP_LOGE(TAG, "VIN transition refused — key identity recovery/cleanup is pending");
        return result;
    }

    // Bind the fingerprint and every following state transition to this exact request. The old
    // HTTP flow released the mutex between fingerprint capture and rotation, letting auto-rekey
    // change the durable key and causing the request to misclassify its own failure.
    bool key_present = false;
    const bool key_probe_ok = storage_ && storage_->probe_blob("private_key", key_present);
    if (key_probe_ok && key_present) result.previous_key_id = key_fingerprint();
    const bool identity_verified = tk::private_key_identity_verified(
        key_probe_ok, key_present, !result.previous_key_id.empty());
    if (!identity_verified) {
        ESP_LOGE(TAG, "VIN transition refused — existing key identity could not be verified");
        result.state = tk::decide_vin_transition_apply(
            false, false, tk::KeyRotationResult::NotCommitted);
        return result;
    }

    // This gate stays asserted until the mandatory reboot. Even a successful rotation leaves
    // this Vehicle object bound to its old VIN, so no signing or automatic generation is valid
    // in the controller→HTTP journal hand-off window.
    vin_transition_pending_.store(true);
    bool staged = false;
    try {
        staged = stage && stage(result.previous_key_id);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "VIN transition staging threw (%s)", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "VIN transition staging threw (unknown)");
    }
    if (!staged) {
        result.state = tk::decide_vin_transition_apply(
            true, false, tk::KeyRotationResult::NotCommitted);
        return result;
    }

    const tk::KeyRotationResult rotation = generate_key_locked_();
    result.state = tk::decide_vin_transition_apply(true, true, rotation);
    if (tk::key_rotation_committed(rotation)) {
        // The discovered BLE MAC belongs to the previous car; boot recovery can retry this via
        // vin_txn if the cross-namespace erase does not commit here.
        const bool mac_removed = config_store_ && config_store_->remove("ble_mac");
        if (!mac_removed) {
            ESP_LOGE(TAG, "old vehicle's BLE MAC cleanup incomplete — VIN journal remains armed");
            result.state = tk::VinTransitionApply::RecoverCommittedIdentity;
        }
    }

    ESP_LOGI(TAG, "reset for new vehicle classified as %d", static_cast<int>(result.state));
    return result;
}

// Idle between two VCSEC health polls. The countdown the web UI shows and the wait it
// counts down come from the SAME constant here, so the row can never promise a retry at a
// time the loop doesn't actually retry. Polls in short steps so a key deletion flagged by
// the message observer mid-wait (a faulting charge poll) re-keys promptly rather than a
// full cycle later.
//
// Deliberately leaves the deadline ARMED on the way out. Clearing it here (or, equivalently,
// just before the probe) opened a phase-less window: the probe runs through send_vcsec_,
// which takes command_mutex_ FIRST and blocks behind any in-flight evcc/manual command, and
// the round-trip that follows is unbounded too — with nothing armed the Bluetooth row lost
// its countdown and fell back to a bare label for all of it. An armed-but-expired deadline
// reports 0 s, which the UI reads as "retrying… right now" — exactly what is happening — and
// ensure_connected_ overrides it with the attempt's own countdown the moment the probe gets
// to run (Connecting outranks Waiting). The next call re-arms it for the next cycle, so from
// the first wait onward there is always exactly one phase to show.
void VehicleController::idle_until_next_health_poll_() {
    constexpr uint32_t kIdleMs = 30000;
    constexpr uint32_t kStepMs = 500;
    retry_deadline_.store(deadline_in_(kIdleMs));
    for (uint32_t w = 0; w < kIdleMs / kStepMs && !pairing_lost_; w++) vTaskDelay(pdMS_TO_TICKS(kStepMs));
}

bool VehicleController::health_probe_(int timeout_ms) {
    // A signed VCSEC GET_STATUS — the one signed command a Charging-Manager key is ALWAYS
    // authorised for, so its outcome unambiguously reflects whitelist state: success ⇒ key
    // still valid; KEY_NOT_ON_WHITELIST or a tagless session-info ("authentication failed")
    // ⇒ key deleted. Because role refusal cannot masquerade as revocation here (there is no
    // role that can't read status), this is the ONE caller that passes auth_fail_is_revocation
    // so make_result_cb_ lets an "authentication failed" feed the two-strike pairing_lost_.
    // NO_WAKE_FAIL, not WAKE_IF_NEEDED: today the library ignores the wake policy on the
    // VCSEC path entirely (the body controller is always on, no wake is ever needed), so
    // this is behaviour-neutral — but "never wake the car from the periodic probe" is a
    // guarantee of ours, and it must not silently invert if a future tesla-ble starts
    // honouring the policy for VCSEC too. NO_WAKE_SKIP would be wrong here: it skips while
    // the car is believed asleep, which would blind both revocation detection and the
    // VCSEC sleep/wake sampling exactly when the car sleeps.
    return send_vcsec_("VCSEC Health Poll", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_information_request_message(
            VCSEC_InformationRequestType_INFORMATION_REQUEST_TYPE_GET_STATUS, b, l);
    }, TeslaBLE::WakePolicy::NO_WAKE_FAIL, timeout_ms, tk::ConnectOrigin::Background,
       /*auth_fail_is_revocation=*/true,
       tk::CompletionTimeoutPolicy::BackgroundHealth);
}

time_t VehicleController::key_created_at() {
    if (!storage_) return 0;
    std::string s;
    if (!storage_->load_str("key_created", s)) return 0;
    return (time_t)atoll(s.c_str());
}

time_t VehicleController::paired_at() {
    if (!storage_ || !has_session()) return 0;
    std::string s;
    if (storage_->load_str("paired_at", s)) {
        time_t t = (time_t)atoll(s.c_str());
        if (t > 1600000000) return t;
    }
    // First time we observe a session with a valid wall clock: stamp it now. For a
    // fresh handshake this is within seconds of pairing; a pairing that predates this
    // tracking (or whose clock was unsynced) gets stamped at first sync instead.
    time_t now = time(nullptr);
    if (now > 1600000000) {
        if (!storage_->save_str("paired_at", std::to_string((long long)now))) {
            ESP_LOGW(TAG, "pairing date not persisted — the UI will re-stamp it on the next sync");
        }
        return now;
    }
    return 0;
}

bool VehicleController::has_key() {
    // Existence probe only — never read the key blob into a vector just to test presence.
    return storage_ && storage_->blob_exists("private_key");
}

bool VehicleController::has_session() {
    // Existence probe only — see has_key(); sampled ~1 Hz from the display/LED tasks.
    return storage_ && storage_->blob_exists("session_vcsec");
}

std::string VehicleController::key_fingerprint() {
    if (!storage_) return "";
    std::vector<uint8_t> pem;
    if (!storage_->load("private_key", pem) || pem.empty()) return "";
    // mbedtls expects the PEM buffer to be NUL-terminated and the length to include it.
    if (pem.back() != '\0') pem.push_back('\0');

    mbedtls_pk_context     pk;   mbedtls_pk_init(&pk);
    mbedtls_entropy_context ent; mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_context drbg; mbedtls_ctr_drbg_init(&drbg);
    std::string fp;

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, nullptr, 0) == 0 &&
        mbedtls_pk_parse_key(&pk, pem.data(), pem.size(), nullptr, 0,
                             mbedtls_ctr_drbg_random, &drbg) == 0 &&
        mbedtls_pk_get_type(&pk) == MBEDTLS_PK_ECKEY) {
        mbedtls_ecp_keypair* kp = mbedtls_pk_ec(pk);
        mbedtls_ecp_group grp;  mbedtls_ecp_group_init(&grp);
        mbedtls_ecp_point Q;    mbedtls_ecp_point_init(&Q);
        uint8_t pub[65];
        size_t  publen = 0;
        if (mbedtls_ecp_export(kp, &grp, nullptr, &Q) == 0 &&
            mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &publen, pub, sizeof(pub)) == 0) {
            // Tesla key id = first 4 bytes of SHA-1 over the uncompressed public point.
            uint8_t sha[20];
            if (mbedtls_sha1(pub, publen, sha) == 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X",
                         sha[0], sha[1], sha[2], sha[3]);
                fp = buf;
            }
        }
        mbedtls_ecp_point_free(&Q);
        mbedtls_ecp_group_free(&grp);
    }

    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return fp;
}

bool VehicleController::pair(tk::ConnectOrigin origin, int timeout_ms) {
    if (timeout_ms <= 0) return false;
    const uint32_t deadline = deadline_in_(static_cast<uint32_t>(timeout_ms));
    tk::SemGuard cmd_guard(command_mutex_, ticks_until_(deadline));
    if (!cmd_guard) {
        if (origin == tk::ConnectOrigin::Foreground) {
            ESP_LOGW(TAG, "pair deadline exhausted waiting for another request");
        } else {
            ESP_LOGD(TAG, "background pair deadline exhausted waiting for another request");
        }
        return false;
    }
    // Check only after taking command_mutex_: a concurrent generate_key() may have changed the
    // identity while this caller waited. has_key() proves storage contains a key;
    // command_identity_ready_ proves this Vehicle uses it and cleanup is complete.
    if (!has_key() || !command_identity_ready_()) {
        ESP_LOGE(TAG, "pair refused — no verified storage-backed runtime key (reboot or regenerate required)");
        return false;
    }
    tk::InFlightGuard inflight(cmd_in_flight_);

    // This firmware only ever enrolls a Charging Manager key (charging + wake),
    // never an owner key — its sole purpose is the evcc BLE integration. Limiting
    // the role keeps the device's stored key from granting full vehicle access.
    const Keys_Role role = Keys_Role_ROLE_CHARGING_MANAGER;

    CommandOutcome outcome = send_vcsec_locked_(
        "Whitelist Add Key",
        [role](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
            return c->build_white_list_message(
                role, VCSEC_KeyFormFactor_KEY_FORM_FACTOR_CLOUD_KEY, b, l);
        },
        TeslaBLE::WakePolicy::NO_WAKE_FAIL, deadline, origin,
        /*auth_fail_is_revocation=*/false,
        origin == tk::ConnectOrigin::Foreground
            ? tk::CompletionTimeoutPolicy::ForegroundWarn
            : tk::CompletionTimeoutPolicy::ExpectedSilent);

    if (!outcome.completed && !outcome.error.empty()) {
        if (origin == tk::ConnectOrigin::Foreground) {
            ESP_LOGW(TAG, "pair request failed before confirmation: %s", outcome.error.c_str());
        } else {
            ESP_LOGD(TAG, "background pair request failed before confirmation: %s",
                     outcome.error.c_str());
        }
    } else if (!outcome.completed) {
        if (origin == tk::ConnectOrigin::Foreground) {
            ESP_LOGW(TAG, "pair not confirmed — confirm the pairing request on the car's screen");
        } else {
            ESP_LOGD(TAG, "background pair request not confirmed");
        }
    } else if (!outcome.success) {
        ESP_LOGW(TAG, "pair rejected: %s",
                 outcome.error.empty() ? "vehicle returned failure" : outcome.error.c_str());
    } else {
        ESP_LOGI(TAG, "pair confirmed on the car's screen");
    }
    return outcome.completed && outcome.success;
}
