/* A noncoherent RAM/register model with explicit DMA drain and fault injection.
 * Physical clocking, bus ordering, and controller behavior remain unqualified. */
#include "a733_dma_controller.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { DESC_A, SOURCE, DEST_A, DESC_B, DEST_B, REGIONS };
static struct region {
    _Alignas(64) uint8_t cpu[256], device[256];
    uint64_t bus;
} ram[REGIONS];
static struct a733_dma_controller controller;
static struct a733_dma_transfer transfers[2];
static struct a733_dma_mapping mappings[2][2];
static struct a733_dma_segment segments[2][2];
static uint32_t registers[0x2000 / 4];
static const uintptr_t base = 0x04601000;
static const uint64_t port = 0x04001000;
static uint8_t fifo[256];
static size_t fifo_position;
static uint64_t now;
static unsigned writes, caches, invalidates, starts, sequence, last_cache, last_barrier;
static unsigned ignore_stop, ignore_start, ignore_pending;
static unsigned preparation_ticks;
static uintptr_t ignored_address;
static uint32_t ignored_value;

static unsigned reg_index(unsigned channel, unsigned offset)
{ return (0x100 + channel * 0x40 + offset) / 4; }

static uint32_t get(void *cookie, uintptr_t address)
{
    assert(cookie == &controller && address >= base && address < base + 0x2000 && !(address & 3));
    return registers[(address - base) / 4];
}

static void put(void *cookie, uintptr_t address, uint32_t value)
{
    assert(cookie == &controller);
    /* Any write to global routing, old interrupt banks, or other processors'
     * channels fails this model immediately. */
    assert(address >= base + 0x100 && address < base + 0x300 && !(address & 3));
    unsigned channel = (unsigned)(address - base - 0x100) / 0x40;
    unsigned offset = (unsigned)(address - base - 0x100) % 0x40;
    assert(offset == 0 || offset == 4 || offset == 8 || offset == 0x34 || offset == 0x38);
    ++writes;
    ++sequence;
    if (address == ignored_address && value == ignored_value) return;
    uint32_t *reg = &registers[(address - base) / 4];
    if (offset == 0) {
        if (value) {
            assert(value == 1 && controller.channels[channel].transfer);
            assert(last_barrier > last_cache && last_cache != 0);
            assert(registers[reg_index(channel, 0x34)] == 12);
            ++starts;
            if (ignore_start) return;
            *reg = 1;
            registers[0x30 / 4] |= 1u << channel;
        } else if (!ignore_stop) *reg = 0;
    } else if (offset == 0x38) {
        if (!ignore_pending) *reg &= ~value;
    } else *reg = value;
}

static int region_for_cpu(void *cpu, size_t bytes)
{
    for (unsigned i = 0; i < REGIONS; ++i)
        if (cpu == ram[i].cpu && bytes == sizeof(ram[i].cpu)) return (int)i;
    return -1;
}

static void maintain(void *cookie, enum a733_dma_cache operation, void *cpu, size_t bytes)
{
    assert(cookie == &controller);
    int region = region_for_cpu(cpu, bytes);
    assert(region >= 0);
    ++caches;
    now += preparation_ticks;
    last_cache = ++sequence;
    if (operation == A733_DMA_INVALIDATE) {
        ++invalidates;
        /* A destination cannot return to the CPU while a channel writing it
         * remains enabled or has outstanding bus traffic. */
        for (unsigned ch = 0; ch < 16; ++ch) {
            const struct a733_dma_transfer *t = controller.channels[ch].transfer;
            if (!t) continue;
            for (size_t j = 0; j < t->mapping_count; ++j) {
                if (t->mappings[j].cpu == cpu && t->mappings[j].access == A733_DMA_WRITE) {
                    assert(registers[reg_index(ch, 0)] == 0);
                    assert(!(registers[0x30 / 4] & (1u << ch)));
                    assert(registers[reg_index(ch, 0x34)] == 0);
                    assert(last_barrier != 0);
                }
            }
        }
        memcpy(ram[region].cpu, ram[region].device, bytes);
    } else {
        assert(operation == A733_DMA_CLEAN || operation == A733_DMA_CLEAN_INVALIDATE);
        memcpy(ram[region].device, ram[region].cpu, bytes);
    }
}

