#pragma once
#include "ota_coordinator.hpp"
#include "logic/ota_contract.hpp"
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <array>

namespace tk::ota {
struct ImagePolicy {
    char expected[32]{}, current[32]{};
    unsigned target_pr{0};
    tk::OtaChannel channel{tk::OtaChannel::Release};
};
// Actual raw-OTA adapter. Only the application coordinator receives its Effects table.
// The caller supplies the existing owner-checked Ota-lease callback, never the network worker.
class RawWriter {
public:
    RawWriter(ImagePolicy policy,void* owner,bool(*release)(void*) noexcept) noexcept
        : policy_(policy),owner_(owner),release_(release) {}
    RawWriter(const RawWriter&) = delete;
    RawWriter& operator=(const RawWriter&) = delete;
    RawWriter(RawWriter&&) = delete;
    RawWriter& operator=(RawWriter&&) = delete;
    Effects effects() noexcept;
    bool configure(ImagePolicy policy) noexcept {
        if (handle_ || partition_ || uncertain_ || validated_) return false;
        policy_ = policy; return true;
    }
private:
    static RawWriter& self(void* p) noexcept { return *static_cast<RawWriter*>(p); }
    static Effect prefix(void*,const std::uint8_t*,std::size_t) noexcept;
    static Effect begin(void*) noexcept;
    static Effect write(void*,const std::uint8_t*,std::size_t) noexcept;
    static Effect end(void*) noexcept;
    static Effect select(void*) noexcept;
    static bool abort(void*) noexcept;
    static bool quiescent(void*) noexcept;
    static bool release(void*) noexcept;
    ImagePolicy policy_;
    const esp_partition_t* partition_{nullptr};
    esp_ota_handle_t handle_{0};
    void* owner_;
    bool(*release_)(void*) noexcept;
    bool uncertain_{false}, validated_{false};
};
} // namespace tk::ota
