#ifndef SUNSTONE_THERMAL_SNAPSHOT_H
#define SUNSTONE_THERMAL_SNAPSHOT_H
#include "thermal_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int thermal_session_prepare(const struct a733_smp_contract *);
const struct thermal_session *thermal_admitted_session(void);
#endif