static void fence(void *cookie)
{ assert(cookie == &controller); last_barrier = ++sequence; }
static uint64_t time_now(void *cookie)
{ assert(cookie == &controller); return now; }
static int validate(void *cookie, const struct a733_dma_mapping *m)
{
    assert(cookie == &controller);
    int region = region_for_cpu(m->cpu, m->bytes);
    return region >= 0 && m->bus == ram[region].bus &&
           ((region == DESC_A || region == DESC_B) == (m->access == A733_DMA_DESCRIPTORS));
}
static int validate_port(void *cookie, const struct a733_dma_endpoint *e, enum a733_dma_access access)
{
    assert(cookie == &controller);
    return e->address == port && e->width == 4 && e->burst == 8 &&
           e->request == (access == A733_DMA_READ ? 17u : 23u);
}

static struct a733_dma_mapping mapping(unsigned region, enum a733_dma_access access)
{
    return (struct a733_dma_mapping){ram[region].cpu, ram[region].bus, sizeof(ram[region].cpu), access};
}

/* A fresh fixture represents an independently reset controller. The production
 * API has no operation that can discard live leases by resetting its context. */
static void setup(void)
{
    memset(&controller, 0, sizeof(controller));
    memset(registers, 0, sizeof(registers));
    registers[0x34 / 4] = 0xff;
    registers[0x38 / 4] = 0xff00;
    for (unsigned i = 0; i < 16; ++i) registers[reg_index(i, 8)] = 0x12340000 + i * 64;
    uint64_t buses[] = {0x41000000, UINT64_C(0x100010000), UINT64_C(0x700020000),
                        0x41000100, UINT64_C(0x700030000)};
    for (unsigned i = 0; i < REGIONS; ++i) {
        ram[i].bus = buses[i];
        memset(ram[i].cpu, i == DESC_A || i == DESC_B ? 0xa5 : 0xcc, sizeof(ram[i].cpu));
        memset(ram[i].device, 0xee, sizeof(ram[i].device));
    }
    for (size_t i = 0; i < sizeof(ram[SOURCE].cpu); ++i) ram[SOURCE].cpu[i] = (uint8_t)(i * 19 + i / 16);
    for (unsigned t = 0; t < 2; ++t) {
        unsigned dst = t ? DEST_B : DEST_A, desc = t ? DESC_B : DESC_A;
        mappings[t][0] = mapping(SOURCE, A733_DMA_READ);
        mappings[t][1] = mapping(dst, A733_DMA_WRITE);
        for (unsigned s = 0; s < 2; ++s)
            segments[t][s] = (struct a733_dma_segment){
                {ram[SOURCE].bus + 32 + s * 96, 0, 4, 8},
                {ram[dst].bus + 16 + s * 128, 0, 4, 8}, 64};
        transfers[t] = (struct a733_dma_transfer){mapping(desc, A733_DMA_DESCRIPTORS),
                                                 segments[t], 2, mappings[t], 2};
    }
    memset(fifo, 0x33, sizeof(fifo));
    fifo_position = 0;
    now = writes = caches = invalidates = starts = sequence = last_cache = last_barrier = 0;
    ignore_stop = ignore_start = ignore_pending = 0;
    preparation_ticks = 0;
    ignored_address = ignored_value = 0;
    const struct a733_dma_operations ops = {get, put, maintain, fence, time_now, validate, validate_port};
    assert(a733_dma_controller_init(&controller, &ops, &controller, base, 0xff, 10) == A733_DMA_CONTROL_OK);
}

static uint8_t *device_address(uint64_t bus, size_t bytes)
{
    for (unsigned i = 0; i < REGIONS; ++i)
        if (bus >= ram[i].bus && bus - ram[i].bus < sizeof(ram[i].device) &&
            bytes <= sizeof(ram[i].device) - (bus - ram[i].bus))
            return ram[i].device + (bus - ram[i].bus);
    assert(!"DMA addressed unmapped memory");
    return NULL;
}
static uint32_t load_word(const uint8_t *p)
{ return p[0] + (uint32_t)p[1] * 256 + (uint32_t)p[2] * 65536 + (uint32_t)p[3] * 16777216; }

