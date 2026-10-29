#ifndef SUNSTONE_CRYPTO_RUN_H
#define SUNSTONE_CRYPTO_RUN_H
#include "a733_ce_hash.h"
#include "a733_ce_cipher.h"
#include "crypto_session.h"
#include "request.h"
struct crypto_run {
 struct a733_ce_hash hash;
 struct a733_ce_cipher cipher;
 uint64_t generation;
 unsigned stage;
 struct crypto_request request;
 unsigned runtime,output_bytes,input_offset;
 uint8_t oracle[32],payload[CRYPTO_INPUT_MAX];
};
/* Caller serializes all steps and retention with the actual pool mutex.
 * 1 means the private output is complete; publication checks an optional
 * runtime oracle or the diagnostic reference, then current authority. */
void crypto_run_clear_clients(struct crypto_run *);
void crypto_run_clear_secrets(struct crypto_run *);
int crypto_run_step(struct crypto_run *,uint64_t);
/* Copy is private until the final eligibility callback and release publication.
 * Caller retains the engine on -1; this helper performs no healthy CE work. */
int crypto_run_publish(const struct crypto_run *,uint8_t *,uint8_t[32],atomic_uint *,int (*)(void *),void *);

#endif
