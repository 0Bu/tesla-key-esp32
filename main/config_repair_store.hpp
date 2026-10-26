#pragma once
// Maintenance-only IDF backend; the complete backend is absent from normal compilation.
#include "sdkconfig.h"
#if defined(CONFIG_TESLA_CONFIG_REPAIR_USB) && CONFIG_TESLA_CONFIG_REPAIR_USB
#include "logic/config_repair.hpp"
#include <nvs.h>
#include <nvs_flash.h>
#include <psa/crypto.h>
#include <mbedtls/pk.h>
#include <esp_mac.h>
#include <algorithm>
#include <cstring>

namespace tk::repair {
// Values never leave the device; retention is checked by a sorted logical-record digest, so NVS
// page movement/garbage collection cannot become a false difference. Unknown namespaces/keys are
// included. Oversized inventory/value, unsupported type or any NVS/PSA error refuses before write.
class IdfStore {
    static constexpr std::size_t kMaxEntries=96, kMaxValue=4096;
    struct Entry { char ns[16]{}; char key[16]{}; nvs_type_t type{}; } entries_[kMaxEntries];
    Wiped<kMaxValue+1> value_;
    nvs_handle_t cfg_{};
    nvs_handle_t tesla_{};
    void clear_value() noexcept {
        volatile std::uint8_t* bytes=value_.bytes.data();
        for(std::size_t i=0;i<value_.bytes.size();++i)bytes[i]=0;
    }
    struct ClearValue { IdfStore& store; ~ClearValue(){store.clear_value();} };
    bool open_{};
    bool write_used_{};
    static constexpr const char* kPartition="nvs";
    struct Handle {
        nvs_handle_t value{};
        ~Handle() { if(value) nvs_close(value); }
    };
    struct Iterator {
        nvs_iterator_t value{};
        ~Iterator() { if(value) nvs_release_iterator(value); }
    };
    struct Hash {
        psa_hash_operation_t value=PSA_HASH_OPERATION_INIT;
        ~Hash() { (void)psa_hash_abort(&value); }
        bool add(const void* p,std::size_t n) { return psa_hash_update(&value,
            static_cast<const std::uint8_t*>(p),n)==PSA_SUCCESS; }
        bool length(std::size_t n) {
            if(n>UINT32_MAX)return false;
            const std::uint8_t bytes[4]={std::uint8_t(n),std::uint8_t(n>>8),std::uint8_t(n>>16),std::uint8_t(n>>24)};
            return add(bytes,sizeof(bytes));
        }
    };
    bool read_value(nvs_handle_t h,const Entry& entry,std::size_t& size) {
        switch(entry.type) {
            case NVS_TYPE_U8: {
                std::uint8_t n{};if(nvs_get_u8(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_I8: {
                std::int8_t n{};if(nvs_get_i8(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_U16: {
                std::uint16_t n{};if(nvs_get_u16(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_I16: {
                std::int16_t n{};if(nvs_get_i16(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_U32: {
                std::uint32_t n{};if(nvs_get_u32(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_I32: {
                std::int32_t n{};if(nvs_get_i32(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_U64: {
                std::uint64_t n{};if(nvs_get_u64(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_I64: {
                std::int64_t n{};if(nvs_get_i64(h,entry.key,&n)!=ESP_OK)return false;
                size=sizeof(n);std::memcpy(value_.bytes.data(),&n,size);return true;
            }
            case NVS_TYPE_STR:case NVS_TYPE_BLOB: {
                std::size_t wanted=0;
                const auto query=entry.type==NVS_TYPE_STR ?
                    nvs_get_str(h,entry.key,nullptr,&wanted) : nvs_get_blob(h,entry.key,nullptr,&wanted);
                if(query!=ESP_OK || wanted>kMaxValue)return false;
                size=wanted;
                const auto result=entry.type==NVS_TYPE_STR ?
                    nvs_get_str(h,entry.key,reinterpret_cast<char*>(value_.bytes.data()),&size) :
                    nvs_get_blob(h,entry.key,value_.bytes.data(),&size);
                return result==ESP_OK && size==wanted;
            }
            default:return false;
        }
    }
public:
    IdfStore()=default;
    IdfStore(const IdfStore&)=delete;
    IdfStore& operator=(const IdfStore&)=delete;
    ~IdfStore() { if(cfg_)nvs_close(cfg_);if(tesla_)nvs_close(tesla_); }
    bool open_existing() {
        // Do not erase on NO_FREE_PAGES/NEW_VERSION_FOUND, and do not create missing namespaces.
        if(nvs_flash_init()!=ESP_OK)return false;
        if(nvs_open_from_partition(kPartition,nvs_contract::kConfigNamespace,NVS_READONLY,&cfg_)!=ESP_OK)
            return false;
        if(nvs_open_from_partition(kPartition,nvs_contract::kTeslaBleNamespace,NVS_READONLY,&tesla_)!=ESP_OK)
            return false;
        open_=true;return true;
    }
    Read journal(const char* key,bool vin) {
        if(!open_)return Read::Error;
        std::size_t size=0;
        const auto err=vin?nvs_get_str(cfg_,key,nullptr,&size):nvs_get_blob(tesla_,key,nullptr,&size);
        if(err==ESP_ERR_NVS_NOT_FOUND)return Read::Missing;
        return err==ESP_OK?Read::Present:Read::Error; // zero/malformed present also refuses
    }
    Read read_cfg(std::uint8_t* destination,std::size_t cap,std::size_t& size) {
        size=0;if(!open_)return Read::Error;
        std::size_t wanted=0;const auto err=nvs_get_blob(cfg_,nvs_contract::kConfigBlob,nullptr,&wanted);
        if(err==ESP_ERR_NVS_NOT_FOUND)return Read::Missing;
        if(err!=ESP_OK || !wanted || wanted>cap)return Read::Error;
        size=wanted;const auto read=nvs_get_blob(cfg_,nvs_contract::kConfigBlob,destination,&size);
        return read==ESP_OK && size==wanted?Read::Present:Read::Error;
    }
    bool public_identity(BoardId& board,PublicPoint& point) {
        ClearValue wipe{*this};
        if(!open_ || esp_efuse_mac_get_default(board.data())!=ESP_OK)return false;
        std::size_t size=0;
        if(nvs_get_blob(tesla_,nvs_contract::kPrivateKey,nullptr,&size)!=ESP_OK || !size || size>kMaxValue)
            return false;
        const auto wanted=size;
        if(nvs_get_blob(tesla_,nvs_contract::kPrivateKey,value_.bytes.data(),&size)!=ESP_OK || size!=wanted)
            return false;
        if(value_.bytes[size-1]!=0)value_.bytes[size++]=0;
        struct Key {
            mbedtls_pk_context pk;
            mbedtls_svc_key_id_t id=MBEDTLS_SVC_KEY_ID_INIT;
            psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
            Key(){mbedtls_pk_init(&pk);}
            ~Key(){(void)psa_destroy_key(id);psa_reset_key_attributes(&attributes);mbedtls_pk_free(&pk);}
        } key;
        bool okay=false;
        if(mbedtls_pk_parse_key(&key.pk,value_.bytes.data(),size,nullptr,0)==0 &&
           mbedtls_pk_get_psa_attributes(&key.pk,PSA_KEY_USAGE_DERIVE,&key.attributes)==0 &&
           psa_get_key_type(&key.attributes)==PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1) &&
           psa_get_key_bits(&key.attributes)==256) {
            psa_set_key_lifetime(&key.attributes,PSA_KEY_LIFETIME_VOLATILE);
            if(mbedtls_pk_import_into_psa(&key.pk,&key.attributes,&key.id)==0) {
                std::size_t length=0;
                okay=psa_export_public_key(key.id,point.data(),point.size(),&length)==PSA_SUCCESS &&
                     length==point.size() && point[0]==4;
            }
        }
        volatile std::uint8_t* bytes=value_.bytes.data();
        for(std::size_t i=0;i<value_.bytes.size();++i)bytes[i]=0;
        return okay;
    }
    bool retention_digest_excluding_cfg(Digest& result) {
        ClearValue wipe{*this};
        if(!open_)return false;
        Iterator iterator;
        auto err=nvs_entry_find(kPartition,nullptr,NVS_TYPE_ANY,&iterator.value);
        std::size_t count=0;
        while(err==ESP_OK) {
            nvs_entry_info_t info{};
            if(nvs_entry_info(iterator.value,&info)!=ESP_OK)return false;
            if(std::strcmp(info.namespace_name,nvs_contract::kConfigNamespace)!=0 ||
               std::strcmp(info.key,nvs_contract::kConfigBlob)!=0) {
                if(count==kMaxEntries)return false;
                auto& entry=entries_[count++];
                std::memcpy(entry.ns,info.namespace_name,sizeof(entry.ns));
                std::memcpy(entry.key,info.key,sizeof(entry.key));entry.type=info.type;
                if(!std::memchr(entry.ns,0,sizeof(entry.ns)) || !std::memchr(entry.key,0,sizeof(entry.key)))
                    return false;
            }
            err=nvs_entry_next(&iterator.value);
        }
        if(err!=ESP_ERR_NVS_NOT_FOUND)return false;
        std::sort(entries_,entries_+count,[](const Entry& a,const Entry& b){
            const auto n=std::strcmp(a.ns,b.ns);return n<0 || (n==0 && std::strcmp(a.key,b.key)<0);
        });
        Hash hash;
        if(psa_hash_setup(&hash.value,PSA_ALG_SHA_256)!=PSA_SUCCESS || !hash.length(count))return false;
        for(std::size_t i=0;i<count;++i) {
            const auto& entry=entries_[i];
            if(i && std::strcmp(entry.ns,entries_[i-1].ns)==0 &&
                     std::strcmp(entry.key,entries_[i-1].key)==0)return false;
            Handle read;
            if(nvs_open_from_partition(kPartition,entry.ns,NVS_READONLY,&read.value)!=ESP_OK)return false;
            std::size_t size=0;
            if(!read_value(read.value,entry,size))return false;
            const auto type=static_cast<std::uint8_t>(entry.type);
            if(!hash.length(std::strlen(entry.ns)) || !hash.add(entry.ns,std::strlen(entry.ns)) ||
               !hash.length(std::strlen(entry.key)) || !hash.add(entry.key,std::strlen(entry.key)) ||
               !hash.add(&type,1) || !hash.length(size) || !hash.add(value_.bytes.data(),size))return false;
        }
        std::size_t size=0;
        return psa_hash_finish(&hash.value,result.data(),result.size(),&size)==PSA_SUCCESS && size==result.size();
    }
    bool write_cfg_commit(const std::uint8_t* bytes,std::size_t size) {
        if(!open_ || write_used_)return false;
        write_used_=true; // no retry even if set/commit returns false after a durable effect
        Handle write;
        if(nvs_open_from_partition(kPartition,nvs_contract::kConfigNamespace,NVS_READWRITE,&write.value)!=ESP_OK)
            return false;
        if(nvs_set_blob(write.value,nvs_contract::kConfigBlob,bytes,size)!=ESP_OK)return false;
        return nvs_commit(write.value)==ESP_OK;
    }
};
} // namespace tk::repair

#endif // CONFIG_TESLA_CONFIG_REPAIR_USB
