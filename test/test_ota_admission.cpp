// Actual raw SDK query/cache bodies. Complete entry/worker/lease tests use test_ota_tls_admission.
#include "logic/health_gate.hpp"
#include "logic/ota_contract.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
namespace {
int checks;
#define CHECK(x) do { ++checks; if(!(x)){std::cerr<<"OTA raw cache "<<__LINE__<<": "<<#x<<'\n';std::exit(1);} } while(false)
using Verification=tk::OtaVerificationState;
std::atomic<Verification> s_verification{Verification::Unknown};
enum esp_ota_img_states_t : unsigned { ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_VALID,
 ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,ESP_OTA_IMG_UNDEFINED=~0u };
struct esp_partition_t {} partition;
bool available=true,query_ok=true,query_throws=false;
int queries=0;
esp_ota_img_states_t raw=ESP_OTA_IMG_VALID;
constexpr int ESP_OK=0;
const esp_partition_t* esp_ota_get_running_partition(){return available?&partition:nullptr;}
int esp_ota_get_state_partition(const esp_partition_t* p,esp_ota_img_states_t* s){
 CHECK(p==&partition);++queries;if(query_throws)throw std::bad_alloc();*s=raw;return query_ok?0:-1;
}
#include "ota_admission_adapter.generated.hpp"
}
int test_ota_admission(){
 for(auto s:{ESP_OTA_IMG_VALID,ESP_OTA_IMG_UNDEFINED,ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY,
             ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,static_cast<esp_ota_img_states_t>(17)}){
  raw=s;const int before=queries;const auto expected=(s==ESP_OTA_IMG_VALID||s==ESP_OTA_IMG_UNDEFINED)
   ?Verification::Stable:(s==ESP_OTA_IMG_NEW||s==ESP_OTA_IMG_PENDING_VERIFY)
   ?Verification::PendingVerify:Verification::Unknown;
  CHECK(ota_refresh_verification_cache()==expected&&queries==before+1);
  raw=ESP_OTA_IMG_INVALID;CHECK(ota_cached_verification_state()==expected&&queries==before+1);
 }
 available=false;CHECK(ota_refresh_verification_cache()==Verification::Unknown);available=true;
 query_ok=false;CHECK(ota_refresh_verification_cache()==Verification::Unknown);query_ok=true;
 query_throws=true;CHECK(ota_refresh_verification_cache()==Verification::Unknown);query_throws=false;
 std::cout<<"OTA raw cache checks "<<checks<<'\n';return checks;
}
