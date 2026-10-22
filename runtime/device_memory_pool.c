#include "device_memory_pool.h"
#include "a733_memory_critical.h"
#include "a733_range.h"
#if defined(__aarch64__) && !defined(__linux__)
#include "aarch64_device_memory.h"
#define DEVICE_MEMORY_NATIVE 1
#endif

#if defined(DEVICE_MEMORY_POOL_TRACE)
#if defined(DEVICE_MEMORY_NATIVE)
#error "DEVICE_MEMORY_POOL_TRACE is for host checks only"
#endif
/* Host checks observe the ledger: event 0 enters and 1 exits the lock, 2 is one
 * placement attempt and 3 cleans `value` bytes. */
void device_memory_trace(unsigned event, size_t value);
#define trace(event, value) device_memory_trace(event, value)
#else
#define trace(event, value) ((void)(value))
#endif
static void enter(void) { a733_memory_critical_enter(); trace(0, 0); }
static void leave(void) { trace(1, 0); a733_memory_critical_exit(); }

/* Slot fields that describe reads without the lock are accessed atomically. */
#define LOAD(field) __atomic_load_n(&(field), __ATOMIC_RELAXED)
#define STORE(field, value) __atomic_store_n(&(field), (value), __ATOMIC_RELAXED)

/* The overlap tests use the unchecked form: device_memory_pool_init() refuses
 * a pool or backing range that wraps, and output_ok() refuses a wrapping
 * output before any overlap test of that output. */
static int output_ok(const struct device_memory_pool *p, const void *out, size_t bytes)
{
    uintptr_t a = (uintptr_t)out;
    return out && a <= UINTPTR_MAX - bytes &&
        !a733_extents_overlap_prechecked(a, bytes, (uintptr_t)p, sizeof(*p)) &&
        !a733_extents_overlap_prechecked(a, bytes, p->backing.cpu, p->backing.bytes);
}

/* Clean [cpu, cpu + bytes) to the point of coherency. aarch64_device_cache()
 * starts with DSB SY, which orders the clean after this core's entry to the
 * critical section and so after the previous holder's release. */
static int clean(uintptr_t cpu, size_t bytes)
{
    trace(3, bytes);
#if defined(DEVICE_MEMORY_NATIVE)
    return aarch64_device_cache(AARCH64_CACHE_CLEAN, cpu, bytes);
#else
    (void)cpu;
    return 1;
#endif
}

int device_memory_pool_init_typed(struct device_memory_pool *p, uintptr_t cpu, uint64_t physical,
    size_t bytes, enum device_memory_class memory_class, enum device_memory_type memory_type)
{
    if (!p || p->initialized || !cpu || !bytes ||
        ((cpu | physical | bytes) & (DEVICE_MEMORY_PAGE - 1)) ||
        bytes > DEVICE_MEMORY_MAX_PAGES * DEVICE_MEMORY_PAGE ||
        cpu > UINTPTR_MAX - bytes || physical > UINT64_MAX - bytes ||
        (uintptr_t)p > UINTPTR_MAX - sizeof(*p) ||
        a733_extents_overlap_prechecked(cpu, bytes, (uintptr_t)p, sizeof(*p)) ||
        (unsigned)memory_class > DEVICE_MEMORY_CLASS_DMA32 ||
        (unsigned)memory_type > DEVICE_MEMORY_NON_CACHEABLE) return 0;
    *p = (struct device_memory_pool){.backing = {cpu, physical, bytes}, .next_generation = 1,
                                    .initialized = 1, .memory_class = memory_class,
                                    .memory_type = memory_type};
    return 1;
}
int device_memory_pool_init(struct device_memory_pool *p, uintptr_t cpu,
                            uint64_t physical, size_t bytes)
{
    return device_memory_pool_init_typed(p, cpu, physical, bytes, DEVICE_MEMORY_CLASS_NONE,
                                         DEVICE_MEMORY_WRITE_BACK);
}

static int lease_ok(const struct device_memory_pool *p, struct device_memory_lease l)
{
    return l.pool == (uintptr_t)p && l.owner && l.generation && l.slot < DEVICE_MEMORY_MAX_ALLOCATIONS;
}

