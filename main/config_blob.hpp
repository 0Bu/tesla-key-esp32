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
// SCOPE. Only the values whose ONE writer is the HTTP/provisioning task. `ble_mac`, `last_time`,
// `reboot_why` and `disp_rot` stay as separate keys on purpose: they have DIFFERENT writers (the
// vehicle loop, the SNTP callback, the display task), and a whole-struct writer would revert
// another owner's field from a stale snapshot — the exact bug this file exists to prevent, pointed
// the other way.

namespace tk {

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
