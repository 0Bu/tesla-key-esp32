#pragma once
// Credential/service configuration in one versioned CRC-checked NVS blob. Encoding/decoding is
// IDF-free; config_blob.cpp owns the NVS adapter and save order. cfg_save commits the blob before
// best-effort legacy mirrors. A failed commit does not prove which snapshot is durable; callers
// must not infer an intact previous configuration from a false result.
//
// Only exact blob absence permits the legacy per-key/default load. Read, allocation, CRC or schema
// errors fail closed without stale-mirror fallback. Ordinary saves preserve the WiFi rollback
// credentials and flags in the same snapshot as the current credentials.
//
// HTTP/provisioning and boot rollback own coherent credential/service snapshots. Other records
// remain separate because their writer or recovery lifetime differs: BLE discovery owns ble_mac;
// clock paths own last_time; reboot producers own reboot_why; the display owns disp_rot; vin_txn
// and boot_fails are recovery journals/latches. last_time is only a restored wall-clock estimate,
// not proof that vehicle-relative sessions can be reused. The exact registry is nvs_contract.hpp.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include "nvs_contract.hpp"
#include "service_target_limits.hpp"

namespace tk {

// CRC-32/ISO-HDLC (reflected, poly 0xEDB88320) — the usual zlib/PNG CRC. Self-contained and
// table-free so the blob's integrity check is host-testable rather than relying on
// esp_rom_crc32, and so it costs no .rodata table on a device this size.
// Known-answer: config_crc32("123456789", 9) == 0xCBF43926.
inline uint32_t config_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));   // mask = -(crc & 1), branchless
    }
    return crc ^ 0xFFFFFFFFu;
}

// The credential/service fields persisted as ONE atomic blob. NOT `ble_mac`, `last_time`,
// `reboot_why` or `disp_rot` — see the ownership note in the header block.
struct ConfigBlob {
    // WiFi credentials, as the setup portal and the STA path use them.
    std::string wifi_ssid;
    std::string wifi_pass;
    // The ONE-SHOT rollback backup: the credentials that were live BEFORE the current save.
    // Same blob as the pair above on purpose — that is the ordering problem this header exists
    // to delete.
    std::string wifi_ssid_backup;
    std::string wifi_pass_backup;
    // Is a rollback armed? Stored explicitly rather than inferred from "the backup SSID is not
    // empty", because those are two different states an empty backup cannot tell apart: a device
    // whose FIRST credentials were just entered through the setup AP has nothing to go back to
    // (nothing armed), while a deliberate rollback TO the unconfigured state is a real one-shot
    // that must survive as such. Inferring intent from the shape of the data would collapse them.
    bool wifi_rollback_active = false;
    // Did the last rollback actually FIRE? The outcome marker, and the only trace of it: a
    // rollback reboots, after which the live SSID is just the old one again and nothing else in
    // the system remembers that a save was undone. Sticky until the next credential save.
    bool wifi_rolled_back = false;
    // The vehicle the device is paired to. 17 chars when set, empty when it has never been
    // configured. Validation (tk::vin_is_plausible, logic/vin.hpp) belongs to the caller — this
    // header stores what it is handed and never normalises it, so a blob round-trip cannot
    // silently change a VIN.
    std::string vin;
    // The HA MQTT bridge broker ("host:port"; empty disables) and the syslog collector
    // ("host:port"; empty disables). Both keep their empty-means-off meaning through the blob:
    // an empty string is a stored value here, never an absent field.
    std::string mqtt_uri;
    std::string syslog_uri;
    // The OTA update channel (0 = release, 1 = dev), packed in reserved flags bits 2 and 3.
    // Here for the same reason: ONE writer (the httpd task, POST /set_ota). Which feed a device
    // follows is a persistent user choice — a board put on the dev channel must still be on it after a reboot.
    int32_t ota_channel = 0;
    // FALSE when not explicitly configured (flags bit 2 unset).
    bool has_ota = false;
};

// Commit credentials typed into the recovery setup form. They replace the live pair and, by
// design, leave nothing to go back to, so the previous one-shot rollback state (armed bit, backup
// pair, last verdict) is retired with them. Carrying it forward would let a later failure of the
// FRESH credentials restore an unrelated older network, and would report an old rollback verdict
// against the new attempt.
inline void config_apply_setup_wifi(ConfigBlob& cfg, const std::string& ssid,
                                    const std::string& pass) {
    cfg.wifi_ssid = ssid;
    cfg.wifi_pass = pass;
    cfg.wifi_ssid_backup.clear();
    cfg.wifi_pass_backup.clear();
    cfg.wifi_rollback_active = false;
    cfg.wifi_rolled_back = false;
}

