#pragma once

#include "logic/config_store.hpp"

class NvsStorageAdapter;

// Coherent credential/service snapshots in one versioned CRC-checked NVS entry. The blob commit
// precedes best-effort per-key mirrors for old builds. Only proven blob absence enables legacy
// loading; unreadable/corrupt blobs never fall back to those possibly stale mirrors. The registry
// in logic/nvs_contract.hpp owns the separate runtime records and recovery journals.

namespace tk {

enum class ConfigLoadState : uint8_t {
    Error,
    Legacy,
    Blob,
};

// Authoritative tri-state read for recovery code. Legacy is returned only when NVS proves the blob
// key is absent and every legacy key is either read or proven absent. A present-but-invalid blob
// or any blob/legacy read failure is Error and leaves `out` untouched, so an armed cross-namespace
// journal cannot be classified from stale or incomplete legacy values.
ConfigLoadState cfg_load_state(NvsStorageAdapter& cfg, ConfigBlob& out);

// Read the current configuration. Returns true if a blob was decoded, false if the legacy per-key
// values were used (which is the normal answer on a device that has not saved since upgrading).
// `out` is populated for a valid blob or a fully read legacy layout; an NVS error or corrupt blob
// leaves `out` untouched and returns false without falling back to stale legacy mirrors. READ-ONLY
// callers only: after a committed blob save the legacy mirrors are best-effort and may be stale, so
// a caller that goes on to cfg_save() must use cfg_load_for_update() instead.
bool cfg_load(NvsStorageAdapter& cfg, ConfigBlob& out);

// Load the configuration for a read-modify-save. Returns true only when `out` is authoritative: a
// decoded blob, or a blob proven ABSENT (the legacy migration path, where the per-key values are
// the only truth). Returns false — leaving `out` untouched — when a blob cannot be read/decoded or
// any legacy key cannot be read during migration. The caller must refuse the mutation and persist
// nothing: the legacy mirrors may be arbitrarily stale, and an identity/credential change built
// on them is not the change the user asked for. The granularity is deliberately all-or-nothing:
// a partially read legacy snapshot is not a coherent identity, so ONE unreadable key refuses the
// whole update.
bool cfg_load_for_update(NvsStorageAdapter& cfg, ConfigBlob& out);

// Encode under a nonblocking private-buffer lease, commit, then mirror legacy keys best-effort.
// Busy/reentry refuses before writes; every owned exit wipes the buffer before release. False
// means encoding/write/commit was not confirmed; a failed commit cannot establish which snapshot
// is durable. A mirror failure after a confirmed blob commit still returns true.
bool cfg_save(NvsStorageAdapter& cfg, const ConfigBlob& in);

// Seed once from the verified boot configuration before starting services. Successful cfg_save()
// commits keep this diagnostic current, including when a configuration restart is deferred.
void cfg_init_wifi_rollback_status(bool rolled_back) noexcept;

// Allocation-free snapshot for cyclic status consumers; never reads NVS.
bool cfg_wifi_rolled_back() noexcept;

}  // namespace tk
