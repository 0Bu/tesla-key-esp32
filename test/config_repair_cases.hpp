#pragma once
// Synthetic logical-store fixture for the actual pure transaction core. No real NVS/key material.
#include "logic/config_repair.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace repair_fixture {
using namespace tk::repair;
struct Owner { bool sealed{}; void seal() noexcept {sealed=true;} };
inline tk::ConfigBlob replacement() {
    tk::ConfigBlob in;in.vin="5YJ3E1EA7KF000316";in.wifi_ssid="fixture-network";
    in.wifi_pass="fixture-password";in.has_ota=true;in.ota_channel=0;return in;
}
inline std::vector<std::uint8_t> encoded(const tk::ConfigBlob& in) {
    tk::ConfigBlobBuffer bytes{};const auto size=tk::config_blob_encode(in,bytes.data(),bytes.size());
    CHECK(size);return {bytes.begin(),bytes.begin()+size};
}
struct Store {
    Owner& owner;
    std::vector<std::uint8_t> cfg=encoded(replacement());
    unsigned reads{},digests{},writes{};
    int fault{};
    bool retained=true;
    explicit Store(Owner& in):owner(in){cfg.back()^=1;}
    Read read_cfg(std::uint8_t* to,std::size_t cap,std::size_t& size) {
        ++reads;
        if(fault==1 || (writes && fault==5))return Read::Error;
        if(fault==2)return Read::Missing;
        if(writes && fault==6){size=cfg.size()-1;std::copy_n(cfg.data(),size,to);return Read::Present;}
        size=cfg.size();if(size>cap)return Read::Error;
        std::copy(cfg.begin(),cfg.end(),to);return Read::Present;
    }
    bool retention_digest_excluding_cfg(Digest& out) {
        ++digests;
        if(fault==3 || (writes && fault==7))return false;
        out.fill(0);out[0]=retained?19:20;return true;
    }
    bool write_cfg_commit(const std::uint8_t* from,std::size_t size) {
        CHECK(owner.sealed);++writes;CHECK(writes==1);
        if(fault==4) return false; // may or may not have landed: caller may not infer either
        cfg.assign(from,from+size);
        if(fault==8){retained=false;return true;}
        if(fault==9)return false; // false although bytes became visible
        if(fault==10)throw std::runtime_error("synthetic post-effect fault");
        return true;
    }
};
inline Request request() {
    Request request;request.owner_confirmed_known_identity=true;
    request.expected_board[0]=2;request.expected_public_key[0]=4;request.expected_public_key[1]=1;
    request.challenge[0]=7;const auto in=replacement();
    std::copy(in.vin.begin(),in.vin.end(),request.known_vin.begin());
    const auto data=encoded(in);request.replacement_size=data.size();
    std::copy(data.begin(),data.end(),request.replacement.begin());return request;
}
inline Evidence evidence(const Request& request) {
    Evidence in;in.explicitly_selected_maintenance=in.before_normal_writers=in.actual_restart_owner=true;
    in.raw_image_stable=in.storage_authoritative=in.identity_valid=true;
    in.board=request.expected_board;in.public_key=request.expected_public_key;in.challenge=request.challenge;
    in.vin_journal=in.key_journal=Read::Missing;return in;
}
inline void cases() {
    using tk::repair::Outcome;
    const auto original=request();
    for(unsigned fault=0;fault!=15;++fault) {
        auto input=original;auto facts=evidence(input);Owner owner;Store store(owner);Attempt once;
        switch(fault) {
            case 0:input.owner_confirmed_known_identity=false;break;
            case 1:facts.explicitly_selected_maintenance=false;break;
            case 2:facts.before_normal_writers=false;break;
            case 3:facts.actual_restart_owner=false;break;
            case 4:facts.raw_image_stable=false;break;
            case 5:facts.storage_authoritative=false;break;
            case 6:facts.identity_valid=false;break;
            case 7:facts.vin_journal=Read::Present;break;
            case 8:facts.vin_journal=Read::Error;break;
            case 9:facts.key_journal=Read::Present;break;
            case 10:facts.key_journal=Read::Error;break;
            case 11:input.expected_board[0]^=1;break;
            case 12:input.expected_public_key[1]^=1;break;
            case 13:input.challenge[0]^=1;break;
            case 14:input.known_vin[16]='7';break;
        }
        const auto result=repair_once(once,store,owner,input,facts);
        CHECK(result.outcome==Outcome::Refused && !result.write_attempted);
        CHECK(store.writes==0 && !owner.sealed);
        CHECK(repair_once(once,store,owner,original,evidence(original)).outcome==Outcome::AlreadyTerminal);
    }
    for(int fault=0;fault!=11;++fault) {
        Owner owner;Store store(owner);store.fault=fault;Attempt once;
        const auto result=repair_once(once,store,owner,original,evidence(original));
        if(fault==0) {
            CHECK(result.outcome==Outcome::Confirmed && result.write_attempted && result.commit_confirmed &&
                  result.readback_confirmed && result.retention_confirmed);
            CHECK(store.cfg==encoded(replacement()) && store.writes==1 && owner.sealed);
        } else if(fault<=3) CHECK(result.outcome==Outcome::Refused && store.writes==0 && !owner.sealed);
        else CHECK(result.outcome==Outcome::Ambiguous && result.write_attempted && store.writes==1 && owner.sealed);
        CHECK(repair_once(once,store,owner,original,evidence(original)).outcome==Outcome::AlreadyTerminal);
    }
    for(unsigned kind=0;kind!=5;++kind) {
        Owner owner;Store store(owner);Attempt once;
        switch(kind) {
            case 0:store.cfg=encoded(replacement());break; // valid ordinary config is not a repair case
            case 1:store.cfg[4]=2;break;                  // unknown/newer schema, even with invalid CRC
            case 2:store.cfg[0]='X';break;
            case 3:store.cfg.resize(4);break;             // no recognized header authority
            case 4:store.cfg.resize(kMaxBlob+1);break;
        }
        CHECK(repair_once(once,store,owner,original,evidence(original)).outcome==Outcome::Refused);
        CHECK(store.writes==0 && !owner.sealed);
    }
    CHECK(classify_blob(Read::Present,original.replacement.data(),7)==BlobClass::RepairableCurrentCorruption);
    auto data=encoded(replacement());data.resize(data.size()-1);
    CHECK(classify_blob(Read::Present,data.data(),data.size())==BlobClass::RepairableCurrentCorruption);
    for(unsigned field=0;field!=8;++field) {
        auto cfg=replacement();
        switch(field) {
            case 0:cfg.vin="5YJ3E1EA7KF000317";break;
            case 1:cfg.wifi_rollback_active=true;break;
            case 2:cfg.wifi_rolled_back=true;break;
            case 3:cfg.wifi_ssid_backup="old";break;
            case 4:cfg.wifi_pass_backup="old-password";break;
            case 5:cfg.has_ota=false;break;
            case 6:cfg.ota_channel=2;break;
            case 7:cfg.mqtt_uri="user:secret@invalid";break;
        }
        CHECK(!replacement_is_complete(cfg,"5YJ3E1EA7KF000316"));
    }
    auto cfg=replacement();cfg.wifi_ssid.clear();cfg.wifi_pass.clear();
    CHECK(replacement_is_complete(cfg,"5YJ3E1EA7KF000316")); // intentionally unconfigured
    cfg.wifi_pass="retained-password";CHECK(!replacement_is_complete(cfg,"5YJ3E1EA7KF000316"));
    // Framing is exact and does not publish a partial request on malformed length/checksum.
    Request untouched=original;std::uint8_t malformed[8]{};
    CHECK(!decode_request(malformed,sizeof(malformed),untouched));
    CHECK(untouched.replacement_size==original.replacement_size && untouched.expected_board==original.expected_board);
}
} // namespace repair_fixture
