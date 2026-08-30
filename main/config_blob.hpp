#pragma once

#include "logic/config_store.hpp"

class NvsStorageAdapter;

// Load/save the credential + service settings as ONE atomic NVS entry.
//
// WHAT THIS REPLACES. Those values used to be independent per-key writes: the setup portal commits
// `wifi_ssid`, `wifi_pass` and `vin` as three separate nvs_set_str + nvs_commit pairs. Checking
// each return value (which the code does) reports a tear but cannot undo the writes that already
// landed, and a power cut between two of them reports nothing at all — leaving, for instance, a new
// SSID beside the old password, which is a device that can no longer join anything and can only be
// fixed over USB.
//
// One CRC-checked blob written with a single nvs_set_blob is all-or-nothing across BOTH failure
// modes. It also removes the write-ordering question the WiFi rollback would otherwise introduce:
// the previous credentials live INSIDE the same blob as the ones they protect, so there is no
// window in which the backup and the live values disagree.
//
// THE LEGACY FALLBACK IS NOT OPTIONAL. Devices already in the field have the per-key layout and no
// blob. cfg_load() reads the blob first and falls back to the individual keys when it is absent or
// fails its CRC; without that, this change would strand every existing device's WiFi credentials
// and VIN on the first OTA. The first successful save migrates a device to the blob; the legacy
// keys are deliberately left in place rather than deleted, so a downgrade to an older build still
// finds its configuration.
//
// SCOPE. Only durable user configuration whose coherent transaction is owned by the
// HTTP/provisioning task belongs in this blob. Other `tesla_cfg` records stay separate either
// because another task owns them (`ble_mac`, `last_time`, `reboot_why`, `disp_rot`) or because they
// are journals/runtime safety state with a different lifetime (`vin_txn`, `boot_fails`). In the
// first case a whole-struct writer could revert another owner's field from a stale snapshot; in the
// second, folding an in-flight transaction or boot latch into a config snapshot would erase its
// recovery semantics.

namespace tk {

enum class ConfigLoadState : uint8_t {
    Error,
    Legacy,
    Blob,
};

// Authoritative tri-state read for recovery code. Legacy is returned only when NVS proves the blob
// key is absent. A present-but-invalid blob or any probe/read failure is Error and leaves `out`
// untouched, so an armed cross-namespace journal cannot be classified from stale legacy mirrors.
ConfigLoadState cfg_load_state(NvsStorageAdapter& cfg, ConfigBlob& out);

// Read the current configuration. Returns true if a blob was decoded, false if the legacy per-key
// values were used (which is the normal answer on a device that has not saved since upgrading).
// `out` is populated either way. The legacy path starts from the compiled Kconfig defaults and
// then applies keys that actually exist; a valid blob remains authoritative, including explicit
// empty values used to disable a service.
bool cfg_load(NvsStorageAdapter& cfg, ConfigBlob& out);

// Persist the whole configuration atomically. Returns false without publishing anything on a failed
// write — the PREVIOUS blob is then still intact, which is the property the caller relies on when
// it answers 500 and skips its reboot.
bool cfg_save(NvsStorageAdapter& cfg, const ConfigBlob& in);

}  // namespace tk
