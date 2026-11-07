// Passive request/config/reply effects for shipped handlers calling the complete OTA application.
// This seam does not replace candidate validation, channel revision, ownership or image effects.
#include "http_effect_reply.hpp"
#include <sdkconfig.h>
#include "logic/http_route.hpp"
#include "logic/config_request.hpp"
#include "logic/config_store.hpp"
#include "effect_reply_context.generated.hpp"

namespace fixture {
std::string http_query,submitted_channel;
int response_status=0,config_saves=0;
bool config_save_ok=true;
tk::ConfigBlob stored_config{};
}
static constexpr size_t kQueryBufBytes=128;
static int validate_query_string(httpd_req_t*){return ESP_OK;}
static int httpd_req_get_url_query_str(httpd_req_t*,char* out,size_t size){
 if(fixture::http_query.empty())return ESP_ERR_NOT_FOUND;
 if(fixture::http_query.size()>=size)return ESP_FAIL;
 std::snprintf(out,size,"%s",fixture::http_query.c_str());return ESP_OK;
}
static esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return ESP_OK;}
static esp_err_t httpd_resp_send(httpd_req_t*,const char* data,size_t size){
 fixture::upload_reply.assign(data?data:"",size);return ESP_OK;
}
static esp_err_t send_json(httpd_req_t*,int status,cJSON* raw){
 auto root=tk::json_owner(raw);fixture::response_status=status;CHECK(root);
 char* text=cJSON_PrintUnformatted(root.get());CHECK(text);
 fixture::upload_reply=text;cJSON_free(text);return ESP_OK;
}
static esp_err_t send_effect_json(GuardedReq rq,int status,cJSON* root){
 return send_json(rq.req(),status,root);
}
static cJSON* make_response(bool ok,const char*,const char*,const char* reason){
 tk::JsonBuilder json;json.boolean(json.root(),"result",ok);json.string(json.root(),"reason",reason);
 return json.release();
}
static tk::ConfigStringSubmission parse_string_submission_(httpd_req_t*,const char*){
 return {tk::ConfigSubmissionStatus::Ready,fixture::submitted_channel};
}
struct PassiveConfigStore{};
static PassiveConfigStore passive_config;
static PassiveConfigStore* g_config=&passive_config;
namespace tk {
static bool cfg_load_for_update(PassiveConfigStore&,ConfigBlob& out){
 out=fixture::stored_config;return true;
}
static bool cfg_save(PassiveConfigStore&,const ConfigBlob& cfg){
 ++fixture::config_saves;if(!fixture::config_save_ok)return false;
 fixture::stored_config=cfg;return true;
}
}
static esp_err_t send_config_unreadable_(httpd_req_t* req,const char*){
 return send_json(req,503,make_response(false,"","","unreadable configuration"));
}
#include "ota_offer_http.generated.hpp"

