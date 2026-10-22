#include "device_memory_pool.h"
#include "a733_memory_critical.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BYTES (16 * DEVICE_MEMORY_PAGE)
#define PHYSICAL UINT64_C(0x41400000)
static unsigned char cpu[BYTES] __attribute__((aligned(4096)));
static unsigned char device[BYTES] __attribute__((aligned(4096)));

static void allocation_cases(void)
{
    struct device_memory_pool pool = {0}, second = {0};
    assert(device_memory_pool_init(&pool, (uintptr_t)cpu, PHYSICAL, BYTES));
    assert(device_memory_pool_init(&second, (uintptr_t)device, PHYSICAL + BYTES, BYTES));
    struct device_memory_lease a, b, stale;
    struct device_memory_extent extent;
    assert(!device_memory_acquire(&pool, 1, 1, (void *)cpu, &extent));
    assert(!device_memory_acquire(&pool, 1, 1, &a, &pool.backing));
    assert(device_memory_acquire(&pool, 1, 4097, &a, &extent));
    assert(extent.cpu == (uintptr_t)cpu && extent.physical == PHYSICAL && extent.bytes == 8192);
    assert(!device_memory_release(&second, a));
    stale = a; ++a.owner; assert(!device_memory_release(&pool, a)); a = stale;
    assert(device_memory_acquire(&pool, 2, 14 * 4096, &b, &extent));
    assert(extent.cpu == (uintptr_t)cpu + 8192 && extent.bytes == 14 * 4096);
    assert(!device_memory_acquire(&pool, 1, 1, &a, &extent));
    assert(device_memory_release(&pool, stale));
    assert(device_memory_acquire(&pool, 1, 1, &a, &extent));
    assert(a.generation != stale.generation && !device_memory_release(&pool, stale));
    assert(device_memory_retain(&pool, b) && !device_memory_release(&pool, b));
    assert(device_memory_release(&pool, a));
    pool.next_generation = UINT64_MAX;
    assert(device_memory_acquire(&pool, 1, 1, &a, &extent));
    assert(a.generation == UINT64_MAX && !pool.next_generation);
    assert(device_memory_release(&pool, a));
    assert(!device_memory_acquire(&pool, 1, 1, &a, &extent));
    assert(!device_memory_pool_init(&pool, (uintptr_t)cpu, PHYSICAL, BYTES));
}

/* The runner compiles the pool with DEVICE_MEMORY_POOL_TRACE; this records
 * the critical-section nesting and the bytes cleaned and zeroed. */
static int depth, max_depth;
static size_t cleaned, zeroed;
void device_memory_trace(enum device_memory_trace_event event, size_t value)
{
    if (event == DEVICE_MEMORY_TRACE_ENTER) { ++depth; if (depth > max_depth) max_depth = depth; }
    else if (event == DEVICE_MEMORY_TRACE_EXIT) { assert(depth > 0); --depth; }
    else if (event == DEVICE_MEMORY_TRACE_CLEAN) cleaned += value;
    else if (event == DEVICE_MEMORY_TRACE_ZERO) zeroed += value;
}

/* Every pair of source state and operation: accepted changes match the table
 * in device_memory_pool.h, and each refused change leaves the slot unchanged. */
