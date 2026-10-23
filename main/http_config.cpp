// Setup / pairing / persisted-config endpoints:
//   POST /gen_keys[?force=1]  (generate key — refuses overwrite w/o force)
//   POST /set_time            (browser wall clock — NTP fallback)
//   POST /set_vin             (persist VIN + reboot)
//   POST /set_mqtt            (persist MQTT broker + reboot)
//   POST /set_syslog          (persist Syslog server + reboot)
//   POST /set_wifi            (persist WiFi credentials + reboot, with a one-shot rollback backup)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "mqtt_probe_owner.hpp"
#include "config_blob.hpp"
#include "logic/config_request.hpp"
#include "logic/http_route.hpp"
#include "logic/json_syntax.hpp"
#include "json_input_validation.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/mqtt_uri.hpp"
#include "logic/wifi_credentials.hpp"
#include "logic/wifi_rollback.hpp"
#include "logic/vin_transition.hpp"
#include "ota_update.hpp"   // request-owned save/restart and typed image confirmation
#include <esp_log.h>
#include <esp_system.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <mqtt_client.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstdlib>
#include <cctype>
#include <memory>
#include <string>

static const char* TAG = "http_server";

namespace {

// Parse one required JSON object without collapsing transport failures into a syntactically valid
// empty configuration. The body buffer and cJSON tree are RAII-owned, so a std::bad_alloc while a
// field is copied still releases both before handle_all turns the exception into a 503.
struct ParsedJsonBody {
    tk::JsonOwner json;
    tk::ConfigSubmissionStatus status{tk::ConfigSubmissionStatus::MalformedJson};

    explicit operator bool() const { return json != nullptr; }
};

tk::ConfigSubmissionStatus body_submission_status_(tk::BodyReadStatus status) {
    switch (status) {
        case tk::BodyReadStatus::Ok:            return tk::ConfigSubmissionStatus::Ready;
        case tk::BodyReadStatus::Empty:         return tk::ConfigSubmissionStatus::MissingBody;
        case tk::BodyReadStatus::TooLarge:      return tk::ConfigSubmissionStatus::TooLarge;
        case tk::BodyReadStatus::NoMemory:      return tk::ConfigSubmissionStatus::BodyNoMemory;
        case tk::BodyReadStatus::ReceiveFailed: return tk::ConfigSubmissionStatus::ReceiveFailed;
    }
    return tk::ConfigSubmissionStatus::ReceiveFailed;
}

ParsedJsonBody parse_json_object_body_(httpd_req_t* req) {
    tk::BodyReadResult body = read_body_result(req);
    if (body.status != tk::BodyReadStatus::Ok) {
        return {tk::JsonOwner{}, body_submission_status_(body.status)};
    }

    std::unique_ptr<char, decltype(&free)> body_owner(body.data, &free);
    // cJSON reports both malformed input and allocator exhaustion as nullptr. The shared wrapper
    // validates without allocating first, then classifies a parser nullptr as resource failure.
    const auto materialized = tk::json_materialize<cJSON>(
        body.data, req->content_len, [](const char* text) { return cJSON_Parse(text); }, tk::kJsonMaxRequestValues);
    if (materialized.status == tk::JsonMaterializeStatus::Malformed) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::MalformedJson};
    }
    if (materialized.status == tk::JsonMaterializeStatus::TooDeep) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonTooDeep};
    }
    if (materialized.status == tk::JsonMaterializeStatus::UnsupportedNul) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonUnsupportedNul};
    }
    if (materialized.status == tk::JsonMaterializeStatus::TooManyValues) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonTooManyValues};
    }
    if (materialized.status == tk::JsonMaterializeStatus::NoMemory) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonNoMemory};
    }
    tk::JsonOwner json(materialized.root);
    if (!cJSON_IsObject(json.get())) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::ObjectRequired};
    }
    if (tk::has_duplicate_json_keys(json.get())) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::DuplicateJsonKey};
    }
    return {std::move(json), tk::ConfigSubmissionStatus::Ready};
}

esp_err_t send_config_body_error_(httpd_req_t* req, const char* command,
                                  const ParsedJsonBody& body) {
    return send_json(req, tk::config_submission_http_status(body.status),
                     make_response(false, command, "",
                                   tk::config_submission_reason(body.status)));
}

const char* required_string_(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : nullptr;
}

tk::ConfigStringSubmission parse_string_submission_(httpd_req_t* req, const char* key) {
    ParsedJsonBody body = parse_json_object_body_(req);
    if (!body) return {body.status, {}};
    const char* value = required_string_(body.json.get(), key);
    if (!value) return {tk::ConfigSubmissionStatus::StringFieldRequired, {}};
    return {tk::ConfigSubmissionStatus::Ready, value};
}

