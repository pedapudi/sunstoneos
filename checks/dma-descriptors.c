/* Descriptor execution model. This does not model cache coherence or MMIO. */
#include "a733_dma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(64) struct {
    uint8_t before[64];
    struct a733_dma_descriptor descriptors[A733_DMA_MAX_SEGMENTS];
    uint8_t after[64];
} pool;
static struct a733_dma_segment segments[A733_DMA_MAX_SEGMENTS];
static uint8_t source[2048], destination[2048], fifo[2048];
static uint64_t source_bus, destination_bus;
static const uint64_t descriptor_bus = 0x41000000;
static const uint64_t fifo_bus = 0x04001000;
static size_t fifo_read, fifo_write;

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] * 256 |
           (uint32_t)p[2] * 65536 | (uint32_t)p[3] * 16777216;
}

static void equal_byte(const void *memory, size_t bytes, uint8_t value)
{
    const uint8_t *p = memory;
    for (size_t i = 0; i < bytes; ++i) assert(p[i] == value);
}

/* Interpret the hardware's six little-endian words independently of builder
 * helpers. Mapping distinct high addresses catches dropped address bits. */
static void execute(size_t count, unsigned source_width, unsigned dest_width,
                    unsigned source_burst, unsigned dest_burst)
{
    static const unsigned bursts[] = {1, 4, 8, 16};
    uint32_t next = (uint32_t)descriptor_bus;
    for (size_t n = 0; n < count; ++n) {
        assert(next >= descriptor_bus && next - descriptor_bus == n * 64);
        const uint8_t *p = pool.descriptors[(next - descriptor_bus) / 64].bytes;
        uint32_t configuration = word(p), parameters = word(p + 16);
        uint64_t src = word(p + 4) + ((uint64_t)((parameters / 2048) & 7) << 32);
        uint64_t dst = word(p + 8) + ((uint64_t)((parameters / 32768) & 7) << 32);
        uint32_t bytes = word(p + 12);
        unsigned source_request = configuration & 63;
        unsigned dest_request = (configuration >> 16) & 63;
        assert((parameters & ~0x3b800u) == 8);
        assert(!(configuration & ~0x07ff07ffu));
        assert((1u << ((configuration >> 9) & 3)) == source_width);
        assert((1u << ((configuration >> 25) & 3)) == dest_width);
        assert(bursts[(configuration >> 6) & 3] == source_burst);
        assert(bursts[(configuration >> 22) & 3] == dest_burst);
        assert(((configuration >> 8) & 1) == (source_request != 0));
        assert(((configuration >> 24) & 1) == (dest_request != 0));
        assert(bytes && bytes % source_width == 0 && bytes % dest_width == 0);
        if (source_request) assert(source_request == 17 && src == fifo_bus);
        else assert(src >= source_bus && src - source_bus <= sizeof(source) - bytes);
        if (dest_request) assert(dest_request == 23 && dst == fifo_bus);
        else assert(dst >= destination_bus && dst - destination_bus <= sizeof(destination) - bytes);
        for (size_t i = 0; i < bytes; ++i) {
            uint8_t value;
            if (source_request) {
                assert(fifo_read < sizeof(fifo));
                value = fifo[fifo_read++];
            } else value = source[src - source_bus + i];
            if (dest_request) {
                assert(fifo_write < sizeof(fifo));
                fifo[fifo_write++] = value;
            } else destination[dst - destination_bus + i] = value;
        }
        equal_byte(p + 24, 40, 0);
        next = word(p + 20);
    }
    assert(next == 0xfffff800);
    equal_byte(pool.before, sizeof(pool.before), 0xa5);
    equal_byte(pool.after, sizeof(pool.after), 0xa5);
    equal_byte(&pool.descriptors[count],
               (A733_DMA_MAX_SEGMENTS - count) * sizeof(pool.descriptors[0]), 0xa5);
}

static void setup(unsigned high)
{
    memset(&pool, 0xa5, sizeof(pool));
    memset(segments, 0, sizeof(segments));
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i * 37 + i / 16);
    memset(destination, 0xcc, sizeof(destination));
    memset(fifo, 0xee, sizeof(fifo));
    fifo_read = fifo_write = 0;
    source_bus = ((uint64_t)high << 32) + 0x01000000;
    destination_bus = ((uint64_t)(7 - high) << 32) + 0x02000000;
    segments[0] = (struct a733_dma_segment) {
        .source = {source_bus + 16, 0, 4, 8},
        .destination = {destination_bus + 16, 0, 4, 8},
        .bytes = 64
    };
}

