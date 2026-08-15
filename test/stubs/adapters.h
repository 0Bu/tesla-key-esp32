#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal host-test contract matching tesla-ble's StorageAdapter.  The fresh CI checkout does
// not materialize managed_components before run-mock-tests.sh, so the NVS adapter test must not
// depend on a component-manager cache left by an earlier firmware build.
namespace TeslaBLE {

class StorageAdapter {
public:
    virtual ~StorageAdapter() = default;
    virtual bool load(const std::string& key, std::vector<uint8_t>& buffer) = 0;
    virtual bool save(const std::string& key, const std::vector<uint8_t>& buffer) = 0;
    virtual bool remove(const std::string& key) = 0;
};

}  // namespace TeslaBLE