static void execute(unsigned channel)
{
    assert(registers[reg_index(channel, 0)] == 1);
    uint32_t next = registers[reg_index(channel, 8)];
    unsigned steps = 0;
    while (next != 0xfffff800) {
        assert(++steps <= 64);
        const uint8_t *p = device_address(next, 24);
        uint32_t config = load_word(p), params = load_word(p + 16), bytes = load_word(p + 12);
        uint64_t src = load_word(p + 4) + ((uint64_t)((params >> 11) & 7) << 32);
        uint64_t dst = load_word(p + 8) + ((uint64_t)((params >> 15) & 7) << 32);
        unsigned src_port = config & 63, dst_port = (config >> 16) & 63;
        assert(bytes <= 256);
        const uint8_t *input = NULL;
        uint8_t *output = NULL;
        if (src_port) assert(src_port == 17 && src == port);
        else input = device_address(src, bytes);
        if (dst_port) assert(dst_port == 23 && dst == port);
        else output = device_address(dst, bytes);
        for (unsigned i = 0; i < bytes; ++i) {
            if (src_port || dst_port) assert(fifo_position < sizeof(fifo));
            uint8_t value = src_port ? fifo[fifo_position++] : input[i];
            if (dst_port) fifo[fifo_position++] = value;
            else output[i] = value;
        }
        next = load_word(p + 20);
    }
    registers[reg_index(channel, 0x38)] |= 7; /* Half, package, queue. */
    /* Queue completion precedes our explicit simulated bus drain. */
}
static void drain(unsigned channel)
{
    assert(registers[reg_index(channel, 0)] == 0);
    registers[0x30 / 4] &= ~(1u << channel);
}
static void unchanged(unsigned region, uint8_t value)
{
    for (unsigned i = 0; i < 256; ++i) assert(ram[region].cpu[i] == value);
}
static void copied(unsigned region)
{
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t expected = 0xcc;
        if (i >= 16 && i < 80) expected = ram[SOURCE].cpu[32 + i - 16];
        if (i >= 144 && i < 208) expected = ram[SOURCE].cpu[128 + i - 144];
        assert(ram[region].cpu[i] == expected);
    }
}

static void normal_completion(void)
{
    setup();
    registers[reg_index(7, 0x38)] = 8;
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
    assert(a733_dma_owns(&controller, &transfers[0]));
    assert(a733_dma_poll(&controller, 0) == A733_DMA_ACTIVE);
    execute(0);
    unchanged(DEST_A, 0xcc); /* CPU still has its cached destination. */
    assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING);
    assert(invalidates == 0 && a733_dma_owns(&controller, &transfers[0]));
    assert(a733_dma_release(&controller, 0) == A733_DMA_CONFLICT);
    drain(0);
    assert(a733_dma_poll(&controller, 0) == A733_DMA_COMPLETE);
    assert(!a733_dma_owns(&controller, &transfers[0]) && invalidates == 1);
    copied(DEST_A);
    assert(registers[reg_index(0, 8)] == 0x12340000 && registers[reg_index(7, 0x38)] == 8);
    unsigned prior = writes;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_COMPLETE && writes == prior);
    assert(a733_dma_release(&controller, 0) == A733_DMA_CONTROL_OK);
}

static void concurrent_ownership(void)
{
    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_claim(&controller, 1) == A733_DMA_CONTROL_OK);
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
    unsigned prior_writes = writes, prior_caches = caches;
    assert(a733_dma_submit(&controller, 1, &transfers[0], 100) == A733_DMA_CONFLICT);
    mappings[1][1] = mapping(DEST_A, A733_DMA_WRITE);
    for (unsigned s = 0; s < 2; ++s) {
        /* Distinct written bytes still share the first transfer's cache lines. */
        segments[1][s].destination.address = ram[DEST_A].bus + 96 + s * 128;
        segments[1][s].bytes = 16;
    }
    assert(a733_dma_submit(&controller, 1, &transfers[1], 100) == A733_DMA_CONFLICT);
    unchanged(DESC_B, 0xa5);
    assert(writes == prior_writes && caches == prior_caches);
    mappings[1][1] = mapping(DEST_B, A733_DMA_WRITE);
    for (unsigned s = 0; s < 2; ++s) {
        segments[1][s].destination.address = ram[DEST_B].bus + 16 + s * 128;
        segments[1][s].bytes = 64;
    }
    assert(a733_dma_submit(&controller, 1, &transfers[1], 100) == A733_DMA_ACTIVE);
    execute(0);
    assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING);
    drain(0);
    assert(a733_dma_poll(&controller, 0) == A733_DMA_COMPLETE);
    assert(a733_dma_owns(&controller, &transfers[1]));
    copied(DEST_A);
    unchanged(DEST_B, 0xcc);
    execute(1);
    assert(a733_dma_poll(&controller, 1) == A733_DMA_STOPPING);
    drain(1);
    assert(a733_dma_poll(&controller, 1) == A733_DMA_COMPLETE);
    copied(DEST_B);
}

