#pragma once

#include <atomic>
#include <cstdint>

// Pure, hardware-free boot admission state.  Vehicle contact is fail-closed until every
// essential service and both VehicleController tasks have started.  Safe mode and a fatal
// partial boot are terminal for this process: neither can accidentally be promoted to Ready.
namespace tk {

enum class RuntimeAdmissionState : std::uint8_t {
    Booting,
    Ready,
    SafeMode,
    Fatal,
};

constexpr const char* runtime_admission_state_name(RuntimeAdmissionState state) noexcept {
    switch (state) {
        case RuntimeAdmissionState::Booting: return "booting";
        case RuntimeAdmissionState::Ready: return "ready";
        case RuntimeAdmissionState::SafeMode: return "safe_mode";
        case RuntimeAdmissionState::Fatal: return "fatal";
    }
    return "fatal";
}

// One byte publishes state and a coarse first-fatal component together. Components contain no
// dynamic error text or user input; detailed diagnostics retain the existing component string.
enum class RuntimeFatalComponent : std::uint8_t {
    Unknown, Clock, Nvs, Configuration, Identity, Bluetooth, Http, Network,
    Vehicle, Ota, HeapSupervisor, Application,
};
struct RuntimeAdmissionSnapshot {
    RuntimeAdmissionState state{RuntimeAdmissionState::Booting};
    RuntimeFatalComponent component{RuntimeFatalComponent::Unknown};
};
constexpr const char* runtime_fatal_component_name(RuntimeFatalComponent component) noexcept {
    switch (component) {
        case RuntimeFatalComponent::Clock: return "clock";
        case RuntimeFatalComponent::Nvs: return "nvs";
        case RuntimeFatalComponent::Configuration: return "configuration";
        case RuntimeFatalComponent::Identity: return "identity";
        case RuntimeFatalComponent::Bluetooth: return "bluetooth";
        case RuntimeFatalComponent::Http: return "http";
        case RuntimeFatalComponent::Network: return "network";
        case RuntimeFatalComponent::Vehicle: return "vehicle";
        case RuntimeFatalComponent::Ota: return "ota";
        case RuntimeFatalComponent::HeapSupervisor: return "heap_supervisor";
        case RuntimeFatalComponent::Application: return "application";
        default: return "unknown";
    }
}
constexpr bool component_prefix(const char* text, const char* prefix) noexcept {
    if (!text) return false;
    while (*prefix) if (*text++ != *prefix++) return false;
    return true;
}
// Ordered immutable prefix data keeps the existing first-match classification without
// duplicating the prefix scanner at every branch. No dynamic text is cached.
namespace runtime_admission_detail {
struct FatalPrefix { const char* text; RuntimeFatalComponent component; };
inline constexpr FatalPrefix kFatalPrefixes[]{
    {"clock", RuntimeFatalComponent::Clock},
    {"NVS", RuntimeFatalComponent::Nvs},
    {"Tesla NVS", RuntimeFatalComponent::Nvs},
    {"configuration", RuntimeFatalComponent::Configuration},
    {"VIN ", RuntimeFatalComponent::Identity},
    {"initial key", RuntimeFatalComponent::Identity},
    {"NimBLE", RuntimeFatalComponent::Bluetooth},
    {"HTTP", RuntimeFatalComponent::Http},
    {"setup HTTP", RuntimeFatalComponent::Http},
    {"WiFi", RuntimeFatalComponent::Network},
    {"Ethernet", RuntimeFatalComponent::Network},
    {"W5500", RuntimeFatalComponent::Network},
    {"LAN", RuntimeFatalComponent::Network},
    {"setup AP", RuntimeFatalComponent::Network},
    {"setup WiFi", RuntimeFatalComponent::Network},
    {"Vehicle", RuntimeFatalComponent::Vehicle},
    {"vehicle runtime", RuntimeFatalComponent::Vehicle},
    {"OTA", RuntimeFatalComponent::Ota},
    {"heap supervisor", RuntimeFatalComponent::HeapSupervisor},
    {"app_main", RuntimeFatalComponent::Application},
    {"safe-mode runtime", RuntimeFatalComponent::Application},
};
}  // namespace runtime_admission_detail
constexpr RuntimeFatalComponent runtime_fatal_component(const char* component) noexcept {
    for (const auto& prefix : runtime_admission_detail::kFatalPrefixes)
        if (component_prefix(component, prefix.text)) return prefix.component;
    return RuntimeFatalComponent::Unknown;
}
constexpr std::uint8_t runtime_admission_pack(RuntimeAdmissionState state,
        RuntimeFatalComponent component = RuntimeFatalComponent::Unknown) noexcept {
    const auto code = static_cast<std::uint8_t>(component);
    return static_cast<std::uint8_t>(state) | static_cast<std::uint8_t>(
        (state == RuntimeAdmissionState::Fatal && code <= 11 ? code : 0) << 2);
}
// This process-only bit linearizes the sole automatic commit reload against Fatal. It is
// not a new NVS allowance or a public state; state/component readers ignore only this bit.
inline constexpr std::uint8_t kRuntimeIdentityReloadClaim = 0x40;
constexpr RuntimeAdmissionSnapshot runtime_admission_unpack(std::uint8_t value) noexcept {
    const auto state = static_cast<RuntimeAdmissionState>(value & 3u);
    const auto component = static_cast<std::uint8_t>((value & ~kRuntimeIdentityReloadClaim) >> 2);
    return {state, state == RuntimeAdmissionState::Fatal && component <= 11
        ? static_cast<RuntimeFatalComponent>(component) : RuntimeFatalComponent::Unknown};
}

enum class RuntimeAdmissionAction : std::uint8_t {
    Wait,
    Run,
    Stop,
};

class RuntimeAdmissionGate {
public:
    bool mark_ready() noexcept {
        std::uint8_t expected = runtime_admission_pack(RuntimeAdmissionState::Booting);
        return state_.compare_exchange_strong(expected, runtime_admission_pack(RuntimeAdmissionState::Ready),
                                              std::memory_order_acq_rel);
    }

