#ifndef SUNSTONE_CRYPTO_SNAPSHOT_H
#define SUNSTONE_CRYPTO_SNAPSHOT_H
#include "crypto_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int crypto_session_prepare(const struct a733_smp_contract *);
const struct crypto_session *crypto_admitted_session(void);
#endif
