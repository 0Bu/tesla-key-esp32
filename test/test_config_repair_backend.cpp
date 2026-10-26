// Actual complete IdfStore + run/finish/report/park + RawConsole bodies. No real NVS/key/TLS.
// SDK objects below are passive fixtures, not target ABI or cryptographic implementations.
#include "config_repair.hpp"
#include "config_repair_store.hpp"
#include "logic/config_commit.hpp"
#include <driver/uart.h>
#include <driver/usb_serial_jtag.h>
#include <hal/uart_ll.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fixture {
using Bytes=std::vector<std::uint8_t>;
int checks=0;
[[noreturn]] void fail(const char* expression,int line){
 std::fprintf(stderr,"F121 backend CHECK %d: %s\n",line,expression);std::fflush(stderr);std::_Exit(1);
}
#define CHECK(x) do{++fixture::checks;if(!(x))fixture::fail(#x,__LINE__);}while(false)
struct Record {nvs_type_t type;Bytes bytes;bool operator==(const Record& o)const{return type==o.type&&bytes==o.bytes;}};
using Key=std::pair<std::string,std::string>;
std::map<Key,Record> records,original;
struct Handle {std::string ns;nvs_open_mode_t mode;};
std::map<nvs_handle_t,Handle> handles;
nvs_handle_t next_handle=1;
std::string mode;
int sets=0,commits=0,identity_calls=0,imports=0,destroy_keys=0,raw_queries=0,read_calls=0;
int info_attempts=0,receipt_attempts=0,seal_calls=0,hash_finishes=0,iterator_ends=0,iterator_releases=0;
bool owner_held=false,owner_sealed=false,owner_alive=false;
std::int64_t now=1000;
std::vector<Bytes> hash_transcripts,unique_transcripts;
std::map<size_t,Bytes> hash_operations;
size_t next_hash=1;
const unsigned char* parsed_key_buffer=nullptr;size_t parsed_key_length=0;
std::array<std::uint8_t,6> board{{2,3,4,5,6,7}};
std::array<std::uint8_t,65> public_point=[] {std::array<std::uint8_t,65> p{};p[0]=4;for(size_t i=1;i<p.size();++i)p[i]=static_cast<std::uint8_t>(i);return p;}();
std::array<std::uint8_t,32> challenge=[] {std::array<std::uint8_t,32> p{};for(size_t i=0;i<p.size();++i)p[i]=static_cast<std::uint8_t>(i+70);return p;}();
Bytes canonical,alternate,frame,terminal_receipt;
uint8_t* actual_frame_buffer=nullptr;
const char synthetic_private[]="SYNTHETIC PUBLIC FIXTURE; NOT A KEY";
// Fixed independently generated serialized-record golden; hash facade checks the actual exact
// bytes passed by production, and returns injective per-process transcript tokens, NOT SHA-256.
#include "config_repair_retention_golden.hpp"
Bytes encode(const tk::ConfigBlob& in){
 tk::ConfigBlobBuffer data{};const auto size=tk::config_blob_encode(in,data.data(),data.size());CHECK(size);
 return {data.begin(),data.begin()+size};
}
void initialize(){
 const uint16_t endian_marker=1;CHECK(*reinterpret_cast<const uint8_t*>(&endian_marker)==1); // Literal numeric golden is LE host-only.
 tk::ConfigBlob replacement;replacement.vin="5YJ3E1EA0KF000001";replacement.has_ota=true;replacement.ota_channel=0;
 canonical=encode(replacement);replacement.ota_channel=1;alternate=encode(replacement);CHECK(alternate.size()==canonical.size());
 Bytes corrupt=canonical;corrupt.back()^=1;
 records[{"tesla_cfg","cfg"}]={NVS_TYPE_BLOB,corrupt};
 records[{"tesla_cfg","vin"}]={NVS_TYPE_STR,{'l','e','g','a','c','y',0}};
 records[{"tesla_ble","private_key"}]={NVS_TYPE_BLOB,Bytes(synthetic_private,synthetic_private+sizeof(synthetic_private)-1)};
 records[{"tesla_ble","sess_vcsec"}]={NVS_TYPE_BLOB,{9,8,7,0}};
 records[{"other","cfg"}]={NVS_TYPE_BLOB,{0,1,2}}; // Same key in another namespace MUST be retained.
 records[{"other","text"}]={NVS_TYPE_STR,{'o','k',0}};
 records[{"other","empty"}]={NVS_TYPE_BLOB,{}};
 records[{"types","u8"}]={NVS_TYPE_U8,{0x81}};records[{"types","i8"}]={NVS_TYPE_I8,{0xff}};
 records[{"types","u16"}]={NVS_TYPE_U16,{0x34,0x12}};records[{"types","i16"}]={NVS_TYPE_I16,{0xfe,0xff}};
 records[{"types","u32"}]={NVS_TYPE_U32,{1,2,3,4}};records[{"types","i32"}]={NVS_TYPE_I32,{0xfc,0xff,0xff,0xff}};
 records[{"types","u64"}]={NVS_TYPE_U64,{1,2,3,4,5,6,7,8}};
 records[{"types","i64"}]={NVS_TYPE_I64,{0xf8,0xff,0xff,0xff,0xff,0xff,0xff,0xff}};
 if(mode=="cfg-valid")records[{"tesla_cfg","cfg"}].bytes=canonical;
 if(mode=="cfg-missing")records.erase({"tesla_cfg","cfg"});
 if(mode=="journal-present")records[{"tesla_cfg","vin_txn"}]={NVS_TYPE_STR,{0}};
 if(mode=="journal-type")records[{"tesla_ble","key_rotate"}]={NVS_TYPE_U8,{0}};
 if(mode=="value-overlong")records[{"other","huge"}]={NVS_TYPE_BLOB,Bytes(4097,0)};
 if(mode=="unsupported-type")records[{"other","float"}]={NVS_TYPE_FLOAT,{0,0,0,0}};
 original=records;
}
const Record* get(nvs_handle_t h,const char* key,nvs_type_t type){
 CHECK(key&&handles.count(h));const auto it=records.find({handles.at(h).ns,key});
 if(it==records.end())return nullptr;
 return it->second.type==type?&it->second:nullptr;
}
esp_err_t get_bytes(nvs_handle_t h,const char* key,void* dst,size_t* size,nvs_type_t type){
 CHECK(size&&key&&handles.count(h));const auto it=records.find({handles.at(h).ns,key});
 if(it==records.end())return ESP_ERR_NVS_NOT_FOUND;
 if(it->second.type!=type)return ESP_ERR_NVS_TYPE_MISMATCH;
 const auto& value=it->second.bytes;
 if(!dst){*size=value.size();return ESP_OK;}
 if(*size<value.size()){*size=value.size();return ESP_ERR_NVS_INVALID_LENGTH;}
 if(!value.empty())std::memcpy(dst,value.data(),value.size());
 *size=value.size();
 if(mode=="length-drift"&&handles.at(h).ns=="tesla_cfg"&&std::strcmp(key,"cfg")==0&&sets==0)++*size;
 if(mode=="retention-length-drift"&&handles.at(h).ns=="tesla_ble"&&std::strcmp(key,"sess_vcsec")==0)--*size;
 return ESP_OK;
}
void check_cfg_only(){
 auto after=records;auto before=original;after.erase({"tesla_cfg","cfg"});before.erase({"tesla_cfg","cfg"});
 CHECK(after==before);
}
void make_frame(){
 using namespace tk::repair;frame.assign(kFrameFixed+canonical.size(),0);
 std::memcpy(frame.data(),"TKAP1",5);frame[5]=std::uint8_t(frame.size());frame[6]=std::uint8_t(frame.size()>>8);frame[7]=1;
 size_t p=8;std::memcpy(frame.data()+p,board.data(),6);p+=6;
 std::memcpy(frame.data()+p,public_point.data(),65);p+=65;
 std::memcpy(frame.data()+p,challenge.data(),32);p+=32;
 std::memcpy(frame.data()+p,"5YJ3E1EA0KF000001",17);p+=17;
 frame[p++]=std::uint8_t(canonical.size());frame[p++]=std::uint8_t(canonical.size()>>8);
 std::memcpy(frame.data()+p,canonical.data(),canonical.size());
 if(mode=="wrong-public")frame[8+6+30]^=1;
 if(mode=="wrong-challenge")frame[8+6+65]^=1;
 const auto crc=tk::config_crc32(frame.data(),frame.size()-4);
 for(unsigned n=0;n!=4;++n)frame[frame.size()-4+n]=std::uint8_t(crc>>(8*n));
}
[[noreturn]] void terminal();
}

