#pragma once
// Passive API-shape facade. These small structures are NOT real PSA layouts or crypto.
#include <stddef.h>
#include <stdint.h>
typedef int32_t psa_status_t;
typedef uint16_t psa_key_type_t;
typedef uint32_t psa_key_usage_t;
typedef uint32_t psa_key_lifetime_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t mbedtls_svc_key_id_t;
struct psa_key_attributes_t {psa_key_type_t type;size_t bits;psa_key_lifetime_t lifetime;};
struct psa_hash_operation_t {size_t token;};
#define PSA_SUCCESS 0
#define PSA_ERROR_GENERIC_ERROR -132
#define PSA_KEY_USAGE_DERIVE 0x4000u
#define PSA_KEY_LIFETIME_VOLATILE 0u
#define PSA_ECC_FAMILY_SECP_R1 0x12u
#define PSA_KEY_TYPE_ECC_KEY_PAIR(family) (static_cast<psa_key_type_t>(0x7100u|(family)))
#define PSA_ALG_SHA_256 0x02000009u
#define PSA_KEY_ATTRIBUTES_INIT {0,0,0}
#define PSA_HASH_OPERATION_INIT {0}
#define MBEDTLS_SVC_KEY_ID_INIT 0
extern "C" {
psa_status_t psa_crypto_init(void);
void psa_reset_key_attributes(psa_key_attributes_t *attributes);
psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key);
psa_status_t psa_export_public_key(mbedtls_svc_key_id_t key,uint8_t *data,size_t data_size,size_t *data_length);
psa_status_t psa_hash_setup(psa_hash_operation_t *operation,psa_algorithm_t alg);
psa_status_t psa_hash_update(psa_hash_operation_t *operation,const uint8_t *input,size_t input_length);
psa_status_t psa_hash_finish(psa_hash_operation_t *operation,uint8_t *hash,size_t hash_size,size_t *hash_length);
psa_status_t psa_hash_abort(psa_hash_operation_t *operation);
}
inline void psa_set_key_lifetime(psa_key_attributes_t *a,psa_key_lifetime_t l){a->lifetime=l;}
inline psa_key_type_t psa_get_key_type(const psa_key_attributes_t *a){return a->type;}
inline size_t psa_get_key_bits(const psa_key_attributes_t *a){return a->bits;}
