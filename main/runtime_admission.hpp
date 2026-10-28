#pragma once

#include "logic/runtime_admission.hpp"
#include <cstdint>

namespace tk {

// Process-global facade over the pure gate.  app_main is the sole state owner; HTTP, the OTA
// health task and the two vehicle task entries are consumers.
bool runtime_admission_mark_ready() noexcept;
bool runtime_admission_mark_safe_mode() noexcept;
void runtime_admission_mark_fatal(RuntimeFatalComponent component = RuntimeFatalComponent::Unknown) noexcept;
RuntimeAdmissionSnapshot runtime_admission_snapshot() noexcept;
// Sole automatic first-key reload; caller already owns ConfigRestart + command transaction.
bool runtime_admission_try_claim_identity_reload() noexcept;
bool runtime_admission_vehicle_ready() noexcept;
RuntimeAdmissionAction runtime_admission_action() noexcept;
RuntimeAdmissionState runtime_admission_state() noexcept;

// Successful loop progress is separate from the one-way Ready transition. The supervisor reads
// this publication without a VehicleController pointer or any of its locks.
void runtime_vehicle_progress_publish(std::uint64_t now_ms) noexcept;
bool runtime_vehicle_progress_fresh(std::uint64_t now_ms) noexcept;

}  // namespace tk
