#include "dma_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4096) uint8_t ram[4 * 4096];
static struct dma_memory_policy policy;
static uint64_t bus = UINT64_C(0x700000000);
static size_t bad_page;
static unsigned fault, probes;

static int translate(void *cookie, uintptr_t cpu, int write, struct dma_translation *t)
{
    assert(cookie == ram && cpu >= (uintptr_t)ram && cpu < (uintptr_t)ram + sizeof(ram));
    ++probes;
    size_t offset = cpu - (uintptr_t)ram;
    t->physical = bus + offset;
    t->attributes = 0xff;
    t->shareability = 3;
    if (offset / 4096 == bad_page) {
        if (fault == 1 || (fault == 2 && write)) return 0;
        if (fault == 3) t->physical += 4096;
        if (fault == 4) t->attributes = 0;
        if (fault == 5) t->shareability = 0;
    }
    return 1;
}

int main(void)
{
    struct dma_memory_window window = {(uintptr_t)ram, bus, sizeof(ram), 3};
    struct dma_fifo_port port = {0x04001000, 17, (1u << 2) | (1u << 4), (1u << 4) | (1u << 8), A733_DMA_READ};
    assert(dma_memory_init(&policy, &window, 1, &port, 1, translate, ram));
    struct a733_dma_mapping mapping = {ram + 4032, bus + 4032, 8192, A733_DMA_WRITE};
    assert(dma_memory_accept(&policy, &mapping) && probes == 3);
    /* The middle page must be checked even when both endpoints are valid. */
    bad_page = 1;
    for (fault = 1; fault <= 5; ++fault) assert(!dma_memory_accept(&policy, &mapping));
    fault = 2;
    mapping.access = A733_DMA_READ;
    assert(dma_memory_accept(&policy, &mapping));
    mapping.access = A733_DMA_DESCRIPTORS;
    assert(!dma_memory_accept(&policy, &mapping));
    mapping.access = A733_DMA_WRITE;
    fault = 0;
    mapping.bus += 64;
    assert(!dma_memory_accept(&policy, &mapping));
    mapping.bus -= 64;
    mapping.bytes += 8192;
    assert(!dma_memory_accept(&policy, &mapping));
    mapping.bytes = 0;
    assert(!dma_memory_accept(&policy, &mapping));
    mapping.bytes = 63;
    assert(!dma_memory_accept(&policy, &mapping));
    struct a733_dma_endpoint fifo = {0x04001000, 17, 4, 8};
    assert(dma_memory_fifo(&policy, &fifo, A733_DMA_READ));
    assert(!dma_memory_fifo(&policy, &fifo, A733_DMA_WRITE));
    fifo.request = 18;
    assert(!dma_memory_fifo(&policy, &fifo, A733_DMA_READ));
    fifo.request = 17; fifo.width = 1;
    assert(!dma_memory_fifo(&policy, &fifo, A733_DMA_READ));
    fifo.width = 4; fifo.burst = 16;
    assert(!dma_memory_fifo(&policy, &fifo, A733_DMA_READ));
    fifo.burst = 8; fifo.address += 4;
    assert(!dma_memory_fifo(&policy, &fifo, A733_DMA_READ));

    struct dma_memory_policy rejected = {0}, empty = {0};
    struct dma_memory_window aliases[] = {window, window};
    assert(!dma_memory_init(&rejected, aliases, 2, NULL, 0, translate, ram));
    aliases[1].cpu += sizeof(ram); /* Different CPU ranges, same physical RAM. */
    assert(!dma_memory_init(&rejected, aliases, 2, NULL, 0, translate, ram));
    aliases[1] = window; aliases[1].physical += sizeof(ram);
    assert(!dma_memory_init(&rejected, aliases, 2, NULL, 0, translate, ram));
    aliases[0].roles = DMA_MEMORY_ROLE(A733_DMA_DESCRIPTORS);
    assert(!dma_memory_init(&rejected, aliases, 1, NULL, 0, translate, ram));
    aliases[0] = window; aliases[0].physical = (UINT64_C(1) << 35) - 64;
    assert(!dma_memory_init(&rejected, aliases, 1, NULL, 0, translate, ram));
    port.address = bus;
    assert(!dma_memory_init(&rejected, &window, 1, &port, 1, translate, ram));
    assert(!memcmp(&rejected, &empty, sizeof(rejected)));

    assert(dma_cache_stride(UINT64_C(0x8444c004)) == 64);
    assert(dma_cache_stride((4u << 24) | (3u << 16)) == 32);
    assert(!dma_cache_stride(4u << 16)); /* Unknown writeback granule. */
    assert(!dma_cache_stride((5u << 24) | (4u << 16))); /* A 128-byte writeback can damage neighbors. */
    assert(!dma_cache_stride((4u << 24) | (5u << 16)));
    puts("PASS DMA memory: page permissions, attributes, high addresses, canonical ranges, FIFO admission, and cache geometry.");
}