/* The held slot a lease names, or NULL. The ledger calls it inside the
 * critical section. */
static struct device_memory_allocation *slot(struct device_memory_pool *p, struct device_memory_lease l)
{
    if (!lease_ok(p, l)) return NULL;
    struct device_memory_allocation *a = &p->allocations[l.slot];
    return a->state != DEVICE_MEMORY_FREE && a->owner == l.owner &&
           a->generation == l.generation ? a : NULL;
}

/* Free a slot. Clearing the generation first, ordered before the other
 * stores, makes a concurrent describe that read any cleared field refuse. */
static void free_slot(struct device_memory_allocation *a)
{
    STORE(a->generation, 0);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    STORE(a->owner, 0);
    STORE(a->first, 0);
    STORE(a->pages, 0);
    STORE(a->state, DEVICE_MEMORY_FREE);
}

struct device_memory_allocation *device_memory_lookup(struct device_memory_pool *p,
                                                      struct device_memory_lease l)
{
    return p && p->initialized ? slot(p, l) : NULL;
}

static struct device_memory_extent extent_of(const struct device_memory_pool *p, unsigned first,
                                             unsigned pages)
{
    size_t offset = (size_t)first * DEVICE_MEMORY_PAGE;
    return (struct device_memory_extent){p->backing.cpu + offset, p->backing.physical + offset,
                                         (size_t)pages * DEVICE_MEMORY_PAGE};
}

/* Lock-free: generations are never reused, acquire publishes a slot's fields
 * before its generation, and release clears the generation before the fields.
 * If the generation equals the lease's both before and after the copy, the
 * copy is the held slot's. */
int device_memory_describe(const struct device_memory_pool *p, struct device_memory_lease l,
                           struct device_memory_extent *out, enum device_memory_state *state)
{
    if (!p || !p->initialized || !lease_ok(p, l) || !output_ok(p, out, sizeof(*out)) ||
        (state && (!output_ok(p, state, sizeof(*state)) ||
         a733_extents_overlap_prechecked((uintptr_t)state, sizeof(*state), (uintptr_t)out, sizeof(*out))))) return 0;
    const struct device_memory_allocation *a = &p->allocations[l.slot];
    if (__atomic_load_n(&a->generation, __ATOMIC_ACQUIRE) != l.generation) return 0;
    uint64_t owner = LOAD(a->owner);
    unsigned first = LOAD(a->first), pages = LOAD(a->pages);
    enum device_memory_state s = LOAD(a->state);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (LOAD(a->generation) != l.generation || owner != l.owner || s == DEVICE_MEMORY_FREE) return 0;
    *out = extent_of(p, first, pages);
    if (state) *state = s;
    return 1;
}

/* First fit over occupied extents. Every jump passes at least one occupied
 * extent; alignment can skip more free pages, but never causes another scan of
 * the same blocking extent. Returns the first page, or UINT32_MAX. Call inside
 * the critical section. */
static unsigned place(const struct device_memory_pool *p, unsigned pages, size_t alignment)
{
    unsigned total = (unsigned)(p->backing.bytes / DEVICE_MEMORY_PAGE), first = 0;
    uint64_t address = p->backing.physical;
    for (unsigned attempt = 0; attempt <= DEVICE_MEMORY_MAX_ALLOCATIONS; ++attempt) {
        trace(2, 0);
        uint64_t delta = (-(uint64_t)address) & (alignment - 1);
        if (delta > p->backing.bytes || delta / DEVICE_MEMORY_PAGE > total - first) return UINT32_MAX;
        first += (unsigned)(delta / DEVICE_MEMORY_PAGE);
        if (first > total || pages > total - first) return UINT32_MAX;
        unsigned next = first;
        for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i) {
            const struct device_memory_allocation *a = &p->allocations[i];
            if (a->state != DEVICE_MEMORY_FREE && first < a->first + a->pages && a->first < first + pages &&
                next < a->first + a->pages) next = a->first + a->pages;
        }
        if (next == first) return first;
        first = next;
        if (first > total) return UINT32_MAX;
        address = p->backing.physical + (uint64_t)first * DEVICE_MEMORY_PAGE;
    }
    return UINT32_MAX;
}

