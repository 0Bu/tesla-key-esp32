#pragma once

#include <adapters.h>
#include <string>
#include <string_view>
#include <vector>
#include <nvs_flash.h>
#include <nvs.h>
#include "logic/nvs_contract.hpp"
#include "logic/nvs_string_load.hpp"
#include "logic/nvs_blob_load.hpp"

class NvsStorageAdapter : public TeslaBLE::StorageAdapter {
public:
    explicit NvsStorageAdapter(
        const char* namespace_name = tk::nvs_contract::kTeslaBleNamespace);
    ~NvsStorageAdapter();

    bool initialize();
    // tesla-ble key/session reads use a two-call NVS probe+read API. The implementation accepts
    // only an exact, stable second length and publishes the candidate atomically on success.
    bool load(const std::string& key, std::vector<uint8_t>& buffer) override;

    // The WRITE path is [[nodiscard]] on purpose, and main/CMakeLists.txt pins
    // -Werror=unused-result on this component so that ignoring one is a BUILD error rather
    // than something a reviewer has to spot. A failed NVS write is otherwise SILENT — the
    // function logs and returns false, execution continues, and the caller believes it
    // persisted something it did not. What is stored here is exactly what the device cannot
    // rebuild by itself: the private key, the pairing session, the VIN, the WiFi credentials.
    // (These two are `override`s, so a call through the tesla-ble base pointer is unaffected —
    // the attribute binds to the static type at OUR call sites, which is where it is needed.)
    [[nodiscard]] bool save(const std::string& key, const std::vector<uint8_t>& buffer) override;
    [[nodiscard]] bool remove(const std::string& key) override;

    // Allocation-free existence probe: asks NVS only for the stored blob length and never
    // materialises the blob in a std::vector like load() does. For hot boolean checks such
    // as VehicleController::has_key()/has_session(), sampled ~1 Hz from several tasks.
    bool blob_exists(const std::string& key) const;
    // Safety-critical tri-state probe: true means the NVS query itself succeeded and `exists`
    // distinguishes present/missing; false means storage could not be read. Unlike blob_exists(),
    // callers must not collapse an I/O/corruption error into ordinary absence.
    bool probe_blob(const std::string& key, bool& exists) const;

    // Config helpers (plain string values). load_str() publishes a complete value atomically and
    // preserves the caller's previous output on every probe/read/length/allocation failure.
    bool load_str(const char* key, std::string& out);
    // Safety-critical tri-state string read. Only a genuine NOT_FOUND is Missing; wrong type,
    // corrupt/empty data and either NVS read failure are Error.
    tk::NvsStringLoadState load_str_state(const char* key, std::string& out);
    [[nodiscard]] bool save_str(const char* key, const std::string& value);

    // Raw blob helpers for the atomic config store (logic/config_store.hpp): ONE CRC-checked
    // entry written with a single nvs_set_blob, so a credential save is all-or-nothing across
    // both a write failure and a power cut. Kept separate from save()/load() above, which map
    // tesla-ble's long library key names through map_key().
    bool load_blob(const char* key, std::vector<uint8_t>& out);
    // Safety-critical variant used while a cross-namespace VIN journal is armed. Only exact
    // NOT_FOUND is Missing; wrong type, zero length, allocation/read failure or length drift is
    // Error and must never authorize legacy fallback plus journal deletion.
    tk::NvsBlobLoadState load_blob_state(const char* key, std::vector<uint8_t>& out);
    [[nodiscard]] bool save_blob(const char* key, const uint8_t* data, size_t len);

private:
    const char* ns_;
    tk::nvs_contract::Namespace ns_kind_{tk::nvs_contract::Namespace::Unknown};
    nvs_handle_t handle_{0};
    bool initialized_{false};

    // The exact registry replaces the former truncation fallback: an unknown logical key,
    // namespace mismatch or wrong storage API is rejected before an NVS call, so two long names
    // can never alias the same 15-byte flash key.
    const char* map_key(std::string_view key, tk::nvs_contract::StorageApi api,
                        bool erase_any_api = false) const;
};
