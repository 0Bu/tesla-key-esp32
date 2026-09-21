#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Pure, hardware-free BLE session state machine and session tracker.
// Single source of truth for Tesla BLE session lifecycle according to normative reference:
// teslamotors/vehicle-command pkg/protocol/signer.go (internal/authentication/signer.go:90-135).
//
// Key Characteristics:
// - Verifies SessionInfo HMAC against the outstanding handshake Request-UUID challenge.
// - Monotonic counter alignment: max(local, reported) per signer.go.
//   Preserves higher local counter if vehicle reports lower counter without epoch change,
//   avoiding session stranding.
// - Exact NVS session compatibility: encodes and decodes the exact Signatures.SessionInfo
//   protobuf format stored in NVS under sess_vcsec and sess_info.
// - Session age validation (rejects sessions older than 3600 seconds on NVS load).
// - Self-contained, pure C++17 SHA-256 and HMAC-SHA256 (no external crypto dependencies,
//   host-tested against official Tesla protocol vectors).
namespace tk {

// Standard SHA-256 implementation (FIPS 180-4) in pure C++17
class Sha256 {
public:
    static constexpr size_t kDigestSize = 32;
    static constexpr size_t kBlockSize = 64;

    Sha256() noexcept { reset(); }

    void reset() noexcept {
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
        count_ = 0;
        buffer_len_ = 0;
    }

    void update(const uint8_t* data, size_t len) noexcept {
        if (!data || len == 0) return;
        count_ += len;

        if (buffer_len_ > 0) {
            size_t to_copy = std::min(len, kBlockSize - buffer_len_);
            std::memcpy(buffer_.data() + buffer_len_, data, to_copy);
            buffer_len_ += to_copy;
            data += to_copy;
            len -= to_copy;
            if (buffer_len_ == kBlockSize) {
                transform_(buffer_.data());
                buffer_len_ = 0;
            }
        }

        while (len >= kBlockSize) {
            transform_(data);
            data += kBlockSize;
            len -= kBlockSize;
        }

        if (len > 0) {
            std::memcpy(buffer_.data(), data, len);
            buffer_len_ = len;
        }
    }

    void final(std::array<uint8_t, kDigestSize>& digest) noexcept {
        uint64_t total_bits = count_ * 8;
        // Pad with 0x80, then zeros, then 64-bit big-endian bit count
        uint8_t pad = 0x80;
        update(&pad, 1);

        uint8_t zero = 0;
        while ((count_ % kBlockSize) != 56) {
            update(&zero, 1);
        }

        uint8_t len_bytes[8];
        for (int i = 7; i >= 0; --i) {
            len_bytes[7 - i] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
        }
        update(len_bytes, 8);

        for (int i = 0; i < 8; ++i) {
            digest[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFF);
        }
    }