// The NVS key inside the `tesla_cfg` namespace is owned by the exact persistence registry.
inline constexpr const char* kConfigBlobKey = nvs_contract::kConfigBlob;

// Magic, so a foreign or garbled entry is rejected before its length fields are ever trusted.
inline constexpr uint8_t kConfigBlobMagic0 = 'T';
inline constexpr uint8_t kConfigBlobMagic1 = 'K';
inline constexpr uint8_t kConfigBlobMagic2 = 'C';
inline constexpr uint8_t kConfigBlobMagic3 = '1';

// Version1 is the current wire layout, including the explicit OTA-channel reserved-bit encoding.
// The reader accepts only the stated version interval and fails closed on an unknown version; it
// does not overwrite the blob or recover from potentially stale mirrors. Any future layout/version
// change needs an explicit upgrade and downgrade/recovery plan. Legacy mirrors are best-effort,
// so their existence cannot establish the authoritative blob's contents.
inline constexpr uint8_t kConfigBlobVersion    = 1;
inline constexpr uint8_t kConfigBlobVersionMin = 1;

// ── FIELD BOUNDS ──────────────────────────────────────────────────────────────────────────────
// What each field can legitimately hold on this device. These are enforced on the WRITE side
// (config_blob_encode refuses an over-long field) before cfg_save attempts a blob write. The decoder has a separate compatibility bound.
inline constexpr size_t kConfigMaxSsidLen     = 32;   // IEEE 802.11 SSID is at most 32 octets
inline constexpr size_t kConfigMaxWifiPassLen = 64;   // WPA2 passphrase 8..63, or a 64-hex PSK
inline constexpr size_t kConfigMaxVinLen      = 17;   // a VIN is exactly 17 (empty = unset)
inline constexpr size_t kConfigMaxUriLen = kServiceTargetPreserveMaxBytes;

// An older v1 writer cannot encode service fields above64. Refuse NEW LAN WiFi trials until
// explicit service edits fit that writer; the preserved bytes are not silently shortened.
inline bool config_service_targets_fit_old_writer(const ConfigBlob& config) noexcept {
    return config.mqtt_uri.size() <= kServiceTargetInputMaxBytes &&
           config.syslog_uri.size() <= kServiceTargetInputMaxBytes;
}

// The DECODE-side sanity bound on a stored string length, deliberately looser than the per-field
// bounds above and deliberately not per-field. A reader stricter than the writer is a way to
// turn a perfectly good stored configuration into a wiped one, so the reader's job here is only
// to refuse corruption: a length that does not fit the remaining body is already rejected by the
// bounds arithmetic, and this catches a garbled length that happens to fit.
inline constexpr size_t kConfigBlobDecodeMaxStr = 256;
static_assert(kServiceTargetInputMaxBytes <= kConfigMaxUriLen &&
              kConfigMaxUriLen <= kConfigBlobDecodeMaxStr);

namespace detail {
// magic(4) + version(1); the CRC is a fixed 4-byte trailer.
inline constexpr size_t kBlobHeaderBytes = 5;
inline constexpr size_t kBlobCrcBytes    = 4;
// A string field on the wire: a 2-byte little-endian length plus that many raw bytes.
inline constexpr size_t blob_str_bytes(size_t max_len) { return 2 + max_len; }
}  // namespace detail

// The largest blob this build can produce, with every field at its bound. Stated here so the
// caller's buffer is sized from the layout instead of from a guess, and so the blob cannot
// silently outgrow it: adding a field without raising the cap is a COMPILE error, not a runtime
// truncation discovered on a device.
inline constexpr size_t kConfigBlobEncodedMax =
    detail::kBlobHeaderBytes +
    detail::blob_str_bytes(kConfigMaxSsidLen) +        // wifi_ssid
    detail::blob_str_bytes(kConfigMaxWifiPassLen) +    // wifi_pass
    detail::blob_str_bytes(kConfigMaxSsidLen) +        // wifi_ssid_backup
    detail::blob_str_bytes(kConfigMaxWifiPassLen) +    // wifi_pass_backup
    1 +                                                // flags
    detail::blob_str_bytes(kConfigMaxVinLen) +         // vin
    detail::blob_str_bytes(kConfigMaxUriLen) +         // mqtt_uri
    detail::blob_str_bytes(kConfigMaxUriLen) +         // syslog_uri
    detail::kBlobCrcBytes;

// cfg_save owns one private static scratch buffer through a nonblocking lease; pure codec
// callers may supply their own array. No serialized-output heap allocation is needed.
inline constexpr size_t kConfigBlobCapBytes = 512;
static_assert(kConfigBlobEncodedMax == 473, "review the exact maximum when field bounds change");
static_assert(kConfigBlobEncodedMax <= kConfigBlobCapBytes, "config encoding exceeds its buffer");
using ConfigBlobBuffer = std::array<uint8_t, kConfigBlobCapBytes>;