// Passive owner/raw-state facade. Complete maintenance run is actual; this is NOT a test of
// the separately reviewed ordinary OTA guard implementation or a real hardware boot state.
using tk::ConfigImageResult;
class OtaConfigRestartGuard {
public:
 OtaConfigRestartGuard()noexcept{CHECK(!fixture::owner_alive);fixture::owner_alive=true;}
 ~OtaConfigRestartGuard()noexcept{fixture::owner_alive=false;if(!fixture::owner_sealed)fixture::owner_held=false;}
 OtaConfigRestartGuard(const OtaConfigRestartGuard&)=delete;
 bool try_begin()noexcept{if(fixture::mode=="owner-busy")return false;fixture::owner_held=true;return true;}
 bool owns_gate()const noexcept{return fixture::owner_held;}
 void seal()noexcept{CHECK(owns_gate());fixture::owner_sealed=true;++fixture::seal_calls;}
};
tk::ConfigImageResult ota_config_image_state(const OtaConfigRestartGuard& owner)noexcept{
 if(!owner.owns_gate())return tk::ConfigImageResult::WrongOwner;
 ++fixture::raw_queries;
 if(fixture::mode=="raw-pending"||(fixture::mode=="raw-late"&&fixture::raw_queries==2))return tk::ConfigImageResult::Pending;
 return tk::ConfigImageResult::Valid;
}
extern "C" int64_t esp_timer_get_time(void){return fixture::now;}
extern "C" void vTaskDelay(TickType_t ticks){if(ticks==10000)fixture::terminal();fixture::now+=static_cast<int64_t>(ticks)*1000;}
extern "C" void esp_fill_random(void* dst,size_t n){CHECK(n==fixture::challenge.size());std::memcpy(dst,fixture::challenge.data(),n);}
extern "C" esp_err_t esp_efuse_mac_get_default(uint8_t* dst){CHECK(dst);std::memcpy(dst,fixture::board.data(),6);return ESP_OK;}
extern "C" esp_err_t nvs_flash_init(void){CHECK(fixture::owner_held);return fixture::mode=="storage-init"?ESP_FAIL:ESP_OK;}
extern "C" esp_err_t nvs_open_from_partition(const char* part,const char* ns,nvs_open_mode_t access,nvs_handle_t* out){
 CHECK(part&&std::strcmp(part,"nvs")==0&&ns&&out);
 CHECK(access==NVS_READONLY||access==NVS_READWRITE);
 bool found=false;for(const auto& row:fixture::records)if(row.first.first==ns)found=true;
 if(!found)return ESP_ERR_NVS_NOT_FOUND;
 if(access==NVS_READWRITE)CHECK(fixture::owner_held&&fixture::owner_sealed&&std::strcmp(ns,"tesla_cfg")==0);
 *out=fixture::next_handle++;fixture::handles[*out]={ns,access};return ESP_OK;
}
extern "C" void nvs_close(nvs_handle_t h){CHECK(fixture::handles.erase(h)==1);}
extern "C" esp_err_t nvs_get_blob(nvs_handle_t h,const char* k,void* dst,size_t* n){return fixture::get_bytes(h,k,dst,n,NVS_TYPE_BLOB);}
extern "C" esp_err_t nvs_get_str(nvs_handle_t h,const char* k,char* dst,size_t* n){return fixture::get_bytes(h,k,dst,n,NVS_TYPE_STR);}
#define NUMBER_GETTER(suffix,type,kind) extern "C" esp_err_t nvs_get_##suffix(nvs_handle_t h,const char* k,type* out){ \
 CHECK(out);const auto* r=fixture::get(h,k,kind);if(!r)return ESP_ERR_NVS_TYPE_MISMATCH; \
 CHECK(r->bytes.size()==sizeof(type));std::memcpy(out,r->bytes.data(),sizeof(type));return ESP_OK;}
