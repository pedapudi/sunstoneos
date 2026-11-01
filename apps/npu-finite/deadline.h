#ifndef SUNSTONE_NPU_FINITE_DEADLINE_H
#define SUNSTONE_NPU_FINITE_DEADLINE_H
#include "sunstone_deadline.h"
/* EL1 on the admitted boot core. EL3 installs and later restores PPI27. */
void npu_finite_deadline_setup(void);
#endif
