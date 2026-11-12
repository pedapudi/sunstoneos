#ifndef SUNSTONE_UMEM_CPU_H
#define SUNSTONE_UMEM_CPU_H
/* The EL1 instructions behind the ledger's hooks (include/umem.h, include/umem_policy.h). */
#include "umem.h"
#include "umem_policy.h"

/* Page-table stores, barriers, TLBI VAAE1IS and DC CIVAC (through aarch64_device_cache). */
extern const struct umem_cpu_ops umem_cpu_el1;
/* The ledger's maintain hook: zero by stores, clean and clean-and-invalidate through aarch64_device_cache, and the
 * barriers `DMB OSHST` and `DMB OSHLD`. Runs at EL1 with the MMU and data cache on. */
int umem_maintain_el1(void *ctx, enum umem_op op, uint64_t base, uint64_t bytes);
#endif
