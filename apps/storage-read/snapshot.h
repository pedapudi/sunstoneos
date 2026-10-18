#ifndef SUNSTONE_STORAGE_READ_SNAPSHOT_H
#define SUNSTONE_STORAGE_READ_SNAPSHOT_H
#include "storage_read_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int storage_read_session_prepare(const struct a733_smp_contract *);
const struct storage_read_session *storage_read_admitted_session(void);
#endif
