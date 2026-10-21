// Compiles the actual extracted handlers and outer catch boundary against locked cJSON.
// Device, vehicle, storage, RTOS and transport doubles are counters/faults, never live I/O.
#include "http_effect_reply.hpp"
#include "time_sync.hpp"
#include "logic/command_result.hpp"
#include "logic/config_request.hpp"
#include "logic/http_route.hpp"
#include "logic/json_syntax.hpp"
#include "json_input_validation.hpp"
#include "logic/key_rotation.hpp"
#include "logic/vin_transition.hpp"
#include "logic/wifi_credentials.hpp"
#include "logic/wifi_rollback.hpp"
#include "logic/mqtt_uri.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/crashinfo.hpp"
#include "logic/ota_contract.hpp"
#include "logic/http_body.hpp"
#include "logic/mcp.hpp"
#include "logic/nvs_contract.hpp"
#include "logic/config_store.hpp"
#include "logic/health_gate.hpp"
#include "logic/heap_watchdog.hpp"
#include <cJSON.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <stdexcept>
#include <iostream>

static int checks;
static bool throw_confirmation;
static bool allocation_fault_armed;
static int allocation_fault_index, effect_allocations;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NOT_FOUND=-2, HTTPD_400_BAD_REQUEST=400;
static constexpr char TAG[]="offline-effect-adapter";
struct httpd_req_t { const char* uri{"/offline"}; int content_len{}; const char* input{"{}"}; };
#include "config_commit_owner.generated.hpp"
#include "effect_reply_context.generated.hpp"
using Handler = esp_err_t(*)(GuardedReq);
static Handler selected;
static bool fail_json, throw_error, throw_remove, throw_effect, save_ok, start_ok, ntp_synced;
static bool fail_json_after_effect, guard_ok, key_refused, require_config_owner;
static int effects, saves, removes, restarts, confirms, clock_calls;
static int transport_calls, fail_transport, response_attempts, response_status, response_ends;
static BrowserClockStatus clock_status;
static bool updating;
static int check_starts, query_read_error;
static std::string query;
static char wire[4096]; static size_t wire_size;
static tk::KeyRotationResult rotation;
static tk::VinTransitionApply vin_result;
static std::string previous_key_id;
static void* gate_malloc(size_t n) {
    if (allocation_fault_armed && ++effect_allocations==allocation_fault_index) return nullptr;
    return fail_json ? nullptr : std::malloc(n);
}
static void gate_free(void* p) { std::free(p); }
static void effect_done() {
    ++effects; allocation_fault_armed=true;
    if (throw_effect) throw std::bad_alloc();
    if (fail_json_after_effect) fail_json=true;
}
template<typename... Args> void offline_log(Args...) noexcept {}
#define ESP_LOGI(...) offline_log(__VA_ARGS__)
#define ESP_LOGW(...) offline_log(__VA_ARGS__)
#define ESP_LOGE(...) offline_log(__VA_ARGS__)
#define CONFIG_TESLA_MQTT_USERNAME ""
#define pdMS_TO_TICKS(x) (x)
constexpr int SHUT_RD=0;
static void vTaskDelay(int) noexcept {}
static void esp_restart();
static int httpd_req_to_sockfd(httpd_req_t*) noexcept { return 0; }
static int shutdown(int,int) noexcept { return 0; }
namespace tk {
enum class StackWatch { Httpd };
static void stack_watch_sample(StackWatch) noexcept {}
}
static tk::OtaIdentityOperationGate s_operation_gate;
enum esp_ota_img_states_t : uint32_t { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY,
    ESP_OTA_IMG_VALID, ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED=UINT32_MAX };
struct esp_partition_t {};
#include "crash_diag_owner.generated.hpp"
static tk::CrashEraseResult crash_erase_result;
static tk::CoredumpOpenResult crash_open_result;
static bool crash_owned, crash_dismissed;
static int crash_erase_calls, crash_read_calls, fail_crash_read, throw_crash_read;
static constexpr uint32_t crash_bytes=1200;
namespace tk {
CoredumpLease::CoredumpLease() noexcept : held_(!crash_owned) { if (held_) crash_owned=true; }
CoredumpLease::~CoredumpLease() { if (held_) crash_owned=false; }
CoredumpOpenResult diag_crash_open_dump(const CoredumpLease& lease,bool,CoredumpDownload& out) {
    out={};
    if (!lease.held()) return CoredumpOpenResult::Busy;
    if (crash_open_result==CoredumpOpenResult::Ready) {
        static esp_partition_t partition;
        out={&partition,crash_bytes};
    }
    return crash_open_result;
}
CrashEraseResult diag_crash_erase(bool dismiss,EffectReplyState& state) noexcept {
    ++crash_erase_calls;
    if (crash_owned || crash_erase_result==CrashEraseResult::Busy) return CrashEraseResult::Busy;
    if (crash_erase_result!=CrashEraseResult::Absent) {
        state.attempting(RequestEffect::CoredumpErase);
        try { effect_done(); } catch (...) { return CrashEraseResult::Unconfirmed; }
    }
    if (crash_erase_permits_dismiss(crash_erase_result)) {
        if (dismiss) crash_dismissed=true;
        state.succeeded();
    }
    return crash_erase_result;
}
}
static esp_err_t esp_partition_read(const esp_partition_t* partition,size_t offset,void* data,size_t bytes) {
    CHECK(partition && crash_owned && bytes<=256 && offset<=crash_bytes && bytes<=crash_bytes-offset);
    ++crash_read_calls;
    if (crash_read_calls==throw_crash_read) throw std::bad_alloc();
    if (crash_read_calls==fail_crash_read) return ESP_FAIL;
    std::memset(data,0x5a,bytes); return ESP_OK;
}

