#pragma once
// The `tesla_cfg` credential/service settings as ONE atomic, CRC-checked, versioned blob.
// Pure, IDF-free, host-testable (test/, built with the plain system toolchain) — like every
// other header in main/logic/, so the device runs exactly the code the host test exercises.
//
// WHY THIS EXISTS — the failure mode it removes. Today every one of these settings is its own
// independent NVS entry: `NvsStorageAdapter::save_str()` (nvs_storage.cpp) does one
// `nvs_set_str` + one `nvs_commit` per call, and the writers call it once per field. The setup
// portal is the clearest case — `provisioning.cpp`'s save handler persists `wifi_ssid`,
// `wifi_pass` and `vin` as THREE separate commits — and `http_config.cpp` adds `/set_vin`,
// `/set_mqtt` and `/set_syslog` on top. There is no transaction across them, so the window
// between two commits is a real state the device can be left in: a power cut (or an NVS write
// error) after the SSID has landed and before the password has leaves the new network's name
// paired with the old network's password. That pair authenticates against nothing; the device
// then joins neither network, and the only way back in is the setup AP or an erase.
//
// The existing code is already as careful as per-key writes allow — the setup handler checks all
// three results and refuses to reboot unless every one succeeded — and that is exactly the limit
// worth naming: checking the return value REPORTS a tear, it cannot UNDO the write that already
// landed, and a power cut reports nothing at all.
//
// The one-shot WiFi rollback backup (added alongside this header) would make that strictly
// worse if it stayed a separate key. The backup exists to survive a bad credential save, so
// backup-vs-credentials becomes a write-ORDERING problem: commit the credentials first and a
// crash in between leaves no backup for the very save that needed one; commit the backup first
// and a crash leaves a "backup" that is armed against a change that never happened. There is no
// order that is right for both. Putting the backup INSIDE the same blob as the credentials it
// protects deletes the question instead of answering it.
//
// So: one entry, written with a single `nvs_set_blob`, which is atomic at the NVS entry level —
// either the whole new blob lands or the previous one remains intact. A save is then
// all-or-nothing across BOTH a mid-write failure AND a power cut, with no per-key rollback and
// no ordering to get right. `NvsStorageAdapter::save()/load()` already are that blob pair (they
// carry the tesla-ble session blobs), so this needs no new NVS plumbing — just the key below.
//
// OWNERSHIP — why exactly these fields and not the others. Everything in here has ONE writer:
// the HTTP/provisioning task (the setup portal, `/set_vin`, `/set_mqtt`, `/set_syslog`,
// `/set_wifi`), which
// is serialized against itself on the single httpd task. The remaining `tesla_cfg` keys are
// DELIBERATELY separate because they have different writers OR journal/runtime-state lifetimes;
// folding them into a whole config snapshot could revert another owner or erase recovery state:
//   * `ble_mac`   — written by the BLE scanner (vehicle_ctrl.cpp) when it learns the car's
//                   address. A cache: a stale one costs one rescan, so it self-heals.
//   * `last_time` — written from the clock path (main.cpp / http_common.cpp) so the wall clock
//                   survives a reboot and the tesla-ble session blobs stay reusable.
//   * `reboot_why`— written on the way DOWN, by whoever ends the boot (the heap watchdog's
//                   `heap:<n>` breadcrumb), and read+cleared on the way up.
//   * `disp_rot`  — written by the display task on a BOOT-button tap.
//   * `vin_txn`   — an HTTP-owned cross-namespace transition journal, cleared only after the
//                   vehicle-key/session side and the config blob agree.
//   * `boot_fails`— boot-owned crash-loop safety state; malformed/read/write failure latches the
//                   recovery-only safe mode rather than being rewritten as configuration.
// None is a normal credential/service value. Its distinct owner or transaction lifetime must not
// be overwritten by publishing an ordinary httpd-owned configuration snapshot.
//
// THE READER MUST KEEP THE LEGACY PER-KEY LAYOUT AS A FALLBACK. When the `cfg` entry is absent
// (a fresh device, or an OTA upgrade from a build that predates this blob) or fails its CRC, the
// caller has to fall back to reading `wifi_ssid` / `wifi_pass` / `vin` / `mqtt_uri` /
// `syslog_uri` individually, exactly as today. That fallback is the whole reason an OTA does not
// strand an existing device: without it the first boot on the new firmware finds no blob, reads
// nothing, and comes up in the setup AP with its credentials and VIN sitting untouched in flash
// and simply unread. (The rollback backup has no legacy key by construction — it is new, and its
// absence on a pre-blob device is the correct state: no rollback pending.)
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include "nvs_contract.hpp"

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
};

// The NVS key inside the `tesla_cfg` namespace is owned by the exact persistence registry.
inline constexpr const char* kConfigBlobKey = nvs_contract::kConfigBlob;

// Magic, so a foreign or garbled entry is rejected before its length fields are ever trusted.
inline constexpr uint8_t kConfigBlobMagic0 = 'T';
inline constexpr uint8_t kConfigBlobMagic1 = 'K';
inline constexpr uint8_t kConfigBlobMagic2 = 'C';
inline constexpr uint8_t kConfigBlobMagic3 = '1';