// Every route below is a read-modify-save of the whole ConfigBlob. When the stored blob exists but
// cannot be read or decoded, the legacy per-key mirrors are only best-effort copies that a later
// committed blob save deliberately leaves stale, so building a change on them would silently write
// an old VIN, WiFi pair or service setting back. Refuse before persisting anything.
esp_err_t send_config_unreadable_(httpd_req_t* req, const char* command, const char* vin = "") {
    return send_json(req, 503,
                     make_response(false, command, vin,
                                   "stored configuration is unreadable; nothing was changed"));
}

}  // namespace

// ─── POST /gen_keys ───────────────────────────────────────────────────────────

esp_err_t handle_gen_keys(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (validate_query_string(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query string");
    }
    OtaIdentityMutationGuard identity_guard(tk::IdentityMutationEntry::HttpGenerateKey);
    if (!identity_guard) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "reason",
            "key generation is blocked during OTA verification/update; no key was changed");
        return send_json(req, 503, json.release());
    }
    // Refuse to silently overwrite an existing key: regenerating un-pairs the device and breaks
    // charging until a physical re-pair. The controller checks this under command_mutex_, not as
    // a racy preflight that auto-pair could invalidate before mutation.
    const bool allow_replace = query_param_is(req, "force", "1");
    rq.reply()->state.shape = tk::ReplyShape::FlatResult;
    rq.reply()->state.attempting(tk::RequestEffect::Identity);
    const VehicleController::KeyGenerationResult generated =
        g_vehicle->generate_key_result(allow_replace);
    if (generated.key_probe_failed) {
        rq.reply()->state.possible_effects = 0;
        rq.reply()->state.outcome = tk::RequestOutcome::NotAttempted;
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "reason",
            "private-key storage could not be verified; no key was changed");
        return send_json(req, 503, json.release());
    }
    if (generated.existing_key_refused) {
        rq.reply()->state.possible_effects = 0;
        rq.reply()->state.outcome = tk::RequestOutcome::NotAttempted;
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "reason",
            "a key already exists — regenerating un-pairs the vehicle; "
            "call /gen_keys?force=1 to replace it");
        return send_json(req, 409, json.release());
    }
    if (generated.transition_blocked) {
        rq.reply()->state.possible_effects = 0;
        rq.reply()->state.outcome = tk::RequestOutcome::NotAttempted;
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "reason",
            "a VIN transition is awaiting reboot; key generation is temporarily blocked");
        return send_json(req, 409, json.release());
    }

    const tk::KeyRotationResult rotation = generated.rotation;
    const bool ok = rotation == tk::KeyRotationResult::Complete;
    // CommitUnknown means the private-key NVS write was attempted but its commit result is
    // unknowable from RAM. The persistent marker makes reboot cleanup safe, and reconstructing
    // from storage is the only authoritative recovery. NotCommitted is a pre-mutation failure;
    // reboot remains the conservative way to restore any gated runtime state. CleanupPending
    // proves the new key committed, so the supervisor retries only idempotent cleanup.
    const bool reboot_to_restore = rotation == tk::KeyRotationResult::NotCommitted ||
                                   rotation == tk::KeyRotationResult::CommitUnknown;
    if (reboot_to_restore) {
        rq.reply()->state.continuation = tk::ReplyContinuation::IdentityRestart;
        rq.reply()->state.recovery_pending = true;
    }
    if (ok) rq.reply()->state.succeeded();
    else if (rotation == tk::KeyRotationResult::CleanupPending)
        rq.reply()->state.identity_recovery();
    tk::JsonBuilder json;
    json.boolean(json.root(), "result", ok);
    json.string(
        json.root(), "reason",
        ok ? "key generated — pairing proceeds automatically"
           : rotation == tk::KeyRotationResult::CommitUnknown
               ? "key commit outcome is ambiguous — rebooting to reload durable identity"
           : reboot_to_restore
               ? "key generation did not commit — rebooting to restore durable identity"
               : "new key saved, but pairing cleanup failed — retrying cleanup");
    esp_err_t r = send_effect_json(rq, ok ? 200 : 500, json.release());
    run_effect_continuation(rq);
    return r;
}

