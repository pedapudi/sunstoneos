#ifndef SUNSTONE_PCIE_SNAPSHOT_H
#define SUNSTONE_PCIE_SNAPSHOT_H
#include "pcie_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int pcie_session_prepare(const struct a733_smp_contract *);
const struct pcie_session *pcie_admitted_session(void);
#endif
