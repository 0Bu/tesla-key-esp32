#pragma once

#include "logic/runtime_admission.hpp"

namespace tk {

// Process-global facade over the pure gate.  app_main is the sole state owner; HTTP, the OTA
// health task and the two vehicle task entries are consumers.
bool runtime_admission_mark_ready() noexcept;
bool runtime_admission_mark_safe_mode() noexcept;
void runtime_admission_mark_fatal() noexcept;
bool runtime_admission_vehicle_ready() noexcept;
RuntimeAdmissionAction runtime_admission_action() noexcept;
RuntimeAdmissionState runtime_admission_state() noexcept;

}  // namespace tk