static void cancellation_and_failed_stop(void)
{
    for (unsigned fault = 0; fault < 2; ++fault) {
        setup();
        assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
        assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
        ignore_stop = fault;
        assert(a733_dma_cancel(&controller, 0) == A733_DMA_STOPPING);
        assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING);
        assert(invalidates == 0 && a733_dma_owns(&controller, &transfers[0]));
        if (!fault) {
            drain(0);
            assert(a733_dma_poll(&controller, 0) == A733_DMA_CANCELED);
            unchanged(DEST_A, 0xcc);
        } else {
            now = 10;
            assert(a733_dma_poll(&controller, 0) == A733_DMA_RECOVERY);
            assert(a733_dma_owns(&controller, &transfers[0]) && invalidates == 0);
            assert(a733_dma_release(&controller, 0) == A733_DMA_RECOVERY);
            assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_RECOVERY);
            assert(a733_dma_claim(&controller, 1) == A733_DMA_CONTROL_OK);
            assert(a733_dma_submit(&controller, 1, &transfers[0], 100) == A733_DMA_CONFLICT);
            unsigned prior = writes;
            assert(a733_dma_poll(&controller, 0) == A733_DMA_RECOVERY && writes == prior);
        }
    }
}

static void faults_and_timeouts(void)
{
    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    preparation_ticks = 3;
    assert(a733_dma_submit(&controller, 0, &transfers[0], 8) == A733_DMA_STOPPING);
    assert(starts == 0 && a733_dma_owns(&controller, &transfers[0]));
    assert(a733_dma_poll(&controller, 0) == A733_DMA_FAILED);
    unchanged(DEST_A, 0xcc);

    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    ignored_address = base + 0x108;
    ignored_value = (uint32_t)ram[DESC_A].bus;
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_STOPPING);
    assert(starts == 0 && a733_dma_owns(&controller, &transfers[0]));
    assert(a733_dma_poll(&controller, 0) == A733_DMA_FAILED);
    unchanged(DEST_A, 0xcc);

    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    ignore_start = 1;
    now = UINT64_MAX - 4;
    assert(a733_dma_submit(&controller, 0, &transfers[0], 8) == A733_DMA_ACTIVE);
    now = 2;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_ACTIVE);
    now = 3;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_FAILED);
    assert(!a733_dma_owns(&controller, &transfers[0]));

    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
    execute(0);
    registers[reg_index(0, 0x38)] |= 8;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING);
    drain(0);
    assert(a733_dma_poll(&controller, 0) == A733_DMA_FAILED); /* Error wins queue end. */

    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
    execute(0);
    ignore_pending = 1;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING);
    drain(0);
    now = 10;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_RECOVERY);
    assert(invalidates == 0 && a733_dma_owns(&controller, &transfers[0]));

    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
    execute(0);
    assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING);
    drain(0);
    ignored_address = base + 0x108;
    ignored_value = 0x12340000;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_RECOVERY);
    assert(invalidates == 0 && a733_dma_owns(&controller, &transfers[0]));

    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
    registers[0x38 / 4] |= 1;
    unsigned prior = writes;
    assert(a733_dma_poll(&controller, 0) == A733_DMA_RECOVERY && writes == prior);
    assert(invalidates == 0 && a733_dma_owns(&controller, &transfers[0]));
}