static esp_ota_img_states_t image_state;
static bool image_query_failure;
static int image_queries;
static bool runtime_ready=true, host_synced=true;
static unsigned host_resets;
static size_t commit_heap=4096;
constexpr int MALLOC_CAP_8BIT=1, MALLOC_CAP_INTERNAL=2;
static const esp_partition_t* esp_ota_get_running_partition() { static esp_partition_t p; return &p; }
static int esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t* out) { ++image_queries; if (image_query_failure) return ESP_FAIL; *out=image_state; return ESP_OK; }
static bool runtime_admission_vehicle_ready() { return runtime_ready; }
namespace tk { using ::runtime_admission_vehicle_ready; }
static bool ble_host_synced() { return host_synced; }
static unsigned ble_host_reset_count() { return host_resets; }
static size_t heap_caps_get_largest_free_block(int caps) { CHECK(caps==(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL)); return commit_heap; }
static int esp_ota_mark_app_valid_cancel_rollback() {
    ++confirms; image_state=ESP_OTA_IMG_VALID;
    if (throw_confirmation) throw std::bad_alloc();
    return ESP_OK;
}
#include "config_commit_runtime.generated.hpp"
static void esp_restart() {
    if (require_config_owner) CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::ConfigRestart);
    ++restarts;
}
static int transport_operation() {
    if (++transport_calls != fail_transport) return ESP_OK;
    return -7;
}
static int httpd_resp_set_status(httpd_req_t*,const char* status) {
    ++response_attempts; response_status=std::atoi(status); return transport_operation();
}
static int httpd_resp_set_type(httpd_req_t*,const char*) { return transport_operation(); }
static int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*) { return transport_operation(); }
static int httpd_resp_send_chunk(httpd_req_t*,const char* body,size_t size) {
    CHECK(!body || size); if (!body) ++response_ends;
    CHECK(wire_size+size<sizeof(wire));
    const int result=transport_operation();
    if (result && size>1) size/=2;
    if (body) std::memcpy(wire+wire_size,body,size);
    wire_size+=size; wire[wire_size]='\0'; return result;
}
static int httpd_resp_sendstr(httpd_req_t* req,const char* body) { return httpd_resp_send_chunk(req,body,std::strlen(body)); }
static int httpd_resp_send_err(httpd_req_t*,int,const char*) { return -4; }
static constexpr size_t kQueryBufBytes=128;
static int validate_query_string(httpd_req_t*) { return 0; }
static bool query_param_is(httpd_req_t*,const char*,const char*) { return true; }
bool clock_synced_via_ntp() noexcept { return ntp_synced; }
int64_t clock_plausibility_ceiling() noexcept { return tk::clock_jan1_seconds(2036); }
BrowserClockResult apply_browser_clock_sample(tk::ClockSample) noexcept {
    ++clock_calls;
    if (clock_status==BrowserClockStatus::Applied || clock_status==BrowserClockStatus::SetFailed) {
        try { effect_done(); } catch (...) { return {BrowserClockStatus::SetFailed, 0}; }
    }
    return {clock_status, 1800000000};
}
BrowserClockResult apply_browser_clock(double) noexcept { return apply_browser_clock_sample({}); }
static size_t httpd_req_get_url_query_len(httpd_req_t*) { return query.size(); }
static int httpd_req_get_url_query_str(httpd_req_t*,char* out,size_t size) {
    if (query_read_error) return query_read_error;
    if (query.empty()) return ESP_ERR_NOT_FOUND;
    if (query.size()>=size) return -3;
    std::snprintf(out,size,"%s",query.c_str()); return 0;
}
static int httpd_query_key_value(const char* raw,const char* key,char* out,size_t size) {
    const std::string text(raw), prefix=std::string(key)+"=";
    size_t start=0;
    while (start<text.size()) {
        const size_t end=text.find('&',start);
        const auto part=text.substr(start,end==std::string::npos?end:end-start);
        if (part.compare(0,prefix.size(),prefix)==0) {
            const auto value=part.substr(prefix.size());
            if (value.size()>=size) return -3;
            std::snprintf(out,size,"%s",value.c_str()); return 0;
        }
        if (end==std::string::npos) break;
        start=end+1;
    }
    return ESP_ERR_NOT_FOUND;
}
static bool ota_is_updating() { return updating; }
static bool ota_check_start(unsigned) { ++check_starts; effect_done(); return start_ok; }
static bool ota_start(unsigned) { effect_done(); return start_ok; }
static void ota_set_channel(tk::OtaChannel) { effect_done(); }
struct OtaIdentityMutationGuard {
    explicit OtaIdentityMutationGuard(tk::IdentityMutationEntry) {}
    explicit operator bool() const { return guard_ok; }
};
struct NvsStorageAdapter {
    bool save_str(const char*,const std::string&) { ++saves; return save_ok; }
    bool remove(const char*) { ++removes; if (throw_remove) throw std::bad_alloc(); return save_ok; }
};
static NvsStorageAdapter config_store;
static NvsStorageAdapter* g_config=&config_store;
static tk::ConfigBlob stored, captured_cfg;
static int mqtt_probes, config_loads;
static bool config_readable;
// Parser classification is a platform seam here; the shipped handlers own admission ordering.
static tk::ConfigSubmissionStatus submitted_status;
namespace tk {
static bool cfg_load_for_update(NvsStorageAdapter&,ConfigBlob& cfg) { ++config_loads; if (!config_readable) return false; cfg=stored; return true; }
static bool cfg_save(NvsStorageAdapter&,const ConfigBlob& cfg) {
    if (require_config_owner) CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::ConfigRestart);
    captured_cfg=cfg; ++saves; effect_done(); return save_ok;
}
}
struct VehicleController {
    struct KeyGenerationResult { tk::KeyRotationResult rotation; bool key_probe_failed{},existing_key_refused{},transition_blocked{}; };
    struct NewVehicleResetResult { tk::VinTransitionApply state; };
    static bool vin_is_plausible(const std::string&) { return true; }
    KeyGenerationResult generate_key_result(bool) {
        if (key_refused) return {rotation,false,true,false};
        effect_done(); return {rotation};
    }
    template<typename Stage> NewVehicleResetResult reset_for_new_vehicle(Stage stage) {
        // Model the platform transaction only; the compiled handler owns every result branch.
        if (vin_result==tk::VinTransitionApply::IdentityUnverified ||
            vin_result==tk::VinTransitionApply::IdentityRecoveryPending) {
            // Actual transaction returns before the stage callback and all identity writes.
            if (fail_json_after_effect) fail_json=true; // fault the refusal response only
            return {vin_result};
        }
        if (!stage(previous_key_id)) return {tk::VinTransitionApply::StageFailed};
        effect_done(); return {vin_result};
    }
    std::string last_command_error() { if (throw_error) throw std::bad_alloc(); return "offline refusal"; }
};
static VehicleController vehicle;
static VehicleController* g_vehicle=&vehicle;
static bool execute_vehicle_command(VehicleController&,tk::CmdKind,const int*,const bool*) { effect_done(); return start_ok; }
static bool request_vin_matches(const char*) { return true; }
static bool parse_uri(const char*,char* vin,size_t nv,char* cmd,size_t nc) { std::snprintf(vin,nv,"TESTVIN0000000001"); std::snprintf(cmd,nc,"charge_start"); return true; }
static bool json_int_arg(const cJSON*,const tk::CmdArg&,int&,const char*&) { return true; }
static bool json_bool_arg(const cJSON*,const tk::CmdArg&,bool&,const char*&) { return true; }
static tk::BodyReadResult read_body_result(httpd_req_t*) { return {nullptr,tk::BodyReadStatus::Empty}; }
static tk::JsonOwner vehicle_state_result_() { return tk::JsonBuilder().finish(); }
static tk::MqttProbeResult mqtt_probe_broker(const std::string&) { ++mqtt_probes; return tk::MqttProbeResult::Ok; }
struct ParsedJsonBody {
    tk::JsonOwner json;
    tk::ConfigSubmissionStatus status{tk::ConfigSubmissionStatus::Ready};
    explicit operator bool() const { return json!=nullptr; }
};
static ParsedJsonBody parse_json_object_body_(httpd_req_t* req) { return {tk::json_owner(cJSON_Parse(req->input))}; }
static const char* required_string_(const cJSON* object,const char* key) { const auto* j=cJSON_GetObjectItemCaseSensitive(object,key); return cJSON_IsString(j)?j->valuestring:nullptr; }
static tk::ConfigStringSubmission parse_string_submission_(httpd_req_t* req,const char* key) {
    if (submitted_status != tk::ConfigSubmissionStatus::Ready) return {submitted_status,{}};
    const auto parsed=parse_json_object_body_(req); const auto* value=required_string_(parsed.json.get(),key);
    return {tk::ConfigSubmissionStatus::Ready,value?value:""};
}
static int send_config_body_error_(httpd_req_t*,const char*,const ParsedJsonBody&) { return -4; }
static int send_config_unreadable_(httpd_req_t*,const char*,const char* = "") { return -4; }
esp_err_t send_json(httpd_req_t*,int,cJSON*);
static esp_err_t send_rpc_error_(httpd_req_t* req,const tk::mcp_json::RpcId& id,int code,const char* reason) { return send_json(req,200,tk::mcp_json::build_error_envelope(id,code,reason).release()); }
static esp_err_t send_rpc_result_(httpd_req_t* req,const tk::mcp_json::RpcId& id,tk::JsonOwner result) { return send_json(req,200,tk::mcp_json::build_result_envelope(id,std::move(result)).release()); }
static esp_err_t handle_all_dispatch(GuardedReq rq) { return selected(rq); }
#include "effect_reply_adapters.generated.hpp"

