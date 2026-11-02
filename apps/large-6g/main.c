/* large-6g: a 6 GiB read-write region with no source, in upper memory above 4 GiB (docs/designs/large-app-regions.md).
 * The OS places it at the first free 1 GiB boundary and the application reports the base it got. The region is not
 * loaded from the card and nothing in a timed loop checks or hashes data.
 *
 *   LARGE_6G_PROBE 0: bandwidth and integrity.
 *       Bandwidth: read, write and copy by one core with 64-byte-per-instruction vector accesses (the kernels of
 *       apps/dram-bandwidth/kernel.c for the read), over a 40 MiB window (two warm-up and eight timed passes) and over
 *       the whole region (two timed passes); a copy moves the first half of the region onto the second. Every pass is
 *       timed with the 24 MHz counter. The only code between a pass's two counter reads is the pass itself and, in the
 *       full passes, one store of the supervision record per 256 MiB.
 *       Integrity, afterwards and untimed: write the 64-bit hash of each word's address over the whole region, read it
 *       back, count the words that differ.
 *   LARGE_6G_PROBE 1: the byte below the region, read at EL0 in the application's space: a fault.
 *   LARGE_6G_PROBE 2: the byte above the region: a fault.
 *   LARGE_6G_PROBE 3: a task that runs in no application space reads the region's first byte: a fault.
 * Probes 1 to 3 first read the region's first and last words, so a fault they report is the probe's own. */
#include "large_6g-app.h"
#include "FreeRTOS.h"
#include "task.h"

#ifndef LARGE_6G_PROBE
#define LARGE_6G_PROBE 0
#endif
#define MIB (UINT64_C(1) << 20)
#define GIB (UINT64_C(1) << 30)
#define WINDOW_BYTES (40 * MIB)
#define WINDOW_WARM 2u
#define WINDOW_PASSES 8u
#define FULL_PASSES 2u
#define CHUNK (256 * MIB)
#define TAG_DONE UINT64_C(0x36474f4e45)        /* "6GONE" */
#define TAG_NOT_READY UINT64_C(0x36474e4f54)   /* "6GNOT" */

/* The kernels. Each processes a multiple of 128 bytes. */
__attribute__((noinline)) static uint64_t k_read(const uint64_t *p, size_t bytes)
{
    uint64_t lo, hi;
    const uint64_t *end = p + bytes / 8;
    __asm__ volatile(
        ".arch_extension simd\n"
        "movi v16.16b,#0\n movi v17.16b,#0\n movi v18.16b,#0\n movi v19.16b,#0\n"
        "1:\n"
        "ld1 {v0.16b-v3.16b},[%[p]],#64\n"
        "ld1 {v4.16b-v7.16b},[%[p]],#64\n"
        "eor v16.16b,v16.16b,v0.16b\n eor v17.16b,v17.16b,v1.16b\n"
        "eor v18.16b,v18.16b,v2.16b\n eor v19.16b,v19.16b,v3.16b\n"
        "eor v16.16b,v16.16b,v4.16b\n eor v17.16b,v17.16b,v5.16b\n"
        "eor v18.16b,v18.16b,v6.16b\n eor v19.16b,v19.16b,v7.16b\n"
        "cmp %[p],%[end]\n b.lo 1b\n"
        "eor v16.16b,v16.16b,v17.16b\n eor v18.16b,v18.16b,v19.16b\n eor v16.16b,v16.16b,v18.16b\n"
        "mov %[lo],v16.d[0]\n mov %[hi],v16.d[1]\n"
        : [p] "+r"(p), [lo] "=r"(lo), [hi] "=r"(hi)
        : [end] "r"(end)
        : "v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","cc","memory");
    return lo ^ hi;
}
__attribute__((noinline)) static void k_write(uint64_t *p, size_t bytes, uint64_t value)
{
    uint64_t *end = p + bytes / 8;
    __asm__ volatile(
        ".arch_extension simd\n"
        "dup v0.2d,%[v]\n mov v1.16b,v0.16b\n mov v2.16b,v0.16b\n mov v3.16b,v0.16b\n"
        "1:\n"
        "st1 {v0.16b-v3.16b},[%[p]],#64\n"
        "st1 {v0.16b-v3.16b},[%[p]],#64\n"
        "cmp %[p],%[end]\n b.lo 1b\n"
        : [p] "+r"(p)
        : [end] "r"(end), [v] "r"(value)
        : "v0","v1","v2","v3","cc","memory");
}
__attribute__((noinline)) static void k_copy(uint64_t *dst, const uint64_t *src, size_t bytes)
{
    const uint64_t *end = src + bytes / 8;
    __asm__ volatile(
        ".arch_extension simd\n"
        "1:\n"
        "ld1 {v0.16b-v3.16b},[%[s]],#64\n"
        "ld1 {v4.16b-v7.16b},[%[s]],#64\n"
        "st1 {v0.16b-v3.16b},[%[d]],#64\n"
        "st1 {v4.16b-v7.16b},[%[d]],#64\n"
        "cmp %[s],%[end]\n b.lo 1b\n"
        : [s] "+r"(src), [d] "+r"(dst)
        : [end] "r"(end)
        : "v0","v1","v2","v3","v4","v5","v6","v7","cc","memory");
}

