#pragma once

#include <psa/crypto.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace tk::ota {
using Digest = std::array<std::uint8_t, 32>;

// The existing pinned PSA provider owns SHA-256; this adds no key, algorithm or trust anchor.
inline psa_status_t sha256(const void* data, std::size_t bytes, Digest& out) noexcept {
    out.fill(0);
    auto status = psa_crypto_init();
    std::size_t written = 0;
    if (status == PSA_SUCCESS)
        status = psa_hash_compute(PSA_ALG_SHA_256, static_cast<const uint8_t*>(data), bytes,
                                 out.data(), out.size(), &written);
    if (status == PSA_SUCCESS && written != out.size()) status = PSA_ERROR_GENERIC_ERROR;
    if (status != PSA_SUCCESS) out.fill(0);
    return status;
}
} // namespace tk::ota
