#pragma once

#include <stddef.h>
#include <stdint.h>

// Return/cleanup seam only. The firmware compiles against the actual pinned PSA
// declarations; this tiny stub proves neither their ABI nor real cryptography.
typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
typedef struct { uint32_t active; } psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)

#ifdef __cplusplus
extern "C" {
#endif
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_setup(psa_hash_operation_t*, psa_algorithm_t);
psa_status_t psa_hash_update(psa_hash_operation_t*, const uint8_t*, size_t);
psa_status_t psa_hash_verify(psa_hash_operation_t*, const uint8_t*, size_t);
psa_status_t psa_hash_abort(psa_hash_operation_t*);
#ifdef __cplusplus
}
#endif
