#include "device_memory_pool.h"
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
 * the critical-section nesting, the number of entries and the bytes cleaned.
 * Event numbers are those of runtime/device_memory_pool.c. */
enum { TRACE_ENTER, TRACE_EXIT, TRACE_ATTEMPT, TRACE_CLEAN };
static int depth, max_depth;
static unsigned entries;
static size_t cleaned;
void device_memory_trace(unsigned event, size_t value);
void device_memory_trace(unsigned event, size_t value)
{
    if (event == TRACE_ENTER) { ++entries; ++depth; if (depth > max_depth) max_depth = depth; }
    else if (event == TRACE_EXIT) { assert(depth > 0); --depth; }
    else if (event == TRACE_CLEAN) cleaned += value;
}

/* Every pair of source state and ledger operation: retain is accepted from
 * every held state, release only from CPU, and a refused operation leaves the
 * slot unchanged. Holders still reach DEVICE through device_memory_lookup().
 * Describe takes no lock and reports the held extent and state. */
static void transition_cases(void)
{
    static const enum device_memory_state sources[] = {DEVICE_MEMORY_CPU, DEVICE_MEMORY_DEVICE,
                                                       DEVICE_MEMORY_RETAINED};
    for (unsigned s = 0; s < 3; ++s) for (unsigned retain = 0; retain < 2; ++retain) {
        struct device_memory_pool pool = {0};
        struct device_memory_lease l;
        struct device_memory_extent e, d;
        enum device_memory_state state;
        assert(device_memory_pool_init(&pool, (uintptr_t)cpu, PHYSICAL, BYTES));
        assert(device_memory_acquire(&pool, 7, 4096, &l, &e));
        if (sources[s] == DEVICE_MEMORY_DEVICE) device_memory_lookup(&pool, l)->state = DEVICE_MEMORY_DEVICE;
        if (sources[s] == DEVICE_MEMORY_RETAINED) assert(device_memory_retain(&pool, l));
        unsigned before_entries = entries;
        assert(device_memory_describe(&pool, l, &d, &state) && state == sources[s] &&
               !memcmp(&d, &e, sizeof(d)) && entries == before_entries);
        struct device_memory_allocation before = pool.allocations[l.slot];
        int done = retain ? device_memory_retain(&pool, l) : device_memory_release(&pool, l);
        int expected = retain || sources[s] == DEVICE_MEMORY_CPU;
        assert(done == expected);
        if (!done) assert(!memcmp(&before, &pool.allocations[l.slot], sizeof(before)));
        else if (retain) assert(pool.allocations[l.slot].state == DEVICE_MEMORY_RETAINED);
        else {
            static const struct device_memory_allocation empty;
            assert(!memcmp(&empty, &pool.allocations[l.slot], sizeof(empty)));
            assert(!device_memory_describe(&pool, l, &d, &state));
        }
        /* A stale or foreign lease changes nothing and describes nothing. */
        struct device_memory_lease stale = l;
        ++stale.generation;
        before = pool.allocations[l.slot];
        assert(!device_memory_retain(&pool, stale) && !device_memory_release(&pool, stale) &&
               !device_memory_describe(&pool, stale, &d, &state));
        stale = l; ++stale.owner;
        assert(!device_memory_describe(&pool, stale, &d, &state));
        assert(!memcmp(&before, &pool.allocations[l.slot], sizeof(before)));
        assert(!depth);
    }
}

/* The clean watermark: a fresh page is never cleaned and a recycled page is
 * cleaned once per acquire; a non-cacheable pool cleans nothing. */
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
    assert(device_memory_pool_init_typed(&nc, (uintptr_t)device, PHYSICAL + BYTES, BYTES,
                                         DEVICE_MEMORY_CLASS_NONE, DEVICE_MEMORY_NON_CACHEABLE));
    cleaned = 0;
    assert(device_memory_acquire(&nc, 1, 4096, &b, &e) && !cleaned);
    assert(device_memory_release(&nc, b) && device_memory_acquire(&nc, 1, 4096, &b, &e) && !cleaned);
    assert(!device_memory_pool_init_typed(&nc, (uintptr_t)device, PHYSICAL, BYTES,
                                          DEVICE_MEMORY_CLASS_DMA32 + 1, DEVICE_MEMORY_WRITE_BACK));
    assert(!depth && max_depth == 1);
}

int main(void)
{
    allocation_cases();
    transition_cases();
    cache_cases();
    puts("PASS reservations: generations, complete extents, stale and cross-pool leases, exhaustion, "
         "retain and release from every state, lock-free describe and clean watermark");
    return 0;
}