// ─── POST /set_time — temporary browser estimate while NTP has not committed ──
// Browser time is neither persisted nor authority for durable identity dates.
esp_err_t handle_set_time(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (tk::http_route_blocks_during_ota_update_409(tk::HttpRoute::SetTime) &&
        ota_is_updating()) {
        return send_json(req, static_cast<int>(tk::HttpRouteOtaConflict::Reject409),
                         make_response(false, "set_time", "", tk::kOtaUpdateInProgressReason.data()));
    }
    ParsedJsonBody parsed = parse_json_object_body_(req);
    if (!parsed) return send_config_body_error_(req, "set_time", parsed);
    cJSON* j = cJSON_GetObjectItemCaseSensitive(parsed.json.get(), "ms");
    if (!cJSON_IsNumber(j)) {
        return send_json(req, 400,
                         make_response(false, "set_time", "", "numeric ms field required"));
    }
    const double epoch_ms = j->valuedouble;
    parsed.json.reset();
    // Even an NTP no-op requires the complete valid body/schema above.
    if (clock_synced_via_ntp()) {
        return send_json(req, 200, make_response(true, "set_time", "", "clock set via NTP"));
    }
    rq.reply()->state.attempting(tk::RequestEffect::Clock);
    const BrowserClockResult result = apply_browser_clock(epoch_ms);
    if (result.status != BrowserClockStatus::Applied &&
        result.status != BrowserClockStatus::SetFailed) {
        rq.reply()->state.refused(tk::RequestEffect::Clock);
        if (result.status == BrowserClockStatus::AlreadyNtpSynced)
            return send_json(req, 200, make_response(true, "set_time", "", "clock set via NTP"));
        const bool busy = result.status == BrowserClockStatus::Busy;
        return send_json(req, busy ? 503 : 400, make_response(false, "set_time", "",
                          busy ? "clock is busy" : "implausible timestamp"));
    }
    if (result.status == BrowserClockStatus::SetFailed)
        return send_effect_json(rq, 500, make_response(false, "set_time", "", "clock set failed"));
    ESP_LOGI(TAG, "temporary clock set from browser: %lld", (long long)result.seconds);
    return send_effect_json(rq, 200, make_response(true, "set_time", "", "clock set"));
}

// ─── POST /set_vin — persist VIN, then reboot ─────────────────────────────────