static void memory_chains(void)
{
    static const unsigned widths[] = {1, 2, 4}, bursts[] = {1, 4, 8, 16};
    for (unsigned h = 0; h < 8; ++h) {
        for (unsigned w = 0; w < 3; ++w) {
            for (unsigned b = 0; b < 4; ++b) {
                setup(h);
                for (unsigned n = 0; n < 3; ++n) {
                    segments[n] = segments[0];
                    segments[n].source.address = source_bus + 16 + n * 80;
                    segments[n].destination.address = destination_bus + 16 + n * 80;
                    segments[n].source.width = widths[w];
                    segments[n].destination.width = widths[2 - w];
                    segments[n].source.burst = bursts[b];
                    segments[n].destination.burst = bursts[3 - b];
                }
                assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, segments, 3) == A733_DMA_OK);
                execute(3, widths[w], widths[2 - w], bursts[b], bursts[3 - b]);
                equal_byte(destination, 16, 0xcc);
                for (unsigned n = 0; n < 3; ++n) {
                    assert(!memcmp(source + 16 + n * 80, destination + 16 + n * 80, 64));
                    equal_byte(destination + 80 + n * 80, 16, 0xcc);
                }
                equal_byte(destination + 256, sizeof(destination) - 256, 0xcc);
            }
        }
    }
    /* A full chain can share immutable source bytes. */
    setup(5);
    for (unsigned n = 0; n < 64; ++n) {
        segments[n] = segments[0];
        segments[n].bytes = 16;
        segments[n].destination.address = destination_bus + 16 + n * 16;
    }
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, segments, 64) == A733_DMA_OK);
    execute(64, 4, 4, 8, 8);
    for (unsigned n = 0; n < 64; ++n)
        assert(!memcmp(source + 16, destination + 16 + n * 16, 16));
    equal_byte(destination, 16, 0xcc);
    equal_byte(destination + 1040, sizeof(destination) - 1040, 0xcc);
}

static void fifo_chains(void)
{
    setup(3);
    for (unsigned n = 0; n < 2; ++n) {
        segments[n] = segments[0];
        segments[n].source.address = source_bus + 16 + n * 64;
        segments[n].destination = (struct a733_dma_endpoint){fifo_bus, 23, 2, 4};
    }
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, segments, 2) == A733_DMA_OK);
    execute(2, 4, 2, 8, 4);
    assert(fifo_write == 128 && !memcmp(source + 16, fifo, 128));
    equal_byte(fifo + 128, sizeof(fifo) - 128, 0xee);
    equal_byte(destination, sizeof(destination), 0xcc);
    memset(&pool, 0xa5, sizeof(pool));
    for (unsigned n = 0; n < 2; ++n) {
        segments[n].source = (struct a733_dma_endpoint){fifo_bus, 17, 2, 4};
        segments[n].destination = (struct a733_dma_endpoint){destination_bus + 16 + n * 64, 0, 4, 8};
    }
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, segments, 2) == A733_DMA_OK);
    execute(2, 2, 4, 4, 8);
    assert(fifo_read == 128 && !memcmp(source + 16, destination + 16, 128));
    equal_byte(destination, 16, 0xcc);
    equal_byte(destination + 144, sizeof(destination) - 144, 0xcc);
}

static void reject(size_t count, size_t capacity, uint64_t bus,
                   enum a733_dma_result expected)
{
    assert(a733_dma_build(pool.descriptors, capacity, bus, segments, count) == expected);
    equal_byte(&pool, sizeof(pool), 0xa5);
}