int device_memory_acquire_aligned(struct device_memory_pool *p, uint64_t owner, size_t bytes,
    size_t alignment, struct device_memory_lease *lease, struct device_memory_extent *out)
{
    if (!p || !p->initialized || !owner || !bytes || !lease || !out ||
        alignment < DEVICE_MEMORY_PAGE || (alignment & (alignment - 1)) ||
        bytes > p->backing.bytes ||
        !output_ok(p, lease, sizeof(*lease)) || !output_ok(p, out, sizeof(*out)) ||
        a733_extents_overlap_prechecked((uintptr_t)lease, sizeof(*lease), (uintptr_t)out, sizeof(*out))) return 0;
    size_t minimum = device_memory_class_alignment(p->memory_class, bytes);
    if (alignment < minimum) alignment = minimum;
    if (alignment > (size_t)DEVICE_MEMORY_MAX_PAGES * DEVICE_MEMORY_PAGE ||
        ((p->backing.cpu ^ p->backing.physical) & (alignment - 1))) return 0;
    unsigned pages = (unsigned)((bytes + DEVICE_MEMORY_PAGE - 1) / DEVICE_MEMORY_PAGE);
    unsigned index = 0, first = UINT32_MAX, dirty = 0;
    uint64_t generation = 0;

    enter();
    while (index < DEVICE_MEMORY_MAX_ALLOCATIONS && p->allocations[index].state != DEVICE_MEMORY_FREE) ++index;
    if (index < DEVICE_MEMORY_MAX_ALLOCATIONS && p->next_generation)
        first = place(p, pages, alignment);
    if (first != UINT32_MAX) {
        struct device_memory_allocation *a = &p->allocations[index];
        generation = p->next_generation++;
        /* A describe that read a field stored below also observes, after its
         * load barrier, the generation cleared by the slot's last release. */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        STORE(a->owner, owner);
        STORE(a->first, first);
        STORE(a->pages, pages);
        STORE(a->state, DEVICE_MEMORY_CPU);
        __atomic_store_n(&a->generation, generation, __ATOMIC_RELEASE);
        /* Pages below the watermark can hold lines a previous holder left
         * dirty. */
        if (first < p->clean_end) dirty = p->clean_end - first < pages ? p->clean_end - first : pages;
        if (first + pages > p->clean_end) p->clean_end = first + pages;
    }
    leave();
    if (first == UINT32_MAX) return 0;

    struct device_memory_extent e = extent_of(p, first, pages);
    if (dirty && p->memory_type == DEVICE_MEMORY_WRITE_BACK &&
        !clean(e.cpu, (size_t)dirty * DEVICE_MEMORY_PAGE)) {
        /* No lease was handed out, so the slot is still this acquire's. */
        enter();
        free_slot(&p->allocations[index]);
        leave();
        return 0;
    }
    *lease = (struct device_memory_lease){(uintptr_t)p, owner, generation, index};
    *out = e;
    return 1;
}
int device_memory_acquire(struct device_memory_pool *p, uint64_t owner, size_t bytes,
                          struct device_memory_lease *lease, struct device_memory_extent *out)
{
    return device_memory_acquire_aligned(p, owner, bytes, DEVICE_MEMORY_PAGE, lease, out);
}

int device_memory_release(struct device_memory_pool *p, struct device_memory_lease l)
{
    if (!p || !p->initialized) return 0;
    enter();
    struct device_memory_allocation *a = slot(p, l);
    int done = a && a->state == DEVICE_MEMORY_CPU;
    if (done) free_slot(a);
    leave();
    return done;
}
int device_memory_retain(struct device_memory_pool *p, struct device_memory_lease l)
{
    if (!p || !p->initialized) return 0;
    enter();
    struct device_memory_allocation *a = slot(p, l);
    if (a) STORE(a->state, DEVICE_MEMORY_RETAINED);
    leave();
    return a != NULL;
}