esp_err_t handle_set_vin(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    ParsedJsonBody parsed = parse_json_object_body_(req);
    if (!parsed) return send_config_body_error_(req, "set_vin", parsed);
    const char* submitted_vin = required_string_(parsed.json.get(), "vin");
    if (!submitted_vin) {
        return send_json(req, 400,
                         make_response(false, "set_vin", "", "string vin field required"));
    }
    std::string vin = submitted_vin;
    parsed.json.reset();

    // Normalise to the canonical stored form (trim, uppercase) before validating or
    // comparing, so "unchanged" is judged on the stored representation, not on casing.
    size_t s = vin.find_first_not_of(" \t\r\n");
    size_t e = vin.find_last_not_of(" \t\r\n");
    vin = (s == std::string::npos) ? std::string{} : vin.substr(s, e - s + 1);
    for (char& c : vin) c = (char)std::toupper((unsigned char)c);

    tk::ConfigBlob current;
    if (!tk::cfg_load_for_update(*g_config, current)) {
        return send_config_unreadable_(req, "set_vin", vin.c_str());
    }

    // Unchanged → nothing to apply: skip the NVS write and the reboot entirely.
    if (vin == current.vin) {
        return send_json(req, 200, make_response(true, "set_vin", vin.c_str(),
                                                 "VIN unchanged — no reboot"));
    }

    // Validate plausibility before applying a *changed* value. Shared with the BLE pairing
    // gate (VehicleController::vin_is_plausible) so the web check and the pairing check agree.
    if (!VehicleController::vin_is_plausible(vin)) {
        return send_json(req, 400, make_response(false, "set_vin", vin.c_str(),
                                                 "VIN must be 17 valid characters"));
    }
    OtaIdentityMutationGuard identity_guard(tk::IdentityMutationEntry::HttpSetVin);
    if (!identity_guard) {
        return send_json(req, 503, make_response(
            false, "set_vin", vin.c_str(),
            "VIN changes are blocked during OTA verification/update; no identity was changed"));
    }

    // Stage the complete configuration and rotate the identity inside ONE controller transaction.
    // The callback runs while command_mutex_ binds the previous fingerprint to this request, so
    // auto-rekey cannot slip between fingerprint capture, journal/config persistence and reset.
    tk::ConfigBlob next = current;
    next.vin = vin;
    bool vin_marker_written = false;
    rq.reply()->state.attempting(tk::RequestEffect::Identity);
    const VehicleController::NewVehicleResetResult reset =
        g_vehicle->reset_for_new_vehicle([&](const std::string& previous_key_id) {
            // Cross-namespace journal: ConfigBlob is in tesla_cfg, key/session state in
            // tesla_ble. It is written before ConfigBlob and before key_rotate/private-key
            // mutation, so every power cut has an unambiguous boot-recovery authority.
            const std::string marker =
                tk::make_vin_transition_marker(current.vin, previous_key_id);
            if (marker.empty()) return false; // never persist an undecodable identity journal
            if (!g_config->save_str(tk::nvs_contract::kVinTransition, marker)) return false;
            vin_marker_written = true;
            return tk::cfg_save(*g_config, next);
        });

    using A = tk::VinTransitionApply;
    const bool ok = reset.state == A::Complete;
    const bool recovery_blocked = reset.state == A::IdentityRecoveryPending;
    if (reset.state == A::IdentityUnverified || recovery_blocked) {
        // Both typed refusals return before staging/journaling/key mutation.
        rq.reply()->state.possible_effects = 0;
        rq.reply()->state.outcome = tk::RequestOutcome::NotAttempted;
    }
    bool reboot = reset.state != A::IdentityUnverified && !recovery_blocked;
    if (reboot) {
        rq.reply()->state.continuation = tk::ReplyContinuation::IdentityRestart;
        // A known committed identity remains partial until its local post-result work ends.
        rq.reply()->state.recovery_pending = true;
    }
    if (reset.state == A::Complete || reset.state == A::RecoverCommittedIdentity)
        rq.reply()->state.identity_recovery();
    bool previous_identity_restored = false;
    bool recovery_pending = false;

    if (reset.state == A::StageFailed || reset.state == A::RollBackPreviousIdentity) {
        // A false NVS commit is conservatively ambiguous: rewrite the complete previous snapshot,
        // then retire the VIN journal only after that rollback commits. key_rotate (if present)
        // remains independent and makes boot erase sessions before reconstructing the old key.
        ESP_LOGE(TAG, "VIN change did not commit a new key; restoring previous configuration");
        const bool rolled_back = !vin_marker_written || tk::cfg_save(*g_config, current);
        const bool marker_removed =
            !vin_marker_written ||
                (rolled_back && g_config->remove(tk::nvs_contract::kVinTransition));
        previous_identity_restored = rolled_back && marker_removed;
        recovery_pending = !previous_identity_restored;
        if (recovery_pending) {
            ESP_LOGE(TAG, "VIN rollback/marker cleanup incomplete — boot recovery remains armed");
        }
    } else if (reset.state == A::RecoverAmbiguousIdentity) {
        // The upstream key write was attempted but its commit result cannot be inferred from
        // this Vehicle's RAM key. Keep BOTH the staged ConfigBlob and vin_txn: after key_rotate
        // cleanup, boot reloads the durable fingerprint and deterministically rolls back the old
        // VIN or completes the new identity.
        recovery_pending = true;
        ESP_LOGE(TAG, "VIN staged but key commit outcome is ambiguous — boot fingerprint recovery remains armed");
    } else if (reset.state == A::RecoverCommittedIdentity) {
        // The request-local result proves the new key commit; never infer this from a fingerprint
        // sampled after releasing command_mutex_. Keep the new VIN + vin_txn for boot cleanup.
        recovery_pending = true;
        ESP_LOGE(TAG, "VIN and new key committed, but cleanup is incomplete — boot recovery remains armed");
    } else if (reset.state == A::Complete) {
        if (!g_config->remove(tk::nvs_contract::kVinTransition)) {
            // The durable new VIN/key tuple is valid. A remaining marker only causes the boot
            // path to repeat idempotent session/MAC cleanup before normal operation.
            ESP_LOGW(TAG, "VIN change committed but transition marker remains for boot recovery");
        }
    }

    if (ok) {
        rq.reply()->state.succeeded();
        rq.reply()->state.recovery_pending = false;
    }
    const char* reason = reset.state == A::Complete
        ? "VIN and new key saved — rebooting"
        : reset.state == A::IdentityUnverified
            ? "existing key identity could not be verified"
            : recovery_blocked
                ? "key identity recovery is pending — retry VIN change after recovery"
            : reset.state == A::RecoverAmbiguousIdentity
                ? "key commit outcome is ambiguous — rebooting for fingerprint recovery"
            : reset.state == A::RecoverCommittedIdentity
                ? "VIN and new key saved; cleanup incomplete — rebooting for recovery"
                : previous_identity_restored
                    ? "VIN change failed; previous identity restored — rebooting to reload its key"
                    : recovery_pending
                        ? "VIN change incomplete — rebooting for recovery"
                        : "VIN change staging failed — rebooting fail-closed";
    esp_err_t r = send_effect_json(rq, ok ? 200 : recovery_blocked ? 409 : 500,
                            make_response(ok, "set_vin", vin.c_str(), reason));
    run_effect_continuation(rq);
    return r;
}

// ─── POST /set_mqtt — verify the broker, persist it, then reboot ──────────────
// Body: {"broker":"host:port"} (a full "mqtt://host:port" URI is also accepted; an
// empty string disables MQTT). Stored in NVS ("mqtt_uri") and applied on reboot —
// the bridge reads it once at start, so a reboot is the clean way to (re)init it.
//
// TEST BEFORE PERSIST. A changed broker is CONNECTED TO before it is written. Without that,
// the only way to learn a broker is wrong is to save it, reboot, and read mqtt.error off
// /status afterwards — and with credentials present the URI is mqtts://, where the usual
// mistake (an untrusted certificate, or a password the broker refuses) is invisible until
// after that reboot. The probe dials the URI logic/mqtt_uri.hpp derives, which is the same
// one mqtt_ha.cpp will dial at boot; anything else would be a green check for a connection
// the bridge never makes.

