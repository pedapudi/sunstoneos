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

int main(void)
{
    allocation_cases();
    puts("PASS reservations: generations, complete extents, stale and cross-pool leases, and exhaustion");
    return 0;
}
