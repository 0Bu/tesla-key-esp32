#pragma once
#include "logic/ota_recovery_memory.hpp"

namespace tk {
// Immutable, allocation-free policy selection. Confirmation and pre-allocation OTA entry
// share one selected recovery threshold at their actual ordinary references; TLS uses its
// separately named record-capacity screen. No cache, setting, calibration exception or reservation.
// Unknown use returns0.
std::size_t ota_recovery_required_bytes(OtaRecoveryUse use) noexcept;
} // namespace tk
