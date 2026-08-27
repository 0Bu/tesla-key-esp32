// Setup / pairing / persisted-config endpoints:
//   POST /gen_keys[?force=1]  (generate key — refuses overwrite w/o force)
//   POST /send_key            (pair with vehicle, Charging Manager only)
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
#include "logic/json_syntax.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/mqtt_uri.hpp"
#include "logic/wifi_credentials.hpp"
#include "logic/vin_transition.hpp"
#include "ota_update.hpp"   // ota_confirm_pending_image() — guard OTA rollback across config reboots
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
        body.data, req->content_len, [](const char* text) { return cJSON_Parse(text); });
    if (materialized.status == tk::JsonMaterializeStatus::Malformed) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::MalformedJson};
    }
    if (materialized.status == tk::JsonMaterializeStatus::TooDeep) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonTooDeep};
    }
    if (materialized.status == tk::JsonMaterializeStatus::UnsupportedNul) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonUnsupportedNul};
    }
    if (materialized.status == tk::JsonMaterializeStatus::NoMemory) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonNoMemory};
    }
    tk::JsonOwner json(materialized.root);
    if (!cJSON_IsObject(json.get())) {
        return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::ObjectRequired};
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

}  // namespace

// ─── POST /gen_keys ───────────────────────────────────────────────────────────

esp_err_t handle_gen_keys(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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
    const VehicleController::KeyGenerationResult generated =
        g_vehicle->generate_key_result(allow_replace);
    if (generated.key_probe_failed) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "reason",
            "private-key storage could not be verified; no key was changed");
        return send_json(req, 503, json.release());
    }
    if (generated.existing_key_refused) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "reason",
            "a key already exists — regenerating un-pairs the vehicle; "
            "call /gen_keys?force=1 to replace it");
        return send_json(req, 409, json.release());
    }
    if (generated.transition_blocked) {
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
    tk::JsonBuilder json;
    json.boolean(json.root(), "result", ok);
    json.string(
        json.root(), "reason",
        ok ? "key generated — use /send_key to pair with vehicle"
           : rotation == tk::KeyRotationResult::CommitUnknown
               ? "key commit outcome is ambiguous — rebooting to reload durable identity"
           : reboot_to_restore
               ? "key generation did not commit — rebooting to restore durable identity"
               : "new key saved, but pairing cleanup failed — retrying cleanup");
    esp_err_t r = send_json(req, ok ? 200 : 500, json.release());
    if (reboot_to_restore) {
        // NotCommitted/CommitUnknown are recovery reboots after an HTTP 500, not successful
        // user configuration commits. Keep a pending OTA rollback-capable.
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return r;
}

// ─── POST /send_key ───────────────────────────────────────────────────────────

esp_err_t handle_send_key(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    // This firmware only enrolls a Charging Manager key (charging + wake), never an
    // owner key — its sole purpose is the evcc BLE integration. Reject an explicit
    // owner request rather than silently enrolling a different role than asked for.
    if (query_param_is(req, "role", "owner")) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "role", "owner");
        json.string(json.root(), "reason",
            "owner role disabled — this device only enrolls Charging Manager keys");
        return send_json(req, 403, json.release());
    }

    bool ok = g_vehicle->pair(tk::ConnectOrigin::Foreground);
    tk::JsonBuilder json;
    json.boolean(json.root(), "result", ok);
    json.string(json.root(), "role", "charging_manager");
    json.string(json.root(), "reason",
        ok ? "key sent — confirm the pairing request on the car's screen"
           : "failed to send key (vehicle not reachable or timed out)");
    return send_json(req, 200, json.release());
}

