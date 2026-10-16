#ifndef SUNSTONE_DEVICE_MEMORY_POOL_H
#define SUNSTONE_DEVICE_MEMORY_POOL_H
#include <stddef.h>
#include <stdint.h>

#define DEVICE_MEMORY_PAGE 4096u
#define DEVICE_MEMORY_MAX_PAGES 512u
#define DEVICE_MEMORY_MAX_ALLOCATIONS 32u

enum device_memory_state { DEVICE_MEMORY_FREE, DEVICE_MEMORY_CPU,
                           DEVICE_MEMORY_DEVICE, DEVICE_MEMORY_RETAINED };
struct device_memory_lease { uintptr_t pool; uint64_t owner, generation; unsigned slot; };
struct device_memory_extent { uintptr_t cpu; uint64_t physical; size_t bytes; };
struct device_memory_allocation {
    uint64_t owner, generation;
    unsigned first, pages;
    enum device_memory_state state;
};
/* Privileged implementation state. Serialize every operation and all consumer
 * transitions under one service lock. Initialize once; never reset to reclaim
 * retained memory. This is cooperative ownership, not protection from a task
 * that can write privileged metadata or access another CPU mapping. */
struct device_memory_pool {
    struct device_memory_extent backing;
    uint64_t next_generation;
    struct device_memory_allocation allocations[DEVICE_MEMORY_MAX_ALLOCATIONS];
    unsigned initialized;
};
/* Portable core for an already reserved region with one immutable CPU memory
 * attribute. Each native initializer obtains its region from linker symbols
 * and verifies that attribute and its translations.
 * This method neither discovers RAM nor admits arbitrary ranges on the board. */
int device_memory_pool_init(struct device_memory_pool *, uintptr_t, uint64_t, size_t);
int device_memory_acquire(struct device_memory_pool *, uint64_t owner, size_t bytes,
                          struct device_memory_lease *, struct device_memory_extent *);
int device_memory_describe(const struct device_memory_pool *, struct device_memory_lease,
                           struct device_memory_extent *, enum device_memory_state *);
int device_memory_release(struct device_memory_pool *, struct device_memory_lease);
int device_memory_retain(struct device_memory_pool *, struct device_memory_lease);
/* Consumer implementation helper. A valid lease identifies one complete
 * allocation. Only a concrete device controller may return DEVICE to CPU after
 * its completion/cache protocol; no public boolean-drain release is supplied. */
struct device_memory_allocation *device_memory_lookup(struct device_memory_pool *,
                                                      struct device_memory_lease);
#endif
