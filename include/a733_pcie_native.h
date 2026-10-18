#ifndef SUNSTONE_A733_PCIE_NATIVE_H
#define SUNSTONE_A733_PCIE_NATIVE_H
#include "a733_pcie.h"
struct a733_platform_locks;
struct a733_pcie_native {
    struct a733_pcie_ops operations;
    struct a733_platform_locks *locks;
    struct a733_pcie *port;
    void *owner_task,*admission_cookie;
    int (*admit)(void *,const struct a733_pcie_config *);
    unsigned initialized;
};
/* Bounded address/width filter; Device mappings and authenticated physical
 * admission are supplied by the native boot owner. No endpoint writes, global
 * SERDES reset, shared parent changes, PCK policy or rail writes are allowed.
 */
int a733_pcie_native_address(uintptr_t,unsigned width,unsigned writing);
int a733_pcie_native_init(struct a733_pcie_native *,struct a733_pcie *,
    struct a733_platform_locks *,void *owner_task,
    int (*admit)(void *,const struct a733_pcie_config *),void *);
#endif