// ─── POST /set_time — set the wall clock from the browser (NTP fallback) ───────
// TLS certificate validation (OTA) and the human-readable key_created/paired_at
// timestamps need a real UTC clock. (tesla-ble signed-command freshness does NOT —
// expires_at is the vehicle's clock plus a monotonic delta.) NTP is the primary
// source; this endpoint is the fallback for networks that block NTP. The web UI posts the browser's clock ({"ms": <epoch ms>})
// on load and before an OTA check, but we only apply it while SNTP has not synced —
// otherwise NTP (which is more trustworthy than a possibly-skewed browser) wins. The
// applied fallback time is persisted so a later offline reboot starts plausibly.
esp_err_t handle_set_time(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    ParsedJsonBody parsed = parse_json_object_body_(req);
    if (!parsed) return send_config_body_error_(req, "set_time", parsed);

    cJSON* j = cJSON_GetObjectItemCaseSensitive(parsed.json.get(), "ms");
    if (!cJSON_IsNumber(j)) {
        return send_json(req, 400,
                         make_response(false, "set_time", "", "numeric ms field required"));
    }
    const double epoch_ms = j->valuedouble;
    parsed.json.reset();

    // NTP already synced -> it is authoritative. Still require a complete, syntactically valid
    // request before accepting the no-op, so body OOM/oversize/receive/schema failures never turn
    // into a misleading success response merely because the clock happened to be synchronized.
    if (clock_synced_via_ntp()) {
        return send_json(req, 200, make_response(true, "set_time", "", "clock set via NTP"));
    }

    // Reject a missing/implausible browser clock so we never push the device clock into the
    // cert-invalid range — floor ~2023-11, ceiling build year + 10 (see browser_time_plausible).
    if (!browser_time_plausible(epoch_ms)) {
        return send_json(req, 400, make_response(false, "set_time", "",
                                                 "implausible timestamp"));
    }

    long long sec = apply_browser_clock(epoch_ms);
    ESP_LOGI(TAG, "clock set from browser: %lld", sec);

    return send_json(req, 200, make_response(true, "set_time", "", "clock set"));
}

// ─── POST /set_vin — persist VIN, then reboot ─────────────────────────────────

