// Pairing lifecycle: the auto-pair supervisor task, key generation/fingerprinting,
// session/cache invalidation, the VIN-change reset, the signed VCSEC health probe
// (revocation canary) and the whitelist-add (pair). Part of the VehicleController
// implementation split — see vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "vehicle_ctrl_internal.hpp"
#include <esp_log.h>
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
//      screen but sends NO completing commandStatus, so this command never finishes
//      cleanly — it just exhausts ~180 s of library retries while sitting at the head
//      of the single FIFO queue, starving everything behind it. So after pair() we
//      drop the link to flush that stuck command from the queue.
//   3. Probe once more on a clean link — now authorised, this establishes the session.
// On any failure the BLE link is dropped, which flushes the library's command
// queue and RX buffer (set_connected(false)) so the next round starts clean.
void VehicleController::auto_pair_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    vTaskDelay(pdMS_TO_TICKS(4000));  // let WiFi/BLE come up first
    bool warned_no_vin = false;
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

        // The car deleted our key (detected via a KEY_NOT_ON_WHITELIST response to any
        // signed command or the health poll below). The stored key is now useless, so
        // re-key — which also clears the session + cache — and fall through to re-pair.
        if (self->pairing_lost_) {
            self->believed_paired_ = false;  // stop the observer acting during re-enrol
            ESP_LOGW(TAG, "auto-pair: KEY DELETED on the car — clearing pairing, generating a new key, restarting enrolment");
            self->repair_notice_ = true;  // tell the UI why it's asking to pair again
            self->generate_key();   // clears pairing_lost_, session and cached data
            ESP_LOGI(TAG, "auto-pair: new key generated (%s) — re-enrol it on the car", self->key_fingerprint().c_str());
            continue;
        }

        if (self->has_session()) {
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
                // keep the pairing and retry. Logged so it's clear the key simply could not
                // be verified (connectivity), rather than a deletion being silently missed.
                ESP_LOGW(TAG, "auto-pair: car not reachable over BLE — can't verify key right now, will retry");
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
        self->get_vehicle_status(st, 6000);
        if (self->has_session()) {
            ESP_LOGI(TAG, "auto-pair: session established");
            continue;
        }

        // 2. Send the whitelist-add ONCE, then flush the queue. This is what makes the car
        //    show the "Add key" dialog on its touchscreen — but the car only shows it while
        //    a Tesla NFC keycard is resting on the center-console reader. We do NOT block
        //    waiting on it: the "Whitelist Add Key" never completes cleanly on this car (no
        //    completing commandStatus) — success is detected by probing (step 3), not by
        //    pair()'s return. The short wait just lets the message reach the car; flushing
        //    (set_connected(false)) then clears the lingering whitelist-add from the single
        //    FIFO queue so the probes below run clean. Sending it only once per round
        //    (instead of every ~45 s block) also stops the car re-prompting after the key
        //    is already registered.
        ESP_LOGI(TAG, "auto-pair: not paired — requesting key enrolment from the car…");
        self->pair(5000);
        if (self->ble_ && self->ble_->is_connected()) self->ble_->disconnect();
        {
            tk::SemGuard g(self->vehicle_mutex_);   // RAII: set_connected() can throw
            self->vehicle_->set_connected(false);
        }
        ESP_LOGI(TAG, "auto-pair: enrolment request sent — place a Tesla NFC keycard on the center-console reader, then confirm 'Add key' on the touchscreen; waiting for the key to register…");

        // 3. Poll for the resulting session at a short cadence so an enrolment that lands
        //    mid-round — the instant a keycard is tapped — is noticed within a few seconds
        //    instead of after a full slow round. A failed probe (not yet enrolled) returns
        //    on its timeout; a successful one (now enrolled) returns in ~1 s and persists
        //    the session, so has_session() flips and we stop here.
        bool established = false;
        for (int i = 0; i < 8; i++) {
            self->get_vehicle_status(st, 3000);
            if (self->has_session()) { established = true; break; }
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        if (established) {
            ESP_LOGI(TAG, "auto-pair: key registered on the car — session established, now PAIRED");
            continue;
        }
        ESP_LOGI(TAG, "auto-pair: not registered yet — place a Tesla NFC keycard on the console reader and confirm 'Add key' on screen (or move closer if the car is out of BLE range)");
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

bool VehicleController::generate_key() {
    tk::MutexGuard cmd_guard(command_mutex_);
    {
        tk::SemGuard g(vehicle_mutex_);   // RAII: regenerate_key() (crypto/NVS) can throw
        vehicle_->regenerate_key();
    }
    // Record when the key was generated so the UI can show the key's creation
    // date next to its fingerprint. Wall-clock comes from the browser (POST
    // /set_time) or the NVS-cached time; if neither is set yet this stamps a
    // near-zero value, which the UI ignores.
    if (storage_) {
        time_t now = time(nullptr);
        storage_->save_str("key_created", std::to_string((long long)now));
    }
    // A new key invalidates any existing pairing: the stored session belonged to the
    // previous key/whitelist entry, so a fresh enrolment + handshake is required.
    // Wipe the session and cached data so has_session() flips to false (the UI shows
    // "not paired" and hides the controls/SOC) and the auto-pair loop re-enrolls.
    clear_session_and_cache_();
    pairing_lost_     = false;  // re-keying is the resolution; clear any pending flag
    auth_fail_streak_ = 0;      // and the streak that may have led here
    ESP_LOGI(TAG, "new key generated");
    return true;
}

// Tear down the current pairing without touching the private key. Used by
// generate_key() (re-key) and reset_for_new_vehicle() (VIN change). Must NOT be
// called while holding vehicle_mutex_ (it takes it to reset the in-memory peers).
void VehicleController::clear_session_and_cache_() {
    // Reset the library's in-memory peer sessions (and flush its command queue / RX
    // buffer) so a stale session key cannot be reused. set_connected(false) does this;
    // only bother when something is actually established to avoid a spurious log on a
    // first-boot key generation.
    bool had_link    = ble_ && ble_->is_connected();
    bool had_session = has_session();
    if (had_link) ble_->disconnect();
    if ((had_link || had_session) && vehicle_) {
        tk::SemGuard g(vehicle_mutex_);   // RAII: set_connected() can throw
        vehicle_->set_connected(false);
    }

    // Erase the persisted sessions so has_session() is false until a fresh handshake.
    if (storage_) {
        storage_->remove("session_vcsec");
        storage_->remove("session_infotainment");
        storage_->remove("paired_at");   // re-pair re-stamps the pairing date
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
    last_reachable_ticks_.store(0);  // and no proven reachability → link_state() back to Unknown
    vcsec_asleep_since_ticks_.store(0);  // forget any debounced sleep run from the old pairing
    ESP_LOGI(TAG, "pairing/session cleared");
}

bool VehicleController::reset_for_new_vehicle() {
    // Regenerating the key already clears the session + cache (see generate_key()).
    generate_key();
    // The discovered BLE MAC belongs to the previous car; drop it so the next boot
    // rediscovers the new vehicle by its VIN-derived advertising name.
    if (config_store_) config_store_->remove("ble_mac");
    ESP_LOGI(TAG, "reset for new vehicle complete");
    return true;
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
    }, TeslaBLE::WakePolicy::NO_WAKE_FAIL, timeout_ms, /*count_as_activity=*/false,
       /*auth_fail_is_revocation=*/true);
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
        storage_->save_str("paired_at", std::to_string((long long)now));
        return now;
    }
    return 0;
}

