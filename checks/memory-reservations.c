#include "a733_dma_reserved.h"
#include "dma_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BYTES (16 * DEVICE_MEMORY_PAGE)
#define PHYSICAL UINT64_C(0x41400000)
#define REGISTERS UINT32_C(0x03002000)
static unsigned char cpu[BYTES] __attribute__((aligned(4096)));
static unsigned char device[BYTES] __attribute__((aligned(4096)));
struct model {
    uint32_t registers[0x2000 / 4];
    uint64_t ticks;
    unsigned callbacks, clean, prepare, acquire;
    int hold_busy, bad_page;
    struct dma_memory_policy memory;
};
static uint32_t read32(void *cookie, uintptr_t address)
{
    struct model *m = cookie; ++m->callbacks;
    assert(address >= REGISTERS && address < REGISTERS + 0x2000);
    return m->registers[(address - REGISTERS) / 4];
}
static void write32(void *cookie, uintptr_t address, uint32_t value)
{
    struct model *m = cookie; ++m->callbacks;
    unsigned offset = (unsigned)(address - REGISTERS);
    assert(offset < 0x2000);
    if (offset == 0x138) m->registers[offset / 4] &= ~value;
    else m->registers[offset / 4] = value;
    if (offset == 0x100) {
        if (value) m->registers[0x30 / 4] = 1;
        else if (!m->hold_busy) m->registers[0x30 / 4] = 0;
    }
}
static int translate(void *cookie, uintptr_t address, int write, struct dma_translation *out)
{
    struct model *m = cookie; (void)write;
    if (address < (uintptr_t)cpu || address >= (uintptr_t)cpu + BYTES ||
        (m->bad_page && address >= (uintptr_t)cpu + 4096 && address < (uintptr_t)cpu + 8192)) return 0;
    *out = (struct dma_translation){PHYSICAL + address - (uintptr_t)cpu, 0xff, 3};
    return 1;
}
static int mapping(void *cookie, const struct a733_dma_mapping *mapping)
{ return dma_memory_accept(&((struct model *)cookie)->memory, mapping); }
static int fifo(void *cookie, const struct a733_dma_endpoint *endpoint, enum a733_dma_access access)
{ (void)cookie; (void)endpoint; (void)access; return 0; }
static void cache(void *cookie, enum a733_dma_cache op, void *ptr, size_t bytes)
{
    struct model *m = cookie; ++m->callbacks;
    size_t offset = (uintptr_t)ptr - (uintptr_t)cpu;
    assert(!(offset & 4095) && !(bytes & 4095) && offset + bytes <= BYTES);
    if (op == A733_DMA_INVALIDATE) { memcpy(cpu + offset, device + offset, bytes); ++m->acquire; }
    else { memcpy(device + offset, cpu + offset, bytes);
        if (op == A733_DMA_CLEAN) ++m->clean; else ++m->prepare; }
}
static void barrier(void *cookie) { ++((struct model *)cookie)->callbacks; }
static uint64_t ticks(void *cookie) { return ((struct model *)cookie)->ticks; }
static const struct a733_dma_operations operations = {read32, write32, cache, barrier, ticks, mapping, fifo};

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

