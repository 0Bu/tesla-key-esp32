#pragma once
#include <cstddef>
#include <cstdint>

// Pure, hardware-free recovery policy. Host owner: test/test_logic.cpp.
// No IDF/RTOS/NVS/cJSON state or allocation is hidden here.
namespace tk {
enum class OtaRecoveryUse : std::uint8_t { TlsAdmission, ImageConfirmation, OperationEntry };
// Shared policy threshold for confirmation and pre-operation admission, not identical
// allocator phases. The named S3 series supports an operating-point policy, not a
// necessary minimum or universal TLS guarantee. Three cold S3 transfers completed at
// 59392 B (32768 B pre-TLS-init / >= 31744 B post-context). Reducing pre-TLS contiguous
// OTA allocations by 5344 B for manifests (-4096 B initial reserve vs 8192 B, -1024 B
// redirect_, -224 B prefix_) calibrates the threshold to 55296 B (54 KiB), yielding
// +1248 B net more pre-init headroom than 59392 B with zero mid-TLS reallocation for
// real ~3 KiB manifests, while admitting warm S3 devices on a 57344 B bisected block.
// Reference queries reclaim no bytes.
inline constexpr std::size_t kOtaRecoveryEntryRequiredB = 55296;
// Pinned fixed-buffer profile: IN16384/OUT4096 + conservative overhead2048 + header13.
// The largest single-record request screen is not a complete TLS heap budget/reservation.
inline constexpr std::size_t kOtaTlsInContentB = 16384;
inline constexpr std::size_t kOtaTlsOutContentB = 4096;
inline constexpr std::size_t kOtaTlsPayloadOverheadMaxB = 2048;
inline constexpr std::size_t kOtaTlsInternalHeaderB = 13;
inline constexpr std::size_t kOtaTlsInRecordBoundB =
    kOtaTlsInContentB + kOtaTlsPayloadOverheadMaxB + kOtaTlsInternalHeaderB;
inline constexpr std::size_t kOtaTlsOutRecordBoundB =
    kOtaTlsOutContentB + kOtaTlsPayloadOverheadMaxB + kOtaTlsInternalHeaderB;
inline constexpr std::size_t kOtaTlsRecordCapacityB =
    kOtaTlsInRecordBoundB > kOtaTlsOutRecordBoundB ? kOtaTlsInRecordBoundB : kOtaTlsOutRecordBoundB;
struct OtaRecoveryRequirements {
    std::size_t entry_bytes;
    std::size_t tls_record_bytes;
};
inline constexpr OtaRecoveryRequirements kOtaRecoveryRequirements{
    kOtaRecoveryEntryRequiredB, kOtaTlsRecordCapacityB};
inline constexpr std::size_t ota_recovery_required_bytes_for(
        OtaRecoveryUse use, OtaRecoveryRequirements requirements = kOtaRecoveryRequirements) noexcept {
    switch (use) {
        case OtaRecoveryUse::ImageConfirmation:
        case OtaRecoveryUse::OperationEntry: return requirements.entry_bytes;
        case OtaRecoveryUse::TlsAdmission: return requirements.tls_record_bytes;
    }
    return 0;
}
inline constexpr bool ota_recovery_requirement_valid(std::size_t required) noexcept {
    return required != 0;
}
inline constexpr bool ota_recovery_memory_affordable(std::size_t internal_largest,
                                                     std::size_t required) noexcept {
    return ota_recovery_requirement_valid(required) && internal_largest >= required;
}
} // namespace tk
