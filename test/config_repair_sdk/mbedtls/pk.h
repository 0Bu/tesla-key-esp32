#pragma once
#include <psa/crypto.h>
// Opaque fixture state only; no claim about mbedTLS structure ABI or private-key parsing.
struct mbedtls_pk_context {bool initialized;};
extern "C" {
void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
int mbedtls_pk_parse_key(mbedtls_pk_context *ctx,const unsigned char *key,size_t keylen,const unsigned char *pwd,size_t pwdlen);
int mbedtls_pk_get_psa_attributes(const mbedtls_pk_context *pk,psa_key_usage_t usage,psa_key_attributes_t *attributes);
int mbedtls_pk_import_into_psa(const mbedtls_pk_context *pk,const psa_key_attributes_t *attributes,mbedtls_svc_key_id_t *key_id);
}