static void reset(Handler handler) {
    CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::Idle);
    crash_erase_result=tk::CrashEraseResult::Erased; crash_open_result=tk::CoredumpOpenResult::Ready;
    crash_owned=false; crash_dismissed=false; crash_erase_calls=0; crash_read_calls=0;
    fail_crash_read=0; throw_crash_read=0;
    require_config_owner=handler==handle_set_wifi||handler==handle_set_mqtt||handler==handle_set_syslog;
    image_state=ESP_OTA_IMG_VALID; image_query_failure=false; image_queries=0; runtime_ready=true; host_synced=true; host_resets=0; commit_heap=4096;
    throw_confirmation=false; allocation_fault_armed=false; allocation_fault_index=0; effect_allocations=0;
    clock_status=BrowserClockStatus::Applied; updating=false; check_starts=0;
    query="ms=1800000000000"; query_read_error=0;
    selected=handler; fail_json=false; throw_error=false; throw_remove=false; throw_effect=false;
    fail_json_after_effect=false; save_ok=true; start_ok=true; ntp_synced=true; guard_ok=true;
    key_refused=false; effects=0; saves=0; removes=0; restarts=0; confirms=0;
    clock_calls=0; transport_calls=0; fail_transport=0;
    response_attempts=0; response_status=0; response_ends=0; wire_size=0; wire[0]='\0';
    rotation=tk::KeyRotationResult::Complete; vin_result=tk::VinTransitionApply::Complete;
    config_loads=0; config_readable=true; submitted_status=tk::ConfigSubmissionStatus::Ready;
    captured_cfg={}; mqtt_probes=0; stored={}; stored.vin="OLDVIN00000000001"; previous_key_id.clear();
}
static tk::JsonOwner reply_json() { fail_json=false; allocation_fault_armed=false; return tk::json_owner(cJSON_Parse(wire)); }
static const cJSON* payload(const tk::JsonOwner& j) {
    const auto* p=cJSON_GetObjectItemCaseSensitive(j.get(),"response"); return p?p:j.get();
}
static void check_outcome(const char* outcome,bool success) {
    CHECK(response_status==200); CHECK(response_attempts==1); const auto json=reply_json(); CHECK(json);
    const auto* value=payload(json); const auto* text=cJSON_GetObjectItemCaseSensitive(value,"outcome");
    CHECK(cJSON_IsString(text)); CHECK(std::strcmp(text->valuestring,outcome)==0);
    CHECK(bool(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"result")))==success);
}
static esp_err_t mcp_command(GuardedReq rq) {
    auto request=tk::json_owner(cJSON_Parse("{\"params\":{\"name\":\"charge_start\"}}"));
    tk::mcp_json::RpcId id; id.kind=tk::mcp_json::RpcIdKind::Number; id.number=-9007199254740991LL;
    return handle_tools_call_(rq,id,std::move(request));
}
// Explicit shell fault after the real handler's single complete response. IDF C calls do not throw.
static esp_err_t shell_throw_after_reply(GuardedReq rq) {
    (void)handle_command(rq);
    throw std::bad_alloc();
}
// Copies of the real one-word wrapper and the outer catch must share the same request state.
static esp_err_t shared_context_fault(GuardedReq rq) {
    const GuardedReq forwarded=rq;
    CHECK(forwarded.req()==rq.req()); CHECK(forwarded.reply()==rq.reply());
    CHECK(rq.reply()==&rq.context->reply);
    forwarded.reply()->state.succeeded();
    throw std::bad_alloc();
}
static_assert(sizeof(GuardedReq)==sizeof(void*), "actual wrapper remains one pointer");
static_assert(noexcept(std::declval<GuardedReq>().req()) &&
              noexcept(std::declval<GuardedReq>().reply()), "actual getters are nothrow");
