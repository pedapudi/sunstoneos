#include "device_memory_pool.h"
#include "a733_memory_critical.h"
#include "a733_range.h"
#if defined(__aarch64__) && !defined(__linux__)
#include "aarch64_device_memory.h"
#define DEVICE_MEMORY_NATIVE 1
#else
#include <string.h>
#endif

#if !defined(DEVICE_MEMORY_POOL_TRACE) || defined(A733_MEMORY_KERNEL_LOCK)
#define trace(event, value) ((void)(value))
#else
#define trace(event, value) device_memory_trace(event, value)
#endif

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
#if defined(DEVICE_MEMORY_NATIVE)
    return aarch64_device_cache(AARCH64_CACHE_CLEAN, cpu, bytes);
#else
    (void)cpu;
    trace(DEVICE_MEMORY_TRACE_CLEAN, bytes);
    return 1;
#endif
}

/* DC ZVA writes zeros to one 64-byte block per instruction without reading
 * it; the caller checked DCZID_EL0 for that block size. */
static void zero(uintptr_t cpu, size_t bytes)
{
#if defined(DEVICE_MEMORY_NATIVE)
    for (uintptr_t a = cpu, end = cpu + bytes; a < end; a += 64)
        __asm__ volatile("dc zva, %0" :: "r"(a) : "memory");
#else
    trace(DEVICE_MEMORY_TRACE_ZERO, bytes);
    memset((void *)cpu, 0, bytes);
#endif
}

static int zero_supported(void)
{
#if defined(DEVICE_MEMORY_NATIVE)
    uint64_t id;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(id));
    return !(id & 16) && (id & 15) == 4; /* Permitted; 2^4 words = 64 bytes. */
#else
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
        (unsigned)memory_class > DEVICE_MEMORY_CLASS_NPU ||
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

