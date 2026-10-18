#ifndef SUNSTONE_I2S_SNAPSHOT_H
#define SUNSTONE_I2S_SNAPSHOT_H
#include "session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int i2s_session_prepare(const struct a733_smp_contract *);
const struct i2s_session *i2s_admitted_session(void);
#endif