static int observed_effect_bits;
static esp_err_t observe_wifi_reply(GuardedReq rq) {
    const auto result=handle_set_wifi(rq);
    observed_effect_bits=rq.reply()->state.possible_effects;
    return result;
}
int main() {
    cJSON_Hooks hooks{gate_malloc,gate_free}; cJSON_InitHooks(&hooks);
    httpd_req_t req;
    for (auto owner : {tk::OtaIdentityGateState::Ota,tk::OtaIdentityGateState::IdentityMutation,
                       tk::OtaIdentityGateState::FaultRestart,tk::OtaIdentityGateState::HealthCommit,
                       tk::OtaIdentityGateState::ConfigRestart}) {
        std::string first;
        for (bool same : {false,true}) {
            reset(handle_set_wifi); stored.wifi_ssid="offline";
            CHECK(s_operation_gate.try_begin(owner));
            req.input=same?"{\"ssid\":\"offline\",\"pass\":\"\"}":"{\"ssid\":\"different\",\"pass\":\"\"}";
            CHECK(handle_all(&req)==ESP_OK); CHECK(response_status==409);
            CHECK(effects==0 && saves==0 && confirms==0 && restarts==0 && image_queries==0);
            CHECK(s_operation_gate.state()==owner); CHECK(s_operation_gate.finish(owner));
            if (first.empty()) first=wire; else CHECK(first==wire);
        }
    }
    for (uint32_t raw : {uint32_t(ESP_OTA_IMG_VALID),uint32_t(ESP_OTA_IMG_UNDEFINED),
             uint32_t(ESP_OTA_IMG_NEW),uint32_t(ESP_OTA_IMG_PENDING_VERIFY),uint32_t(ESP_OTA_IMG_INVALID),
             uint32_t(ESP_OTA_IMG_ABORTED),uint32_t(99)}) for (bool query_error : {false,true}) {
        std::string first;
        for (bool same : {false,true}) {
            reset(handle_set_wifi); stored.wifi_ssid="offline";
            image_state=static_cast<esp_ota_img_states_t>(raw); image_query_failure=query_error;
            const bool allowed=!query_error && (raw==ESP_OTA_IMG_VALID||raw==ESP_OTA_IMG_UNDEFINED);
            req.input=same?"{\"ssid\":\"offline\",\"pass\":\"\"}":"{\"ssid\":\"different\",\"pass\":\"\"}";
            CHECK(handle_all(&req)==ESP_OK); CHECK((response_status?response_status:200)==(allowed?200:409));
            CHECK(saves==int(allowed&&!same) && effects==int(allowed&&!same));
            CHECK(restarts==int(allowed&&!same) && confirms==0 && image_queries==1);
            CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::Idle);
            if (first.empty()) first=wire; else CHECK(first==wire);
        }
    }
    for (auto handler : {handle_set_mqtt,handle_set_syslog}) for (bool degraded : {false,true}) {
        for (int health=0;health<6;++health) {
            reset(handler); image_state=ESP_OTA_IMG_PENDING_VERIFY; fail_json_after_effect=degraded;
            if (health==1) runtime_ready=false;
            if (health==2) host_synced=false;
            if (health==3) host_resets=1;
            if (health==4) commit_heap=4095;
            if (health==5) image_query_failure=true;
            req.input=handler==handle_set_mqtt?"{\"broker\":\"example.test:1883\"}":"{\"server\":\"example.test:514\"}";
            CHECK(handle_all(&req)==ESP_OK); CHECK(saves==1 && restarts==1 && confirms==int(health==0));
            const auto json=reply_json(); CHECK(json); const auto* value=payload(json);
            CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"result")));
            const char* codes[]={"confirmed","runtime_not_ready","host_unsynced","host_reset","critical_heap","state_unknown"};
            const auto* image=cJSON_GetObjectItemCaseSensitive(value,"firmware_verification");
            CHECK(cJSON_IsString(image) && std::strcmp(image->valuestring,codes[health])==0);
            if (health) {
                const auto* reason=cJSON_GetObjectItemCaseSensitive(value,"reason");
                CHECK(cJSON_IsString(reason) && std::strstr(reason->valuestring,"configuration saved for the next boot")!=nullptr);
            }
            CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::Idle);
        }
    }
    for (auto handler : {handle_set_mqtt,handle_set_syslog}) {
        for (bool readable : {false,true}) for (auto status : {
                tk::ConfigSubmissionStatus::MissingBody, tk::ConfigSubmissionStatus::TooLarge,
                tk::ConfigSubmissionStatus::BodyNoMemory, tk::ConfigSubmissionStatus::ReceiveFailed,
                tk::ConfigSubmissionStatus::MalformedJson, tk::ConfigSubmissionStatus::JsonTooDeep,
                tk::ConfigSubmissionStatus::JsonUnsupportedNul, tk::ConfigSubmissionStatus::JsonNoMemory,
                tk::ConfigSubmissionStatus::ObjectRequired, tk::ConfigSubmissionStatus::StringFieldRequired,
                tk::ConfigSubmissionStatus::JsonTooManyValues, tk::ConfigSubmissionStatus::DuplicateJsonKey}) {
            reset(handler); config_readable=readable; submitted_status=status;
            CHECK(handle_all(&req)==ESP_OK);
            CHECK(response_status==tk::config_submission_http_status(status));
            CHECK(config_loads==0 && mqtt_probes==0 && effects==0 && saves==0 && restarts==0 && confirms==0);
            const auto json=reply_json(); CHECK(json);
            const auto* reason=cJSON_GetObjectItemCaseSensitive(payload(json),"reason");
            CHECK(cJSON_IsString(reason)); CHECK(std::strcmp(reason->valuestring,tk::config_submission_reason(status))==0);
        }
        reset(handler); config_readable=false;
        req.input=handler==handle_set_mqtt?"{\"broker\":\"example.test:1883\"}":"{\"server\":\"example.test:514\"}";
        CHECK(handle_all(&req)==-4); CHECK(config_loads==1);
        CHECK(mqtt_probes==0 && effects==0 && saves==0 && restarts==0 && confirms==0);
    }
    for (bool mqtt : {false,true}) for (bool unchanged : {false,true}) {
        reset(mqtt?handle_set_mqtt:handle_set_syslog);
        const std::string value=std::string(mqtt?60:61,'h')+(mqtt?":1883":":514");
        if (unchanged) (mqtt?stored.mqtt_uri:stored.syslog_uri)=value;
        const std::string body=std::string("{\"")+(mqtt?"broker":"server")+"\":\""+value+"\"}";
        req.input=body.c_str(); CHECK(handle_all(&req)==0);
        CHECK((response_status?response_status:200)==(unchanged?200:400));
        CHECK(effects==0 && saves==0 && mqtt_probes==0 && restarts==0 && confirms==0);
    }
    for (bool mqtt : {false,true}) {
        reset(mqtt?handle_set_mqtt:handle_set_syslog);
        stored.mqtt_uri.assign(120,'m'); stored.syslog_uri.assign(120,'s');
        req.input=mqtt?"{\"broker\":\"example.test:1883\"}":"{\"server\":\"example.test:514\"}";
        CHECK(handle_all(&req)==0 && response_status==200 && saves==1);
        CHECK((mqtt?captured_cfg.syslog_uri:captured_cfg.mqtt_uri)==std::string(120,mqtt?'s':'m'));
        CHECK((mqtt?captured_cfg.mqtt_uri:captured_cfg.syslog_uri)==(mqtt?"example.test:1883":"example.test:514"));
        CHECK(effects==1 && restarts==1 && confirms==0 && mqtt_probes==int(mqtt));
    }
    std::string legacy_wifi_refusal;
    for (bool mqtt : {false,true}) for (size_t size : {64u,65u,120u})
    for (bool changed : {false,true}) {
        reset(observe_wifi_reply); observed_effect_bits=-1;
        stored.wifi_ssid="offline"; stored.wifi_pass="";
        (mqtt?stored.mqtt_uri:stored.syslog_uri).assign(size,'h');
        req.input=changed?"{\"ssid\":\"new-offline\",\"pass\":\"\"}":"{\"ssid\":\"offline\",\"pass\":\"\"}";
        CHECK(handle_all(&req)==0);
        const bool save=changed && size<=64;
        const int expected=size>64?409:200;
        CHECK((response_status?response_status:200)==expected);
        CHECK(saves==int(save) && effects==int(save) && restarts==int(save) && confirms==0);
        CHECK(observed_effect_bits==(save?int(tk::RequestEffect::Configuration):0));
        if (size>64) {
            if (legacy_wifi_refusal.empty()) legacy_wifi_refusal=wire;
            else CHECK(legacy_wifi_refusal==wire); // match/other credentials, either target: byte equal
        }
    }
    for (auto handler : {handle_set_time,handle_ota_check}) {
        for (auto status : {BrowserClockStatus::Applied, BrowserClockStatus::AlreadyNtpSynced,
                            BrowserClockStatus::Busy, BrowserClockStatus::Invalid, BrowserClockStatus::SetFailed}) {
            reset(handler); ntp_synced=false; clock_status=status;
            req.input="{\"ms\":1800000000000}";
            CHECK(handle_all(&req)==0);
            const bool permits=status==BrowserClockStatus::Applied || status==BrowserClockStatus::AlreadyNtpSynced;
            const int expected=status==BrowserClockStatus::Invalid?400:status==BrowserClockStatus::Busy?503
                               :status==BrowserClockStatus::SetFailed?500:200;
            // Ordinary 200 uses the IDF default (no explicit status setter in this double).
            CHECK((response_status?response_status:200)==expected && response_attempts<=1 && clock_calls==1);
            CHECK(check_starts==int(handler==handle_ota_check && permits));
            const auto json=reply_json(); CHECK(json);
            const auto* value=payload(json);
            CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,handler==handle_ota_check?"started":"result"))==permits);
            CHECK(effects==int(status==BrowserClockStatus::Applied || status==BrowserClockStatus::SetFailed)
                           +int(handler==handle_ota_check && permits));
        }
        reset(handler); ntp_synced=false; updating=true; req.input="{\"ms\":1800000000000}";
        CHECK(handle_all(&req)==0 && response_status==409 && effects==0 && clock_calls==0 && check_starts==0);
        reset(handler); ntp_synced=false; clock_status=BrowserClockStatus::SetFailed;
        fail_json_after_effect=true; CHECK(handle_all(&req)==0);
        check_outcome("unknown",false); CHECK(check_starts==0);
    }
    for (auto handler : {handle_set_time,handle_ota_check}) {
        for (bool allocation_failure : {false,true}) for (int failure : {1,2,3}) {
            reset(handler); ntp_synced=false; clock_status=BrowserClockStatus::SetFailed;
            fail_json_after_effect=allocation_failure; fail_transport=failure;
            req.input="{\"ms\":1800000000000}";
            CHECK(handle_all(&req)==-7 && clock_calls==1 && effects==1 && check_starts==0);
            CHECK(response_attempts==1 && transport_calls==failure); // terminal; never retry
        }
    }
    for (const char* invalid : {"ms=", "ms=-1800000000000", "ms=01800000000000", "ms=1800000000000x",
                                "ms=1800000000000.0", "ms=18446744073709551616", "ms=2082758400000"}) {
        reset(handle_ota_check); ntp_synced=false; query=invalid;
        CHECK(handle_all(&req)==0 && response_status==400 && effects==0 && clock_calls==0 && check_starts==0);
        reset(handle_ota_check); ntp_synced=true; query=invalid;
        CHECK(handle_all(&req)==0 && response_status==200 && effects==1 && clock_calls==0 && check_starts==1);
    }
    for (int error : {ESP_FAIL,-3}) {
        reset(handle_ota_check); ntp_synced=false; query_read_error=error;
        CHECK(handle_all(&req)==0 && response_status==400 && effects==0 && clock_calls==0 && check_starts==0);
    }
    for (const char* absent : {"", "pr=7"}) {
        reset(handle_ota_check); ntp_synced=false; query=absent;
        CHECK(handle_all(&req)==0 && response_status==200 && clock_calls==0 && check_starts==1);
    }
    reset(handle_set_vin); previous_key_id="broken-format";
    req.input="{\"vin\":\"NEWVIN00000000001\"}";
    CHECK(handle_all(&req)==0); CHECK(response_status==500);
    CHECK(saves==0 && removes==0 && effects==0); // no empty journal, config write or key mutation
    CHECK(confirms==0); // existing StageFailed recovery policy remains unmodified

    reset(handle_set_wifi); req.input="{\"ssid\":\"offline\",\"pass\":\"\"}";
    CHECK(handle_all(&req)==0); CHECK(response_status==200 && saves==1 && restarts==1);
    const std::string changed_wifi_reply(wire);
    reset(handle_set_wifi); stored.wifi_ssid="offline";
    req.input="{\"ssid\":\"offline\",\"pass\":\"\"}";
    CHECK(handle_all(&req)==0); CHECK(response_status==0 && saves==0 && restarts==0); // implicit IDF200, no status override
    CHECK(changed_wifi_reply==wire); // accepted text/envelope does not expose credential equality

    reset(shared_context_fault);
    CHECK(handle_all(&req)==0); check_outcome("succeeded",true); CHECK(effects==0);
    for (auto handler : {handle_command,mcp_command}) {
        for (bool known : {false,true}) {
            reset(handler); start_ok=known; throw_error=true;
            CHECK(handle_all(&req)==0); CHECK(effects==1); CHECK(restarts==0);
            const auto json=reply_json(); CHECK(json);
            const auto* p=handler==mcp_command?cJSON_GetObjectItemCaseSensitive(json.get(),"result"):payload(json);
            CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"result"))==known);
            CHECK(std::strcmp(cJSON_GetObjectItemCaseSensitive(p,"outcome")->valuestring,known?"succeeded":"unknown")==0);
            if (handler==mcp_command) CHECK(cJSON_GetObjectItemCaseSensitive(json.get(),"id")->valuedouble==-9007199254740991.0);
        }
    }
    for (auto handler : {handle_set_wifi,handle_set_mqtt,handle_set_syslog}) {
        for (bool stored_ok : {false,true}) {
            reset(handler); save_ok=stored_ok; fail_json_after_effect=true;
            req.input=handler==handle_set_wifi?"{\"ssid\":\"offline\",\"pass\":\"\"}":handler==handle_set_mqtt?"{\"broker\":\"example.test:1883\"}":"{\"server\":\"example.test:514\"}";
            CHECK(handle_all(&req)==0); CHECK(saves==1); CHECK(effects==1);
            check_outcome(stored_ok?"succeeded":"unknown",stored_ok);
            CHECK(confirms==0); CHECK(restarts==int(stored_ok));
        }
        reset(handler); fail_transport=3;
        CHECK(handle_all(&req)==-7); CHECK(saves==1); CHECK(effects==1); CHECK(restarts==1); CHECK(confirms==0); CHECK(response_attempts==1);
    }
    for (auto result : {tk::KeyRotationResult::NotCommitted,tk::KeyRotationResult::CommitUnknown,tk::KeyRotationResult::CleanupPending,tk::KeyRotationResult::Complete}) {
        reset(handle_gen_keys); rotation=result; fail_json_after_effect=true;
        CHECK(handle_all(&req)==0); CHECK(effects==1); CHECK(confirms==0);
        const bool complete=result==tk::KeyRotationResult::Complete;
        check_outcome(complete?"succeeded":result==tk::KeyRotationResult::CleanupPending?"applied_recovery_pending":"unknown",complete);
        CHECK(restarts==int(result==tk::KeyRotationResult::NotCommitted||result==tk::KeyRotationResult::CommitUnknown));
    }
    for (auto result : {tk::VinTransitionApply::IdentityUnverified,tk::VinTransitionApply::IdentityRecoveryPending,tk::VinTransitionApply::StageFailed,tk::VinTransitionApply::RollBackPreviousIdentity,tk::VinTransitionApply::RecoverAmbiguousIdentity,tk::VinTransitionApply::RecoverCommittedIdentity,tk::VinTransitionApply::Complete}) {
        reset(handle_set_vin); vin_result=result; fail_json_after_effect=true; req.input="{\"vin\":\"NEWVIN00000000001\"}";
        CHECK(handle_all(&req)==0); CHECK(confirms==0);
        const bool complete=result==tk::VinTransitionApply::Complete;
        const bool refused=result==tk::VinTransitionApply::IdentityUnverified||result==tk::VinTransitionApply::IdentityRecoveryPending;
        if (refused) { CHECK(response_status==503); CHECK(effects==0); CHECK(saves==0); CHECK(removes==0); }
        else {
            const bool rollback=result==tk::VinTransitionApply::StageFailed||result==tk::VinTransitionApply::RollBackPreviousIdentity;
            CHECK(saves==(rollback?3:2)); CHECK(effects==(rollback?3:2));
            CHECK(removes==int(rollback||complete));
            check_outcome(complete?"succeeded":result==tk::VinTransitionApply::RecoverCommittedIdentity?"applied_recovery_pending":"unknown",complete);
        }
        CHECK(restarts==int(!refused));
    }
    reset(handle_set_vin); throw_remove=true; // typed committed result, then post-result cleanup throws
    CHECK(handle_all(&req)==0); check_outcome("applied_recovery_pending",false); CHECK(restarts==1); CHECK(confirms==0);
    reset(handle_gen_keys); throw_effect=true;
    CHECK(handle_all(&req)==0); check_outcome("unknown",false); CHECK(restarts==0); // no guessed continuation
    for (auto handler : {handle_ota_check,handle_ota_update}) {
        for (bool started : {false,true}) {
            reset(handler); ntp_synced=false; start_ok=started; fail_json_after_effect=true;
            CHECK(handle_all(&req)==0); check_outcome(started?"succeeded":"unknown",started);
            CHECK(effects==(handler==handle_ota_check?2:1)); CHECK(clock_calls==(handler==handle_ota_check?1:0));
            if (handler==handle_ota_check) {
                const auto json=reply_json(); const auto* value=cJSON_GetObjectItemCaseSensitive(json.get(),"started");
                CHECK(started?cJSON_IsTrue(value):cJSON_IsNull(value));
            }
        }
        reset(handler); start_ok=false; CHECK(handle_all(&req)==0); CHECK(response_status==409); CHECK(response_attempts==1);
    }
    reset(handle_set_time); ntp_synced=false; fail_json_after_effect=true; req.input="{\"ms\":1800000000000}";
    CHECK(handle_all(&req)==0); check_outcome("unknown",false); CHECK(clock_calls==1); CHECK(restarts==0);
    reset(handle_set_ota); fail_json_after_effect=true; req.input="{\"channel\":\"dev\"}";
    CHECK(handle_all(&req)==0); check_outcome("succeeded",true); CHECK(saves==1); CHECK(restarts==0);
    for (int failing_call : {1,2,3}) {
        reset(handle_command); throw_error=true; fail_transport=failing_call;
        CHECK(handle_all(&req)==-7); CHECK(effects==1); CHECK(response_attempts==1); CHECK(transport_calls==failing_call);
    }
    reset(shell_throw_after_reply);
    CHECK(handle_all(&req)==ESP_FAIL); CHECK(effects==1); CHECK(response_attempts==1);
    CHECK(transport_calls==3); // actual outer catch cannot start another reply after the shell fault
    reset(handle_set_mqtt); image_state=ESP_OTA_IMG_PENDING_VERIFY; throw_confirmation=true;
    req.input="{\"broker\":\"example.test:1883\"}";
    CHECK(handle_all(&req)==0); CHECK(saves==1); CHECK(confirms==1); CHECK(restarts==1);
    CHECK(std::strstr(wire,"mark_outcome_unknown")!=nullptr);
    reset(handle_gen_keys); key_refused=true; CHECK(handle_all(&req)==0); CHECK(response_status==409); CHECK(effects==0); CHECK(restarts==0);
    // Exhaust every allocation of each actual post-effect builder AND its actual serializer.
    req.input="{\"ssid\":\"offline\",\"pass\":\"\",\"broker\":\"example.test:1883\",\"server\":\"example.test:514\",\"vin\":\"NEWVIN00000000001\",\"ms\":1800000000000,\"channel\":\"dev\"}";
    for (auto handler : {handle_command,mcp_command,handle_set_wifi,handle_set_mqtt,handle_set_syslog,
                         handle_gen_keys,handle_set_vin,handle_set_time,handle_set_ota,handle_ota_check,handle_ota_update}) {
        reset(handler); ntp_synced=false; CHECK(handle_all(&req)==0);
        const int count=effect_allocations; CHECK(count>0);
        for (int failure=1; failure<=count; ++failure) {
            reset(handler); ntp_synced=false; allocation_fault_index=failure;
            CHECK(handle_all(&req)==0); CHECK(response_status==200); CHECK(response_attempts==1);
            const auto json=reply_json(); CHECK(json);
            const auto* p=handler==mcp_command?cJSON_GetObjectItemCaseSensitive(json.get(),"result"):payload(json);
            const auto* degraded=cJSON_GetObjectItemCaseSensitive(p,"reply_degraded"); CHECK(cJSON_IsTrue(degraded));
            const auto* outcome=cJSON_GetObjectItemCaseSensitive(p,"outcome"); CHECK(cJSON_IsString(outcome));
            CHECK(std::strcmp(outcome->valuestring,handler==handle_set_time?"unknown":"succeeded")==0);
            CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"result"))==(handler!=handle_set_time));
            CHECK(restarts==int(handler==handle_set_wifi||handler==handle_set_mqtt||handler==handle_set_syslog||handler==handle_set_vin));
            CHECK(confirms==0);
        }
    }

    // Actual crash HTTP bodies plus actual F031 serializer/outer catch and locked cJSON.
    for (const char* invalid : {"raw=1&clear=1","clear=1&raw=1","raw=0","RAW=1","clear=01","raw=1&","x=1"}) {
        reset(handle_coredump); query=invalid;
        CHECK(handle_all(&req)==ESP_OK && response_status==400);
        CHECK(crash_erase_calls==0 && crash_read_calls==0 && effects==0);
    }
    reset(handle_coredump); query="raw=1"; query_read_error=ESP_FAIL;
    CHECK(handle_all(&req)==ESP_OK && response_status==400 && crash_read_calls==0 && effects==0);
    for (const char* mode : {"","raw=1"}) {
        reset(handle_coredump); query=mode;
        CHECK(handle_all(&req)==ESP_OK && wire_size==crash_bytes && response_ends==1);
        CHECK(crash_read_calls==5 && !crash_owned && effects==0 && response_attempts==0);
        for (int read=1; read<=5; ++read) for (bool throwing : {false,true}) {
            reset(handle_coredump); query=mode;
            if (throwing) throw_crash_read=read; else fail_crash_read=read;
            CHECK(handle_all(&req)==ESP_FAIL && response_ends==0 && response_attempts==0);
            CHECK(crash_read_calls==read && !crash_owned && effects==0);
            CHECK(wire_size==size_t(read-1)*256);
        }
        // Type, both headers, all five chunks, and the terminator: no fallback/retry.
        for (int call=1; call<=9; ++call) {
            reset(handle_coredump); query=mode; fail_transport=call;
            CHECK(handle_all(&req)!=ESP_OK && transport_calls==call && response_attempts==0);
            CHECK(response_ends==int(call==9) && !crash_owned && effects==0);
            if (call<=3) CHECK(crash_read_calls==0 && wire_size==0);
        }
    }
    for (auto opened : {tk::CoredumpOpenResult::Absent,tk::CoredumpOpenResult::Error,
                        tk::CoredumpOpenResult::Busy,tk::CoredumpOpenResult::Unverified}) {
        reset(handle_coredump); query=""; crash_open_result=opened;
        CHECK(handle_all(&req)==ESP_OK);
        CHECK(response_status==(opened==tk::CoredumpOpenResult::Absent?404:
                                opened==tk::CoredumpOpenResult::Error?503:409));
        CHECK(crash_read_calls==0 && effects==0 && !crash_owned);
    }
    for (auto handler : {handle_coredump,handle_crash_dismiss}) {
        for (auto result : {tk::CrashEraseResult::Busy,tk::CrashEraseResult::Absent,
                            tk::CrashEraseResult::Erased,tk::CrashEraseResult::Unconfirmed}) {
            reset(handler); query="clear=1"; crash_erase_result=result;
            CHECK(handle_all(&req)==ESP_OK && crash_erase_calls==1 && crash_read_calls==0);
            const bool success=tk::crash_erase_permits_dismiss(result);
            CHECK(response_status==(result==tk::CrashEraseResult::Busy?409:200));
            CHECK(crash_dismissed==(success && handler==handle_crash_dismiss));
            const auto json=reply_json(); CHECK(json);
            CHECK(bool(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(),"ok")))==success);
            if (result==tk::CrashEraseResult::Unconfirmed)
                CHECK(std::strstr(wire,"unknown") && std::strstr(wire,"do not retry automatically"));
        }
        for (auto result : {tk::CrashEraseResult::Erased,tk::CrashEraseResult::Unconfirmed}) {
            reset(handler); query="clear=1"; crash_erase_result=result;
            CHECK(handle_all(&req)==ESP_OK);
            const int allocations=effect_allocations; CHECK(allocations>0);
            for (int index=1; index<=allocations; ++index) {
                reset(handler); query="clear=1"; crash_erase_result=result; allocation_fault_index=index;
                CHECK(handle_all(&req)==ESP_OK && crash_erase_calls==1 && effects==1);
                check_outcome(result==tk::CrashEraseResult::Erased?"succeeded":"unknown",
                              result==tk::CrashEraseResult::Erased);
                CHECK(std::strstr(wire,"reply_degraded"));
            }
        }
        reset(handler); query="clear=1"; throw_effect=true;
        CHECK(handle_all(&req)==ESP_OK && effects==1 && !crash_dismissed);
        CHECK(std::strstr(wire,"unknown"));
        for (int call=1; call<=3; ++call) {
            reset(handler); query="clear=1"; fail_transport=call;
            CHECK(handle_all(&req)!=ESP_OK && crash_erase_calls==1 && effects==1);
            CHECK(response_attempts==1 && transport_calls==call);
        }
    }
    cJSON_InitHooks(nullptr);
    std::cout << "OK " << checks << " actual effect-adapter checks passed\n";
}
