#ifndef SUNSTONE_OS_IOMMU_H
#define SUNSTONE_OS_IOMMU_H
/* The OS image's IOMMU (drivers/iommu/a733_iommu_owner.h, docs/iommu.md): one owner for every master an OS service
 * drives. A service attaches its masters, from its own owner task, while their power domain is on and their clocks
 * run, and detaches them from its close or quiesce path after draining them; the last detach disables both banks and
 * returns the IOMMU clock lease. Device addresses come from os_iommu_iova, so that every master's spans share the one
 * directory without overlap. Calls are serialized and bounded; each returns 1 done or -1 refused or failed. */
#include "../../drivers/iommu/a733_iommu_owner.h"

/* Reserve bytes (a page multiple) of device address space below A733_IOMMU_IOVA_LIMIT, aligned to align (a power of
 * two, at least a page). The space is handed out in address order and reused from its start once every master has
 * detached. */
int os_iommu_iova(uint64_t bytes, uint64_t align, uint64_t *iova);
int os_iommu_attach(uint16_t masters, const struct a733_iommu_span *, unsigned count);
int os_iommu_map(const struct a733_iommu_span *, unsigned count);
int os_iommu_unmap(const struct a733_iommu_span *, unsigned count);
int os_iommu_detach(uint16_t masters);
/* Read master's fault words into out: 1 none, 0 a fault recorded, -1 failed. */
int os_iommu_scan(unsigned master, struct a733_iommu_owner_fault *out);
/* The owner's records (state, entry words, fault step), for a service's result; read between calls. */
const struct a733_iommu_owner *os_iommu_owner(void);
#endif
