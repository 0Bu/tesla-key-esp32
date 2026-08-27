#pragma once

#include <atomic>
#include <cstdint>

// Pure boot-time gate between syslog_start() and the newly-created consumer task. FreeRTOS may
// schedule the task before xTaskCreate() returns, so the task must wait until startup either
// commits all boot-lifetime resources or cancels. Terminal states cannot be reopened: a late task
// observation after cancellation always exits instead of touching resources that startup owns.
namespace tk {

enum class SyslogStartState : std::uint8_t {
    Idle,
    Waiting,
    Running,
    Cancelled,
};

enum class SyslogStartAction : std::uint8_t {
    Wait,
    Run,
    Cancel,
};

class SyslogStartGate {
public:
    bool begin() noexcept {
        SyslogStartState expected = SyslogStartState::Idle;
        return state_.compare_exchange_strong(expected, SyslogStartState::Waiting,
                                              std::memory_order_acq_rel);
    }

    bool commit() noexcept {
        SyslogStartState expected = SyslogStartState::Waiting;
        return state_.compare_exchange_strong(expected, SyslogStartState::Running,
                                              std::memory_order_acq_rel);
    }

    bool cancel() noexcept {
        SyslogStartState expected = SyslogStartState::Waiting;
        return state_.compare_exchange_strong(expected, SyslogStartState::Cancelled,
                                              std::memory_order_acq_rel);
    }

    SyslogStartAction action() const noexcept {
        switch (state_.load(std::memory_order_acquire)) {
            case SyslogStartState::Idle:
            case SyslogStartState::Waiting:
                return SyslogStartAction::Wait;
            case SyslogStartState::Running:
                return SyslogStartAction::Run;
            case SyslogStartState::Cancelled:
                return SyslogStartAction::Cancel;
        }
        return SyslogStartAction::Cancel;
    }

    SyslogStartState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<SyslogStartState> state_{SyslogStartState::Idle};
};

}  // namespace tk