static void refusal(void)
{
    static const unsigned dirty[] = {0, 4, 0x28, 0x34, 0x38, 0x3c};
    for (unsigned i = 0; i < sizeof(dirty) / sizeof(dirty[0]); ++i) {
        setup();
        registers[reg_index(0, dirty[i])] = 1;
        assert(a733_dma_claim(&controller, 0) == A733_DMA_UNAVAILABLE && writes == 0);
    }
    setup();
    registers[0x30 / 4] = 1;
    assert(a733_dma_claim(&controller, 0) == A733_DMA_UNAVAILABLE && writes == 0);
    assert(a733_dma_claim(&controller, 8) == A733_DMA_UNAVAILABLE && writes == 0);
    assert(a733_dma_claim(&controller, 16) == A733_DMA_INVALID && writes == 0);
    registers[0x30 / 4] = 0;
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONFLICT);
    mappings[0][1].bus += 64; /* Noncanonical translation rejected by platform. */
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_INVALID);
    mappings[0][1] = mapping(DEST_A, A733_DMA_READ); /* Wrong DMA direction. */
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_INVALID);
    mappings[0][1] = mapping(DEST_A, A733_DMA_WRITE);
    segments[0][1].bytes = 63; /* Later encoder error must leave all output intact. */
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_INVALID);
    unchanged(DESC_A, 0xa5);
    assert(writes == 0 && caches == 0 && !a733_dma_owns(&controller, &transfers[0]));
}

static void fifo_transfers(void)
{
    for (unsigned receive = 0; receive < 2; ++receive) {
        setup();
        for (unsigned t = 0; t < 2; ++t) {
            for (unsigned s = 0; s < 2; ++s) {
                struct a733_dma_endpoint *e = receive ? &segments[t][s].source : &segments[t][s].destination;
                *e = (struct a733_dma_endpoint){port, receive ? 17 : 23, 4, 8};
            }
            transfers[t].mapping_count = 1;
            if (receive) mappings[t][0] = mappings[t][1];
        }
        for (unsigned i = 0; i < sizeof(fifo); ++i) fifo[i] = (uint8_t)(i + 3);
        assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
        assert(a733_dma_claim(&controller, 1) == A733_DMA_CONTROL_OK);
        assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
        unsigned prior = writes;
        assert(a733_dma_submit(&controller, 1, &transfers[1], 100) == A733_DMA_CONFLICT);
        assert(writes == prior);
        unchanged(DESC_B, 0xa5);
        execute(0);
        assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING);
        drain(0);
        assert(a733_dma_poll(&controller, 0) == A733_DMA_COMPLETE);
        assert(fifo_position == 128);
        if (receive) {
            assert(!memcmp(ram[DEST_A].cpu + 16, fifo, 64));
            assert(!memcmp(ram[DEST_A].cpu + 144, fifo + 64, 64));
        } else {
            assert(!memcmp(fifo, ram[SOURCE].cpu + 32, 64));
            assert(!memcmp(fifo + 64, ram[SOURCE].cpu + 128, 64));
        }
    }
}

/* Interrupt waiting: only an ACTIVE channel whose deadline is beyond the
 * margin qualifies, and only interrupt-enabled acknowledgements are counted. */
static void event_wait(void)
{
    setup();
    assert(a733_dma_claim(&controller, 0) == A733_DMA_CONTROL_OK);
    assert(!a733_dma_event_wait(&controller, 0, 0) && !a733_dma_event_wait(&controller, 1, 0));
    now = UINT64_MAX - 4; /* The deadline arithmetic crosses counter wrap. */
    assert(a733_dma_submit(&controller, 0, &transfers[0], 100) == A733_DMA_ACTIVE);
    assert(a733_dma_event_wait(&controller, 0, 99) && !a733_dma_event_wait(&controller, 0, 100));
    now = 5;
    assert(a733_dma_event_wait(&controller, 0, 89) && !a733_dma_event_wait(&controller, 0, 90));
    registers[reg_index(0, 0x38)] = 1; /* Half package is not interrupt-enabled. */
    assert(a733_dma_poll(&controller, 0) == A733_DMA_ACTIVE && controller.events == 0);
    execute(0);
    assert(a733_dma_poll(&controller, 0) == A733_DMA_STOPPING && controller.events == 1);
    assert(!a733_dma_event_wait(&controller, 0, 0));
    drain(0);
    assert(a733_dma_poll(&controller, 0) == A733_DMA_COMPLETE && controller.events == 1);
    assert(!a733_dma_event_wait(&controller, 0, 0));
}

int main(void)
{
    normal_completion();
    concurrent_ownership();
    cancellation_and_failed_stop();
    faults_and_timeouts();
    refusal();
    fifo_transfers();
    event_wait();
    puts("PASS DMA controller: noncoherent transfers, concurrent leases, bounded cancellation, and retained buffers after faults and interrupt-wait eligibility. Hardware untested.");
}