bool VehicleController::has_key() {
    if (!storage_) return false;
    std::vector<uint8_t> buf;
    return storage_->load("private_key", buf);
}

bool VehicleController::has_session() {
    if (!storage_) return false;
    std::vector<uint8_t> buf;
    return storage_->load("session_vcsec", buf);
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

bool VehicleController::pair(int timeout_ms) {
    tk::MutexGuard cmd_guard(command_mutex_);
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    last_result_ = false;
    last_error_.clear();

    // This firmware only ever enrolls a Charging Manager key (charging + wake),
    // never an owner key — its sole purpose is the evcc BLE integration. Limiting
    // the role keeps the device's stored key from granting full vehicle access.
    const Keys_Role role = Keys_Role_ROLE_CHARGING_MANAGER;

    {
        // RAII give — the whitelist builder inside send_command_result can throw (Scenario B).
        tk::SemGuard g(vehicle_mutex_);
        // Use send_command_result to get a callback when the whitelist message is delivered.
        // The user still needs to confirm the pairing request shown on the car's screen.
        vehicle_->send_command_result(
            UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
            "Whitelist Add Key",
            [role](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
                return c->build_white_list_message(role, VCSEC_KeyFormFactor_KEY_FORM_FACTOR_CLOUD_KEY, b, l);
            },
            make_result_cb_(),
            TeslaBLE::WakePolicy::NO_WAKE_FAIL);
    }

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!ok) ESP_LOGW(TAG, "pair not confirmed — confirm the pairing request on the car's screen");
    else     ESP_LOGI(TAG, "pair confirmed on the car's screen");
    return ok;
}
