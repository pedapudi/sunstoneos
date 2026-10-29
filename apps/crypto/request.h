#ifndef SUNSTONE_CRYPTO_REQUEST_H
#define SUNSTONE_CRYPTO_REQUEST_H
#include "crypto_session.h"
#include <stddef.h>
#define CRYPTO_REQUEST_MAGIC UINT64_C(0x4153373343525131)
#define CRYPTO_INPUT_MAX 2048u
#define CRYPTO_REQUEST_HEADER 48u
#define CRYPTO_REQUEST_V1_MAX (CRYPTO_REQUEST_HEADER+CRYPTO_INPUT_MAX)
#define CRYPTO_REQUEST_MAX 2824u
enum crypto_operation { CRYPTO_SHA256=1,CRYPTO_AES_ENCRYPT=2,CRYPTO_AES_DECRYPT=3,CRYPTO_HASH=4,CRYPTO_HMAC=5,CRYPTO_CIPHER_ENCRYPT=6,CRYPTO_CIPHER_DECRYPT=7 };
struct crypto_request {
 uint64_t magic;
 uint32_t version,operation;
 uint64_t id;
 uint32_t bytes,reserved;
 uint8_t key[16],input[CRYPTO_INPUT_MAX];
 uint32_t method,mode,key_bytes,iv_bytes,aad_bytes,tag_bytes;
 uint8_t secret[128],iv[64],aad[512];
};
_Static_assert(offsetof(struct crypto_request,input)==CRYPTO_REQUEST_HEADER&&
 offsetof(struct crypto_request,method)==2096&&offsetof(struct crypto_request,secret)==2120&&
 offsetof(struct crypto_request,iv)==2248&&offsetof(struct crypto_request,aad)==2312&&
 sizeof(struct crypto_request)==CRYPTO_REQUEST_MAX,"Crypto request wire layout");
/* Call only before release-publishing the one-shot request. No provider access.
 * Copies the complete bytes before validating; caller keeps output private on
 * failure. Source need not survive acceptance. Session is the retained copy. */
int crypto_request_copy(struct crypto_request *,const struct crypto_session *,const void *,size_t);
void crypto_key_clear(uint8_t[16]);
void crypto_secret_clear(void *,size_t);
void crypto_request_clear(struct crypto_request *);
unsigned crypto_request_output(const struct crypto_request *);
#endif
