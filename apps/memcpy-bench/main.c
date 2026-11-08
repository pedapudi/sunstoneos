/* memcpy-bench: the speed of the strict and the unaligned memory routines on one Cortex-A55 core at the granted clock.
 *
 * The application's memcpy and memset calls are renamed to the unaligned routines (include/sunstone_relaxed_memory.h, as for
 * every application), so this file names the two versions it measures: the strict ones that the pre-MMU code links
 * (runtime/memory.c, memmove.c) and ss_memcpy_unaligned, ss_memset_unaligned (runtime/memory_unaligned.c).
 *
 * Each case calls one routine on a source and a destination in the 16 MiB work region, for 32 MiB of copied bytes (so a case
 * with a small size makes many calls), and records the elapsed 24 MHz counter ticks. The sizes cover a block that stays in the
 * L1 data cache, one that stays in the L2 and one that comes from DRAM. The offsets give an aligned pair, a source that is
 * not aligned like the destination, and a pair that both lie off an 8-byte boundary. The result lists the cases in order.
 *
 * Six further cases copy rows the way the encoder fills the position window of its attention step: 176 rows of 382 bytes, each
 * after a memset of 40 bytes, from a table whose rows are 2 bytes off an 8-byte boundary, into rows 382 bytes apart. The
 * table rows lie 17000 bytes apart and rotate through 8 MiB, so the source is not in the cache ("cold rows"), or 1000 bytes
 * apart in 176 KB ("warm rows"). Each is run with the strict memcpy, ss_memcpy_unaligned and ss_memmove_unaligned.
 *
 * Three more cases write the rows into a destination that is not in the cache, as the encoder's position window is after the
 * queue has cleaned and invalidated its input buffer: the destination moves by 67232 bytes a pass through 3 MiB. They run
 * the strict routines, ss_memcpy_unaligned and ss_memset_unaligned, and the first unaligned routines (kept here for the
 * comparison), which stored the head and the tail of a copy again out of order. */
#include "memcpy_bench-app.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef memcpy
#undef memset
void *memcpy(void *restrict dest, const void *restrict src, size_t count);
void *memset(void *dest, int value, size_t count);
void *ss_memcpy_unaligned(void *restrict dest, const void *restrict src, size_t count);
void *ss_memset_unaligned(void *dest, int value, size_t count);
void *ss_memmove_unaligned(void *dest, const void *src, size_t count);

struct bench_case { uint32_t routine, size, src_offset, dst_offset; uint64_t iterations, ticks; };
_Static_assert(sizeof(struct bench_case) == 32, "record layout");
struct bench_result { uint32_t cases, cluster_mhz_hint; uint64_t counter_hz; struct bench_case record[96]; };
_Static_assert(sizeof(struct bench_result) <= 4096, "output_limit");

enum { STRICT_COPY, UNALIGNED_COPY, STRICT_SET, UNALIGNED_SET, ROWS_STRICT, ROWS_COPY, ROWS_MOVE, COLD_STRICT, COLD_COPY, COLD_OVERLAP };
#define TARGET_BYTES (32u << 20)

static struct bench_result result;

static inline void one(unsigned routine, uint8_t *d, const uint8_t *s, size_t n)
{
    switch (routine) {
    case STRICT_COPY: memcpy(d, s, n); break;
    case UNALIGNED_COPY: ss_memcpy_unaligned(d, s, n); break;
    case STRICT_SET: memset(d, 0x5a, n); break;
    default: ss_memset_unaligned(d, 0x5a, n); break;
    }
    __asm__ volatile("" ::: "memory");
}

static void measure(unsigned routine, uint8_t *work, uint32_t size, uint32_t src_offset, uint32_t dst_offset)
{
    uint8_t *const source = work + src_offset;
    uint8_t *const dest = work + (8u << 20) + dst_offset;
    const uint64_t iterations = size >= TARGET_BYTES ? 1 : TARGET_BYTES / size;
    for (unsigned warm = 0; warm < 2; ++warm) one(routine, dest, source, size);
    ss_progress();
    const uint64_t begin = ss_counter_ticks();
    for (uint64_t i = 0; i < iterations; ++i) one(routine, dest, source, size);
    const uint64_t end = ss_counter_ticks();
    struct bench_case *c = &result.record[result.cases++];
    *c = (struct bench_case){routine, size, src_offset, dst_offset, iterations, end - begin};
}

/* The first unaligned routines: both ends are loaded first and stored last, so the stores are out of address order. */
static void *overlapped_copy(void *dest, const void *src, size_t count)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    uint64_t t0, t1, t2, t3, h0, h1;
    __builtin_memcpy(&t0, s + count - 32, 8); __builtin_memcpy(&t1, s + count - 24, 8);
    __builtin_memcpy(&t2, s + count - 16, 8); __builtin_memcpy(&t3, s + count - 8, 8);
    __builtin_memcpy(&h0, s, 8); __builtin_memcpy(&h1, s + 8, 8);
    uint8_t *const begin = d, *const end = d + count;
    const size_t skew = 16 - ((uintptr_t)d & 15);
    d += skew; s += skew; count -= skew;
    for (; count >= 32; count -= 32, d += 32, s += 32) {
        uint64_t a, b, c, e;
        __builtin_memcpy(&a, s, 8); __builtin_memcpy(&b, s + 8, 8); __builtin_memcpy(&c, s + 16, 8); __builtin_memcpy(&e, s + 24, 8);
        __builtin_memcpy(d, &a, 8); __builtin_memcpy(d + 8, &b, 8); __builtin_memcpy(d + 16, &c, 8); __builtin_memcpy(d + 24, &e, 8);
    }
    __builtin_memcpy(begin, &h0, 8); __builtin_memcpy(begin + 8, &h1, 8);
    __builtin_memcpy(end - 32, &t0, 8); __builtin_memcpy(end - 24, &t1, 8);
    __builtin_memcpy(end - 16, &t2, 8); __builtin_memcpy(end - 8, &t3, 8);
    return dest;
}

