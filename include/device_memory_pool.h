#ifndef SUNSTONE_DEVICE_MEMORY_POOL_H
#define SUNSTONE_DEVICE_MEMORY_POOL_H
#include <stddef.h>
#include <stdint.h>

#define DEVICE_MEMORY_PAGE 4096u
#define DEVICE_MEMORY_MAX_PAGES 196608u /* 768 MiB; ledger size is independent. */
#define DEVICE_MEMORY_MAX_ALLOCATIONS 32u

enum device_memory_state { DEVICE_MEMORY_FREE, DEVICE_MEMORY_CPU,
                           DEVICE_MEMORY_DEVICE, DEVICE_MEMORY_RETAINED };
/* A class fixes the minimum alignment of every extent acquired from a pool.
 * NONE keeps the caller's alignment; the local-layout pools, the extended
 * `uncached` pool and pools built inside a lease use it. DMA32 follows the GPU
 * MMU's largest pages: 2 MiB for requests of at least 2 MiB, 64 KiB for
 * requests of at least 64 KiB, otherwise 4 KiB. */
enum device_memory_class { DEVICE_MEMORY_CLASS_NONE, DEVICE_MEMORY_CLASS_DMA32 };
/* The CPU mapping of the whole backing range. Acquire cleans recycled pages of
 * a write-back pool; a Normal non-cacheable pool needs no cleaning. */
enum device_memory_type { DEVICE_MEMORY_WRITE_BACK, DEVICE_MEMORY_NON_CACHEABLE };
struct device_memory_lease { uintptr_t pool; uint64_t owner, generation; unsigned slot; };
struct device_memory_extent { uintptr_t cpu; uint64_t physical; size_t bytes; };
/* A slot. Acquire writes `owner`, `first`, `pages` and `state`, then publishes
 * `generation` with a release store; release clears `generation` first. While
 * a lease is held only its holder changes `state`, and nothing changes the
 * other fields, so describe can read a slot without the lock. */
struct device_memory_allocation {
    uint64_t owner, generation;
    unsigned first, pages;
    enum device_memory_state state;
};
/* Acquire, release and retain run inside the kernel critical section
 * (taskENTER_CRITICAL), which serializes every core; none may be called from
 * an interrupt handler. Describe takes no lock. Initialize a pool once, before
 * any other task can reach it, and never reset it to reclaim retained memory.
 * Ownership is cooperative: a task that can write this structure can corrupt
 * it.
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

/* Minimum alignment of an extent of `bytes` in a pool of class `c`. */
static inline size_t device_memory_class_alignment(enum device_memory_class c, size_t bytes)
{
    if (c != DEVICE_MEMORY_CLASS_DMA32) return DEVICE_MEMORY_PAGE;
    return bytes >= ((size_t)2 << 20) ? (size_t)2 << 20 :
           bytes >= ((size_t)64 << 10) ? (size_t)64 << 10 : DEVICE_MEMORY_PAGE;
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
 * returns; if that clean is refused, the slot is released and acquire refuses.
 * That clean uses EL1-only instructions, so acquire on a write-back pool must
 * run at EL1. */
int device_memory_acquire(struct device_memory_pool *, uint64_t owner, size_t bytes,
                          struct device_memory_lease *, struct device_memory_extent *);
/* Power-of-two physical/CPU alignment, at least 4096 bytes, raised to the
 * class minimum. Both aliases must have the same alignment phase. Searches
 * occupied extents, never every page; placement is first fit. */
int device_memory_acquire_aligned(struct device_memory_pool *, uint64_t owner, size_t bytes,
    size_t alignment, struct device_memory_lease *, struct device_memory_extent *);
/* The extent and state of a held lease, read without the lock: a check per
 * device access costs a few loads and one load barrier. */
int device_memory_describe(const struct device_memory_pool *, struct device_memory_lease,
                           struct device_memory_extent *, enum device_memory_state *);
/* Release frees a CPU-owned slot. Retain marks a slot RETAINED from any held
 * state and succeeds again on a retained slot. Nothing in the ledger frees a
 * retained slot. */
int device_memory_release(struct device_memory_pool *, struct device_memory_lease);
int device_memory_retain(struct device_memory_pool *, struct device_memory_lease);
/* Consumer implementation helper: the slot a valid lease names, or NULL. It
 * takes no lock. The holder changes its own slot's `state` through it, between
 * CPU and DEVICE or to RETAINED, serialized under the holder's service lock.
 * These writes are not checked against any transition rule, so the ledger
 * does not enforce that a retained slot stays retained. A holder never writes
 * FREE or any other field. Only a concrete device controller may return
 * DEVICE to CPU after its completion and cache protocol. */
struct device_memory_allocation *device_memory_lookup(struct device_memory_pool *,
                                                      struct device_memory_lease);
#endif
