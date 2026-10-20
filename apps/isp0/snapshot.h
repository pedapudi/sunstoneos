#ifndef SUNSTONE_ISP0_SNAPSHOT_H
#define SUNSTONE_ISP0_SNAPSHOT_H
#include "a733_isp0_application.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int isp0_session_prepare(const struct a733_smp_contract *);
const struct a733_isp0_session *isp0_admitted_session(void);
const uint8_t *isp0_admitted_session_digest(void);
#endif