// ── VERSIONING ────────────────────────────────────────────────────────────────────────────────
// This build WRITES exactly kConfigBlobVersion and READS everything from kConfigBlobVersionMin
// up to it. Accepting an OLDER blob is the load-bearing half: a device upgraded by OTA carries
// the blob the PREVIOUS firmware wrote, and refusing it would drop that user's WiFi credentials
// and VIN on the upgrade — the legacy per-key fallback cannot save them either, because a
// blob-era device stopped populating those keys.
//
// ADDING A FIELD (a version 2) is therefore three edits and one rule:
//   1. add the member to ConfigBlob, plus a `bool has_<block> = false;` marker if "absent" is
//      NOT the same as the member's default (see below);
//   2. APPEND its bytes at the END of config_blob_encode — never in the middle, never reusing a
//      byte — and bump kConfigBlobVersion to 2;
//   3. in config_blob_decode, read it under `if (version >= 2) { … c.has_<block> = true; }`, so
//      a v1 blob still decodes and simply leaves the member at its default.
// The `has_*` marker exists for the case where the CALLER, not this header, owns the default: if
// the field used to be a compile-time (Kconfig) setting, then "the blob predates the field" must
// read as the compile-time value, and silently defaulting it here would turn a working setting
// off on the OTA that introduced it. Where absent and default genuinely mean the same thing, no
// marker is needed.
//
// THE TRAP: a bump is never free, and it cuts BACKWARDS. A build that does not know a version
// REJECTS the blob, and on the device that reads as a WIPED CONFIGURATION — the blob is refused,
// the legacy per-key fallback is empty on a blob-era device, and the board comes up in the setup
// AP with its credentials intact in flash and unread. So a downgrade (an OTA rollback, a
// flash-back to an older image, two branches that both invent "v2") loses the user's settings
// without losing the bytes. Two parallel branches must not claim the same version number: the
// one that ships second takes the later number.
inline constexpr uint8_t kConfigBlobVersion    = 1;
inline constexpr uint8_t kConfigBlobVersionMin = 1;

// ── FIELD BOUNDS ──────────────────────────────────────────────────────────────────────────────
// What each field can legitimately hold on this device. These are enforced on the WRITE side
// (config_blob_encode refuses an over-long field) so an oversized value can never reach flash.
inline constexpr size_t kConfigMaxSsidLen     = 32;   // IEEE 802.11 SSID is at most 32 octets
inline constexpr size_t kConfigMaxWifiPassLen = 64;   // WPA2 passphrase 8..63, or a 64-hex PSK
inline constexpr size_t kConfigMaxVinLen      = 17;   // a VIN is exactly 17 (empty = unset)
inline constexpr size_t kConfigMaxUriLen      = 64;   // "host:port" for the broker / collector

// The DECODE-side sanity bound on a stored string length, deliberately looser than the per-field
// bounds above and deliberately not per-field. A reader stricter than the writer is a way to
// turn a perfectly good stored configuration into a wiped one, so the reader's job here is only
// to refuse corruption: a length that does not fit the remaining body is already rejected by the
// bounds arithmetic, and this catches a garbled length that happens to fit.
inline constexpr size_t kConfigBlobDecodeMaxStr = 256;

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

// The buffer size callers allocate. Headroom over the current layout so a small v2 field does
// not force every call site to change with it — but bounded, and asserted against, so it stays a
// stated cap rather than an assumption. A blob this size is a stack/static array on purpose:
// this is a device where the binding limit is the largest CONTIGUOUS free block, so the config
// save path deliberately makes no heap allocation at all.
inline constexpr size_t kConfigBlobCapBytes = 384;

static_assert(kConfigBlobEncodedMax == 361,
              "the serialized layout moved — if that was intended, bump kConfigBlobVersion and "
              "update this number and the layout comment together");
static_assert(kConfigBlobEncodedMax <= kConfigBlobCapBytes,
              "the config blob outgrew the buffer callers allocate — raise kConfigBlobCapBytes "
              "in the same commit as the field that grew it");

// A caller-owned buffer of exactly the stated cap: `tk::ConfigBlobBuffer buf; auto n =
// tk::config_blob_encode(b, buf.data(), buf.size());`
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
//     .     1  flags: bit0 = wifi_rollback_active, bit1 = wifi_rolled_back (others reserved 0)
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
    // Two booleans in one byte, so a future flag costs nothing and cannot move the layout.
    w.put_u8(static_cast<uint8_t>((c.wifi_rollback_active ? 1u : 0u) |
                                  (c.wifi_rolled_back     ? 2u : 0u)));
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
// the caller falls back to the legacy per-key layout (see the header block). This is the load
// side of the same all-or-nothing guarantee the single nvs_set_blob gives the write side.
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
    // (A version 2 block is read here, guarded by `if (version >= 2)`.)

    // Exact per version: a v1 blob must END right here. Accepting a prefix and ignoring the rest
    // would let a TRUNCATED future blob decode as a valid v1 whose newer fields silently take
    // their defaults — a wrong configuration that looks like a successfully loaded one, which is
    // strictly worse than falling back to the legacy layout.
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