/* The integrity pattern: the splitmix64 finalizer of the word's address. */
static inline uint64_t pattern(uint64_t address)
{
    uint64_t z = address + UINT64_C(0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

/* A whole-region pass is run in chunks so that the supervision record is stored between them. */
enum { OP_READ, OP_WRITE, OP_COPY };
static uint64_t sink;
static void run_op(unsigned op, uint64_t *base, uint64_t bytes, uint64_t *other)
{
    for (uint64_t off = 0; off < bytes; off += CHUNK) {
        uint64_t n = bytes - off < CHUNK ? bytes - off : CHUNK;
        if (op == OP_READ) sink ^= k_read(base + off / 8, n);
        else if (op == OP_WRITE) k_write(base + off / 8, n, UINT64_C(0xa5a5a5a55a5a5a5a) ^ off);
        else k_copy(other + off / 8, base + off / 8, n);
        if (bytes > CHUNK) ss_progress();
    }
}
static uint64_t timed(unsigned op, uint64_t *base, uint64_t bytes, uint64_t *other)
{
    uint64_t t0 = ss_counter_ticks();
    run_op(op, base, bytes, other);
    return ss_counter_ticks() - t0;
}

/* The result, little-endian, 512 bytes. */
struct result {
    uint64_t tag, state, error, region_bytes, region_base, probe;
    uint64_t window_bytes, full_bytes;                       /* bytes read, written or copied per pass */
    uint64_t window_ticks[3][WINDOW_PASSES];                 /* read, write, copy */
    uint64_t full_ticks[3][FULL_PASSES];
    uint64_t integrity_words, integrity_mismatches, integrity_first_bad, integrity_write_ticks, integrity_read_ticks;
    uint64_t sink;
    uint64_t reserved[64 - 8 - 3 * WINDOW_PASSES - 3 * FULL_PASSES - 6];
};
_Static_assert(sizeof(struct result) == 512, "result is the manifest's output_limit");

#if LARGE_6G_PROBE == 3
static StaticTask_t intruder_state;
static StackType_t intruder_stack[512];
static volatile uint8_t *intruder_target;
static void intruder(void *unused)
{
    (void)unused;
    (void)*intruder_target;                    /* no application space: a permission fault at EL0 */
    ss_fail(0xe03);
}
#endif

void large_6g_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct result r;
    while (!ss_clock_ready()) { ss_progress(); vTaskDelay(1); }
    int ready;
    do { ss_progress(); ready = ss_large_wait(LARGE_6G_LARGE_BIG, 1000000); } while (ready == 0 && ss_live() >= 0);
    struct ss_large_status status;
    ss_large_status(LARGE_6G_LARGE_BIG, &status);
    r.state = status.state; r.error = status.error; r.probe = LARGE_6G_PROBE;
    size_t bytes = 0;
    uint64_t *base = ready == 1 ? ss_large_base(LARGE_6G_LARGE_BIG, &bytes) : 0;
    r.region_bytes = bytes; r.region_base = (uint64_t)(uintptr_t)base;
    if (!base || bytes != 6 * GIB) { r.tag = TAG_NOT_READY; ss_finish(&r, sizeof r); }
#if LARGE_6G_PROBE
    /* The region itself is reachable, so what follows is the probe's own fault. */
    sink ^= base[0] ^ base[bytes / 8 - 1];
    ss_progress();
#if LARGE_6G_PROBE == 1
    sink ^= *(volatile const uint8_t *)((const uint8_t *)base - 1);
#elif LARGE_6G_PROBE == 2
    sink ^= *(volatile const uint8_t *)((const uint8_t *)base + bytes);
#else
    intruder_target = (volatile uint8_t *)base;
    xTaskCreateStatic(intruder, "intruder", 512, NULL, 3, intruder_stack, &intruder_state);
    for (;;) { ss_progress(); vTaskDelay(10); }
#endif
    /* Reached only if the probe did not fault. */
    r.tag = TAG_DONE; r.sink = sink;
    ss_finish(&r, sizeof r);
#else
    uint64_t *copy_dst = base + (bytes / 2) / 8;
    r.window_bytes = WINDOW_BYTES; r.full_bytes = bytes;
    for (unsigned op = 0; op < 3; ++op) {
        for (unsigned i = 0; i < WINDOW_WARM + WINDOW_PASSES; ++i) {
            uint64_t t = timed(op, base, WINDOW_BYTES, base + (128 * MIB) / 8);
            if (i >= WINDOW_WARM) r.window_ticks[op][i - WINDOW_WARM] = t;
            ss_progress();
        }
    }
    for (unsigned op = 0; op < 3; ++op)
        for (unsigned i = 0; i < FULL_PASSES; ++i) {
            /* A copy moves the first half onto the second: bytes/2 read and as many written. */
            r.full_ticks[op][i] = op == OP_COPY ? timed(op, base, bytes / 2, copy_dst) : timed(op, base, bytes, 0);
            ss_progress();
        }
    (void)copy_dst;
    /* Integrity: the address hash over the whole region, written and read back; untimed in the sense that no figure
     * of speed is taken from it (the two durations are recorded for the deadline only). */
    uint64_t words = bytes / 8, bad = 0, first = 0;
    uint64_t t0 = ss_counter_ticks();
    for (uint64_t off = 0; off < words; off += CHUNK / 8) {
        uint64_t end = off + CHUNK / 8 < words ? off + CHUNK / 8 : words;
        for (uint64_t i = off; i < end; ++i) base[i] = pattern((uint64_t)(uintptr_t)&base[i]);
        ss_progress();
    }
    r.integrity_write_ticks = ss_counter_ticks() - t0;
    t0 = ss_counter_ticks();
    for (uint64_t off = 0; off < words; off += CHUNK / 8) {
        uint64_t end = off + CHUNK / 8 < words ? off + CHUNK / 8 : words;
        for (uint64_t i = off; i < end; ++i)
            if (base[i] != pattern((uint64_t)(uintptr_t)&base[i])) { if (!bad) first = (uint64_t)(uintptr_t)&base[i]; ++bad; }
        ss_progress();
    }
    r.integrity_read_ticks = ss_counter_ticks() - t0;
    r.integrity_words = words; r.integrity_mismatches = bad; r.integrity_first_bad = first;
    r.sink = sink; r.tag = TAG_DONE;
    ss_finish(&r, sizeof r);
#endif
}