namespace fixture {
using OfferHandler=esp_err_t(*)(GuardedReq);
tk::JsonOwner request_offer(OfferHandler handler,std::string query={}){
 http_query=std::move(query);response_status=0;upload_reply.clear();
 httpd_req_t request{&http_server_marker,0};HttpGuardContext context{&request,{}};
 CHECK(handler(GuardedReq{&context})==ESP_OK);
 return tk::json_owner(cJSON_Parse(upload_reply.c_str()));
}
void change_channel(tk::OtaChannel channel){
 submitted_channel=tk::ota_channel_name(channel);
 const auto json=request_offer(handle_set_ota);
 CHECK(response_status==200&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(),"result")));
 CHECK(ota_get_channel()==channel);
}
std::string_view json_text(const tk::JsonOwner& json,const char* name){
 const cJSON* item=cJSON_GetObjectItemCaseSensitive(json.get(),name);
 CHECK(cJSON_IsString(item));return item->valuestring;
}
// Externally returned status and notes for a source whose offer must not be current.
void no_current_offer(const char* state,const char* source,const char* configured,const char* message,
                      bool withdrawn=false){
 const auto json=request_offer(handle_ota_status);CHECK(response_status==200&&json);
 const cJSON* changed=cJSON_GetObjectItemCaseSensitive(json.get(),"offer_changed");
 CHECK(withdrawn?cJSON_IsTrue(changed):changed==nullptr);
 CHECK(json_text(json,"state")==state&&json_text(json,"channel")==source);
 CHECK(json_text(json,"configured_channel")==configured&&json_text(json,"message")==message);
 CHECK(json_text(json,"available").empty()&&json_text(json,"offer_id").empty());
 CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json.get(),"update_available")));
 CHECK(!cJSON_GetObjectItemCaseSensitive(json.get(),"image_sha256"));
 upload_status.clear();const auto notes=request_offer(handle_ota_changelog);
 CHECK(!notes&&upload_status=="204 No Content"&&upload_reply.empty());
}
// Externally returned current offer and its bound notes, exactly as a client receives them.
void current_offer(const std::string& id,const char* source,const char* version,unsigned pr,const char* notes){
 const auto json=request_offer(handle_ota_status);CHECK(response_status==200&&json);
 CHECK(json_text(json,"state")=="idle"&&json_text(json,"channel")==source&&json_text(json,"offer_id")==id);
 CHECK(json_text(json,"configured_channel")==source&&json_text(json,"available")==version);
 CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(),"update_available")));
 CHECK(!cJSON_GetObjectItemCaseSensitive(json.get(),"offer_changed"));
 const cJSON* pr_item=cJSON_GetObjectItemCaseSensitive(json.get(),"pr");
 CHECK(pr?cJSON_IsNumber(pr_item)&&pr_item->valuedouble==pr:pr_item==nullptr);
 const auto image=image_body();tk::ota::Digest digest{};
 CHECK(tk::ota::sha256(image.data(),image.size(),digest)==PSA_SUCCESS);
 constexpr char hex[]="0123456789abcdef";std::string hash;
 for(auto b:digest){hash+=hex[b>>4];hash+=hex[b&15];}
 CHECK(json_text(json,"image_sha256")==hash);
 const cJSON* bytes=cJSON_GetObjectItemCaseSensitive(json.get(),"image_bytes");
 CHECK(cJSON_IsNumber(bytes)&&bytes->valuedouble==static_cast<double>(image.size()));
 upload_status.clear();const auto text=request_offer(handle_ota_changelog,"offer="+id);
 CHECK(!text&&upload_status.empty()&&response_status==0&&upload_reply==notes);
}
void refused_offer(std::string query,int expected=409){
 const int old_connects=connects.load();
 const auto json=request_offer(handle_ota_update,std::move(query));
 CHECK(response_status==expected&&cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json.get(),"result")));
 if(expected==409)CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(),"offer_changed")));
 CHECK(connects==old_connects&&begins==0&&writes==0&&ends==0&&selectors==0&&!selected_task&&!ota_is_busy());
}
void offer_binding_checks(){
 reset_for_image();stored_config={};config_saves=0;
 const std::string first=confirmed_offer;
 auto json=request_offer(handle_ota_status);CHECK(response_status==200);
 CHECK(std::string_view(cJSON_GetObjectItemCaseSensitive(json.get(),"channel")->valuestring)=="release");
 CHECK(std::string_view(cJSON_GetObjectItemCaseSensitive(json.get(),"offer_id")->valuestring)==first);
 current_offer(first,"release","1.6.3",0,"A change");
 // Same-channel save persists at most once and is an explicit identity-preserving no-op.
 change_channel(tk::OtaChannel::Release);change_channel(tk::OtaChannel::Release);
 CHECK(config_saves==1&&std::string_view(ota_get_status().offer_id.data())==first);
 refused_offer("");refused_offer("pr=42&offer="+first);
 refused_offer("offer=000000000000000000000001");
 refused_offer("offer="+first+"&OFFER="+first,400);
 CHECK(ota_get_status().update_available); // A bad client cannot erase a valid successor.
 // Client B changes and changes back: old Release identity never revives (ABA).
 change_channel(tk::OtaChannel::Dev);
 json=request_offer(handle_ota_status);
 CHECK(!ota_get_status().update_available&&ota_get_status().available[0]==0);
 CHECK(std::string_view(cJSON_GetObjectItemCaseSensitive(json.get(),"channel")->valuestring)=="release");
 CHECK(std::string_view(cJSON_GetObjectItemCaseSensitive(json.get(),"configured_channel")->valuestring)=="dev");
 // The withdrawn completed offer no longer reads as an available update.
 const char* changed=tk::ota_start_reason(tk::OtaStartResult::OfferChanged,false);
 no_current_offer("idle","release","dev",changed,true);
 auto notes=request_offer(handle_ota_changelog,"offer="+first);CHECK(response_status==409&&notes);
 change_channel(tk::OtaChannel::Release);no_current_offer("idle","release","release",changed,true);
 refused_offer("offer="+first);
 // A new same-channel check by B replaces A's ID even when the candidate bytes are identical;
 // A's notes ID cannot read B's successor notes.
 reset();changelog="{\"version\":\"1.6.3\",\"changelog\":\"Successor notes\"}";
 CHECK(ota_check(0).update_available);join_worker();
 const std::string second=ota_get_status().offer_id.data();reset();
 CHECK(second!=first);refused_offer("offer="+first);
 CHECK(std::string_view(ota_get_status().offer_id.data())==second);
 notes=request_offer(handle_ota_changelog,"offer="+first);CHECK(response_status==409&&notes);
 CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(notes.get(),"offer_changed")));
 current_offer(second,"release","1.6.3",0,"Successor notes");
 // Accepted metadata work loses its offer if either channel changes before completion,
 // including deferred HTTP work whose Release capture precedes client B's change.
 for(bool deferred:{false,true})for(bool aba:{false,true}){
  reset();
  if(deferred)CHECK(ota_check_start_after_http(0,&http_server_marker)==tk::OtaStartResult::Started);
  else CHECK(ota_check_start(0)==tk::OtaStartResult::Started);
  change_channel(tk::OtaChannel::Dev);if(aba)change_channel(tk::OtaChannel::Release);
  no_current_offer("checking","release",aba?"release":"dev","checking for updates");
  if(deferred){run_queued_http();CHECK(s_operation.load()->channel==tk::OtaChannel::Release);}
  run_selected();join_worker();CHECK(!ota_get_status().update_available&&ota_get_status().offer_id[0]==0);
  size_t n=99;char text[64];CHECK(!ota_get_changelog(text,sizeof(text),n)&&n==0);
  no_current_offer("error","release",aba?"release":"dev","update channel changed; check again");
  CHECK(connects==2&&begins==0&&writes==0&&selectors==0);
 }
 // Version-identical candidate mutation (manifest or notes) cannot begin an image transfer.
 for(bool notes_changed:{false,true}){
  reset_for_image();
  if(notes_changed)changelog="{\"version\":\"1.6.3\",\"changelog\":\"A different change\"}";
  else manifest+=" ";
  json=request_offer(handle_ota_update,"offer="+confirmed_offer);CHECK(response_status==200);
  CHECK(selected_task);run_selected();join_worker();
  CHECK(ota_get_state()==OtaState::Error&&begins==0&&writes==0&&ends==0&&selectors==0&&connects==2);
  refused_offer("offer="+confirmed_offer);
 }
 // PR identity is captured, not taken from another client's current query.
 reset_for_image({65536},42);refused_offer("offer="+confirmed_offer);
 // Malformed image metadata and a legacy version-only manifest never publish an offer.
 for(const std::string& malformed:{
  std::string("{\"version\":\"1.6.3\"}"),
  std::string("{\"version\":\"1.6.3\",\"builds\":[]}"),
  std::string("{\"version\":\"1.6.3\",\"builds\":[{\"parts\":[]}]}")}){
  reset();manifest=malformed;const auto result=ota_check();join_worker();
  CHECK(!result.ok&&!ota_get_status().update_available&&begins==0&&selectors==0);
 }
 reset();
}
void dev_preview_binding_checks(){
 for(bool preview:{false,true}){
  reset();const unsigned pr=preview?42:0;
  const std::string version=preview?"1.6.3-PR-42":"1.6.3-dev.1";
  const auto channel=preview?tk::OtaChannel::Release:tk::OtaChannel::Dev;
  ota_set_channel(channel);image_version=version;manifest=selected_manifest(version);
  changelog="{\"version\":\""+version+"\",\"changelog\":\"Bound notes\"}";
  CHECK(ota_check(pr).update_available);join_worker();
  const auto checked=ota_get_status();CHECK(checked.channel==channel&&checked.target_pr==pr&&checked.offer_id[0]);
  current_offer(checked.offer_id.data(),tk::ota_channel_name(channel),version.c_str(),pr,"Bound notes");
  raw_end_error=ESP_ERR_OTA_VALIDATE_FAILED; // Reach mandatory SDK verification without a terminal boot.
  const auto result=request_offer(handle_ota_update,"offer="+std::string(checked.offer_id.data())+(pr?"&pr=42":""));
  CHECK(response_status==200&&result);run_selected();join_worker();
  CHECK(begins==1&&writes>0&&ends==1&&selectors==0&&!ota_is_busy());
  const auto failed=ota_get_status(); // The install error keeps its confirmed source and PR.
  CHECK(failed.state==OtaState::Error&&failed.channel==channel&&failed.target_pr==pr);
 }
 // A replaced Dev candidate is refused under its own source, never the default Release.
 reset();ota_set_channel(tk::OtaChannel::Dev);image_version="1.6.3-dev.1";
 manifest=selected_manifest(image_version);changelog="{}";
 CHECK(ota_check(0).update_available);join_worker();
 const std::string dev_offer=ota_get_status().offer_id.data();manifest+=" ";
 const auto replaced=request_offer(handle_ota_update,"offer="+dev_offer);
 CHECK(response_status==200&&replaced);run_selected();join_worker();
 const char* changed=tk::ota_start_reason(tk::OtaStartResult::OfferChanged,false);
 no_current_offer("error","dev","dev",changed);
 CHECK(begins==0&&writes==0&&selectors==0);refused_offer("offer="+dev_offer);
 // A confirmed Dev candidate that is no longer newer is refused in one Error record.
 reset();ota_set_channel(tk::OtaChannel::Dev);image_version="1.6.3-dev.1";
 manifest=selected_manifest(image_version);changelog="{}";
 CHECK(ota_check(0).update_available);join_worker();
 const std::string older_offer=ota_get_status().offer_id.data();manifest=selected_manifest("1.6.2");
 const auto older=request_offer(handle_ota_update,"offer="+older_offer);
 CHECK(response_status==200&&older);run_selected();join_worker();
 no_current_offer("error","dev","dev","no newer version available");
 CHECK(begins==0&&writes==0&&selectors==0);refused_offer("offer="+older_offer);
 // Reverse direction while the real deferred check still owns its captured Dev source.
 reset();ota_set_channel(tk::OtaChannel::Dev);image_version="1.6.3-dev.1";
 manifest=selected_manifest(image_version);changelog="{}";
 CHECK(ota_check_start_after_http(0,&http_server_marker)==tk::OtaStartResult::Started);
 change_channel(tk::OtaChannel::Release);run_queued_http();run_selected();join_worker();
 CHECK(ota_get_state()==OtaState::Error&&!ota_get_status().update_available);
 CHECK(ota_get_status().channel==tk::OtaChannel::Dev&&ota_get_status().configured_channel==tk::OtaChannel::Release);
 CHECK(begins==0&&writes==0&&selectors==0);
 reset();
}
void hash_binding_checks(){
 tk::ota::Digest digest{},expected{};
 CHECK(tk::parse_ota_sha256("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",expected));
 CHECK(tk::ota::sha256("abc",3,digest)==PSA_SUCCESS&&digest==expected);
 for(auto call:{ota_hash_fixture::Call::Compute,ota_hash_fixture::Call::Setup,
                ota_hash_fixture::Call::Update,ota_hash_fixture::Call::Finish}){
  reset_for_image();ota_hash_fixture::reset();ota_hash_fixture::fail_call=call;
  const auto json=request_offer(handle_ota_update,"offer="+confirmed_offer);CHECK(response_status==200&&json);
  run_selected();join_worker();
  CHECK(selectors==0&&ends==0&&!raw_handle_live&&!ota_is_busy()&&!ota_get_status().update_available);
  if(call==ota_hash_fixture::Call::Compute||call==ota_hash_fixture::Call::Setup)CHECK(writes==0);
  ota_hash_fixture::reset();
 }
 reset();
}
void image_binding_checks(){
 for(int mutation:{1,2,3}){
  reset_for_image();image_mutation=mutation;
  raw_end_error=ESP_ERR_OTA_VALIDATE_FAILED; // An omitted hash guard must fail the end-count assertion.
  CHECK(start_confirmed()==tk::OtaStartResult::Started);run_selected();join_worker();
  CHECK(begins==1&&writes>0&&ends==0&&selectors==0&&aborts==1&&!raw_handle_live&&!ota_is_busy());
  CHECK(ota_get_state()==OtaState::Error&&!ota_get_status().update_available);
 }
 // The manifest's byte count is independent of its digest: correct bytes/hash but a
 // different declared count must still refuse before SDK end and boot selection.
 reset();manifest=selected_manifest();
 const auto size=manifest.find("\"size\":1024");CHECK(size!=std::string::npos);
 manifest.replace(size,std::strlen("\"size\":1024"),"\"size\":1025");
 CHECK(ota_check().ok);join_worker();confirmed_offer=ota_get_status().offer_id.data();
 raw_end_error=ESP_ERR_OTA_VALIDATE_FAILED;
 CHECK(start_confirmed()==tk::OtaStartResult::Started);run_selected();join_worker();
 CHECK(begins==1&&ends==0&&selectors==0&&aborts==1&&!ota_is_busy());
 reset();
}
void exhaustion_checks(bool channel){
 reset_for_image();const auto old=confirmed_offer;
 if(channel){
  s_ota_channel.store(tk::kOtaChannelRevisionMask,std::memory_order_release);
  ota_set_channel(tk::OtaChannel::Dev);ota_set_channel(tk::OtaChannel::Release);
  CHECK(s_ota_channel.load()&tk::kOtaChannelExhausted);
 }else{
  s_offer_serial.store(UINT32_MAX-1u,std::memory_order_release);
 }
 ota_set_channel(tk::OtaChannel::Dev); // Exhaustion still names the captured source.
 CHECK(ota_check_start()==tk::OtaStartResult::Unavailable);
 CHECK(!ota_get_status().update_available&&ota_get_status().offer_id[0]==0);
 CHECK(ota_get_status().channel==tk::OtaChannel::Dev&&ota_get_status().state==OtaState::Error);
 CHECK(ota_check_start()==tk::OtaStartResult::Unavailable);
 refused_offer("offer="+old);
 CHECK(init_calls==0&&begins==0&&selectors==0&&!s_operation.load());
 // Exhaustion cannot revive a feed identity and does not remove signed local recovery.
 ota_set_channel(tk::OtaChannel::Dev);upload_body=image_body();raw_end_error=ESP_ERR_OTA_VALIDATE_FAILED;
 httpd_req_t request{&http_server_marker,upload_body.size()};bool handed=false;
 CHECK(ota_upload_start(&request,handed)==tk::OtaStartResult::Started&&handed);
 run_selected();CHECK(begins==1&&ends==1&&selectors==0&&!ota_is_busy());
}
} // namespace fixture