NUMBER_GETTER(u8,uint8_t,NVS_TYPE_U8) NUMBER_GETTER(i8,int8_t,NVS_TYPE_I8)
NUMBER_GETTER(u16,uint16_t,NVS_TYPE_U16) NUMBER_GETTER(i16,int16_t,NVS_TYPE_I16)
NUMBER_GETTER(u32,uint32_t,NVS_TYPE_U32) NUMBER_GETTER(i32,int32_t,NVS_TYPE_I32)
NUMBER_GETTER(u64,uint64_t,NVS_TYPE_U64) NUMBER_GETTER(i64,int64_t,NVS_TYPE_I64)
#undef NUMBER_GETTER
extern "C" esp_err_t nvs_set_blob(nvs_handle_t h,const char* k,const void* src,size_t n){
 CHECK(fixture::owner_held&&fixture::owner_sealed&&fixture::seal_calls==1);
 CHECK(fixture::handles.count(h)&&fixture::handles.at(h).mode==NVS_READWRITE);
 CHECK(fixture::handles.at(h).ns=="tesla_cfg"&&std::strcmp(k,"cfg")==0);
 CHECK(++fixture::sets==1&&src&&n==fixture::canonical.size());
 CHECK(std::memcmp(src,fixture::canonical.data(),n)==0);
 fixture::records[{"tesla_cfg","cfg"}].bytes={static_cast<const uint8_t*>(src),static_cast<const uint8_t*>(src)+n};
 if(fixture::mode=="readback-corrupt")fixture::records[{"tesla_cfg","cfg"}].bytes.back()^=1;
 if(fixture::mode=="readback-substitute")fixture::records[{"tesla_cfg","cfg"}].bytes=fixture::alternate;
 if(fixture::mode=="retention-change")fixture::records[{"tesla_ble","sess_vcsec"}].bytes[0]^=1;
 return fixture::mode=="set-visible-failure"?ESP_FAIL:ESP_OK;
}
extern "C" esp_err_t nvs_commit(nvs_handle_t h){
 CHECK(fixture::handles.count(h)&&fixture::handles.at(h).mode==NVS_READWRITE&&fixture::owner_sealed);
 CHECK(++fixture::commits==1);return fixture::mode=="commit-visible-failure"?ESP_FAIL:ESP_OK;
}
struct nvs_opaque_iterator_t{std::vector<fixture::Key> keys;size_t index=0;};
static std::set<nvs_iterator_t> iterators;
extern "C" esp_err_t nvs_entry_find(const char* p,const char* ns,nvs_type_t type,nvs_iterator_t* out){
 CHECK(p&&std::strcmp(p,"nvs")==0&&ns==nullptr&&type==NVS_TYPE_ANY&&out&&!*out);
 if(fixture::mode=="iterator-find")return ESP_FAIL;
 auto* it=new nvs_opaque_iterator_t;for(const auto& row:fixture::records)it->keys.push_back(row.first);
 std::reverse(it->keys.begin(),it->keys.end()); // Production must sort; fixture input is not pre-sorted.
 if(it->keys.empty()){delete it;return ESP_ERR_NVS_NOT_FOUND;}
 iterators.insert(it);*out=it;return ESP_OK;
}
extern "C" esp_err_t nvs_entry_info(const nvs_iterator_t it,nvs_entry_info_t* out){
 CHECK(it&&iterators.count(it)&&out&&it->index<it->keys.size());if(fixture::mode=="iterator-info")return ESP_FAIL;
 const auto& key=it->keys[it->index];CHECK(key.first.size()<16&&key.second.size()<16);
 std::memset(out,0,sizeof(*out));std::memcpy(out->namespace_name,key.first.data(),key.first.size());
 std::memcpy(out->key,key.second.data(),key.second.size());out->type=fixture::records.at(key).type;return ESP_OK;
}
extern "C" esp_err_t nvs_entry_next(nvs_iterator_t* it){
 CHECK(it&&*it&&iterators.count(*it));if(fixture::mode=="iterator-next")return ESP_FAIL;
 if(++(*it)->index==(*it)->keys.size()){
  CHECK(iterators.erase(*it)==1);delete *it;*it=nullptr;++fixture::iterator_ends;return ESP_ERR_NVS_NOT_FOUND;
 }
 return ESP_OK;
}
extern "C" void nvs_release_iterator(nvs_iterator_t it){CHECK(it&&iterators.erase(it)==1);delete it;++fixture::iterator_releases;}