    static std::array<uint8_t, kDigestSize> hash(const uint8_t* data, size_t len) noexcept {
        Sha256 ctx;
        ctx.update(data, len);
        std::array<uint8_t, kDigestSize> out{};
        ctx.final(out);
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, uint32_t n) noexcept { return (x >> n) | (x << (32 - n)); }
    static uint32_t choose(uint32_t e, uint32_t f, uint32_t g) noexcept { return (e & f) ^ (~e & g); }
    static uint32_t majority(uint32_t a, uint32_t b, uint32_t c) noexcept { return (a & b) ^ (a & c) ^ (b & c); }
    static uint32_t sig0(uint32_t x) noexcept { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    static uint32_t sig1(uint32_t x) noexcept { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    static uint32_t theta0(uint32_t x) noexcept { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    static uint32_t theta1(uint32_t x) noexcept { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    void transform_(const uint8_t* chunk) noexcept {
        static constexpr uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(chunk[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8)  |
                   (static_cast<uint32_t>(chunk[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = theta1(w[i - 2]) + w[i - 7] + theta0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + sig1(e) + choose(e, f, g) + K[i] + w[i];
            uint32_t t2 = sig0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    uint32_t state_[8];
    uint64_t count_{0};
    size_t buffer_len_{0};
    std::array<uint8_t, kBlockSize> buffer_{};
};

// Standard HMAC-SHA256 implementation (RFC 2104)
class HmacSha256 {
public:
    static constexpr size_t kTagSize = 32;

    static std::array<uint8_t, kTagSize> compute(const uint8_t* key, size_t key_len,
                                                 const uint8_t* data, size_t data_len) noexcept {
        std::array<uint8_t, Sha256::kBlockSize> k_pad{};
        if (key_len > Sha256::kBlockSize) {
            auto kh = Sha256::hash(key, key_len);
            std::memcpy(k_pad.data(), kh.data(), kh.size());
        } else if (key && key_len > 0) {
            std::memcpy(k_pad.data(), key, key_len);
        }

        std::array<uint8_t, Sha256::kBlockSize> ipad = k_pad;
        std::array<uint8_t, Sha256::kBlockSize> opad = k_pad;
        for (size_t i = 0; i < Sha256::kBlockSize; ++i) {
            ipad[i] ^= 0x36;
            opad[i] ^= 0x5c;
        }

        Sha256 inner;
        inner.update(ipad.data(), ipad.size());
        inner.update(data, data_len);
        std::array<uint8_t, Sha256::kDigestSize> inner_hash{};
        inner.final(inner_hash);

        Sha256 outer;
        outer.update(opad.data(), opad.size());
        outer.update(inner_hash.data(), inner_hash.size());
        std::array<uint8_t, kTagSize> out_tag{};
        outer.final(out_tag);
        return out_tag;
    }

    static bool constant_time_equals(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
        if (!a || !b) return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < len; ++i) {
            diff |= (a[i] ^ b[i]);
        }
        return diff == 0;
    }
};

// Data model representing the exact Signatures.SessionInfo protobuf message
struct SessionInfoData {
    uint32_t counter{0};
    std::vector<uint8_t> public_key{};
    std::array<uint8_t, 16> epoch{};
    uint32_t clock_time{0};
    uint32_t status{0}; // 0 = OK, 1 = KEY_NOT_ON_WHITELIST
    uint32_t handle{0};
};

// Protobuf wire-type constants
namespace proto {
    inline constexpr uint32_t kWireVarint = 0;
    inline constexpr uint32_t kWireFixed32 = 5;
    inline constexpr uint32_t kWireLengthDelimited = 2;

    inline bool read_varint(const uint8_t*& p, const uint8_t* end, uint64_t& val) noexcept {
        val = 0;
        uint32_t shift = 0;
        while (p < end && shift < 64) {
            uint8_t b = *p++;
            val |= (static_cast<uint64_t>(b & 0x7F) << shift);
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }

    inline void write_varint(std::vector<uint8_t>& buf, uint64_t val) {
        while (val >= 0x80) {
            buf.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
            val >>= 7;
        }
        buf.push_back(static_cast<uint8_t>(val & 0x7F));
    }
} // namespace proto

// Decode Signatures.SessionInfo protobuf bytes (wire compatible with Nanopb)
inline bool decode_session_info(const uint8_t* data, size_t len, SessionInfoData& out) noexcept {
    if (!data || len == 0) return false;
    out = SessionInfoData{};

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    while (p < end) {
        uint64_t key = 0;
        if (!proto::read_varint(p, end, key)) return false;
        uint32_t field_num = static_cast<uint32_t>(key >> 3);
        uint32_t wire_type = static_cast<uint32_t>(key & 0x07);

        switch (field_num) {
            case 1: { // counter (varint)
                if (wire_type != proto::kWireVarint) return false;
                uint64_t v = 0;
                if (!proto::read_varint(p, end, v)) return false;
                out.counter = static_cast<uint32_t>(v);
                break;
            }
            case 2: { // publicKey (bytes)
                if (wire_type != proto::kWireLengthDelimited) return false;
                uint64_t plen = 0;
                if (!proto::read_varint(p, end, plen)) return false;
                if (plen > 128 || p + plen > end) return false;
                out.public_key.assign(p, p + plen);
                p += plen;
                break;
            }
            case 3: { // epoch (bytes, 16 bytes)
                if (wire_type != proto::kWireLengthDelimited) return false;
                uint64_t elen = 0;
                if (!proto::read_varint(p, end, elen)) return false;
                if (elen != 16 || p + 16 > end) return false;
                std::memcpy(out.epoch.data(), p, 16);
                p += 16;
                break;
            }
            case 4: { // clock_time (fixed32)
                if (wire_type != proto::kWireFixed32) return false;
                if (p + 4 > end) return false;
                out.clock_time = static_cast<uint32_t>(p[0]) |
                                 (static_cast<uint32_t>(p[1]) << 8) |
                                 (static_cast<uint32_t>(p[2]) << 16) |
                                 (static_cast<uint32_t>(p[3]) << 24);
                p += 4;
                break;
            }
            case 5: { // status (varint)
                if (wire_type != proto::kWireVarint) return false;
                uint64_t v = 0;
                if (!proto::read_varint(p, end, v)) return false;
                out.status = static_cast<uint32_t>(v);
                break;
            }
            case 6: { // handle (varint)
                if (wire_type != proto::kWireVarint) return false;
                uint64_t v = 0;
                if (!proto::read_varint(p, end, v)) return false;
                out.handle = static_cast<uint32_t>(v);
                break;
            }
            default: { // Skip unrecognized fields
                if (wire_type == proto::kWireVarint) {
                    uint64_t skip = 0;
                    if (!proto::read_varint(p, end, skip)) return false;
                } else if (wire_type == proto::kWireFixed32) {
                    if (p + 4 > end) return false;
                    p += 4;
                } else if (wire_type == proto::kWireLengthDelimited) {
                    uint64_t skip_len = 0;
                    if (!proto::read_varint(p, end, skip_len)) return false;
                    if (p + skip_len > end) return false;
                    p += skip_len;
                } else {
                    return false; // Unsupported wire type
                }
                break;
            }
        }
    }
    return true;
}

// Encode Signatures.SessionInfo protobuf bytes (wire compatible with Nanopb)
inline std::vector<uint8_t> encode_session_info(const SessionInfoData& in) {
    std::vector<uint8_t> out;
    out.reserve(64);

    // Field 1: counter (varint)
    if (in.counter != 0) {
        proto::write_varint(out, (1 << 3) | proto::kWireVarint);
        proto::write_varint(out, in.counter);
    }
    // Field 2: publicKey (bytes)
    if (!in.public_key.empty()) {
        proto::write_varint(out, (2 << 3) | proto::kWireLengthDelimited);
        proto::write_varint(out, in.public_key.size());
        out.insert(out.end(), in.public_key.begin(), in.public_key.end());
    }
    // Field 3: epoch (16 bytes)
    bool has_epoch = false;
    for (uint8_t b : in.epoch) {
        if (b != 0) { has_epoch = true; break; }
    }
    if (has_epoch) {
        proto::write_varint(out, (3 << 3) | proto::kWireLengthDelimited);
        proto::write_varint(out, 16);
        out.insert(out.end(), in.epoch.begin(), in.epoch.end());
    }
    // Field 4: clock_time (fixed32)
    if (in.clock_time != 0) {
        proto::write_varint(out, (4 << 3) | proto::kWireFixed32);
        out.push_back(static_cast<uint8_t>(in.clock_time & 0xFF));
        out.push_back(static_cast<uint8_t>((in.clock_time >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((in.clock_time >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((in.clock_time >> 24) & 0xFF));
    }
    // Field 5: status (varint)
    if (in.status != 0) {
        proto::write_varint(out, (5 << 3) | proto::kWireVarint);
        proto::write_varint(out, in.status);
    }
    // Field 6: handle (varint)
    if (in.handle != 0) {
        proto::write_varint(out, (6 << 3) | proto::kWireVarint);
        proto::write_varint(out, in.handle);
    }
    return out;
}

enum class SessionState : uint8_t {
    Unauthenticated,
    Authenticating,
    Established,
    Invalid,
};

enum class SessionUpdateResult : uint8_t {
    Ok,
    UuidMismatch,
    InvalidHmac,
    KeyNotOnWhitelist,
    StatusError,
    StaleClockTime,
    DecodeError,
};

// Pure-logic session tracker managing handshake, counter progression, and NVS persistence
class SessionTracker {
public:
    static constexpr uint32_t kDefaultMaxSessionAgeS = 3600;

    SessionTracker() noexcept = default;

    void reset() noexcept {
        state_ = SessionState::Unauthenticated;
        epoch_.fill(0);
        counter_ = 0;
        clock_time_ = 0;
        handshake_uuid_.fill(0);
        public_key_.clear();
        has_handshake_ = false;
    }

    // Start handshake: records challenge UUID and transitions to Authenticating
    void start_handshake(const std::array<uint8_t, 16>& request_uuid) noexcept {
        handshake_uuid_ = request_uuid;
        has_handshake_ = true;
        state_ = SessionState::Authenticating;
    }

    [[nodiscard]] bool has_pending_handshake() const noexcept { return has_handshake_; }
    [[nodiscard]] const std::array<uint8_t, 16>& handshake_uuid() const noexcept { return handshake_uuid_; }

    // Update session from an incoming SessionInfo message.
    // - incoming_uuid: UUID in response (must match handshake_uuid).
    // - encoded_info / info_len: serialized SessionInfo protobuf bytes.
    // - tag / tag_len: session info HMAC tag from SignatureData (must be 32 bytes).
    // - session_info_key / key_len: derived HMAC key (EXPECTED_SESSION_INFO_KEY).
    SessionUpdateResult update_session(const uint8_t* incoming_uuid, size_t uuid_len,
                                      const uint8_t* encoded_info, size_t info_len,
                                      const uint8_t* tag, size_t tag_len,
                                      const uint8_t* session_info_key, size_t key_len) {
        // 1. UUID challenge verification (must match active handshake request UUID)
        if (has_handshake_) {
            if (uuid_len != 16 || incoming_uuid == nullptr ||
                std::memcmp(incoming_uuid, handshake_uuid_.data(), 16) != 0) {
                return SessionUpdateResult::UuidMismatch;
            }
        }

        // 2. HMAC-SHA256 validation: HMAC(session_info_key, challenge || encoded_info)
        const bool has_key = (session_info_key != nullptr && key_len > 0);
        const bool has_tag = (tag != nullptr && tag_len > 0);

        if (has_key) {
            // Key is provided: HMAC tag is required and must match exactly
            if (!has_tag || tag_len != HmacSha256::kTagSize) {
                return SessionUpdateResult::InvalidHmac;
            }
            std::vector<uint8_t> hmac_input;
            hmac_input.reserve((incoming_uuid ? uuid_len : 0) + info_len);
            if (incoming_uuid && uuid_len > 0) {
                hmac_input.insert(hmac_input.end(), incoming_uuid, incoming_uuid + uuid_len);
            }
            if (encoded_info && info_len > 0) {
                hmac_input.insert(hmac_input.end(), encoded_info, encoded_info + info_len);
            }

            auto expected_tag = HmacSha256::compute(session_info_key, key_len,
                                                   hmac_input.data(), hmac_input.size());
            if (!HmacSha256::constant_time_equals(expected_tag.data(), tag, HmacSha256::kTagSize)) {
                return SessionUpdateResult::InvalidHmac;
            }
        } else if (has_tag) {
            // Tag was provided but no key available to verify it: fail closed
            return SessionUpdateResult::InvalidHmac;
        }

        // 3. Protobuf decode
        SessionInfoData info{};
        if (!decode_session_info(encoded_info, info_len, info)) {
            return SessionUpdateResult::DecodeError;
        }

        // 4. Status check
        if (info.status == 1) { // KEY_NOT_ON_WHITELIST
            reset();
            return SessionUpdateResult::KeyNotOnWhitelist;
        }
        if (info.status != 0) {
            state_ = SessionState::Invalid;
            return SessionUpdateResult::StatusError;
        }

        // 5. Monotonic counter alignment & epoch progression (signer.go:103-107 & patch 0005)
        bool epoch_changed = (info.epoch != epoch_);
        bool time_advanced = (info.clock_time >= clock_time_);

        if (epoch_changed) {
            // Epoch changed: vehicle established a new session epoch.
            // Adopt vehicle's starting counter for this new epoch.
            counter_ = info.counter;
            epoch_ = info.epoch;
            clock_time_ = info.clock_time;
            if (!info.public_key.empty()) {
                public_key_ = info.public_key;
            }
            state_ = SessionState::Established;
            has_handshake_ = false;
            return SessionUpdateResult::Ok;
        }

        if (time_advanced) {
            // Same epoch: vehicle clock advanced (or equal).
            // Monotonic counter progression per signer.go: max(local, reported).
            // Preserves local counter if vehicle reported lower counter without epoch change.
            counter_ = std::max(counter_, info.counter);
            clock_time_ = info.clock_time;
            if (!info.public_key.empty()) {
                public_key_ = info.public_key;
            }
            state_ = SessionState::Established;
            has_handshake_ = false;
            return SessionUpdateResult::Ok;
        }

        // Epoch unchanged and vehicle clock went backwards: stale response
        return SessionUpdateResult::StaleClockTime;
    }

    // Allocate next TX counter for an outgoing command. Increments monotonically.
    bool next_tx_counter(uint32_t& out_counter) noexcept {
        if (state_ != SessionState::Established) return false;
        if (counter_ == 0xFFFFFFFF) return false; // Rollover rejection per signer.go:171
        counter_++;
        out_counter = counter_;
        return true;
    }

    // Direct setter for session state (used when peer.cpp manages crypto directly)
    void set_established(const std::array<uint8_t, 16>& epoch, uint32_t counter, uint32_t clock_time) noexcept {
        epoch_ = epoch;
        counter_ = counter;
        clock_time_ = clock_time;
        state_ = SessionState::Established;
        has_handshake_ = false;
    }

    // Export session state for durable NVS storage (exact protobuf format)
    [[nodiscard]] std::vector<uint8_t> export_for_nvs() const {
        SessionInfoData d{};
        d.counter = counter_;
        d.public_key = public_key_;
        d.epoch = epoch_;
        d.clock_time = clock_time_;
        d.status = 0;
        return encode_session_info(d);
    }

    // Import session from NVS storage with staleness check (age > max_age_s rejected)
    bool import_from_nvs(const uint8_t* data, size_t len,
                         uint32_t current_time_s = 0,
                         uint32_t max_age_s = kDefaultMaxSessionAgeS) {
        if (!data || len == 0) return false;

        SessionInfoData info{};
        if (!decode_session_info(data, len, info)) return false;
        if (info.status != 0) return false;

        // Session age validation (vehicle.cpp:1165: reject if > 3600s old)
        if (current_time_s > 0 && info.clock_time > 0) {
            int64_t age = static_cast<int64_t>(current_time_s) - static_cast<int64_t>(info.clock_time);
            if (age > static_cast<int64_t>(max_age_s)) {
                return false; // Stale session
            }
        }

        epoch_ = info.epoch;
        counter_ = info.counter;
        clock_time_ = info.clock_time;
        public_key_ = info.public_key;
        state_ = SessionState::Established;
        has_handshake_ = false;
        return true;
    }

    [[nodiscard]] bool is_authenticated() const noexcept { return state_ == SessionState::Established; }
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] uint32_t counter() const noexcept { return counter_; }
    [[nodiscard]] uint32_t clock_time() const noexcept { return clock_time_; }
    [[nodiscard]] const std::array<uint8_t, 16>& epoch() const noexcept { return epoch_; }

private:
    SessionState state_{SessionState::Unauthenticated};
    std::array<uint8_t, 16> epoch_{};
    uint32_t counter_{0};
    uint32_t clock_time_{0};
    std::vector<uint8_t> public_key_{};
    std::array<uint8_t, 16> handshake_uuid_{};
    bool has_handshake_{false};
};

using BleSessionTracker = SessionTracker;

}  // namespace tk
