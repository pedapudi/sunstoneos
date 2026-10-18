#include "device_memory_pool.h"

static int overlap(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{ return a < b + bn && b < a + an; }
static int output_ok(const struct device_memory_pool *p, const void *out, size_t bytes)
{
    uintptr_t a = (uintptr_t)out;
    return out && a <= UINTPTR_MAX - bytes &&
        !overlap(a, bytes, (uintptr_t)p, sizeof(*p)) &&
        !overlap(a, bytes, p->backing.cpu, p->backing.bytes);
}

int device_memory_pool_init(struct device_memory_pool *p, uintptr_t cpu,
                            uint64_t physical, size_t bytes)
{
    if (!p || p->initialized || !cpu || !bytes ||
        ((cpu | physical | bytes) & (DEVICE_MEMORY_PAGE - 1)) ||
        bytes > DEVICE_MEMORY_MAX_PAGES * DEVICE_MEMORY_PAGE ||
        cpu > UINTPTR_MAX - bytes || physical > UINT64_MAX - bytes ||
        (uintptr_t)p > UINTPTR_MAX - sizeof(*p) ||
        (cpu < (uintptr_t)p + sizeof(*p) && (uintptr_t)p < cpu + bytes)) return 0;
    *p = (struct device_memory_pool){.backing = {cpu, physical, bytes},
                                    .next_generation = 1, .initialized = 1};
    return 1;
}

struct device_memory_allocation *device_memory_lookup(struct device_memory_pool *p,
                                                      struct device_memory_lease l)
{
    if (!p || !p->initialized || l.pool != (uintptr_t)p || !l.owner || !l.generation ||
        l.slot >= DEVICE_MEMORY_MAX_ALLOCATIONS) return NULL;
    struct device_memory_allocation *a = &p->allocations[l.slot];
    return a->state != DEVICE_MEMORY_FREE && a->owner == l.owner &&
           a->generation == l.generation ? a : NULL;
}

int device_memory_describe(const struct device_memory_pool *p, struct device_memory_lease l,
                           struct device_memory_extent *out, enum device_memory_state *state)
{
    if (!p || !p->initialized || l.pool != (uintptr_t)p || !output_ok(p, out, sizeof(*out)) || !l.owner || !l.generation ||
        l.slot >= DEVICE_MEMORY_MAX_ALLOCATIONS ||
        (state && (!output_ok(p, state, sizeof(*state)) ||
         overlap((uintptr_t)state, sizeof(*state), (uintptr_t)out, sizeof(*out))))) return 0;
    const struct device_memory_allocation *a = &p->allocations[l.slot];
    if (a->state == DEVICE_MEMORY_FREE || a->owner != l.owner || a->generation != l.generation) return 0;
    size_t offset = (size_t)a->first * DEVICE_MEMORY_PAGE;
    *out = (struct device_memory_extent){p->backing.cpu + offset,
        p->backing.physical + offset, (size_t)a->pages * DEVICE_MEMORY_PAGE};
    if (state) *state = a->state;
    return 1;
}

int device_memory_acquire_aligned(struct device_memory_pool *p, uint64_t owner, size_t bytes,
    size_t alignment, struct device_memory_lease *lease, struct device_memory_extent *out)
{
    if (!p || !p->initialized || !owner || !bytes || !lease || !out ||
        alignment < DEVICE_MEMORY_PAGE || (alignment & (alignment - 1)) ||
        alignment > (size_t)DEVICE_MEMORY_MAX_PAGES * DEVICE_MEMORY_PAGE ||
        ((p->backing.cpu ^ p->backing.physical) & (alignment - 1)) ||
        bytes > p->backing.bytes || !p->next_generation ||
        !output_ok(p, lease, sizeof(*lease)) || !output_ok(p, out, sizeof(*out)) ||
        overlap((uintptr_t)lease, sizeof(*lease), (uintptr_t)out, sizeof(*out))) return 0;
    unsigned pages = (unsigned)((bytes + DEVICE_MEMORY_PAGE - 1) / DEVICE_MEMORY_PAGE);
    unsigned total = (unsigned)(p->backing.bytes / DEVICE_MEMORY_PAGE), slot = 0;
    while (slot < DEVICE_MEMORY_MAX_ALLOCATIONS && p->allocations[slot].state != DEVICE_MEMORY_FREE) ++slot;
    if (slot == DEVICE_MEMORY_MAX_ALLOCATIONS) return 0;
    uint64_t address = p->backing.physical;
    unsigned first = 0;
    /* Every jump passes at least one occupied extent. Alignment can skip more
     * free pages, but never causes another scan of the same blocking extent. */
    for (unsigned attempt = 0; attempt <= DEVICE_MEMORY_MAX_ALLOCATIONS; ++attempt) {
        uint64_t delta = (-(uint64_t)address) & (alignment - 1);
        if (delta > p->backing.bytes || delta / DEVICE_MEMORY_PAGE > total - first) return 0;
        first += (unsigned)(delta / DEVICE_MEMORY_PAGE);
        if (first > total || pages > total - first) return 0;
        unsigned next = first;
        for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i) {
            const struct device_memory_allocation *a = &p->allocations[i];
            if (a->state != DEVICE_MEMORY_FREE && first < a->first + a->pages && a->first < first + pages &&
                next < a->first + a->pages) next = a->first + a->pages;
        }
        if (next == first) {
            uint64_t generation = p->next_generation++;
            p->allocations[slot] = (struct device_memory_allocation){owner, generation, first, pages, DEVICE_MEMORY_CPU};
            *lease = (struct device_memory_lease){(uintptr_t)p, owner, generation, slot};
            return device_memory_describe(p, *lease, out, NULL);
        }
        first = next;
        if (first > total) return 0;
        address = p->backing.physical + (uint64_t)first * DEVICE_MEMORY_PAGE;
    }
    return 0;
}
int device_memory_acquire(struct device_memory_pool *p, uint64_t owner, size_t bytes,
                          struct device_memory_lease *lease, struct device_memory_extent *out)
{
    return device_memory_acquire_aligned(p, owner, bytes, DEVICE_MEMORY_PAGE, lease, out);
}

int device_memory_release(struct device_memory_pool *p, struct device_memory_lease l)
{
    struct device_memory_allocation *a = device_memory_lookup(p, l);
    if (!a || a->state != DEVICE_MEMORY_CPU) return 0;
    *a = (struct device_memory_allocation){0};
    return 1;
}
int device_memory_retain(struct device_memory_pool *p, struct device_memory_lease l)
{
    struct device_memory_allocation *a = device_memory_lookup(p, l);
    if (!a) return 0;
    a->state = DEVICE_MEMORY_RETAINED;
    return 1;
}