extern "C" psa_status_t psa_crypto_init(void){return PSA_SUCCESS;}
extern "C" void mbedtls_pk_init(mbedtls_pk_context* p){p->initialized=true;}
extern "C" void mbedtls_pk_free(mbedtls_pk_context* p){
 CHECK(p->initialized);p->initialized=false;
 if(fixture::parsed_key_buffer){
  for(size_t n=0;n<fixture::parsed_key_length;++n)CHECK(fixture::parsed_key_buffer[n]==0);
  fixture::parsed_key_buffer=nullptr;fixture::parsed_key_length=0;
 }
}
extern "C" int mbedtls_pk_parse_key(mbedtls_pk_context* p,const unsigned char* bytes,size_t n,const unsigned char* pwd,size_t pwdlen){
 CHECK(p->initialized&&pwd==nullptr&&pwdlen==0&&n==sizeof(fixture::synthetic_private));
 CHECK(std::memcmp(bytes,fixture::synthetic_private,n)==0);
 fixture::parsed_key_buffer=bytes;fixture::parsed_key_length=n;++fixture::identity_calls;
 return fixture::mode=="key-parse"?-1:0;
}
extern "C" int mbedtls_pk_get_psa_attributes(const mbedtls_pk_context* p,psa_key_usage_t usage,psa_key_attributes_t* a){
 CHECK(p->initialized&&usage==PSA_KEY_USAGE_DERIVE);a->type=PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1);
 a->bits=fixture::mode=="key-bits"?384:256;return 0;
}
extern "C" int mbedtls_pk_import_into_psa(const mbedtls_pk_context* p,const psa_key_attributes_t* a,mbedtls_svc_key_id_t* id){
 CHECK(p->initialized&&a->lifetime==PSA_KEY_LIFETIME_VOLATILE&&id);*id=7;++fixture::imports;return 0;
}
extern "C" psa_status_t psa_export_public_key(mbedtls_svc_key_id_t id,uint8_t* dst,size_t cap,size_t* n){
 CHECK(id==7&&cap==65&&n);std::memcpy(dst,fixture::public_point.data(),65);*n=65;
 if(fixture::mode=="key-refresh"&&fixture::identity_calls==2)dst[40]^=1;
 return PSA_SUCCESS;
}
extern "C" psa_status_t psa_destroy_key(mbedtls_svc_key_id_t id){CHECK(id==0||id==7);if(id)++fixture::destroy_keys;return PSA_SUCCESS;}
extern "C" void psa_reset_key_attributes(psa_key_attributes_t* a){*a={0,0,0};}
extern "C" psa_status_t psa_hash_setup(psa_hash_operation_t* op,psa_algorithm_t alg){
 CHECK(alg==PSA_ALG_SHA_256&&op->token==0);op->token=fixture::next_hash++;fixture::hash_operations[op->token]={};
 return fixture::mode=="hash-setup"?PSA_ERROR_GENERIC_ERROR:PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_update(psa_hash_operation_t* op,const uint8_t* data,size_t n){
 CHECK(fixture::hash_operations.count(op->token)&&(!n||data));auto& bytes=fixture::hash_operations.at(op->token);
 bytes.insert(bytes.end(),data,data+n);return PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_finish(psa_hash_operation_t* op,uint8_t* out,size_t cap,size_t* n){
 CHECK(fixture::hash_operations.count(op->token)&&cap==32&&out&&n);
 auto transcript=fixture::hash_operations.at(op->token);fixture::hash_transcripts.push_back(transcript);++fixture::hash_finishes;
 if(fixture::hash_finishes==1&&fixture::mode!="unsupported-type"&&fixture::mode!="value-overlong"&&fixture::mode!="key-journal-late"&&fixture::mode!="journal-late")
  CHECK(transcript==fixture::golden_retention); // Independent literal golden, never generated by product.
 auto& list=fixture::unique_transcripts;auto found=std::find(list.begin(),list.end(),transcript);
 if(found==list.end()){list.push_back(transcript);found=list.end()-1;}
 const size_t token=static_cast<size_t>(found-list.begin())+1;std::memset(out,0,cap);
 for(size_t i=0;i<sizeof(token);++i){out[i]=std::uint8_t(token>>(8*i));}
 *n=cap;
 fixture::hash_operations.erase(op->token);op->token=0;return PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_abort(psa_hash_operation_t* op){if(op->token)fixture::hash_operations.erase(op->token);op->token=0;return PSA_SUCCESS;}

namespace fixture {
std::vector<uint8_t> serial_input,serial_output;size_t serial_offset=0;
bool driver_installed=false,serial_zero=false,serial_bad_count=false,tx_never_idle=false;
int installs=0,serial_reads=0,serial_writes=0,io_yields=0,tx_polls=0;
int serial_read(void* p,uint32_t size,uint32_t wait){
 CHECK(wait==0&&size<=64&&p);++serial_reads;if(serial_bad_count)return static_cast<int>(size+1);
 if(serial_zero)return 0;
 const auto n=std::min({size,uint32_t(3),static_cast<uint32_t>(serial_input.size()-serial_offset)});
 std::memcpy(p,serial_input.data()+serial_offset,n);serial_offset+=n;return static_cast<int>(n);
}
int serial_write(const void* p,size_t n,uint32_t wait){
 CHECK(wait==0&&n<=64&&p);++serial_writes;if(serial_bad_count)return static_cast<int>(n+1);
 const size_t count=serial_zero?0:std::min(n,size_t(3));const auto* b=static_cast<const uint8_t*>(p);
 serial_output.insert(serial_output.end(),b,b+count);return static_cast<int>(count);
}
}
uart_dev_t repair_fixture_uart;
extern "C" bool uart_is_driver_installed(uart_port_t p){CHECK(p==UART_NUM_0);return fixture::driver_installed;}
extern "C" esp_err_t uart_param_config(uart_port_t p,const uart_config_t* cfg){CHECK(p==UART_NUM_0&&cfg->baud_rate==115200);return ESP_OK;}
extern "C" esp_err_t _uart_set_pin6(uart_port_t p,int tx,int rx,int rts,int cts,int dtr,int dsr){CHECK(p==0&&tx==-1&&rx==-1&&rts==-1&&cts==-1&&dtr==-1&&dsr==-1);return ESP_OK;}
extern "C" esp_err_t uart_driver_install(uart_port_t p,int rx,int tx,int q,QueueHandle_t* queue,int flags){CHECK(p==0&&rx==1024&&tx==0&&q==0&&queue==nullptr&&flags==0);++fixture::installs;return ESP_OK;}
extern "C" int uart_read_bytes(uart_port_t p,void* dst,uint32_t n,uint32_t wait){CHECK(p==0);return fixture::serial_read(dst,n,wait);}
extern "C" uint32_t uart_ll_get_txfifo_len(uart_dev_t* p){CHECK(p==&repair_fixture_uart);return fixture::serial_zero?0:3;}
extern "C" void uart_ll_write_txfifo(uart_dev_t* p,const uint8_t* data,uint32_t n){CHECK(p==&repair_fixture_uart&&n<=3);if(n)(void)fixture::serial_write(data,n,0);}
extern "C" bool uart_ll_is_tx_idle(uart_dev_t* p){CHECK(p==&repair_fixture_uart);++fixture::tx_polls;return !fixture::tx_never_idle;}
extern "C" bool usb_serial_jtag_is_driver_installed(void){return fixture::driver_installed;}
extern "C" esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t* cfg){CHECK(cfg->rx_buffer_size==1024&&cfg->tx_buffer_size==256);++fixture::installs;return ESP_OK;}
extern "C" int usb_serial_jtag_read_bytes(void* dst,uint32_t n,uint32_t wait){return fixture::serial_read(dst,n,wait);}
extern "C" int usb_serial_jtag_write_bytes(const void* src,size_t n,uint32_t wait){return fixture::serial_write(src,n,wait);}
extern "C" esp_err_t usb_serial_jtag_wait_tx_done(uint32_t wait){CHECK(wait==0);++fixture::tx_polls;return fixture::tx_never_idle?ESP_FAIL:ESP_OK;}

#include "config_repair_backend.generated.hpp"

namespace fixture {
struct TestConsole final:tk::repair::Console {
 size_t position=0;
 bool write_exact(const uint8_t* p,size_t n,uint64_t until)noexcept override{
  CHECK(now>=0&&until==static_cast<uint64_t>(now)+5000000);
  if(n==108){CHECK(std::memcmp(p,"TKCR1",5)==0);++info_attempts;
   CHECK(std::memcmp(p+5,board.data(),6)==0&&std::memcmp(p+11,public_point.data(),65)==0);
   CHECK(std::memcmp(p+76,challenge.data(),32)==0);return mode!="info-failure";
  }
  CHECK(n==10&&std::memcmp(p,"TKRS1",5)==0);++receipt_attempts;terminal_receipt.assign(p,p+n);
  return mode!="receipt-failure";
 }
 bool read_exact(uint8_t* p,size_t n,uint64_t until)noexcept override{
  CHECK(until==30001000&&p&&n&&position+n<=frame.size());++read_calls;
  if(position==0)actual_frame_buffer=p;
  if(mode=="read-failure")return false;
  if(position==0){
   if(mode=="journal-late")records[{"tesla_cfg","vin_txn"}]={NVS_TYPE_STR,{0}};
   if(mode=="key-journal-late")records[{"tesla_ble","key_rotate"}]={NVS_TYPE_BLOB,{0}};
   if(mode=="owner-late")owner_held=false;
  }
  std::memcpy(p,frame.data()+position,n);
  if(mode=="read-time-progress"&&position==0)now+=5000;
  position+=n;return true;
 }
};
[[noreturn]] void terminal(){
 using tk::repair::Outcome;
 if(mode=="console-entry-installed"){
  CHECK(info_attempts==0&&receipt_attempts==0&&sets==0&&commits==0&&installs==0&&!owner_alive);
  std::printf("F121 console entry parks before run; checks %d\n",checks);std::fflush(stdout);std::_Exit(0);
 }
 CHECK(receipt_attempts==1&&terminal_receipt.size()==10);
 const bool effect=mode=="run-positive"||mode=="read-time-progress"||mode=="receipt-failure"||mode=="set-visible-failure"||
   mode=="commit-visible-failure"||mode=="readback-corrupt"||mode=="readback-substitute"||mode=="retention-change";
 const auto outcome=static_cast<Outcome>(terminal_receipt[5]);
 if(effect){
  CHECK(sets==1&&owner_held&&owner_sealed&&seal_calls==1&&terminal_receipt[6]==1);
  if(mode=="run-positive"||mode=="read-time-progress"||mode=="receipt-failure"){
   CHECK(outcome==Outcome::Confirmed&&commits==1&&terminal_receipt[7]==1&&terminal_receipt[8]==1&&terminal_receipt[9]==1);
   CHECK(info_attempts==1&&read_calls==2&&raw_queries==2&&identity_calls==2);
   CHECK(records.at({"tesla_cfg","cfg"}).bytes==canonical);check_cfg_only();
   CHECK(hash_finishes==2&&hash_transcripts[0]==golden_retention&&hash_transcripts[1]==golden_retention);
  }else{
   CHECK(outcome==Outcome::Ambiguous&&terminal_receipt[9]==0);
   if(mode=="set-visible-failure")CHECK(commits==0&&terminal_receipt[7]==0&&terminal_receipt[8]==0);
   if(mode=="commit-visible-failure")CHECK(commits==1&&terminal_receipt[7]==0&&terminal_receipt[8]==0);
   if(mode=="readback-corrupt"||mode=="readback-substitute")CHECK(commits==1&&terminal_receipt[7]==1&&terminal_receipt[8]==0);
   if(mode=="retention-change")CHECK(commits==1&&terminal_receipt[7]==1&&terminal_receipt[8]==1&&hash_transcripts.size()==2&&hash_transcripts[0]!=hash_transcripts[1]);
  }
 }else CHECK(outcome==Outcome::Refused&&sets==0&&commits==0&&!owner_sealed&&seal_calls==0&&terminal_receipt[6]==0);
 CHECK(imports==destroy_keys&&iterators.empty()&&hash_operations.empty());
 if(actual_frame_buffer){for(size_t n=0;n<frame.size();++n)CHECK(actual_frame_buffer[n]==0);}
 // Real run parks while its owner and static IdfStore remain alive. This terminal fixture exits
 // without unwinding; it is NOT per-task/LSan lifetime, NVS persistence or hardware evidence.
 CHECK(owner_alive);std::printf("F121 backend terminal checks %d\n",checks);std::fflush(stdout);std::_Exit(0);
}
void backend_direct(){
 {tk::repair::IdfStore store;CHECK(store.open_existing());tk::repair::BoardId b{};tk::repair::PublicPoint p{};
 CHECK(store.public_identity(b,p)&&b==board&&p==public_point);
 CHECK(store.journal("vin_txn",true)==tk::repair::Read::Missing&&store.journal("key_rotate",false)==tk::repair::Read::Missing);
 tk::repair::Wiped<tk::repair::kMaxBlob> data;size_t size=0;
 CHECK(store.read_cfg(data.bytes.data(),data.bytes.size(),size)==tk::repair::Read::Present);
 CHECK(tk::repair::classify_blob(tk::repair::Read::Present,data.bytes.data(),size)==tk::repair::BlobClass::RepairableCurrentCorruption);
 tk::repair::Digest a{},bhash{};CHECK(store.retention_digest_excluding_cfg(a));CHECK(store.retention_digest_excluding_cfg(bhash)&&a==bhash);
 CHECK(hash_transcripts.size()==2&&hash_transcripts[0]==golden_retention&&hash_transcripts[1]==golden_retention);
 CHECK(sets==0&&commits==0&&iterator_ends==2&&iterator_releases==0);}
 CHECK(handles.empty()&&iterators.empty()&&hash_operations.empty()&&imports==destroy_keys);
}
void console_checks(){
 tk::repair::RawConsole io;
 if(mode=="console-installed"){driver_installed=true;CHECK(!io.init()&&installs==0);return;}
 CHECK(io.init()&&installs==1);
 serial_input.resize(157);for(size_t i=0;i<serial_input.size();++i)serial_input[i]=static_cast<uint8_t>(i);
 std::array<uint8_t,157> received{};
 const auto until=static_cast<uint64_t>(now)+2000;
 if(mode=="console-expired"){CHECK(!io.read_exact(received.data(),1,static_cast<uint64_t>(now))&&serial_reads==0);return;}
 if(mode=="console-zero"){serial_zero=true;CHECK(!io.read_exact(received.data(),1,until)&&serial_reads>0);return;}
 if(mode=="console-bad-count"){serial_bad_count=true;CHECK(!io.read_exact(received.data(),1,until));return;}
 CHECK(io.read_exact(received.data(),received.size(),until));CHECK(Bytes(received.begin(),received.end())==serial_input&&serial_reads>1);
 if(mode=="console-tx-deadline"){tx_never_idle=true;CHECK(!io.write_exact(received.data(),received.size(),until)&&tx_polls>0);return;}
 CHECK(mode=="console-positive");
 CHECK(io.write_exact(received.data(),received.size(),until));CHECK(serial_output==serial_input&&serial_writes>1&&tx_polls>0);
 CHECK(sets==0&&commits==0);
}
}
int main(int argc,char** argv){
 CHECK(argc==2);fixture::mode=argv[1];fixture::initialize();fixture::make_frame();
 if(fixture::mode=="backend-direct"){
  fixture::owner_held=true;fixture::backend_direct();std::printf("F121 backend direct checks %d\n",fixture::checks);return 0;
 }
 if(fixture::mode=="console-entry-installed"){fixture::driver_installed=true;tk::repair::console_entry();}
 if(fixture::mode.rfind("console-",0)==0){fixture::console_checks();std::printf("F121 console checks %d\n",fixture::checks);return 0;}
 fixture::TestConsole console;tk::repair::run(console);
}