// What the probe learned, handed from the mqtt task to this one. Plain data, no std::string:
// the callback runs off the httpd worker with no try/catch above it, so an allocation there
// could unwind through esp-mqtt's C frames into abort().
struct MqttProbeCtx {
    SemaphoreHandle_t   sem       = nullptr;
    bool                connected = false;
    tk::MqttProbeResult result    = tk::MqttProbeResult::Timeout;
};

struct EspMqttProbeOps {
    using Client = esp_mqtt_client_handle_t;
    using Semaphore = SemaphoreHandle_t;

    static void stop(Client client) noexcept { (void)esp_mqtt_client_stop(client); }
    static void destroy(Client client) noexcept { (void)esp_mqtt_client_destroy(client); }
    static void delete_semaphore(Semaphore sem) noexcept { vSemaphoreDelete(sem); }
};

struct MqttProbeResources {
    // Declaration order matters: owner is destroyed first, joining the callback task while its
    // plain-data context is still alive; ctx is destroyed only after all C handles are gone.
    MqttProbeCtx ctx{};
    tk::MqttProbeResourceOwner<EspMqttProbeOps> owner{};
};

static void on_mqtt_probe(void* handler_args, esp_event_base_t, int32_t id, void* event_data) {
    auto* ctx = static_cast<MqttProbeCtx*>(handler_args);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        ctx->connected = true;
        ctx->result    = tk::MqttProbeResult::Ok;
        xSemaphoreGive(ctx->sem);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (!ctx->connected) xSemaphoreGive(ctx->sem);
        break;
    case MQTT_EVENT_ERROR: {
        auto* e = static_cast<esp_mqtt_event_handle_t>(event_data);
        if (e && e->error_handle) {
            // A refusal and an unreachable broker are different answers to the user: one means
            // "your credentials are wrong" (their input, 400), the other "nothing answered"
            // (the network, 502). Collapsing them into one message is what makes a broker
            // outage look like a typo.
            ctx->result = (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
                              ? tk::MqttProbeResult::Unreachable
                              : tk::MqttProbeResult::Refused;
        }
        // Wake the request thread now, with the precise cause, rather than waiting for the
        // DISCONNECTED that usually — but not always — follows: a late one past the timeout
        // would report a generic "did not answer" and lose the real reason. A second give on a
        // binary semaphore is a harmless no-op.
        if (!ctx->connected) xSemaphoreGive(ctx->sem);
        break;
    }
    default: break;
    }
}

// Connect to `uri` once and report how it went. Never persists anything. All throwing request
// preparation happens before resource acquisition; acquired C handles are RAII-owned so an
// exception added later cannot leak the semaphore/client or leave a callback racing its stack ctx.
//
// It blocks the httpd task for up to 8 s, which also blocks every other request — evcc's poll
// included. That is accepted rather than overlooked: this endpoint already ends in a reboot, a BLE
// command on the same task routinely takes 3-5 s, and the alternative (answering 202 and reporting
// the verdict somewhere else) would give the browser no way to show the user why their broker was
// rejected at the moment they typed it.
static tk::MqttProbeResult mqtt_probe_broker(const std::string& uri) {
    const bool tls = tk::mqtt_uri_is_tls(uri);

    // Can we afford a second client beside the live bridge's? INTERNAL largest block, for the
    // same reason the heap watchdog samples it: mbedTLS's buffers are contiguous internal DRAM
    // and a plain 8BIT query would report any PSRAM as if it could satisfy them. Refusing here
    // costs the user a retry; not refusing costs a bad_alloc on the HTTP task.
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!tk::mqtt_probe_affordable(largest, tls)) {
        ESP_LOGW(TAG, "set_mqtt: broker check skipped — largest block %u < %u needed; not saving",
                 (unsigned) largest, (unsigned) tk::mqtt_probe_heap_need(tls));
        return tk::MqttProbeResult::NoHeap;
    }

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri.c_str();
    if (tls) cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    // sdkconfig credentials are immortal literals. Referencing them directly avoids two needless
    // std::string allocations and guarantees there is no throwing construction after acquire.
    if (CONFIG_TESLA_MQTT_USERNAME[0] != '\0')
        cfg.credentials.username = CONFIG_TESLA_MQTT_USERNAME;
    if (CONFIG_TESLA_MQTT_PASSWORD[0] != '\0')
        cfg.credentials.authentication.password = CONFIG_TESLA_MQTT_PASSWORD;
    cfg.credentials.client_id = "teslakey_probe";
    cfg.session.keepalive     = 15;
    // One attempt, not a retry loop: this is a question, and esp-mqtt's default reconnect would
    // keep dialling a wrong broker in the background after the answer was already reported.
    cfg.network.disable_auto_reconnect = true;

    MqttProbeResources resources;
    resources.owner.sem = xSemaphoreCreateBinary();
    resources.ctx.sem = resources.owner.sem;
    if (!resources.ctx.sem) return tk::MqttProbeResult::Internal;
    resources.owner.client = esp_mqtt_client_init(&cfg);
    if (!resources.owner.client) return tk::MqttProbeResult::Internal;
    if (esp_mqtt_client_register_event(resources.owner.client,
                                       static_cast<esp_mqtt_event_id_t>(MQTT_EVENT_ANY),
                                       on_mqtt_probe, &resources.ctx) != ESP_OK) {
        return tk::MqttProbeResult::Internal;
    }

    // A failed start emits no event at all, so waiting would burn the whole window and then
    // blame a timeout. Nothing was started, so nothing is stopped.
    if (esp_mqtt_client_start(resources.owner.client) != ESP_OK)
        return tk::MqttProbeResult::Internal;
    resources.owner.started = true;

    const bool answered = xSemaphoreTake(resources.ctx.sem, pdMS_TO_TICKS(8000)) == pdTRUE;

    if (!answered) return tk::MqttProbeResult::Timeout;
    // Join the callback task before reading the shared verdict. The owner still destroys the
    // client/semaphore after this function copies the now-stable plain-data result.
    if (esp_mqtt_client_stop(resources.owner.client) != ESP_OK)
        return tk::MqttProbeResult::Internal;
    resources.owner.started = false;
    return resources.ctx.result;
}

