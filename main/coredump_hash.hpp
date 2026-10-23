#pragma once

#include "logic/coredump_image.hpp"
#include <psa/crypto.h>

namespace tk {
namespace coredump_detail {

// A diagnostic image contains private stack data. Clear the bounded read buffers on
// every return/exception, independently of the PSA operation's cleanup status.
struct HashBuffers {
    uint8_t chunk[256]{};
    uint8_t trailer[32]{};
    ~HashBuffers() noexcept {
        volatile uint8_t* p = chunk;
        for (unsigned i = 0; i < sizeof(chunk); ++i) p[i] = 0;
        p = trailer;
        for (unsigned i = 0; i < sizeof(trailer); ++i) p[i] = 0;
    }
};

// Construct only after successful psa_crypto_init. Abort is attempted exactly once;
// an abort error is uncertainty, never a claim that physical cleanup succeeded.
class HashOperation {
public:
    HashOperation() noexcept = default;
    HashOperation(const HashOperation&) = delete;
    HashOperation& operator=(const HashOperation&) = delete;
    ~HashOperation() noexcept {
        if (armed_) {
            armed_ = false;
            try { (void)psa_hash_abort(&operation_); } catch (...) {}
        }
    }
    psa_hash_operation_t* get() noexcept { return &operation_; }
    bool close() {
        if (!armed_) return false;
        armed_ = false;                    // a throw/error cannot cause a retry
        return psa_hash_abort(&operation_) == PSA_SUCCESS;
    }

private:
    psa_hash_operation_t operation_ = PSA_HASH_OPERATION_INIT;
    bool armed_{true};
};

}  // namespace coredump_detail

// diag_crash.cpp owns the exception boundary and partition callback. Admission,
// checksum and parsing share this same checked input and its finite work budget.
template <class Input>
bool verify_coredump_checksum(Input& input, const CoredumpHeader& outer) {
    if (!outer.readable || !input.image_ready() || input.failed() ||
        outer.length != input.length() || outer.length < kCoredumpChecksumBytes) return false;
    coredump_detail::HashBuffers buffers;
    if (psa_crypto_init() != PSA_SUCCESS) return false;  // no abort before init
    coredump_detail::HashOperation operation;
    bool valid = psa_hash_setup(operation.get(), PSA_ALG_SHA_256) == PSA_SUCCESS;
    const uint32_t body_bytes = outer.length - kCoredumpChecksumBytes;
    if (valid) valid = input.read(body_bytes, buffers.trailer, sizeof(buffers.trailer));
    for (uint32_t offset = 0; valid && offset < body_bytes;) {
        const uint32_t remaining = body_bytes - offset;
        const uint32_t length = remaining < sizeof(buffers.chunk) ? remaining : sizeof(buffers.chunk);
        valid = input.read(offset, buffers.chunk, length);
        if (valid) valid = psa_hash_update(operation.get(), buffers.chunk, length) == PSA_SUCCESS;
        offset += length;
    }
    if (valid) valid = psa_hash_verify(operation.get(), buffers.trailer, sizeof(buffers.trailer)) == PSA_SUCCESS;
    const bool closed = operation.close();           // also on every normal error path
    return valid && closed && !input.failed();
}

}  // namespace tk
