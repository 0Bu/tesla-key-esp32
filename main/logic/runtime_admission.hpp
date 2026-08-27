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

enum class RuntimeAdmissionAction : std::uint8_t {
    Wait,
    Run,
    Stop,
};

class RuntimeAdmissionGate {
public:
    bool mark_ready() noexcept {
        RuntimeAdmissionState expected = RuntimeAdmissionState::Booting;
        return state_.compare_exchange_strong(expected, RuntimeAdmissionState::Ready,
                                              std::memory_order_acq_rel);
    }

    bool mark_safe_mode() noexcept {
        RuntimeAdmissionState expected = RuntimeAdmissionState::Booting;
        return state_.compare_exchange_strong(expected, RuntimeAdmissionState::SafeMode,
                                              std::memory_order_acq_rel);
    }

    void mark_fatal() noexcept {
        state_.store(RuntimeAdmissionState::Fatal, std::memory_order_release);
    }

    bool vehicle_ready() const noexcept {
        return state_.load(std::memory_order_acquire) == RuntimeAdmissionState::Ready;
    }

    RuntimeAdmissionAction action() const noexcept {
        switch (state_.load(std::memory_order_acquire)) {
            case RuntimeAdmissionState::Booting:  return RuntimeAdmissionAction::Wait;
            case RuntimeAdmissionState::Ready:    return RuntimeAdmissionAction::Run;
            case RuntimeAdmissionState::SafeMode: return RuntimeAdmissionAction::Stop;
            case RuntimeAdmissionState::Fatal:    return RuntimeAdmissionAction::Stop;
        }
        return RuntimeAdmissionAction::Stop;
    }

    RuntimeAdmissionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<RuntimeAdmissionState> state_{RuntimeAdmissionState::Booting};
};

}  // namespace tk
