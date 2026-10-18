#ifndef SUNSTONE_MONITOR_SNAPSHOT_H
#define SUNSTONE_MONITOR_SNAPSHOT_H
#include "monitor_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int monitor_session_prepare(const struct a733_smp_contract *);
const struct monitor_session *monitor_admitted_session(void);
#endif
