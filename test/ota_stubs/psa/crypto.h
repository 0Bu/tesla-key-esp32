#pragma once
// Passive PSA seam for the actual OTA translation units. Hash bytes use the host OS crypto
// implementation; failure injection covers resource/cleanup control, not target PSA/ABI proof.
#include <array>
#include <cstddef>
#include <cstdint>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#if __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#else
// OpenSSL 3 runtime ABI; the pinned IDF image provides libcrypto.so.3 without dev headers.
extern "C" {
struct evp_md_ctx_st; struct evp_md_st; struct engine_st;
using EVP_MD_CTX = evp_md_ctx_st;
using EVP_MD = evp_md_st;
EVP_MD_CTX* EVP_MD_CTX_new();
void EVP_MD_CTX_free(EVP_MD_CTX*);
const EVP_MD* EVP_sha256();
int EVP_DigestInit_ex(EVP_MD_CTX*, const EVP_MD*, engine_st*);
int EVP_DigestUpdate(EVP_MD_CTX*, const void*, std::size_t);
int EVP_DigestFinal_ex(EVP_MD_CTX*, unsigned char*, unsigned int*);
}
#endif
#endif

using psa_status_t = int32_t;
using psa_algorithm_t = uint32_t;
inline constexpr psa_status_t PSA_SUCCESS = 0, PSA_ERROR_GENERIC_ERROR = -132,
    PSA_ERROR_NOT_SUPPORTED = -134, PSA_ERROR_INVALID_ARGUMENT = -135,
    PSA_ERROR_BAD_STATE = -137, PSA_ERROR_INSUFFICIENT_MEMORY = -141;
inline constexpr psa_algorithm_t PSA_ALG_SHA_256 = 0x02000009;
struct psa_hash_operation_t {
#if defined(__APPLE__)
    CC_SHA256_CTX context{};
#else
    EVP_MD_CTX* context{nullptr};
#endif
    bool active{false};
};
#define PSA_HASH_OPERATION_INIT {}

namespace ota_hash_fixture {
enum Call : size_t { None, Init, Compute, Setup, Update, Finish, Abort };
inline Call fail_call{None};
inline unsigned fail_occurrence{1};
inline psa_status_t fail_status{PSA_ERROR_INSUFFICIENT_MEMORY};
inline std::array<unsigned, 7> calls{};
inline psa_status_t injected(Call call) {
    return ++calls[call] == fail_occurrence && fail_call == call ? fail_status : PSA_SUCCESS;
}
inline void reset() { calls.fill(0); fail_call = None; fail_occurrence = 1; fail_status = PSA_ERROR_INSUFFICIENT_MEMORY; }
inline void dispose(psa_hash_operation_t* op) {
#if !defined(__APPLE__)
    EVP_MD_CTX_free(op->context); op->context = nullptr;
#endif
    op->active = false;
}
inline psa_status_t begin(psa_hash_operation_t* op) {
    if (!op || op->active) return PSA_ERROR_BAD_STATE;
#if defined(__APPLE__)
    op->active = CC_SHA256_Init(&op->context) == 1;
#else
    op->context = EVP_MD_CTX_new();
    if (!op->context) return PSA_ERROR_INSUFFICIENT_MEMORY;
    op->active = true; // A failing DigestInit still owns the context until abort.
    if (EVP_DigestInit_ex(op->context, EVP_sha256(), nullptr) != 1) return PSA_ERROR_GENERIC_ERROR;
#endif
    return op->active ? PSA_SUCCESS : PSA_ERROR_GENERIC_ERROR;
}
inline psa_status_t append(psa_hash_operation_t* op, const uint8_t* data, size_t bytes) {
    if (!op || !op->active || (!data && bytes)) return PSA_ERROR_BAD_STATE;
#if defined(__APPLE__)
    if (bytes > UINT32_MAX) return PSA_ERROR_INVALID_ARGUMENT;
    const bool ok = !bytes || CC_SHA256_Update(&op->context, data, static_cast<CC_LONG>(bytes)) == 1;
#else
    const bool ok = !bytes || EVP_DigestUpdate(op->context, data, bytes) == 1;
#endif
    return ok ? PSA_SUCCESS : PSA_ERROR_GENERIC_ERROR;
}
inline psa_status_t finish(psa_hash_operation_t* op, uint8_t* out, size_t capacity, size_t* written) {
    if (!op || !op->active || !out || capacity < 32 || !written) return PSA_ERROR_BAD_STATE;
#if defined(__APPLE__)
    const bool ok = CC_SHA256_Final(out, &op->context) == 1;
    *written = ok ? 32 : 0;
#else
    unsigned n = 0;
    const bool ok = EVP_DigestFinal_ex(op->context, out, &n) == 1;
    *written = n;
#endif
    if (ok) dispose(op);
    return ok ? PSA_SUCCESS : PSA_ERROR_GENERIC_ERROR;
}
}
inline psa_status_t psa_crypto_init() { return ota_hash_fixture::injected(ota_hash_fixture::Init); }
inline psa_status_t psa_hash_setup(psa_hash_operation_t* op, psa_algorithm_t algorithm) {
    const auto status = ota_hash_fixture::injected(ota_hash_fixture::Setup);
    return status != PSA_SUCCESS ? status : algorithm != PSA_ALG_SHA_256 ? PSA_ERROR_NOT_SUPPORTED : ota_hash_fixture::begin(op);
}
inline psa_status_t psa_hash_update(psa_hash_operation_t* op, const uint8_t* data, size_t bytes) {
    const auto status = ota_hash_fixture::injected(ota_hash_fixture::Update);
    return status != PSA_SUCCESS ? status : ota_hash_fixture::append(op, data, bytes);
}
inline psa_status_t psa_hash_finish(psa_hash_operation_t* op, uint8_t* out, size_t capacity, size_t* written) {
    const auto status = ota_hash_fixture::injected(ota_hash_fixture::Finish);
    return status != PSA_SUCCESS ? status : ota_hash_fixture::finish(op, out, capacity, written);
}
inline psa_status_t psa_hash_abort(psa_hash_operation_t* op) {
    const auto status = ota_hash_fixture::injected(ota_hash_fixture::Abort);
    if (status != PSA_SUCCESS) return status;
    if (!op) return PSA_ERROR_INVALID_ARGUMENT;
    ota_hash_fixture::dispose(op); return PSA_SUCCESS;
}
inline psa_status_t psa_hash_compute(psa_algorithm_t algorithm, const uint8_t* data, size_t bytes,
                                    uint8_t* out, size_t capacity, size_t* written) {
    const auto status = ota_hash_fixture::injected(ota_hash_fixture::Compute);
    if (status != PSA_SUCCESS) return status;
    if (algorithm != PSA_ALG_SHA_256) return PSA_ERROR_NOT_SUPPORTED;
    psa_hash_operation_t op{};
    auto result = ota_hash_fixture::begin(&op);
    if (result == PSA_SUCCESS) result = ota_hash_fixture::append(&op, data, bytes);
    if (result == PSA_SUCCESS) result = ota_hash_fixture::finish(&op, out, capacity, written);
    ota_hash_fixture::dispose(&op);
    return result;
}
