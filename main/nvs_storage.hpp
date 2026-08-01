#pragma once

#include <adapters.h>
#include <string>
#include <vector>
#include <nvs_flash.h>
#include <nvs.h>

class NvsStorageAdapter : public TeslaBLE::StorageAdapter {
public:
    explicit NvsStorageAdapter(const char* namespace_name = "tesla_ble");
    ~NvsStorageAdapter();

    bool initialize();
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

    // Config helpers (plain string values)
    bool load_str(const char* key, std::string& out);
    [[nodiscard]] bool save_str(const char* key, const std::string& value);

    // Raw blob helpers for the atomic config store (logic/config_store.hpp): ONE CRC-checked
    // entry written with a single nvs_set_blob, so a credential save is all-or-nothing across
    // both a write failure and a power cut. Kept separate from save()/load() above, which map
    // tesla-ble's long library key names through map_key().
    bool load_blob(const char* key, std::vector<uint8_t>& out);
    [[nodiscard]] bool save_blob(const char* key, const uint8_t* data, size_t len);

private:
    const char* ns_;
    nvs_handle_t handle_{0};
    bool initialized_{false};

    // NVS keys are max 15 chars — map long library keys to short ones
    std::string map_key(const std::string& key) const;
};
