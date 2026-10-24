#include "runtime_admission.hpp"
#include "logic/heap_supervision.hpp"

namespace tk {
namespace {
RuntimeAdmissionGate s_runtime_admission;
VehicleProgress s_vehicle_progress;
}

bool runtime_admission_mark_ready() noexcept { return s_runtime_admission.mark_ready(); }
bool runtime_admission_mark_safe_mode() noexcept { return s_runtime_admission.mark_safe_mode(); }
void runtime_admission_mark_fatal() noexcept { s_runtime_admission.mark_fatal(); }
bool runtime_admission_vehicle_ready() noexcept { return s_runtime_admission.vehicle_ready(); }
RuntimeAdmissionAction runtime_admission_action() noexcept { return s_runtime_admission.action(); }
RuntimeAdmissionState runtime_admission_state() noexcept { return s_runtime_admission.state(); }

void runtime_vehicle_progress_publish(std::uint64_t now_ms) noexcept {
    s_vehicle_progress.publish(now_ms);
}
bool runtime_vehicle_progress_fresh(std::uint64_t now_ms) noexcept {
    return s_vehicle_progress.fresh(now_ms);
}

}  // namespace tk
