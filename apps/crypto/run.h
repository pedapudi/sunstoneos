#ifndef SUNSTONE_CRYPTO_RUN_H
#define SUNSTONE_CRYPTO_RUN_H
#include "a733_ce_hash.h"
#include "a733_ce_cipher.h"
#include "crypto_session.h"
struct crypto_run {
 struct a733_ce_hash hash;
 struct a733_ce_cipher cipher;
 uint64_t generation;
 unsigned stage;
 uint8_t payload[160];
};
/* Caller serializes all steps and retention with the actual pool mutex.
 * 1 means all private results match; caller must check authority again before publication. */
int crypto_run_step(struct crypto_run *,uint64_t);
/* Copy is private until the final eligibility callback and release publication.
 * Caller retains the engine on -1; this helper performs no healthy CE work. */
int crypto_run_publish(const struct crypto_run *,uint8_t[160],uint8_t[32],atomic_uint *,int (*)(void *),void *);

#endif