namespace detail {
// A bounds-checked forward cursor. Every put() is a no-op once the buffer has overflowed or a
// field has been refused, so the caller checks `ok` ONCE at the end instead of at every step —
// a per-step check that is easy to forget is how a truncated blob would get written.
struct BlobWriter {
    uint8_t* out;
    size_t   cap;
    size_t   n{0};
    bool     ok{true};

    void put_u8(uint8_t x) {
        if (!ok) return;
        if (n >= cap) { ok = false; return; }
        out[n++] = x;
    }
    // Explicit little-endian byte packing with masks, rather than a memcpy of the object
    // representation: this is a wire format read back by another build, so it must not inherit
    // the host's endianness, and the masks keep the shift results in range on a signed
    // promotion.
    void put_u16(uint16_t x) {
        put_u8(static_cast<uint8_t>(x & 0xFFu));
        put_u8(static_cast<uint8_t>((x >> 8) & 0xFFu));
    }
    void put_u32(uint32_t x) {
        put_u8(static_cast<uint8_t>(x & 0xFFu));
        put_u8(static_cast<uint8_t>((x >> 8) & 0xFFu));
        put_u8(static_cast<uint8_t>((x >> 16) & 0xFFu));
        put_u8(static_cast<uint8_t>((x >> 24) & 0xFFu));
    }
    // Length-prefixed, never NUL-terminated: an empty string is a stored value ("MQTT disabled")
    // and must survive the round-trip as one, and a credential is arbitrary bytes that may not
    // be treated as a C string.
    void put_str(const std::string& s, size_t max_len) {
        if (!ok) return;
        if (s.size() > max_len) { ok = false; return; }   // refuse; never silently truncate
        put_u16(static_cast<uint16_t>(s.size()));
        for (const char c : s) put_u8(static_cast<uint8_t>(c));
    }
};
}  // namespace detail

// ── SERIALIZED LAYOUT (version 1) ─────────────────────────────────────────────────────────────
//   off  size  field
//     0     4  magic 'T','K','C','1'
//     4     1  version (== kConfigBlobVersion on write)
//     5   2+n  wifi_ssid          (u16 length, then n raw bytes)
//     .   2+n  wifi_pass
//     .   2+n  wifi_ssid_backup
//     .   2+n  wifi_pass_backup
//     .     1  flags: bit0 = wifi_rollback_active, bit1 = wifi_rolled_back,
//                     bit2 = has_ota, bit3 = ota_channel (0 = release, 1 = dev; others reserved 0)
//     .   2+n  vin
//     .   2+n  mqtt_uri
//     .   2+n  syslog_uri
//     .     4  CRC-32 over every byte before it (little-endian)
//
// Writes `c` into `out` and returns the number of bytes written, or 0 if it did not write a
// usable blob — the buffer was too small, or a field exceeded its bound (see kConfigMax*Len).
// 0 means "nothing to store": the caller must NOT write a partial buffer, since a short entry
// would fail its own CRC on the next boot and read as a wiped configuration.
inline size_t config_blob_encode(const ConfigBlob& c, uint8_t* out, size_t cap) {
    if (out == nullptr) return 0;
    detail::BlobWriter w{out, cap};
    w.put_u8(kConfigBlobMagic0);
    w.put_u8(kConfigBlobMagic1);
    w.put_u8(kConfigBlobMagic2);
    w.put_u8(kConfigBlobMagic3);
    w.put_u8(kConfigBlobVersion);
    w.put_str(c.wifi_ssid,        kConfigMaxSsidLen);
    w.put_str(c.wifi_pass,        kConfigMaxWifiPassLen);
    w.put_str(c.wifi_ssid_backup, kConfigMaxSsidLen);
    w.put_str(c.wifi_pass_backup, kConfigMaxWifiPassLen);
    // Flags byte: bit0 = wifi_rollback_active, bit1 = wifi_rolled_back.
    // To preserve 100% backward compatibility for downgrades to older builds (which only read v1),
    // OTA channel configuration is packed in the reserved bits of the v1 flags byte:
    // bit2 = has_ota (explicitly set channel), bit3 = ota_channel (0 = release, 1 = dev).
    // Older builds ignore bits 2..7, ensuring all credentials survive downgrade without NVS wipe.
    w.put_u8(static_cast<uint8_t>((c.wifi_rollback_active ? 1u : 0u) |
                                  (c.wifi_rolled_back     ? 2u : 0u) |
                                  (c.has_ota              ? 4u : 0u) |
                                  ((c.ota_channel != 0)   ? 8u : 0u)));
    w.put_str(c.vin,        kConfigMaxVinLen);
    w.put_str(c.mqtt_uri,   kConfigMaxUriLen);
    w.put_str(c.syslog_uri, kConfigMaxUriLen);
    if (!w.ok) return 0;
    // The CRC covers the magic and the version too: a blob whose version byte was corrupted must
    // fail as corruption rather than be rejected as an unknown (and possibly newer) version.
    w.put_u32(config_crc32(out, w.n));
    if (!w.ok) return 0;
    return w.n;
}

