#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace tk::nvs_contract {

// ESP-IDF limits both NVS namespace and entry names to 15 bytes. This is the single,
// hardware-free inventory for every record owned by the firmware or by the pinned tesla-ble
// StorageAdapter contract. Runtime access through NvsStorageAdapter is fail-closed against this
// table; direct U8 display migration access is separately pinned by the static host gate.
inline constexpr char kConfigNamespace[] = "tesla_cfg";
inline constexpr char kTeslaBleNamespace[] = "tesla_ble";

inline constexpr char kConfigBlob[] = "cfg";
inline constexpr char kLegacyWifiSsid[] = "wifi_ssid";
inline constexpr char kLegacyWifiPass[] = "wifi_pass";
inline constexpr char kLegacyVin[] = "vin";
inline constexpr char kLegacyMqttUri[] = "mqtt_uri";
inline constexpr char kLegacySyslogUri[] = "syslog_uri";
inline constexpr char kLastTime[] = "last_time";
inline constexpr char kVinTransition[] = "vin_txn";
inline constexpr char kBleMac[] = "ble_mac";
inline constexpr char kRebootReason[] = "reboot_why";
inline constexpr char kBootFailures[] = "boot_fails";
inline constexpr char kDisplayRotation[] = "disp_rot";
inline constexpr char kLegacyDisplayFlip[] = "disp_flip";

inline constexpr char kPrivateKey[] = "private_key";
inline constexpr char kSessionVcsec[] = "session_vcsec";
inline constexpr char kSessionInfotainment[] = "session_infotainment";
inline constexpr char kPairedAt[] = "paired_at";
inline constexpr char kKeyCreated[] = "key_created";
inline constexpr char kKeyRotation[] = "key_rotate";

enum class Namespace { Config, TeslaBle, Unknown };
enum class StorageApi { Blob, String, RawBlob, DirectU8 };
enum class Owner {
    ConfigHttp,
    LegacyConfigMirror,
    Clock,
    VinTransition,
    BleDiscovery,
    HeapWatchdog,
    BootGuard,
    Display,
    TeslaBleLibrary,
    Pairing,
    KeyRotation,
};
enum class Retention {
    DurableAcrossOta,
    LegacyDowngradeMirror,
    ReplaceableCache,
    RecoveryJournal,
    MigrationOnly,
};

struct Entry {
    Namespace name_space;
    std::string_view logical_key;
    std::string_view stored_key;
    StorageApi api;
    Owner owner;
    Retention retention;
    bool secret;
};

inline constexpr std::array<Entry, 19> kEntries{{
    {Namespace::Config, kConfigBlob, kConfigBlob, StorageApi::RawBlob,
     Owner::ConfigHttp, Retention::DurableAcrossOta, true},
    {Namespace::Config, kLegacyWifiSsid, kLegacyWifiSsid, StorageApi::String,
     Owner::LegacyConfigMirror, Retention::LegacyDowngradeMirror, true},
    {Namespace::Config, kLegacyWifiPass, kLegacyWifiPass, StorageApi::String,
     Owner::LegacyConfigMirror, Retention::LegacyDowngradeMirror, true},
    {Namespace::Config, kLegacyVin, kLegacyVin, StorageApi::String,
     Owner::LegacyConfigMirror, Retention::LegacyDowngradeMirror, true},
    {Namespace::Config, kLegacyMqttUri, kLegacyMqttUri, StorageApi::String,
     Owner::LegacyConfigMirror, Retention::LegacyDowngradeMirror, true},
    {Namespace::Config, kLegacySyslogUri, kLegacySyslogUri, StorageApi::String,
     Owner::LegacyConfigMirror, Retention::LegacyDowngradeMirror, true},
    {Namespace::Config, kLastTime, kLastTime, StorageApi::String,
     Owner::Clock, Retention::ReplaceableCache, false},
    {Namespace::Config, kVinTransition, kVinTransition, StorageApi::String,
     Owner::VinTransition, Retention::RecoveryJournal, true},
    {Namespace::Config, kBleMac, kBleMac, StorageApi::String,
     Owner::BleDiscovery, Retention::ReplaceableCache, true},
    {Namespace::Config, kRebootReason, kRebootReason, StorageApi::String,
     Owner::HeapWatchdog, Retention::RecoveryJournal, false},
    {Namespace::Config, kBootFailures, kBootFailures, StorageApi::String,
     Owner::BootGuard, Retention::RecoveryJournal, false},
    {Namespace::Config, kDisplayRotation, kDisplayRotation, StorageApi::DirectU8,
     Owner::Display, Retention::DurableAcrossOta, false},
    {Namespace::Config, kLegacyDisplayFlip, kLegacyDisplayFlip, StorageApi::DirectU8,
     Owner::Display, Retention::MigrationOnly, false},

    {Namespace::TeslaBle, kPrivateKey, kPrivateKey, StorageApi::Blob,
     Owner::TeslaBleLibrary, Retention::DurableAcrossOta, true},
    {Namespace::TeslaBle, kSessionVcsec, "sess_vcsec", StorageApi::Blob,
     Owner::TeslaBleLibrary, Retention::ReplaceableCache, true},
    {Namespace::TeslaBle, kSessionInfotainment, "sess_info", StorageApi::Blob,
     Owner::TeslaBleLibrary, Retention::ReplaceableCache, true},
    {Namespace::TeslaBle, kPairedAt, kPairedAt, StorageApi::String,
     Owner::Pairing, Retention::ReplaceableCache, false},
    {Namespace::TeslaBle, kKeyCreated, kKeyCreated, StorageApi::String,
     Owner::Pairing, Retention::DurableAcrossOta, false},
    {Namespace::TeslaBle, kKeyRotation, kKeyRotation, StorageApi::Blob,
     Owner::KeyRotation, Retention::RecoveryJournal, true},
}};

constexpr Namespace classify_namespace(std::string_view value) {
    return value == kConfigNamespace ? Namespace::Config
         : value == kTeslaBleNamespace ? Namespace::TeslaBle
                                       : Namespace::Unknown;
}

constexpr const Entry* find(Namespace name_space, std::string_view logical_key) {
    for (const auto& entry : kEntries) {
        if (entry.name_space == name_space && entry.logical_key == logical_key) return &entry;
    }
    return nullptr;
}

constexpr bool valid() {
    if (std::string_view(kConfigNamespace).size() > 15 ||
        std::string_view(kTeslaBleNamespace).size() > 15) return false;
    for (std::size_t i = 0; i < kEntries.size(); ++i) {
        const auto& entry = kEntries[i];
        if (entry.name_space == Namespace::Unknown || entry.logical_key.empty() ||
            entry.stored_key.empty() || entry.stored_key.size() > 15) return false;
        for (std::size_t j = i + 1; j < kEntries.size(); ++j) {
            const auto& other = kEntries[j];
            if (entry.name_space != other.name_space) continue;
            if (entry.logical_key == other.logical_key || entry.stored_key == other.stored_key)
                return false;
        }
    }
    return true;
}

static_assert(valid(), "NVS contract contains an unknown, oversized, or colliding key");

}  // namespace tk::nvs_contract
