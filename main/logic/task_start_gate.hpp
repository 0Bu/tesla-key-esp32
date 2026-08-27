#pragma once

#include <atomic>
#include <cstdint>

// Pure, hardware-free coordination for starting two FreeRTOS tasks as one lifecycle unit.
// The first task may be scheduled as soon as xTaskCreate() returns, so both entry functions must
// consult this gate before touching a watchdog, mutex, BLE object or vehicle. Production and the
// host fault-injection checks share this exact state machine.
namespace tk {

enum class TaskStartState : std::uint8_t {
    Idle,
    Creating,
    Running,
    Cancelled,
};

enum class TaskEntryAction : std::uint8_t {
    Wait,
    Run,
    Cancel,
};

class DualTaskStartGate {
public:
    bool begin() {
        TaskStartState expected = TaskStartState::Idle;
        if (!state_.compare_exchange_strong(expected, TaskStartState::Creating,
                                            std::memory_order_acq_rel)) {
            return false;
        }
        cancelled_acks_.store(0, std::memory_order_release);
        return true;
    }

    bool release() {
        TaskStartState expected = TaskStartState::Creating;
        return state_.compare_exchange_strong(expected, TaskStartState::Running,
                                              std::memory_order_acq_rel);
    }

    bool cancel() {
        TaskStartState expected = TaskStartState::Creating;
        return state_.compare_exchange_strong(expected, TaskStartState::Cancelled,
                                              std::memory_order_acq_rel);
    }

    TaskEntryAction entry_action() const {
        switch (state_.load(std::memory_order_acquire)) {
            case TaskStartState::Creating:  return TaskEntryAction::Wait;
            case TaskStartState::Running:   return TaskEntryAction::Run;
            case TaskStartState::Cancelled: return TaskEntryAction::Cancel;
            case TaskStartState::Idle:       return TaskEntryAction::Cancel;
        }
        return TaskEntryAction::Cancel;
    }

    void acknowledge_cancel() {
        cancelled_acks_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool cancelled_tasks_acknowledged(std::uint8_t expected) const {
        return state_.load(std::memory_order_acquire) == TaskStartState::Cancelled &&
               cancelled_acks_.load(std::memory_order_acquire) == expected;
    }

    bool reset_cancelled(std::uint8_t expected_acks) {
        if (!cancelled_tasks_acknowledged(expected_acks)) return false;
        TaskStartState expected = TaskStartState::Cancelled;
        return state_.compare_exchange_strong(expected, TaskStartState::Idle,
                                              std::memory_order_acq_rel);
    }

    TaskStartState state() const { return state_.load(std::memory_order_acquire); }

private:
    std::atomic<TaskStartState> state_{TaskStartState::Idle};
    std::atomic<std::uint8_t> cancelled_acks_{0};
};

}  // namespace tk
