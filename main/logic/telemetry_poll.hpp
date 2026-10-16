#pragma once
// Active-window telemetry scheduling; host-test owner: test/test_logic.cpp.
#include <cstdint>

namespace tk {

enum class TelemetryPollDomain : uint8_t { Climate, Drive, Tires, Closures };

struct TelemetryPollInputs {
    bool paired;
    bool window;
    bool connected;
    bool command_in_flight;
    uint32_t now_ticks;
    uint32_t interval_ticks;
};

struct TelemetryPollState {
    uint32_t last_attempt_ticks = 0;
    TelemetryPollDomain rotation = TelemetryPollDomain::Climate;
    uint8_t climate_attempts = 0;

    void reset() noexcept { *this = {}; }

    bool due(const TelemetryPollInputs& in) const noexcept {
        return in.paired && in.window && in.connected && !in.command_in_flight &&
               in.now_ticks - last_attempt_ticks > in.interval_ticks;
    }

    TelemetryPollDomain next(bool climate_usable) const noexcept {
        // Retry a missing/stale climate sample at the next ordinary cadence. Limit consecutive
        // attempts so an unsupported or unresponsive climate domain cannot starve the others.
        return !climate_usable && climate_attempts < 2 ? TelemetryPollDomain::Climate : rotation;
    }

    // Call before any fallible snapshot/builder/enqueue work so exception containment cannot
    // turn memory pressure into a new allocation attempt and error log every loop iteration.
    void begin_attempt(uint32_t now_ticks) noexcept {
        last_attempt_ticks = now_ticks;
    }

    void note_admission(bool climate_usable, bool enqueued) noexcept {
        if (!enqueued) return;  // Queue refusal retains the pending opportunity.
        const auto domain = next(climate_usable);
        if (climate_usable || domain != TelemetryPollDomain::Climate) {
            climate_attempts = 0;
        } else if (climate_attempts < 2) {
            ++climate_attempts;
        }
        // A priority retry does not move the normal rotation back to Drive: Tires and Closures
        // must still get their turns when climate repeatedly fails after a successful enqueue.
        if (domain == rotation) {
            rotation = static_cast<TelemetryPollDomain>((static_cast<uint8_t>(rotation) + 1) % 4);
        }
    }
};

}  // namespace tk