esp_err_t handle_set_vin(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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
    tk::cfg_load(*g_config, current);

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
    const VehicleController::NewVehicleResetResult reset =
        g_vehicle->reset_for_new_vehicle([&](const std::string& previous_key_id) {
            // Cross-namespace journal: ConfigBlob is in tesla_cfg, key/session state in
            // tesla_ble. It is written before ConfigBlob and before key_rotate/private-key
            // mutation, so every power cut has an unambiguous boot-recovery authority.
            const std::string marker =
                tk::make_vin_transition_marker(current.vin, previous_key_id);
            if (!g_config->save_str(tk::nvs_contract::kVinTransition, marker)) return false;
            vin_marker_written = true;
            return tk::cfg_save(*g_config, next);
        });

    using A = tk::VinTransitionApply;
    const bool ok = reset.state == A::Complete;
    const bool recovery_blocked = reset.state == A::IdentityRecoveryPending;
    bool reboot = reset.state != A::IdentityUnverified && !recovery_blocked;
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
    esp_err_t r = send_json(req, ok ? 200 : recovery_blocked ? 409 : 500,
                            make_response(ok, "set_vin", vin.c_str(), reason));
    if (reboot) {
        // VIN/key identity changes are admitted only in Stable state by OtaIdentityMutationGuard.
        // Therefore this reboot can never be evidence for a PendingVerify image; recovery and
        // failure reboots likewise leave whatever OTA state exists untouched.
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
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

esp_err_t handle_set_mqtt(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    const tk::ConfigStringSubmission submitted = parse_string_submission_(req, "broker");
    tk::ConfigBlob cfg;
    return tk::apply_config_string(
        submitted,
        [](const std::string& value) { return tk::mqtt_trim(value); },
        [&]() {
            tk::cfg_load(*g_config, cfg);
            return cfg.mqtt_uri;
        },
        [](const std::string& value) { return tk::mqtt_broker_is_plausible(value); },
        [&](const std::string& broker) {
            // Only a non-empty broker is probed — an empty value explicitly disables the bridge.
            if (broker.empty()) return tk::ConfigProbeVerdict{};
            const std::string uri = tk::mqtt_effective_uri(
                broker, !std::string(CONFIG_TESLA_MQTT_USERNAME).empty());
            const tk::MqttProbeResult result = mqtt_probe_broker(uri);
            if (result == tk::MqttProbeResult::Ok) return tk::ConfigProbeVerdict{};
            ESP_LOGW(TAG, "set_mqtt: broker check failed (%s) — not saving",
                     tk::mqtt_probe_reason(result));
            return tk::ConfigProbeVerdict{false, tk::mqtt_probe_http_status(result),
                                          tk::mqtt_probe_reason(result)};
        },
        [&](const std::string& broker) {
            cfg.mqtt_uri = broker;
            return tk::cfg_save(*g_config, cfg);
        },
        [&](int status, bool ok, const char* reason) {
            return send_json(req, status, make_response(ok, "set_mqtt", "", reason));
        },
        [&]() {
            ota_confirm_pending_image(tk::OtaRebootClass::SuccessfulUserConfigCommit);
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        },
        {"MQTT already disabled — no reboot", "MQTT broker unchanged — no reboot",
         "invalid broker (use host:port)", "MQTT disabled — rebooting",
         "MQTT broker saved — rebooting", "failed to save MQTT broker"});
}

// ─── POST /set_syslog — persist the Syslog server, then reboot ───────────────────
// Body: {"server":"host:port"} (a bare host defaults to port 514; an empty string
// disables Syslog). Stored in NVS ("syslog_uri") and applied on reboot — the
// forwarder resolves it once at start (syslog.cpp), same as /set_mqtt.

esp_err_t handle_set_syslog(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    const tk::ConfigStringSubmission submitted = parse_string_submission_(req, "server");
    tk::ConfigBlob cfg;
    return tk::apply_config_string(
        submitted,
        [](const std::string& value) {
            const size_t start = value.find_first_not_of(" \t\r\n");
            const size_t end = value.find_last_not_of(" \t\r\n");
            return start == std::string::npos ? std::string{}
                                              : value.substr(start, end - start + 1);
        },
        [&]() {
            tk::cfg_load(*g_config, cfg);
            return cfg.syslog_uri;
        },
        [](const std::string& value) { return tk::syslog_target_is_plausible(value); },
        [](const std::string&) { return tk::ConfigProbeVerdict{}; },
        [&](const std::string& server) {
            cfg.syslog_uri = server;
            return tk::cfg_save(*g_config, cfg);
        },
        [&](int status, bool ok, const char* reason) {
            return send_json(req, status, make_response(ok, "set_syslog", "", reason));
        },
        [&]() {
            ota_confirm_pending_image(tk::OtaRebootClass::SuccessfulUserConfigCommit);
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        },
        {"Syslog already disabled — no reboot", "Syslog server unchanged — no reboot",
         "invalid server (use host:port)", "Syslog disabled — rebooting",
         "Syslog server saved — rebooting", "failed to save Syslog server"});
}

// ─── POST /set_wifi — change the WiFi credentials over the LAN, undoably ──────
//
// Until this route existed, WiFi credentials could ONLY be changed from the open setup AP, and only
// by overwriting the stored pair in place. Both halves of that are bad in the same way: you had to
// be physically near the device to move it to a different network, and a typo was unrecoverable —
// the old credentials were gone, the new ones did not work, and the only way back was a USB cable.
//
// So the save is a TRANSACTION. The previous SSID/password are stashed as a one-shot backup inside
// the SAME atomic blob as the new ones (logic/config_store.hpp — no write ordering to get wrong),
// and the boot after this reboot decides: if the new credentials get a lease, the backup is
// dropped; if the AP keeps refusing them, main.cpp restores the backup and reboots back onto the
// network that worked. The decision itself is the host-tested logic/wifi_rollback.hpp, and it is
// deliberately asymmetric — a rollback DESTROYS the new credentials, so only an AP that sustains
// its refusal spends them, while an absent SSID (a router still rebooting) is given minutes.
esp_err_t handle_set_wifi(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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

    tk::ConfigBlob cfg;
    tk::cfg_load(*g_config, cfg);

    if (cfg.wifi_ssid == ssid && cfg.wifi_pass == pass) {
        return send_json(req, 200, make_response(true, "set_wifi", "",
                                                 "WiFi unchanged — no reboot"));
    }

    // Arm the one-shot rollback only when there is something to roll back TO. On a device with no
    // stored credentials there is no better previous state, and arming it would mean a failed first
    // attempt "restores" an empty configuration — which is just the setup portal with extra steps.
    if (!cfg.wifi_ssid.empty()) {
        cfg.wifi_ssid_backup     = cfg.wifi_ssid;
        cfg.wifi_pass_backup     = cfg.wifi_pass;
        cfg.wifi_rollback_active = true;
    }
    // A new attempt retires the previous verdict: /status.wifi.rolled_back describes the LAST
    // attempt, and leaving it set would report an old failure against new credentials.
    cfg.wifi_rolled_back = false;
    cfg.wifi_ssid = ssid;
    cfg.wifi_pass = pass;

    const bool ok = tk::cfg_save(*g_config, cfg);
    esp_err_t r = send_json(req, ok ? 200 : 500,
        make_response(ok, "set_wifi", "",
                      ok ? "WiFi credentials saved — rebooting to join the new network"
                         : "config write failed"));
    if (ok) {
        ota_confirm_pending_image(tk::OtaRebootClass::SuccessfulUserConfigCommit);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return r;
}
