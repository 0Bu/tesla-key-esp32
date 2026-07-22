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
    bool save(const std::string& key, const std::vector<uint8_t>& buffer) override;
    bool remove(const std::string& key) override;

    // Allocation-free existence probe: asks NVS only for the stored blob length and never
    // materialises the blob in a std::vector like load() does. For hot boolean checks such
    // as VehicleController::has_key()/has_session(), sampled ~1 Hz from several tasks.
    bool blob_exists(const std::string& key) const;

    // Config helpers (plain string values)
    bool load_str(const char* key, std::string& out);
    bool save_str(const char* key, const std::string& value);

private:
    const char* ns_;
    nvs_handle_t handle_{0};
    bool initialized_{false};

    // NVS keys are max 15 chars — map long library keys to short ones
    std::string map_key(const std::string& key) const;
};