#define ROW_COUNT 176u
#define ROW_BYTES 382u

/* One pass of the position window: for each row a memset of 40 bytes, then the copy of the row. */
static inline void rows_pass(unsigned routine, uint8_t *dst, const uint8_t *table, uint32_t stride, uint32_t span, uint64_t pass)
{
    for (unsigned row = 0; row < ROW_COUNT; ++row) {
        uint8_t *const d = dst + (size_t)row * ROW_BYTES;
        const uint8_t *const s = table + 34 + (((pass * ROW_COUNT + row) * (uint64_t)stride) % span);
        ss_memset_unaligned(d, 0, 40);
        if (routine == ROWS_STRICT) memcpy(d + 40, s, ROW_BYTES - 40);
        else if (routine == ROWS_COPY) ss_memcpy_unaligned(d + 40, s, ROW_BYTES - 40);
        else ss_memmove_unaligned(d + 40, s, ROW_BYTES - 40);
    }
    __asm__ volatile("" ::: "memory");
}

/* Rows into a destination that is not in the cache: the strict routine, the unaligned routine, the first unaligned routine. */
static void measure_cold_destination(unsigned routine, uint8_t *work, uint64_t passes)
{
    const uint8_t *const table = work;
    uint8_t *const dst = work + (12u << 20);
    const uint32_t pass_bytes = ROW_COUNT * ROW_BYTES;
    uint64_t begin = 0;
    for (uint64_t pass = 0; pass < passes + 1; ++pass) {
        if (pass == 1) { ss_progress(); begin = ss_counter_ticks(); }
        uint8_t *const base = dst + (size_t)((pass * pass_bytes) % (3u << 20));
        for (unsigned row = 0; row < ROW_COUNT; ++row) {
            uint8_t *const d = base + (size_t)row * ROW_BYTES;
            const uint8_t *const s = table + 34 + row * 1000u;
            if (routine == COLD_STRICT) { memset(d, 0, 40); memcpy(d + 40, s, ROW_BYTES - 40); }
            else if (routine == COLD_COPY) { ss_memset_unaligned(d, 0, 40); ss_memcpy_unaligned(d + 40, s, ROW_BYTES - 40); }
            else { ss_memset_unaligned(d, 0, 40); overlapped_copy(d + 40, s, ROW_BYTES - 40); }
        }
        __asm__ volatile("" ::: "memory");
    }
    const uint64_t end = ss_counter_ticks();
    struct bench_case *c = &result.record[result.cases++];
    *c = (struct bench_case){routine, ROW_COUNT * ROW_BYTES, 1000, 0, passes, end - begin};
}

static void measure_rows(unsigned routine, uint8_t *work, uint32_t stride, uint32_t span, uint64_t passes)
{
    uint8_t *const table = work, *const dst = work + (12u << 20);
    rows_pass(routine, dst, table, stride, span, 0);
    ss_progress();
    const uint64_t begin = ss_counter_ticks();
    for (uint64_t pass = 0; pass < passes; ++pass) rows_pass(routine, dst, table, stride, span, pass);
    const uint64_t end = ss_counter_ticks();
    struct bench_case *c = &result.record[result.cases++];
    *c = (struct bench_case){routine, ROW_COUNT * ROW_BYTES, stride, 0, passes, end - begin};
}

void memcpy_bench_main(const struct ss_grant *grant)
{
    uint8_t *const work = grant->regions[MEMCPY_BENCH_REGION_WORK].base;
    while (!ss_clock_ready()) { if (ss_live() < 0) ss_fail(1); ss_progress(); }
    for (size_t i = 0; i < (16u << 20); i += 8) *(volatile uint64_t *)(work + i) = i * 0x9e3779b97f4a7c15ull;
    result.counter_hz = 24000000;
    static const uint32_t sizes[] = {64, 1024, 4096, 32768, 262144, 4194304};
    static const uint32_t offsets[][2] = {{0, 0}, {3, 0}, {5, 11}};
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s)
        for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; ++o)
            for (unsigned routine = STRICT_COPY; routine <= UNALIGNED_COPY; ++routine)
                measure(routine, work, sizes[s], offsets[o][0], offsets[o][1]);
    static const uint32_t set_sizes[] = {64, 4096, 262144};
    for (unsigned s = 0; s < sizeof set_sizes / sizeof set_sizes[0]; ++s)
        for (unsigned o = 0; o < 2; ++o)
            for (unsigned routine = STRICT_SET; routine <= UNALIGNED_SET; ++routine)
                measure(routine, work, set_sizes[s], 0, o ? 3 : 0);
    for (unsigned routine = ROWS_STRICT; routine <= ROWS_MOVE; ++routine) measure_rows(routine, work, 17000, 8u << 20, 400);
    for (unsigned routine = ROWS_STRICT; routine <= ROWS_MOVE; ++routine) measure_rows(routine, work, 1000, 176000, 400);
    for (unsigned routine = COLD_STRICT; routine <= COLD_OVERLAP; ++routine) measure_cold_destination(routine, work, 400);
    ss_finish(&result, sizeof result);
}