static void transition_cases(void)
{
    enum op { TO_DEVICE, TO_CPU, RETAIN, RELEASE, TO_FREE, TO_RETAINED };
    static const enum device_memory_state sources[] = {DEVICE_MEMORY_CPU, DEVICE_MEMORY_DEVICE,
                                                       DEVICE_MEMORY_RETAINED};
    for (unsigned s = 0; s < 3; ++s) for (unsigned o = TO_DEVICE; o <= TO_RETAINED; ++o) {
        struct device_memory_pool pool = {0};
        struct device_memory_lease l;
        struct device_memory_extent e;
        assert(device_memory_pool_init(&pool, (uintptr_t)cpu, PHYSICAL, BYTES));
        assert(device_memory_acquire(&pool, 7, 4096, &l, &e));
        if (sources[s] == DEVICE_MEMORY_DEVICE) assert(device_memory_set_state(&pool, l, DEVICE_MEMORY_DEVICE));
        if (sources[s] == DEVICE_MEMORY_RETAINED) assert(device_memory_retain(&pool, l));
        struct device_memory_allocation before = pool.allocations[l.slot];
        int done = o == TO_DEVICE ? device_memory_set_state(&pool, l, DEVICE_MEMORY_DEVICE) :
                   o == TO_CPU ? device_memory_set_state(&pool, l, DEVICE_MEMORY_CPU) :
                   o == RETAIN ? device_memory_retain(&pool, l) :
                   o == RELEASE ? device_memory_release(&pool, l) :
                   o == TO_FREE ? device_memory_set_state(&pool, l, DEVICE_MEMORY_FREE) :
                   device_memory_set_state(&pool, l, DEVICE_MEMORY_RETAINED);
        enum device_memory_state from = sources[s], to = pool.allocations[l.slot].state;
        int expected = (o == TO_DEVICE && from == DEVICE_MEMORY_CPU) ||
                       (o == TO_CPU && from == DEVICE_MEMORY_DEVICE) || o == RETAIN ||
                       (o == RELEASE && from == DEVICE_MEMORY_CPU);
        assert(done == expected);
        if (!done) assert(!memcmp(&before, &pool.allocations[l.slot], sizeof(before)));
        else assert(to == (o == TO_DEVICE ? DEVICE_MEMORY_DEVICE : o == TO_CPU ? DEVICE_MEMORY_CPU :
                           o == RETAIN ? DEVICE_MEMORY_RETAINED : DEVICE_MEMORY_FREE));
        /* A stale or foreign lease changes nothing. */
        struct device_memory_lease stale = l;
        ++stale.generation;
        before = pool.allocations[l.slot];
        assert(!device_memory_set_state(&pool, stale, DEVICE_MEMORY_DEVICE) &&
               !device_memory_set_state(&pool, stale, DEVICE_MEMORY_CPU) &&
               !device_memory_retain(&pool, stale) && !device_memory_release(&pool, stale));
        assert(!memcmp(&before, &pool.allocations[l.slot], sizeof(before)));
        assert(!depth);
    }
}

/* The clean watermark: a fresh page is never cleaned, a recycled page is
 * cleaned once per acquire, and the zeroed form zeroes and cleans its whole
 * extent; a non-cacheable pool cleans nothing. */
static void cache_cases(void)
{
    struct device_memory_pool pool = {0}, nc = {0};
    struct device_memory_lease a, b;
    struct device_memory_extent e;
    assert(device_memory_pool_init(&pool, (uintptr_t)cpu, PHYSICAL, BYTES));
    cleaned = 0;
    assert(device_memory_acquire(&pool, 1, 3 * 4096, &a, &e) && !cleaned && pool.clean_end == 3);
    assert(device_memory_acquire(&pool, 1, 2 * 4096, &b, &e) && !cleaned && pool.clean_end == 5);
    assert(device_memory_release(&pool, a));
    /* Four pages at page 0: three recycled pages are cleaned, page 5 is fresh
     * and placed after b. */
    assert(device_memory_acquire(&pool, 1, 4 * 4096, &a, &e) && e.cpu == (uintptr_t)cpu + 5 * 4096);
    assert(!cleaned && pool.clean_end == 9);
    assert(device_memory_release(&pool, a) && device_memory_release(&pool, b));
    assert(device_memory_acquire(&pool, 1, 10 * 4096, &a, &e) && e.cpu == (uintptr_t)cpu);
    assert(cleaned == 9 * 4096 && pool.clean_end == 10);
    assert(device_memory_release(&pool, a));
    cleaned = 0;
    memset(cpu, 0x5a, sizeof(cpu));
    assert(device_memory_acquire_zeroed(&pool, 1, 12 * 4096 - 5, 4096, &a, &e));
    assert(e.bytes == 12 * 4096 && zeroed == e.bytes && cleaned == e.bytes && pool.clean_end == 12);
    for (size_t i = 0; i < e.bytes; ++i) assert(!cpu[i]);
    assert(cpu[e.bytes] == 0x5a);
    assert(device_memory_pool_init_typed(&nc, (uintptr_t)device, PHYSICAL + BYTES, BYTES,
                                         DEVICE_MEMORY_CLASS_NONE, DEVICE_MEMORY_NON_CACHEABLE));
    cleaned = zeroed = 0;
    assert(device_memory_acquire_zeroed(&nc, 1, 4096, 4096, &b, &e) && zeroed == 4096 && !cleaned);
    assert(device_memory_release(&nc, b) && device_memory_acquire(&nc, 1, 4096, &b, &e) && !cleaned);
    assert(!device_memory_pool_init_typed(&nc, (uintptr_t)device, PHYSICAL, BYTES,
                                          DEVICE_MEMORY_CLASS_NPU + 1, DEVICE_MEMORY_WRITE_BACK));
    assert(!depth && max_depth == 1);
}

int main(void)
{
    allocation_cases();
    transition_cases();
    cache_cases();
    puts("PASS reservations: generations, complete extents, stale and cross-pool leases, exhaustion, "
         "every state change, clean watermark and zeroed extents");
    return 0;
}
