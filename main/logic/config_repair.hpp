#pragma once
// Pure, hardware-free maintenance decision/transaction core. Never used by ordinary startup.
// Host owner proposed in test/logic_test_ownership.json: test_config_repair().
// The separate maintenance shell supplies raw NVS reads, identity derivation and owner admission.
#include "config_store.hpp"
#include "vin.hpp"
#include "wifi_credentials.hpp"
#include "mqtt_uri.hpp"
#include "syslog_policy.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace tk::repair {
using Digest = std::array<std::uint8_t, 32>;
using PublicPoint = std::array<std::uint8_t, 65>;
using BoardId = std::array<std::uint8_t, 6>;
using Nonce = std::array<std::uint8_t, 32>;
inline constexpr std::size_t kMaxBlob = kConfigBlobCapBytes;

enum class Read { Missing, Present, Error };
enum class BlobClass { RepairableCurrentCorruption, Valid, Missing, Unreadable, ForeignOrUnknown };
inline BlobClass classify_blob(Read state, const std::uint8_t* bytes, std::size_t size) {
    if (state == Read::Missing) return BlobClass::Missing;
    if (state != Read::Present) return BlobClass::Unreadable;
    // Headerless truncation, unknown magic/version or over-limit data has no safe schema authority.
    if (!bytes || size < detail::kBlobHeaderBytes || size > kMaxBlob ||
        bytes[0] != kConfigBlobMagic0 || bytes[1] != kConfigBlobMagic1 ||
        bytes[2] != kConfigBlobMagic2 || bytes[3] != kConfigBlobMagic3 ||
        bytes[4] != kConfigBlobVersion) return BlobClass::ForeignOrUnknown;
    ConfigBlob decoded;
    return config_blob_decode(bytes, size, decoded) ? BlobClass::Valid
                                                  : BlobClass::RepairableCurrentCorruption;
}
inline bool text_without_control(const std::string& text) noexcept {
    for (unsigned char c : text) if (c < 0x20 || c == 0x7f) return false;
    return true;
}
inline bool replacement_is_complete(const ConfigBlob& in, std::string_view owner_known_vin) {
    // The complete replacement retires the unreadable WiFi trial explicitly; no field is guessed
    // from damaged bytes or legacy mirrors. Empty WiFi means intentionally unconfigured.
    if (!vin_is_plausible(std::string(owner_known_vin)) || in.vin != owner_known_vin ||
        !in.wifi_ssid_backup.empty() || !in.wifi_pass_backup.empty() ||
        in.wifi_rollback_active || in.wifi_rolled_back || !in.has_ota ||
        (in.ota_channel != 0 && in.ota_channel != 1)) return false;
    if (in.wifi_ssid.empty() ? !in.wifi_pass.empty() :
        wifi_credentials_error(in.wifi_ssid, in.wifi_pass) != WifiCredentialError::None) return false;
    if (!text_without_control(in.wifi_ssid) || !text_without_control(in.wifi_pass) ||
        !text_without_control(in.mqtt_uri) || !text_without_control(in.syslog_uri) ||
        in.mqtt_uri.size() > kServiceTargetInputMaxBytes ||
        in.syslog_uri.size() > kServiceTargetInputMaxBytes) return false;
    return mqtt_broker_is_plausible(in.mqtt_uri) && syslog_target_is_plausible(in.syslog_uri);
}
struct Request {
    bool owner_confirmed_known_identity{};
    BoardId expected_board{};
    PublicPoint expected_public_key{}; // full independently known P-256 point, never a short ID
    Nonce challenge{};
    std::array<char, 17> known_vin{};
    std::array<std::uint8_t, kMaxBlob> replacement{};
    std::size_t replacement_size{};
};
inline constexpr std::size_t kFramePrefix=5+2;
inline constexpr std::size_t kFrameFixed=kFramePrefix+1+6+65+32+17+2+4;
inline constexpr std::size_t kMaxFrame=kFrameFixed+kMaxBlob;
inline void clear_request(Request& request) noexcept {
    volatile std::uint8_t* bytes=reinterpret_cast<volatile std::uint8_t*>(&request);
    for(std::size_t i=0;i<sizeof(request);++i)bytes[i]=0;
}
inline bool decode_request(const std::uint8_t* frame,std::size_t size,Request& out) noexcept {
    if(!frame || size<kFrameFixed || size>kMaxFrame || std::memcmp(frame,"TKAP1",5)!=0 ||
       (std::size_t(frame[5])|(std::size_t(frame[6])<<8))!=size)return false;
    const auto wanted=std::uint32_t(frame[size-4])|(std::uint32_t(frame[size-3])<<8)|
        (std::uint32_t(frame[size-2])<<16)|(std::uint32_t(frame[size-1])<<24);
    if(config_crc32(frame,size-4)!=wanted || frame[7]!=1)return false;
    std::size_t pos=8;
    Request staged;staged.owner_confirmed_known_identity=true;
    std::memcpy(staged.expected_board.data(),frame+pos,6);pos+=6;
    std::memcpy(staged.expected_public_key.data(),frame+pos,65);pos+=65;
    std::memcpy(staged.challenge.data(),frame+pos,32);pos+=32;
    std::memcpy(staged.known_vin.data(),frame+pos,17);pos+=17;
    const auto length=std::size_t(frame[pos])|(std::size_t(frame[pos+1])<<8);pos+=2;
    if(length>kMaxBlob || pos+length+4!=size)return false;
    staged.replacement_size=length;
    std::memcpy(staged.replacement.data(),frame+pos,length);
    out=staged;clear_request(staged);return true;
}
struct Evidence {
    bool explicitly_selected_maintenance{};
    bool before_normal_writers{};
    bool actual_restart_owner{};
    bool raw_image_stable{};
    bool storage_authoritative{};
    bool identity_valid{};
    BoardId board{};
    PublicPoint public_key{};
    Nonce challenge{};
    Read vin_journal{Read::Error};
    Read key_journal{Read::Error};
};
inline bool authority_matches(const Request& request, const Evidence& evidence) noexcept {
    return request.owner_confirmed_known_identity && evidence.explicitly_selected_maintenance &&
        evidence.before_normal_writers && evidence.actual_restart_owner &&
        evidence.raw_image_stable && evidence.storage_authoritative && evidence.identity_valid &&
        evidence.vin_journal == Read::Missing && evidence.key_journal == Read::Missing &&
        request.expected_board == evidence.board && request.expected_public_key[0] == 4 &&
        request.expected_public_key == evidence.public_key && request.challenge == evidence.challenge;
}
enum class Outcome { Refused, Confirmed, Ambiguous, AlreadyTerminal };
struct Receipt {
    Outcome outcome{Outcome::Refused};
    bool write_attempted{};
    bool commit_confirmed{};
    bool readback_confirmed{};
    bool retention_confirmed{};
};
class Attempt {
public:
    bool begin() noexcept { if (terminal_) return false; terminal_ = true; return true; }
private:
    bool terminal_{}; // one APPLY, even a refusal, per explicitly selected maintenance boot
};
template <std::size_t Size> struct Wiped {
    std::array<std::uint8_t, Size> bytes{};
    ~Wiped() { volatile std::uint8_t* p = bytes.data(); for (std::size_t i=0;i<Size;++i) p[i]=0; }
};
// Store supplies only read_cfg, retention_digest_excluding_cfg and one write_cfg_commit operation.
// Owner supplies a real held lease's seal(); no API here can generate a key, clear a peer,
// delete/restore NVS, update mirrors, confirm an image, restart or dispatch a vehicle command.
template <typename Store, typename Owner>
Receipt repair_once(Attempt& once, Store& store, Owner& owner,
                    const Request& request, const Evidence& evidence) noexcept {
    Receipt receipt;
    if (!once.begin()) { receipt.outcome=Outcome::AlreadyTerminal; return receipt; }
    try {
        if (!authority_matches(request,evidence) || request.replacement_size > kMaxBlob) return receipt;
        ConfigBlob replacement;
        if (!config_blob_decode(request.replacement.data(),request.replacement_size,replacement) ||
            !replacement_is_complete(replacement,
                std::string_view(request.known_vin.data(),request.known_vin.size()))) return receipt;
        Wiped<kMaxBlob> canonical;
        const auto canonical_size=config_blob_encode(replacement,canonical.bytes.data(),canonical.bytes.size());
        if (!canonical_size || canonical_size!=request.replacement_size ||
            std::memcmp(canonical.bytes.data(),request.replacement.data(),canonical_size)!=0) return receipt;
        Wiped<kMaxBlob> current;
        std::size_t current_size=0;
        const auto raw=store.read_cfg(current.bytes.data(),current.bytes.size(),current_size);
        if (classify_blob(raw,current.bytes.data(),current_size)!=BlobClass::RepairableCurrentCorruption)
            return receipt;
        Digest before{},after{};
        if (!store.retention_digest_excluding_cfg(before)) return receipt;
        // Consume the effect boundary BEFORE even nvs_set_blob. Any failure may have landed data;
        // the lease remains sealed and the maintenance caller must park on every outcome.
        owner.seal();
        receipt.write_attempted=true;
        if (!store.write_cfg_commit(canonical.bytes.data(),canonical_size)) {
            receipt.outcome=Outcome::Ambiguous; return receipt;
        }
        receipt.commit_confirmed=true;
        Wiped<kMaxBlob> actual;
        std::size_t actual_size=0;
        if (store.read_cfg(actual.bytes.data(),actual.bytes.size(),actual_size)!=Read::Present ||
            actual_size!=canonical_size ||
            std::memcmp(actual.bytes.data(),canonical.bytes.data(),actual_size)!=0) {
            receipt.outcome=Outcome::Ambiguous; return receipt;
        }
        ConfigBlob decoded;
        if (!config_blob_decode(actual.bytes.data(),actual_size,decoded)) {
            receipt.outcome=Outcome::Ambiguous; return receipt;
        }
        receipt.readback_confirmed=true;
        if (!store.retention_digest_excluding_cfg(after) || before!=after) {
            receipt.outcome=Outcome::Ambiguous; return receipt;
        }
        receipt.retention_confirmed=true;
        receipt.outcome=Outcome::Confirmed;
    } catch (...) {
        receipt.outcome=receipt.write_attempted ? Outcome::Ambiguous : Outcome::Refused;
    }
    return receipt;
}
} // namespace tk::repair