static void composed_case(unsigned mode)
{
    struct device_memory_pool pool = {0};
    struct a733_dma_controller controller = {0};
    struct a733_dma_reserved service = {0};
    struct model model = {0};
    memset(cpu, 0xa5, BYTES); memset(device, 0x5a, BYTES);
    assert(device_memory_pool_init(&pool, (uintptr_t)cpu, PHYSICAL, BYTES));
    struct dma_memory_window window = {(uintptr_t)cpu, PHYSICAL, BYTES, 7};
    assert(dma_memory_init(&model.memory, &window, 1, NULL, 0, translate, &model));
    model.registers[0x34 / 4] = 1;
    assert(a733_dma_controller_init(&controller, &operations, &model, REGISTERS, 1, 10) == A733_DMA_CONTROL_OK);
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_reserved_init(&service, &pool, &controller, 0, 7));
    struct device_memory_lease desc, source, destination;
    struct device_memory_extent de, se, dest;
    assert(device_memory_acquire(&pool, 7, 64, &desc, &de));
    assert(device_memory_acquire(&pool, 7, 5000, &source, &se));
    assert(device_memory_acquire(&pool, 7, 5000, &destination, &dest));
    struct a733_dma_reserved_buffer buffers[] = {{source, A733_DMA_READ}, {destination, A733_DMA_WRITE}};
    struct a733_dma_segment segment = {{se.physical, 0, 4, 4}, {dest.physical, 0, 4, 4}, 5000};
    unsigned before = model.callbacks;
    assert(a733_dma_reserved_submit(&service, desc,
        (const void *)((uintptr_t)buffers + 1), 1, &segment, 1, 100) == A733_DMA_INVALID);
    assert(a733_dma_reserved_submit(&service, desc, buffers, 2,
        (const void *)((uintptr_t)&segment + 1), 1, 100) == A733_DMA_INVALID);
    if (mode == 3) model.bad_page = 1;
    enum a733_dma_control_status status = a733_dma_reserved_submit(&service, desc, buffers, 2, &segment, 1, 100);
    if (mode == 3) {
        assert(status == A733_DMA_INVALID && before == model.callbacks);
        assert(device_memory_release(&pool, source) && device_memory_release(&pool, destination));
        assert(device_memory_release(&pool, desc));
        return;
    }
    assert(status == A733_DMA_ACTIVE && service.active);
    assert(model.clean == 2 && model.prepare == 1 && !model.acquire);
    assert(service.transfer.descriptors.bytes == 4096 && service.mappings[0].bytes == 8192 && service.mappings[1].bytes == 8192);
    assert(!device_memory_release(&pool, desc) && !device_memory_release(&pool, source) && !device_memory_release(&pool, destination));
    memset(buffers, 0, sizeof(buffers)); memset(&segment, 0, sizeof(segment));
    assert(service.segments[0].bytes == 5000 && service.segments[0].source.address == se.physical);
    if (mode == 4 || mode == 5 || mode == 6) {
        before = model.callbacks;
        if (mode == 4) {
            struct a733_dma_reserved copied = service;
            assert(a733_dma_reserved_poll(&copied) == A733_DMA_RECOVERY);
        } else if (mode == 6) {
            service.lease_count = SIZE_MAX;
            assert(a733_dma_reserved_poll(&service) == A733_DMA_RECOVERY);
        } else {
            struct a733_dma_controller unrelated = controller;
            unrelated.channels[0].status = A733_DMA_COMPLETE;
            unrelated.channels[0].transfer = NULL;
            service.controller = &unrelated;
            assert(a733_dma_reserved_cancel(&service) == A733_DMA_RECOVERY);
        }
        assert(model.callbacks == before && a733_dma_owns(&controller, &service.transfer));
        assert(!device_memory_release(&pool, source) && !device_memory_release(&pool, desc));
        assert(device_memory_lookup(&pool, destination)->state == DEVICE_MEMORY_RETAINED);
        return;
    }
    if (mode == 2) {
        model.hold_busy = 1;
        assert(a733_dma_reserved_cancel(&service) == A733_DMA_STOPPING);
        model.ticks = 11;
        assert(a733_dma_reserved_poll(&service) == A733_DMA_RECOVERY);
        before = model.callbacks;
        assert(a733_dma_reserved_poll(&service) == A733_DMA_RECOVERY && model.callbacks == before);
        assert(!device_memory_release(&pool, desc) && !device_memory_release(&pool, source) && !device_memory_release(&pool, destination));
        assert(device_memory_lookup(&pool, desc)->state == DEVICE_MEMORY_RETAINED);
        assert(!model.acquire);
        return;
    }
    if (mode == 1) {
        assert(a733_dma_reserved_cancel(&service) == A733_DMA_STOPPING);
        assert(a733_dma_reserved_poll(&service) == A733_DMA_CANCELED);
    } else {
        size_t so = se.cpu - (uintptr_t)cpu, doff = dest.cpu - (uintptr_t)cpu;
        memcpy(device + doff, device + so, 5000);
        memset(device + doff + 5000, 0x7b, 8192 - 5000);
        model.registers[0x138 / 4] = 4;
        assert(a733_dma_reserved_poll(&service) == A733_DMA_COMPLETE);
        assert(!memcmp(cpu + doff, cpu + so, 5000));
        for (size_t i = 5000; i < 8192; ++i) assert(cpu[doff + i] == 0x7b);
    }
    assert(!service.active && model.acquire == 1 && !a733_dma_owns(&controller, &service.transfer));
    assert(device_memory_release(&pool, desc) && device_memory_release(&pool, source) && device_memory_release(&pool, destination));
    for (size_t i = 5 * 4096; i < BYTES; ++i) assert(cpu[i] == 0xa5);
}
int main(void)
{
    allocation_cases();
    for (unsigned i = 0; i < 7; ++i) composed_case(i);
    puts("PASS reservations: generations, complete extents, copied DMA ownership, completion, cancellation and retained recovery");
    return 0;
}