[[gnu::noinline]] static esp_err_t apply_mqtt_submission_(
    GuardedReq rq, tk::ConfigBlob& cfg, const tk::ConfigStringSubmission& submitted) {
    return tk::apply_config_string(
        submitted,
        [](const std::string& value) { return tk::mqtt_trim(value); },
        [&]() { return cfg.mqtt_uri; },
        [](const std::string& value) { return tk::mqtt_broker_is_plausible(value); },
        [&](const std::string& broker) {
            // Only a non-empty broker is probed — an empty value explicitly disables the bridge.
            if (!broker.empty()) {
                const std::string uri = tk::mqtt_effective_uri(
                    broker, !std::string(CONFIG_TESLA_MQTT_USERNAME).empty());
                const tk::MqttProbeResult result = mqtt_probe_broker(uri);
                if (result != tk::MqttProbeResult::Ok) {
                    ESP_LOGW(TAG, "set_mqtt: broker check failed (%s) — not saving",
                             tk::mqtt_probe_reason(result));
                    return tk::ConfigProbeVerdict{false, tk::mqtt_probe_http_status(result),
                                                  tk::mqtt_probe_reason(result)};
                }
            }
            return rq.context->config_restart.try_begin()
                ? tk::ConfigProbeVerdict{}
                : tk::ConfigProbeVerdict{false, 409, tk::kConfigRestartBusyReason};
        },
        [&](const std::string& broker) {
            cfg.mqtt_uri = broker;
            rq.reply()->state.attempting(tk::RequestEffect::Configuration);
            const bool stored = tk::cfg_save(*g_config, cfg);
            if (stored) finish_config_save(rq);
            return stored;
        },
        [&](int status, bool ok, const char* reason) {
            return send_config_reply(rq, status, ok, "set_mqtt", reason);
        },
        [&]() {
            run_effect_continuation(rq);
        },
        {"MQTT already disabled — no reboot", "MQTT broker unchanged — no reboot",
         "invalid broker (use host:port)", "MQTT disabled — rebooting",
         "MQTT broker saved — rebooting", "failed to save MQTT broker"});
}