/* The slot a lease names, or NULL. Call inside the critical section. */
static struct device_memory_allocation *slot(const struct device_memory_pool *p,
                                             struct device_memory_lease l)
{
    if (l.pool != (uintptr_t)p || !l.owner || !l.generation ||
        l.slot >= DEVICE_MEMORY_MAX_ALLOCATIONS) return NULL;
    struct device_memory_allocation *a = (struct device_memory_allocation *)&p->allocations[l.slot];
    return a->state != DEVICE_MEMORY_FREE && a->owner == l.owner &&
           a->generation == l.generation ? a : NULL;
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

int device_memory_describe(const struct device_memory_pool *p, struct device_memory_lease l,
                           struct device_memory_extent *out, enum device_memory_state *state)
{
    if (!p || !p->initialized || !output_ok(p, out, sizeof(*out)) ||
        (state && (!output_ok(p, state, sizeof(*state)) ||
         a733_extents_overlap_prechecked((uintptr_t)state, sizeof(*state), (uintptr_t)out, sizeof(*out))))) return 0;
    a733_memory_critical_enter();
    const struct device_memory_allocation *a = slot(p, l);
    struct device_memory_allocation copy = a ? *a : (struct device_memory_allocation){0};
    a733_memory_critical_exit();
    if (!a) return 0;
    *out = extent_of(p, copy.first, copy.pages);
    if (state) *state = copy.state;
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
        trace(DEVICE_MEMORY_TRACE_ATTEMPT, 0);
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

static int acquire(struct device_memory_pool *p, uint64_t owner, size_t bytes, size_t alignment,
                   int zeroed, struct device_memory_lease *lease, struct device_memory_extent *out)
{
    if (!p || !p->initialized || !owner || !bytes || !lease || !out ||
        alignment < DEVICE_MEMORY_PAGE || (alignment & (alignment - 1)) ||
        bytes > p->backing.bytes ||
        !output_ok(p, lease, sizeof(*lease)) || !output_ok(p, out, sizeof(*out)) ||
        a733_extents_overlap_prechecked((uintptr_t)lease, sizeof(*lease), (uintptr_t)out, sizeof(*out)) ||
        (zeroed && !zero_supported())) return 0;
    size_t minimum = device_memory_class_alignment(p->memory_class, bytes);
    if (alignment < minimum) alignment = minimum;
    if (alignment > (size_t)DEVICE_MEMORY_MAX_PAGES * DEVICE_MEMORY_PAGE ||
        ((p->backing.cpu ^ p->backing.physical) & (alignment - 1))) return 0;
    unsigned pages = (unsigned)((bytes + DEVICE_MEMORY_PAGE - 1) / DEVICE_MEMORY_PAGE);
    unsigned index = 0, first = UINT32_MAX, dirty = 0;
    struct device_memory_lease made = {0};

    a733_memory_critical_enter();
    while (index < DEVICE_MEMORY_MAX_ALLOCATIONS && p->allocations[index].state != DEVICE_MEMORY_FREE) ++index;
    if (index < DEVICE_MEMORY_MAX_ALLOCATIONS && p->next_generation)
        first = place(p, pages, alignment);
    if (first != UINT32_MAX) {
        uint64_t generation = p->next_generation++;
        p->allocations[index] = (struct device_memory_allocation){owner, generation, first, pages,
                                                                  DEVICE_MEMORY_CPU};
        made = (struct device_memory_lease){(uintptr_t)p, owner, generation, index};
        /* Pages below the watermark can hold lines a previous holder left
         * dirty; the zeroed form cleans the lines DC ZVA itself dirtied. */
        if (zeroed) dirty = pages;
        else if (first < p->clean_end) dirty = p->clean_end - first < pages ? p->clean_end - first : pages;
        if (first + pages > p->clean_end) p->clean_end = first + pages;
    }
    a733_memory_critical_exit();
    if (first == UINT32_MAX) return 0;

    struct device_memory_extent e = extent_of(p, first, pages);
    if (zeroed) zero(e.cpu, e.bytes);
    if (dirty && p->memory_type == DEVICE_MEMORY_WRITE_BACK &&
        !clean(e.cpu, (size_t)dirty * DEVICE_MEMORY_PAGE)) {
        a733_memory_critical_enter();
        struct device_memory_allocation *a = slot(p, made);
        if (a && a->state == DEVICE_MEMORY_CPU) *a = (struct device_memory_allocation){0};
        a733_memory_critical_exit();
        return 0;
    }
    *lease = made;
    *out = e;
    return 1;
}

int device_memory_acquire_aligned(struct device_memory_pool *p, uint64_t owner, size_t bytes,
    size_t alignment, struct device_memory_lease *lease, struct device_memory_extent *out)
{
    return acquire(p, owner, bytes, alignment, 0, lease, out);
}
int device_memory_acquire_zeroed(struct device_memory_pool *p, uint64_t owner, size_t bytes,
    size_t alignment, struct device_memory_lease *lease, struct device_memory_extent *out)
{
    return acquire(p, owner, bytes, alignment, 1, lease, out);
}
int device_memory_acquire(struct device_memory_pool *p, uint64_t owner, size_t bytes,
                          struct device_memory_lease *lease, struct device_memory_extent *out)
{
    return acquire(p, owner, bytes, DEVICE_MEMORY_PAGE, 0, lease, out);
}

/* Apply one accepted change, or refuse and leave the slot unchanged. */
enum change { CHANGE_DEVICE, CHANGE_CPU, CHANGE_RETAIN, CHANGE_RELEASE };
static int change(struct device_memory_pool *p, struct device_memory_lease l, enum change c)
{
    if (!p || !p->initialized) return 0;
    int done = 0;
    a733_memory_critical_enter();
    struct device_memory_allocation *a = slot(p, l);
    if (a) {
        enum device_memory_state s = a->state;
        if (c == CHANGE_DEVICE && s == DEVICE_MEMORY_CPU) { a->state = DEVICE_MEMORY_DEVICE; done = 1; }
        else if (c == CHANGE_CPU && s == DEVICE_MEMORY_DEVICE) { a->state = DEVICE_MEMORY_CPU; done = 1; }
        else if (c == CHANGE_RETAIN) { a->state = DEVICE_MEMORY_RETAINED; done = 1; }
        else if (c == CHANGE_RELEASE && s == DEVICE_MEMORY_CPU) {
            *a = (struct device_memory_allocation){0};
            done = 1;
        }
    }
    a733_memory_critical_exit();
    return done;
}

int device_memory_set_state(struct device_memory_pool *p, struct device_memory_lease l,
                            enum device_memory_state state)
{
    if (state == DEVICE_MEMORY_DEVICE) return change(p, l, CHANGE_DEVICE);
    if (state == DEVICE_MEMORY_CPU) return change(p, l, CHANGE_CPU);
    return 0;
}
int device_memory_release(struct device_memory_pool *p, struct device_memory_lease l)
{
    return change(p, l, CHANGE_RELEASE);
}
int device_memory_retain(struct device_memory_pool *p, struct device_memory_lease l)
{
    return change(p, l, CHANGE_RETAIN);
}
