#ifndef SUNSTONE_DEVICE_MEMORY_POOL_H
#define SUNSTONE_DEVICE_MEMORY_POOL_H
#include <stddef.h>
#include <stdint.h>

#define DEVICE_MEMORY_PAGE 4096u
#define DEVICE_MEMORY_MAX_PAGES 196608u /* 768 MiB; ledger size is independent. */
#define DEVICE_MEMORY_MAX_ALLOCATIONS 32u

enum device_memory_state { DEVICE_MEMORY_FREE, DEVICE_MEMORY_CPU,
                           DEVICE_MEMORY_DEVICE, DEVICE_MEMORY_RETAINED };
/* A class fixes the minimum alignment of every extent acquired from a pool,
 * following the largest page of the device MMU that maps it. NONE keeps the
 * caller's alignment; local-layout pools and pools built inside a lease use it. */
enum device_memory_class { DEVICE_MEMORY_CLASS_NONE, DEVICE_MEMORY_CLASS_DMA32,
                           DEVICE_MEMORY_CLASS_UNCACHED, DEVICE_MEMORY_CLASS_GPU,
                           DEVICE_MEMORY_CLASS_NPU };
/* The CPU mapping of the whole backing range. Acquire cleans recycled pages of
 * a write-back pool; a Normal non-cacheable pool needs no cleaning. */
enum device_memory_type { DEVICE_MEMORY_WRITE_BACK, DEVICE_MEMORY_NON_CACHEABLE };
struct device_memory_lease { uintptr_t pool; uint64_t owner, generation; unsigned slot; };
struct device_memory_extent { uintptr_t cpu; uint64_t physical; size_t bytes; };
struct device_memory_allocation {
    uint64_t owner, generation;
    unsigned first, pages;
    enum device_memory_state state;
};
/* Every operation except initialization runs inside the kernel critical
 * section (taskENTER_CRITICAL), which serializes all cores; none may be called
 * from an interrupt handler. Initialize once, before any other task can reach
 * the pool, and never reset it to reclaim retained memory. Ownership is
 * cooperative: a task that can write this structure can corrupt it.
 *
 * `clean_end` is the end, in pages, of the highest page ever handed out. A
 * page at or above it has never been handed out and holds no dirty line, so
 * acquire cleans only the part of an extent below it. */
struct device_memory_pool {
    struct device_memory_extent backing;
    uint64_t next_generation;
    struct device_memory_allocation allocations[DEVICE_MEMORY_MAX_ALLOCATIONS];
    unsigned initialized;
    enum device_memory_class memory_class;
    enum device_memory_type memory_type;
    unsigned clean_end;
};

/* Minimum alignment of an extent of `bytes` in a pool of class `c`.
 * GPU and DMA32: 2 MiB from 2 MiB, 64 KiB from 64 KiB, otherwise 4 KiB; the
 * GPU MMU maps 4 KiB, 64 KiB and 2 MiB pages. NPU: 16 MiB from 16 MiB,
 * otherwise 1 MiB; the NPU MMU maps 4 KiB, 64 KiB, 1 MiB and 16 MiB pages.
 * UNCACHED and NONE: 4 KiB. */
static inline size_t device_memory_class_alignment(enum device_memory_class c, size_t bytes)
{
    switch (c) {
    case DEVICE_MEMORY_CLASS_GPU: case DEVICE_MEMORY_CLASS_DMA32:
        return bytes >= ((size_t)2 << 20) ? (size_t)2 << 20 :
               bytes >= ((size_t)64 << 10) ? (size_t)64 << 10 : DEVICE_MEMORY_PAGE;
    case DEVICE_MEMORY_CLASS_NPU:
        return bytes >= ((size_t)16 << 20) ? (size_t)16 << 20 : (size_t)1 << 20;
    default:
        return DEVICE_MEMORY_PAGE;
    }
}

/* Portable core for an already reserved region with one immutable CPU memory
 * attribute. Each native initializer obtains its region from linker symbols
 * and verifies that attribute and its translations. It neither discovers RAM
 * nor admits arbitrary ranges on the board. The plain form creates a
 * write-back pool of class NONE. A pool built inside a lease is initialized by
 * the lease holder directly after acquire, before it writes the lease. */
int device_memory_pool_init(struct device_memory_pool *, uintptr_t, uint64_t, size_t);
int device_memory_pool_init_typed(struct device_memory_pool *, uintptr_t, uint64_t, size_t,
                                  enum device_memory_class, enum device_memory_type);
/* Acquire returns a CPU-owned extent at 4 KiB alignment raised to the class
 * minimum. On a write-back pool, the part of the extent that an earlier holder
 * can have left dirty is cleaned to the point of coherency before acquire
 * returns; if that clean is refused, the slot is released and acquire refuses. */
int device_memory_acquire(struct device_memory_pool *, uint64_t owner, size_t bytes,
                          struct device_memory_lease *, struct device_memory_extent *);
/* Power-of-two physical/CPU alignment, at least 4096 bytes, raised to the
 * class minimum. Both aliases must have the same alignment phase. Searches
 * occupied extents, never every page; placement is first fit. */
int device_memory_acquire_aligned(struct device_memory_pool *, uint64_t owner, size_t bytes,
    size_t alignment, struct device_memory_lease *, struct device_memory_extent *);
/* As acquire_aligned, and the whole extent then reads as zero to the CPU and
 * to a device: DC ZVA over the extent, followed by a clean of the whole extent
 * on a write-back pool. Refuses unless DC ZVA is permitted with 64-byte
 * blocks. Use it instead of memset over a fresh extent. */
int device_memory_acquire_zeroed(struct device_memory_pool *, uint64_t owner, size_t bytes,
    size_t alignment, struct device_memory_lease *, struct device_memory_extent *);
int device_memory_describe(const struct device_memory_pool *, struct device_memory_lease,
                           struct device_memory_extent *, enum device_memory_state *);
/* Accepted changes; every other request is refused and leaves the slot
 * unchanged:
 *   set_state(DEVICE)  from CPU
 *   set_state(CPU)     from DEVICE
 *   retain             from CPU or DEVICE; from RETAINED it succeeds unchanged
 *   release            from CPU, to FREE
 * Nothing leaves RETAINED. The lease holder maintains the caches of its write
 * sets around each transition; the ledger records ownership only. */
int device_memory_set_state(struct device_memory_pool *, struct device_memory_lease,
                            enum device_memory_state);
int device_memory_release(struct device_memory_pool *, struct device_memory_lease);
int device_memory_retain(struct device_memory_pool *, struct device_memory_lease);
/* Transitional: returns the slot without entering the critical section.
 * Remaining callers write `state` directly; each such write becomes a
 * device_memory_set_state() call, after which this function is removed.
 * New code must not call it. */
struct device_memory_allocation *device_memory_lookup(struct device_memory_pool *,
                                                      struct device_memory_lease);
#endif