esp_err_t handle_set_mqtt(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (tk::http_route_blocks_during_ota_update_409(tk::HttpRoute::SetMqtt) &&
        ota_is_updating()) {
        return send_json(req, static_cast<int>(tk::HttpRouteOtaConflict::Reject409),
                         make_response(false, "set_mqtt", "", tk::kOtaUpdateInProgressReason.data()));
    }
    const tk::ConfigStringSubmission submitted = parse_string_submission_(req, "broker");
    if (submitted.status != tk::ConfigSubmissionStatus::Ready) {
        return send_json(req, tk::config_submission_http_status(submitted.status),
            make_response(false, "set_mqtt", "", tk::config_submission_reason(submitted.status)));
    }
    tk::ConfigBlob cfg;
    if (!tk::cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_mqtt");
    return apply_mqtt_submission_(rq, cfg, submitted);
}

// ─── POST /set_syslog — persist the Syslog server, then reboot ───────────────────
// Body: {"server":"host:port"} (a bare host defaults to port 514; an empty string
// disables Syslog). Stored in NVS ("syslog_uri") and applied on reboot — the
// forwarder resolves it once at start (syslog.cpp), same as /set_mqtt.

[[gnu::noinline]] static esp_err_t apply_syslog_submission_(
    GuardedReq rq, tk::ConfigBlob& cfg, const tk::ConfigStringSubmission& submitted) {
    return tk::apply_config_string(
        submitted,
        [](const std::string& value) {
            const size_t start = value.find_first_not_of(" \t\r\n");
            const size_t end = value.find_last_not_of(" \t\r\n");
            return start == std::string::npos ? std::string{}
                                              : value.substr(start, end - start + 1);
        },
        [&]() { return cfg.syslog_uri; },
        [](const std::string& value) { return tk::syslog_target_is_plausible(value); },
        [&](const std::string&) {
            return rq.context->config_restart.try_begin()
                ? tk::ConfigProbeVerdict{}
                : tk::ConfigProbeVerdict{false, 409, tk::kConfigRestartBusyReason};
        },
        [&](const std::string& server) {
            cfg.syslog_uri = server;
            rq.reply()->state.attempting(tk::RequestEffect::Configuration);
            const bool stored = tk::cfg_save(*g_config, cfg);
            if (stored) finish_config_save(rq);
            return stored;
        },
        [&](int status, bool ok, const char* reason) {
            return send_config_reply(rq, status, ok, "set_syslog", reason);
        },
        [&]() {
            run_effect_continuation(rq);
        },
        {"Syslog already disabled — no reboot", "Syslog server unchanged — no reboot",
         "invalid server (use host:port)", "Syslog disabled — rebooting",
         "Syslog server saved — rebooting", "failed to save Syslog server"});
}

esp_err_t handle_set_syslog(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (tk::http_route_blocks_during_ota_update_409(tk::HttpRoute::SetSyslog) &&
        ota_is_updating()) {
        return send_json(req, static_cast<int>(tk::HttpRouteOtaConflict::Reject409),
                         make_response(false, "set_syslog", "", tk::kOtaUpdateInProgressReason.data()));
    }
    const tk::ConfigStringSubmission submitted = parse_string_submission_(req, "server");
    if (submitted.status != tk::ConfigSubmissionStatus::Ready) {
        return send_json(req, tk::config_submission_http_status(submitted.status),
            make_response(false, "set_syslog", "", tk::config_submission_reason(submitted.status)));
    }
    tk::ConfigBlob cfg;
    if (!tk::cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_syslog");
    return apply_syslog_submission_(rq, cfg, submitted);
}

// ─── POST /set_wifi — change the WiFi credentials over the LAN, undoably ──────
//
// stage_wifi_change preserves the last proven credentials inside the same ConfigBlob while a new
// pair is on trial. Boot either clears that backup after a lease or uses logic/wifi_rollback.hpp
// to decide a bounded restore. Save/response/restart ordering is shared with other config routes.
[[gnu::noinline]] static esp_err_t save_wifi_submission_(GuardedReq rq,
                                                         const tk::ConfigBlob& cfg) {
    rq.reply()->state.attempting(tk::RequestEffect::Configuration);
    const bool ok = tk::cfg_save(*g_config, cfg);
    if (ok) {
        rq.reply()->state.succeeded();
        rq.reply()->state.continuation = tk::ReplyContinuation::SuccessfulConfigRestart;
    }
    esp_err_t r = send_effect_json(rq, ok ? 200 : 500,
        make_response(ok, "set_wifi", "",
                      ok ? "WiFi credentials accepted"
                         : "config write failed"));
    run_effect_continuation(rq);
    return r;
}

[[gnu::noinline]] static esp_err_t apply_wifi_submission_(GuardedReq rq,
                                                          const std::string& ssid,
                                                          const std::string& pass) {
    httpd_req_t* req = rq.req();
    tk::ConfigBlob cfg;
    if (!tk::cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_wifi");

    // Refuse uniformly before comparing credentials: the refusal must not reveal equality.
    if (!tk::config_service_targets_fit_old_writer(cfg)) {
        return send_json(req, 409, make_response(false, "set_wifi", "",
            "shorten existing service addresses to 64 bytes before changing WiFi"));
    }

    if (!rq.context->config_restart.try_begin())
        return send_json(req, 409, make_response(false, "set_wifi", "", tk::kConfigRestartBusyReason));
    if (!tk::config_wifi_may_save(ota_config_image_state(rq.context->config_restart)))
        return send_json(req, 409, make_response(false, "set_wifi", "", tk::kConfigWifiVerificationReason));

    if (cfg.wifi_ssid == ssid && cfg.wifi_pass == pass) {
        return send_json(req, 200, make_response(true, "set_wifi", "",
                                                 "WiFi credentials accepted"));
    }

    // Arm the one-shot rollback only when there is something to roll back TO, and keep the last
    // PROVEN pair as the backup while an earlier change is still on trial (a deferred restart):
    // the host-tested logic/wifi_rollback.hpp. A new attempt also retires the previous verdict, so
    // /status.wifi.rolled_back describes the LAST attempt.
    tk::stage_wifi_change(cfg, ssid, pass);

    return save_wifi_submission_(rq, cfg);
}

esp_err_t handle_set_wifi(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (tk::http_route_blocks_during_ota_update_409(tk::HttpRoute::SetWifi) &&
        ota_is_updating()) {
        return send_json(req, static_cast<int>(tk::HttpRouteOtaConflict::Reject409),
                         make_response(false, "set_wifi", "", tk::kOtaUpdateInProgressReason.data()));
    }
    ParsedJsonBody parsed = parse_json_object_body_(req);
    if (!parsed) return send_config_body_error_(req, "set_wifi", parsed);

    const char* submitted_ssid = required_string_(parsed.json.get(), "ssid");
    const char* submitted_pass = required_string_(parsed.json.get(), "pass");
    // An explicit empty passphrase represents an open AP; a missing/wrong-typed field represents
    // a malformed request. Keeping those states apart prevents bad JSON from silently removing a
    // previously stored password and rebooting the board onto a different network contract.
    if (!submitted_ssid || !submitted_pass) {
        return send_json(req, 400,
                         make_response(false, "set_wifi", "",
                                       "string ssid and pass fields required"));
    }
    std::string ssid = submitted_ssid;
    std::string pass = submitted_pass;
    parsed.json.reset();

    // Exactly the same contract is enforced by the captive setup endpoint and host-tested in
    // logic/wifi_credentials.hpp. Empty means open AP; 64 bytes are accepted only as a raw hex PSK.
    const tk::WifiCredentialError wifi_error = tk::wifi_credentials_error(ssid, pass);
    if (wifi_error != tk::WifiCredentialError::None) {
        return send_json(req, 400, make_response(false, "set_wifi", "",
                                                 tk::wifi_credentials_reason(wifi_error)));
    }

    return apply_wifi_submission_(rq, ssid, pass);
}

// ─── POST /set_ota — update the OTA update channel (live, no reboot) ─────────────
// Body: {"channel":"release"|"dev"}
// Persists the channel in ConfigBlob v1 reserved flag bits and updates the active channel.
esp_err_t handle_set_ota(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (tk::http_route_blocks_during_ota_update_409(tk::HttpRoute::SetOta) &&
        ota_is_updating()) {
        return send_json(req, static_cast<int>(tk::HttpRouteOtaConflict::Reject409),
                         make_response(false, "set_ota", "", tk::kOtaUpdateInProgressReason.data()));
    }
    const tk::ConfigStringSubmission submitted = parse_string_submission_(req, "channel");
    if (submitted.status != tk::ConfigSubmissionStatus::Ready) {
        return send_json(req, tk::config_submission_http_status(submitted.status),
                         make_response(false, "set_ota", "",
                                       tk::config_submission_reason(submitted.status)));
    }

    const std::string& channel = submitted.value;
    if (!tk::ota_channel_valid(channel)) {
        return send_json(req, 400, make_response(false, "set_ota", "", "invalid channel (use release or dev)"));
    }

    tk::ConfigBlob cfg;
    if (!tk::cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_ota");
    const tk::OtaChannel want = tk::ota_channel_parse(channel);
    if (cfg.has_ota && tk::ota_channel_from_int(cfg.ota_channel) == want) {
        rq.reply()->state.shape = tk::ReplyShape::FlatResult;
        rq.reply()->state.attempting(tk::RequestEffect::Configuration);
        ota_set_channel(want);
        rq.reply()->state.succeeded();
        tk::JsonBuilder resp;
        resp.boolean(resp.root(), "ok", true);
        resp.boolean(resp.root(), "result", true);
        resp.string(resp.root(), "channel", tk::ota_channel_name(want));
        return send_effect_json(rq, 200, resp.release());
    }

    cfg.ota_channel = tk::ota_channel_to_int(want);
    cfg.has_ota = true;
    rq.reply()->state.attempting(tk::RequestEffect::Configuration);
    if (!tk::cfg_save(*g_config, cfg)) {
        return send_effect_json(rq, 500, make_response(false, "set_ota", "", "failed to save config"));
    }

    rq.reply()->state.shape = tk::ReplyShape::FlatResult;
    ota_set_channel(want);
    rq.reply()->state.succeeded();
    ESP_LOGI(TAG, "OTA channel set to %s", tk::ota_channel_name(want));

    tk::JsonBuilder resp;
    resp.boolean(resp.root(), "ok", true);
    resp.boolean(resp.root(), "result", true);
    resp.string(resp.root(), "channel", tk::ota_channel_name(want));
    return send_effect_json(rq, 200, resp.release());
}
