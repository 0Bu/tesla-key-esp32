#pragma once

#include <atomic>
#include <cstdint>

// Pure boot-time acknowledgement gate for the NimBLE host task. ESP-IDF 5.5's
// nimble_port_freertos_init() does not expose xTaskCreatePinnedToCore() failure to its caller, so
// successful return from that void wrapper is not evidence that a host task exists. The first
// on_sync callback is the positive acknowledgement. A timeout is terminal: a callback arriving
// after app_main has failed the essential-service boundary must never resurrect boot admission.
namespace tk {

enum class NimbleStartState : std::uint8_t {
    Idle,
    AwaitingSync,
    Synced,
    TimedOut,
};

enum class NimbleStartAction : std::uint8_t {
    Wait,
    Ready,
    Fail,
};

class NimbleStartGate {
public:
    bool begin() noexcept {
        NimbleStartState expected = NimbleStartState::Idle;
        return state_.compare_exchange_strong(expected, NimbleStartState::AwaitingSync,
                                              std::memory_order_acq_rel);
    }

    bool acknowledge_sync() noexcept {
        NimbleStartState expected = NimbleStartState::AwaitingSync;
        return state_.compare_exchange_strong(expected, NimbleStartState::Synced,
                                              std::memory_order_acq_rel);
    }

    bool mark_timed_out() noexcept {
        NimbleStartState expected = NimbleStartState::AwaitingSync;
        return state_.compare_exchange_strong(expected, NimbleStartState::TimedOut,
                                              std::memory_order_acq_rel);
    }

    NimbleStartAction action() const noexcept {
        switch (state_.load(std::memory_order_acquire)) {
            case NimbleStartState::Idle:
            case NimbleStartState::AwaitingSync:
                return NimbleStartAction::Wait;
            case NimbleStartState::Synced:
                return NimbleStartAction::Ready;
            case NimbleStartState::TimedOut:
                return NimbleStartAction::Fail;
        }
        return NimbleStartAction::Fail;
    }

    NimbleStartState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<NimbleStartState> state_{NimbleStartState::Idle};
};

}  // namespace tk