static void invalid_chains(void)
{
    setup(1);
    reject(0, 64, descriptor_bus, A733_DMA_ARGUMENT);
    reject(65, 64, descriptor_bus, A733_DMA_ARGUMENT);
    reject(1, 0, descriptor_bus, A733_DMA_ARGUMENT);
    reject(1, 64, descriptor_bus + 4, A733_DMA_ALIGNMENT);
    reject(1, 64, UINT64_C(1) << 32, A733_DMA_ADDRESS);
    reject(1, 64, 0xfffff800, A733_DMA_ADDRESS);
    reject(2, 64, 0xfffff7c0, A733_DMA_ADDRESS);
    segments[1] = segments[0];
    segments[1].source.width = 8;
    reject(2, 64, descriptor_bus, A733_DMA_FORMAT);
    segments[1] = segments[0];
    segments[1].source.burst = 2;
    reject(2, 64, descriptor_bus, A733_DMA_FORMAT);
    segments[1] = segments[0];
    segments[1].destination.request = 64;
    reject(2, 64, descriptor_bus, A733_DMA_FORMAT);
    segments[1] = segments[0];
    segments[1].source.request = 17;
    segments[1].destination.request = 23;
    reject(2, 64, descriptor_bus, A733_DMA_FORMAT);
    segments[1] = segments[0];
    segments[1].bytes = 0;
    reject(2, 64, descriptor_bus, A733_DMA_FORMAT);
    segments[1].bytes = A733_DMA_MAX_SEGMENT_BYTES + 4;
    reject(2, 64, descriptor_bus, A733_DMA_FORMAT);
    segments[1].bytes = 63;
    reject(2, 64, descriptor_bus, A733_DMA_ALIGNMENT);
    segments[1] = segments[0];
    ++segments[1].source.address;
    reject(2, 64, descriptor_bus, A733_DMA_ALIGNMENT);
    segments[1] = segments[0];
    segments[1].source.address = UINT64_C(1) << 35;
    reject(2, 64, descriptor_bus, A733_DMA_ADDRESS);
    segments[1].source.address = (UINT64_C(1) << 35) - 32;
    reject(2, 64, descriptor_bus, A733_DMA_ADDRESS);
    segments[1].source.address = UINT64_MAX - 3;
    reject(2, 64, descriptor_bus, A733_DMA_ADDRESS);
    segments[1] = segments[0];
    segments[1].destination.address = descriptor_bus + 64;
    reject(2, 64, descriptor_bus, A733_DMA_ALIAS);
    segments[1] = segments[0];
    reject(2, 64, descriptor_bus, A733_DMA_ALIAS); /* Two writes to one range. */
    segments[1].destination.address = segments[0].source.address + 32;
    reject(2, 64, descriptor_bus, A733_DMA_ALIAS); /* Later write damages source. */
    segments[1].destination.address = destination_bus + 96;
    segments[1].source.address = segments[0].destination.address + 32;
    reject(2, 64, descriptor_bus, A733_DMA_ALIAS); /* Copy depends on prior write. */
    segments[1].source = (struct a733_dma_endpoint){segments[0].source.address, 17, 4, 8};
    reject(2, 64, descriptor_bus, A733_DMA_ALIAS); /* Port aliases memory. */
    assert(a733_dma_build(NULL, 64, descriptor_bus, segments, 1) == A733_DMA_ARGUMENT);
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, NULL, 1) == A733_DMA_ARGUMENT);
    assert(a733_dma_build((void *)((uint8_t *)pool.descriptors + 1), 64,
                         descriptor_bus, segments, 1) == A733_DMA_ALIGNMENT);
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus,
                         (const void *)pool.descriptors, 1) == A733_DMA_ALIAS);
    equal_byte(&pool, sizeof(pool), 0xa5);
}

static void encoding_boundaries(void)
{
    setup(1);
    segments[0].source.address = UINT64_C(0x100000000);
    segments[0].destination = (struct a733_dma_endpoint){UINT64_C(0x700000200), 19, 2, 4};
    segments[0].bytes = 16;
    static const uint8_t golden[24] = {
        0x80,0x04,0x53,0x03, 0,0,0,0, 0,2,0,0,
        16,0,0,0, 8,0x88,3,0, 0,0xf8,0xff,0xff
    };
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, segments, 1) == A733_DMA_OK);
    assert(!memcmp(pool.descriptors[0].bytes, golden, sizeof(golden)));
    setup(1);
    segments[0].source.address = (UINT64_C(1) << 35) - 64;
    assert(a733_dma_build(pool.descriptors, 64, 0xfffff7c0, segments, 1) == A733_DMA_OK);
    assert(word(pool.descriptors[0].bytes + 4) == 0xffffffc0);
    assert(((word(pool.descriptors[0].bytes + 16) >> 11) & 7) == 7);
    assert(word(pool.descriptors[0].bytes + 20) == 0xfffff800);
    setup(1);
    segments[0].source.address = UINT64_C(0xffffffe0);
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, segments, 1) == A733_DMA_OK);
    assert(word(pool.descriptors[0].bytes + 4) == 0xffffffe0);
    assert(((word(pool.descriptors[0].bytes + 16) >> 11) & 7) == 0);
    setup(1);
    segments[0].bytes = A733_DMA_MAX_SEGMENT_BYTES;
    assert(a733_dma_build(pool.descriptors, 64, descriptor_bus, segments, 1) == A733_DMA_OK);
    assert(word(pool.descriptors[0].bytes + 12) == 1024 * 1024);
}

int main(void)
{
    memory_chains();
    fifo_chains();
    invalid_chains();
    encoding_boundaries();
    puts("PASS DMA descriptor chains: byte movement, FIFO streams, 35-bit addresses, guards, and atomic refusal. Hardware untested.");
}