    bool mark_safe_mode() noexcept {
        std::uint8_t expected = runtime_admission_pack(RuntimeAdmissionState::Booting);
        return state_.compare_exchange_strong(expected, runtime_admission_pack(RuntimeAdmissionState::SafeMode),
                                              std::memory_order_acq_rel);
    }

    void mark_fatal(RuntimeFatalComponent component = RuntimeFatalComponent::Unknown) noexcept {
        auto current = state_.load(std::memory_order_acquire);
        do {
            if (runtime_admission_unpack(current).state == RuntimeAdmissionState::Fatal) return;
        } while (!state_.compare_exchange_weak(current,
            static_cast<std::uint8_t>(runtime_admission_pack(RuntimeAdmissionState::Fatal, component) |
                                      (current & kRuntimeIdentityReloadClaim)),
            std::memory_order_acq_rel, std::memory_order_acquire));
    }

    bool try_claim_identity_reload() noexcept {
        // Exact unclaimed Ready only: Fatal before this CAS wins; Fatal afterwards remains
        // publishable and preserves the claim. No second task/local once object can rearm it.
        std::uint8_t expected = runtime_admission_pack(RuntimeAdmissionState::Ready);
        return state_.compare_exchange_strong(expected,
            static_cast<std::uint8_t>(runtime_admission_pack(RuntimeAdmissionState::Ready) |
                                      kRuntimeIdentityReloadClaim),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool vehicle_ready() const noexcept {
        return state() == RuntimeAdmissionState::Ready;
    }

    RuntimeAdmissionAction action() const noexcept {
        switch (state()) {
            case RuntimeAdmissionState::Booting:  return RuntimeAdmissionAction::Wait;
            case RuntimeAdmissionState::Ready:    return RuntimeAdmissionAction::Run;
            case RuntimeAdmissionState::SafeMode: return RuntimeAdmissionAction::Stop;
            case RuntimeAdmissionState::Fatal:    return RuntimeAdmissionAction::Stop;
        }
        return RuntimeAdmissionAction::Stop;
    }

    RuntimeAdmissionState state() const noexcept {
        return snapshot().state;
    }
    RuntimeAdmissionSnapshot snapshot() const noexcept {
        return runtime_admission_unpack(state_.load(std::memory_order_acquire));
    }

private:
    std::atomic<std::uint8_t> state_{runtime_admission_pack(RuntimeAdmissionState::Booting)};
};

static_assert(sizeof(RuntimeAdmissionGate) == sizeof(std::atomic<std::uint8_t>),
              "state/component/reload claim must retain one atomic byte");
}  // namespace tk