// Returns true ONLY if the blob is complete, magic-matched, version-known, in-bounds and
// CRC-valid. On ANY failure `out` is left completely untouched — never a partial decode — and
// the caller fails closed (the legacy per-key fallback applies only when the blob is absent).
// This is the load side of the same all-or-nothing guarantee the single nvs_set_blob gives the write side.
inline bool config_blob_decode(const uint8_t* in, size_t len, ConfigBlob& out) {
    if (in == nullptr || len < detail::kBlobHeaderBytes + detail::kBlobCrcBytes) return false;
    if (in[0] != kConfigBlobMagic0 || in[1] != kConfigBlobMagic1 ||
        in[2] != kConfigBlobMagic2 || in[3] != kConfigBlobMagic3) return false;
    const uint8_t version = in[4];
    // Older is ACCEPTED (that is the OTA-upgrade path); newer is refused, because this build
    // cannot know where an unknown field ends and so cannot read even the fields it does know.
    if (version < kConfigBlobVersionMin || version > kConfigBlobVersion) return false;

    const uint32_t want = static_cast<uint32_t>(in[len - 4]) |
                          (static_cast<uint32_t>(in[len - 3]) << 8) |
                          (static_cast<uint32_t>(in[len - 2]) << 16) |
                          (static_cast<uint32_t>(in[len - 1]) << 24);
    if (config_crc32(in, len - 4) != want) return false;

    const size_t body_end = len - detail::kBlobCrcBytes;   // exclusive of the trailing CRC
    size_t p = detail::kBlobHeaderBytes;
    ConfigBlob c;   // staged locally: `out` is only touched once everything has parsed

    auto get_str = [&](std::string& s) -> bool {
        if (p + 2 > body_end) return false;
        const uint16_t n = static_cast<uint16_t>(static_cast<uint16_t>(in[p]) |
                                                 static_cast<uint16_t>(in[p + 1] << 8));
        p += 2;
        if (n > kConfigBlobDecodeMaxStr || p + n > body_end) return false;
        s.assign(reinterpret_cast<const char*>(in + p), n);
        p += n;
        return true;
    };

    if (!get_str(c.wifi_ssid) || !get_str(c.wifi_pass) ||
        !get_str(c.wifi_ssid_backup) || !get_str(c.wifi_pass_backup)) return false;
    if (p + 1 > body_end) return false;
    const uint8_t flags = in[p++];
    if (!get_str(c.vin) || !get_str(c.mqtt_uri) || !get_str(c.syslog_uri)) return false;
    c.has_ota     = (flags & 4u) != 0;
    c.ota_channel = (flags & 8u) != 0 ? 1 : 0;

    // Exact per version: a v1 blob must END right here. Accepting a prefix and ignoring the rest
    // would let a TRUNCATED future blob decode as a valid v1 whose newer fields silently take
    // their defaults — a wrong configuration that looks like a successfully loaded one, which is
    // strictly worse than refusing the invalid configuration blob (failing closed).
    if (p != body_end) return false;

    c.wifi_rollback_active = (flags & 1u) != 0;
    c.wifi_rolled_back     = (flags & 2u) != 0;
    out = c;
    return true;
}

// Did a config save achieve what its CALLER requires? Adapted from the sibling project's
// predicate of the same name (which weighs its atomic blob against a self-healing link cache);
// here the two durability domains are the atomic `cfg` blob and any separately keyed
// journal/runtime record listed in the header block:
//
//   * a credential/service save (the setup portal, /set_vin, /set_mqtt, /set_syslog,
//     /set_wifi) owns blob
//     fields only. Once that ONE atomic write lands the request IS committed, and a failure
//     while maintaining an unrelated self-healing key must not turn it into a false HTTP 500 —
//     the user would re-enter credentials that are already stored;
//   * a caller that genuinely depends on one of those keys says so with require_aux, and then a
//     failure there does fail the request.
//
// Kept here, pure, so the distinction is asserted by the host test instead of collapsing back
// into "any NVS error means nothing was saved" inside a handler.
inline bool config_save_succeeded(bool blob_ok, bool aux_ok, bool require_aux) {
    return blob_ok && (!require_aux || aux_ok);
}

}  // namespace tk
