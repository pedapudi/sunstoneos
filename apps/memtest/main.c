/* SPDX-License-Identifier: Apache-2.0 */
/* Returning memory qualification image. One task on the boot Cortex-A55
 * admits the host's run contract, tests the DRAM outside the protected window
 * and returns to the ROM's FEL interface with a checksummed record at
 * MEMTEST_RECORD_ADDRESS. docs/memory-qualification.md describes the run. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "memtest.h"
#include "a733_mapping_check.h"
#include "a733_cpu_clock.h"

#define RECORD ((volatile struct memtest_record *)MEMTEST_RECORD_ADDRESS)
#define DRAM_END (MEMTEST_DRAM_BASE + ((uint64_t)MEMTEST_DRAM_BLOCKS << MEMTEST_BLOCK_SHIFT))
#define PAGE_LINES 64u
/* Model march cost for the first group, before any group has been timed:
 * 1.45 s per 256 MiB at the slow rate of the cost model. */
#define MARCH_TICKS_PER_MIB UINT64_C(136000)
/* Time kept for the harness after the march when the contract selects it. */
#define HARNESS_RESERVE (5u * MEMTEST_COUNTER_HZ / 2u)
#define HARNESS_SAMPLE_LIMIT (MEMTEST_COUNTER_HZ / 2u)

/* Data-line and address-line patterns. Their bits 34-63 are clear of K's. */
#define ADDRESS_PATTERN UINT64_C(0x00000002a5a55a5a)
#define ADDRESS_TAG UINT64_C(0x0000000100000000)
#define WALK1(i) (MEMTEST_CLEAR_OF_K(UINT64_C(1) << (i)))
#define WALK8(i) (WALK1(i) && WALK1(i + 1) && WALK1(i + 2) && WALK1(i + 3) && \
                  WALK1(i + 4) && WALK1(i + 5) && WALK1(i + 6) && WALK1(i + 7))
_Static_assert(WALK8(0) && WALK8(8) && WALK8(16) && WALK8(24) && WALK8(32) && WALK8(40) &&
               WALK8(48) && WALK8(56) && MEMTEST_CLEAR_OF_K(0), "walking patterns clear of K");
_Static_assert(MEMTEST_CLEAR_OF_K(ADDRESS_PATTERN) && MEMTEST_CLEAR_OF_K(ADDRESS_TAG + 33),
               "address-line words clear of K");
_Static_assert(MEMTEST_CLEAR_OF_K(DRAM_END), "chase pointers clear of K");

/* Host-written contract at a linker-placed address; zero in the image. */
struct memtest_contract memtest_contract __attribute__((section(".memtest_contract"), aligned(64)));
/* Recovery region: four pages, from the linker script. */
extern unsigned char __memtest_recovery_start[], __memtest_recovery_end[];
/* Geometry; the emulation checks replace it with a smaller map. */
struct memtest_geometry memtest_geometry = MEMTEST_GEOMETRY_BOARD;

#define FIXED(i) (memtest_geometry.fixed[i])
static StaticTask_t task;
static StackType_t stack[4096];
static struct memtest_contract contract;
static struct memtest_plan plan;
static struct memtest_walk walk;
static uint64_t start, deadline;
static unsigned clock_reads;

static void barrier_dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static void clean_invalidate(uint64_t address)
{ __asm__ volatile("dc civac, %0" :: "r"(address) : "memory"); }

static void renew(void)
{
    uint64_t now = counter();
    if (now < walk.renew_at) return;
    register uint64_t x0 __asm__("x0") = 0x734;
    __asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
    walk.renew_at = now + MEMTEST_RENEW_TICKS;
    walk.renewals++;
}

static _Noreturn void finish(unsigned status, unsigned stop)
{
    volatile struct memtest_record *r = RECORD;
    r->status = status;
    r->stop = stop;
    r->renewals = walk.renewals;
    r->end_ticks = counter();
    r->checksum = memtest_hash(MEMTEST_HASH_SEED, (const volatile uint64_t *)r,
                               offsetof(struct memtest_record, checksum) / 8u);
    barrier_dsb();
    platform_exit(status == MEMTEST_RUN_PASS ? 0 : 1);
}

static _Noreturn void refuse(enum memtest_refusal reason)
{
    RECORD->refusal = reason;
    finish(MEMTEST_RUN_REFUSED, MEMTEST_STOP_NONE);
}

static void mismatch(uint64_t address, uint64_t expected, uint64_t observed, unsigned test, unsigned element)
{
    volatile struct memtest_record *r = RECORD;
    uint64_t n = r->mismatch_count++;
    if (n < MEMTEST_MISMATCHES)
        r->mismatch[n] = (struct memtest_mismatch){address, expected, observed, test, element};
}

static void result(unsigned test, unsigned status, uint64_t words, uint64_t ticks)
{
    RECORD->test[test] = (struct memtest_result){status, 0, words, ticks};
}

/* Recovery digest: clean and invalidate the vector and code lines (lines
 * 0-31 of the first two pages) and lines 32-63 of all four pages, then hash
 * them as DRAM holds them. */
static uint64_t recovery_digest(void)
{
    uint64_t base = (uintptr_t)__memtest_recovery_start, hash = MEMTEST_HASH_SEED;
    for (unsigned page = 0; page < 4; ++page)
        for (unsigned line = page < 2 ? 0 : 32; line < PAGE_LINES; ++line)
            clean_invalidate(base + page * 4096u + line * 64u);
    barrier_dsb();
    for (unsigned page = 0; page < 4; ++page)
        for (unsigned line = page < 2 ? 0 : 32; line < PAGE_LINES; ++line)
            hash = memtest_hash(hash, (const volatile uint64_t *)(uintptr_t)(base + page * 4096u + line * 64u), 8);
    return hash;
}

static void check_digest(void)
{
    uint64_t digest = recovery_digest();
    RECORD->digest_last = digest;
    if (digest != RECORD->digest_admission) finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_DIGEST);
}

static uint64_t block_address(uint32_t block)
{ return MEMTEST_DRAM_BASE + ((uint64_t)block << MEMTEST_BLOCK_SHIFT); }

/* Walk every span of the tested map outside the excluded ranges, ascending.
 * visit returns 0 to stop. */
static int each_span(int (*visit)(uint64_t, uint64_t))
{
    uint32_t block = memtest_geometry.first_block;
    for (unsigned i = 0; i <= contract.excluded_count; ++i) {
        uint32_t end = i < contract.excluded_count ? contract.excluded[i].start : memtest_geometry.end_block;
        if (block < end && !visit(block_address(block), block_address(end))) return 0;
        if (i < contract.excluded_count) block = contract.excluded[i].end;
    }
    return 1;
}

static void walk_ascending(uint64_t from, uint64_t to)
{
    walk.cursor = from;
    walk.end = to;
    walk.block_step = (int64_t)MEMTEST_BLOCK_BYTES;
    walk.line_step = 64;
    walk.seed = contract.seed;
    walk.complement = 0;
    walk.deadline = deadline;
}

/* 1. Window scan. */
static uint64_t scan_pages;
static int scan_span(uint64_t from, uint64_t to)
{
    walk_ascending(from, to);
    int status = memtest_scan(&walk);
    if (status == MEMTEST_WALK_HIT) {
        for (uint64_t page = walk.found; page < walk.found + 0x8000u; page += 4096u)
            for (unsigned word = 0; word < 2; ++word) {
                uint64_t address = page + 0x7c0u + word * 0x800u;
                uint64_t value = *(volatile uint64_t *)(uintptr_t)address;
                if (value - walk.match_base >= walk.match_bytes) continue;
                volatile struct memtest_record *r = RECORD;
                r->scan_page = address;
                r->scan_value = value;
                /* The offsets (address - DRAM base) of the tested word and of
                 * the beacon it read differ in the faulty DRAM address bits. */
                r->address_failed |= ((address - MEMTEST_DRAM_BASE) ^ ((value ^ MEMTEST_K) - MEMTEST_DRAM_BASE)) &
                                     ((UINT64_C(1) << 34) - 64u);
                result(MEMTEST_TEST_SCAN, MEMTEST_FAIL, scan_pages * 2u, counter() - start);
                finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_SCAN);
            }
        /* The kernel's compare and this rescan disagree: treat it as a hit. */
        result(MEMTEST_TEST_SCAN, MEMTEST_FAIL, scan_pages * 2u, counter() - start);
        finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_SCAN);
    }
    if (status == MEMTEST_WALK_BUDGET) finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_BUDGET);
    scan_pages += (to - from) / 4096u;
    return 1;
}

static void window_scan(void)
{
    uint64_t begin = counter();
    RECORD->phase = MEMTEST_TEST_SCAN << 4;
    walk.match_base = MEMTEST_K | (uintptr_t)__memtest_recovery_start;
    walk.match_bytes = (uintptr_t)__memtest_recovery_end - (uintptr_t)__memtest_recovery_start;
    each_span(scan_span);
    RECORD->address_tested |= ((UINT64_C(1) << 34) - 1u) & ~((UINT64_C(1) << 26) - 1u);
    result(MEMTEST_TEST_SCAN, MEMTEST_PASS, scan_pages * 2u, counter() - begin);
}

/* 2. Data lines: walking ones and zeros over one word and one line, with
 * each pattern cleaned to DRAM and invalidated before it is read back. */
static unsigned data_failed;
static uint64_t data_words;
static void data_line(uint64_t address, const uint64_t *pattern, unsigned words)
{
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)address;
    for (unsigned i = 0; i < words; ++i) p[i] = pattern[i];
    clean_invalidate(address);
    barrier_dsb();
    for (unsigned i = 0; i < words; ++i) {
        uint64_t value = p[i];
        if (value != pattern[i]) {
            mismatch(address + 8u * i, pattern[i], value, MEMTEST_TEST_DATA, 0);
            data_failed = 1;
        }
    }
    data_words += words;
}

static void data_lines(void)
{
    uint64_t begin = counter(), pattern[8];
    RECORD->phase = MEMTEST_TEST_DATA << 4;
    for (unsigned invert = 0; invert < 2; ++invert) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            pattern[0] = (UINT64_C(1) << bit) ^ (invert ? ~UINT64_C(0) : 0);
            data_line(FIXED(MEMTEST_FIXED_DATA).address, pattern, 1);
        }
        for (unsigned bit = 0; bit < 512; ++bit) {
            for (unsigned i = 0; i < 8; ++i)
                pattern[i] = (i == bit / 64u ? UINT64_C(1) << (bit % 64u) : 0) ^ (invert ? ~UINT64_C(0) : 0);
            data_line(FIXED(MEMTEST_FIXED_DATA).address, pattern, 8);
        }
    }
    result(MEMTEST_TEST_DATA, data_failed ? MEMTEST_FAIL : MEMTEST_PASS, data_words, counter() - begin);
}

/* 3. Address lines, bits 6-25: a pattern at the base and a tag in each
 * partner base ^ 2^b. All 21 words are written, cleaned and invalidated
 * before any is read. */
static void address_lines(void)
{
    uint64_t begin = counter(), failed = 0;
    RECORD->phase = MEMTEST_TEST_ADDRESS << 4;
    uint64_t at = FIXED(MEMTEST_FIXED_ADDRESS).address;
    volatile uint64_t *base = (volatile uint64_t *)(uintptr_t)at;
    *base = ADDRESS_PATTERN;
    for (unsigned b = 6; b < 26; ++b)
        *(volatile uint64_t *)(uintptr_t)(at ^ (UINT64_C(1) << b)) = ADDRESS_TAG + b;
    clean_invalidate(at);
    for (unsigned b = 6; b < 26; ++b) clean_invalidate(at ^ (UINT64_C(1) << b));
    barrier_dsb();
    uint64_t value = *base;
    if (value != ADDRESS_PATTERN) {
        mismatch(at, ADDRESS_PATTERN, value, MEMTEST_TEST_ADDRESS, 0);
        /* A stuck or open bit moved its partner's tag onto the base. */
        if (value - ADDRESS_TAG >= 6 && value - ADDRESS_TAG < 26) failed |= UINT64_C(1) << (value - ADDRESS_TAG);
        else failed |= 1;
    }
    for (unsigned b = 6; b < 26; ++b) {
        uint64_t address = at ^ (UINT64_C(1) << b);
        value = *(volatile uint64_t *)(uintptr_t)address;
        if (value != ADDRESS_TAG + b) {
            mismatch(address, ADDRESS_TAG + b, value, MEMTEST_TEST_ADDRESS, 0);
            failed |= UINT64_C(1) << b;
        }
    }
    volatile struct memtest_record *r = RECORD;
    r->address_tested |= ((UINT64_C(1) << 26) - 1u) & ~UINT64_C(63);
    r->address_failed |= failed & ~UINT64_C(63);
    result(MEMTEST_TEST_ADDRESS, failed ? MEMTEST_FAIL : MEMTEST_PASS, 21, counter() - begin);
}

/* A value that reads as another word's pattern proves an address decode
 * fault. The run stops before its next write. */
static int foreign(uint64_t candidate, uint64_t address, uint64_t align)
{
    return !(candidate & (align - 1u)) && candidate >= MEMTEST_DRAM_BASE && candidate < DRAM_END &&
           candidate != address;
}

/* 4. Page signature. */
static uint64_t signature_pages, signature_bad;
static int signature_write_span(uint64_t from, uint64_t to)
{
    walk_ascending(from, to);
    if (memtest_sig_write(&walk) == MEMTEST_WALK_BUDGET) finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_BUDGET);
    signature_pages += (to - from) / 4096u;
    return 1;
}

static void signature_classify(uint64_t from, uint64_t to)
{
    for (uint64_t page = from; page < to; page += 4096u) {
        uint64_t value = *(volatile uint64_t *)(uintptr_t)page;
        if (value == (page ^ contract.seed)) continue;
        mismatch(page, page ^ contract.seed, value, MEMTEST_TEST_SIGNATURE, 0);
        signature_bad++;
        if (foreign(value ^ contract.seed, page, 4096u)) {
            result(MEMTEST_TEST_SIGNATURE, MEMTEST_FAIL, signature_pages, counter() - start);
            finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_DECODE);
        }
    }
}

static int signature_read_span(uint64_t from, uint64_t to)
{
    walk_ascending(from, to);
    for (;;) {
        int status = memtest_sig_read(&walk);
        if (status == MEMTEST_WALK_DONE) return 1;
        if (status == MEMTEST_WALK_BUDGET) finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_BUDGET);
        /* The kernel stopped inside block walk.cursor at batch walk.found. */
        signature_classify(walk.found, walk.cursor + MEMTEST_BLOCK_BYTES);
        renew();
        walk.cursor += MEMTEST_BLOCK_BYTES;
    }
}

static void signature(void)
{
    uint64_t begin = counter();
    RECORD->phase = MEMTEST_TEST_SIGNATURE << 4;
    each_span(signature_write_span);
    check_digest();
    RECORD->phase = (MEMTEST_TEST_SIGNATURE << 4) | 1u;
    each_span(signature_read_span);
    result(MEMTEST_TEST_SIGNATURE, signature_bad ? MEMTEST_FAIL : MEMTEST_PASS, signature_pages,
           counter() - begin);
}

/* 5. March C-: up(w P); up(r P, w ~P); up(r ~P, w P); down(r P, w ~P);
 * down(r ~P, w P); down(r P). Each element runs over every member of a group
 * before the next element starts. */
struct element { int (*kernel)(struct memtest_walk *); unsigned down, complement, writes; };
static const struct element elements[6] = {
    {memtest_march_w, 0, 0, 1}, {memtest_march_rw, 0, 0, 1}, {memtest_march_rw, 0, 1, 1},
    {memtest_march_rw, 1, 0, 1}, {memtest_march_rw, 1, 1, 1}, {memtest_march_r, 1, 0, 0},
};
static uint64_t march_words, march_bad, slowest_per_mib = MARCH_TICKS_PER_MIB;

/* Recheck a block whose accumulator was nonzero. A read-and-write element
 * left the complement of each word it read, so the read value is ~current. */
static void march_rescan(uint64_t block, unsigned index)
{
    const struct element *e = &elements[index];
    uint64_t found = 0;
    for (uint64_t address = block; address < block + MEMTEST_BLOCK_BYTES; address += 8u) {
        volatile uint64_t *word = (volatile uint64_t *)(uintptr_t)address;
        uint64_t current = *word, observed = e->writes ? ~current : current;
        uint64_t expected = address + contract.seed;
        if (e->complement) expected = ~expected;
        if (observed == expected) continue;
        if (foreign(observed - contract.seed, address, 8u) || foreign(~observed - contract.seed, address, 8u)) {
            mismatch(address, expected, observed, MEMTEST_TEST_MARCH, index);
            RECORD->resume = address;
            finish(MEMTEST_RUN_STOPPED, MEMTEST_STOP_DECODE);
        }
        mismatch(address, expected, observed, MEMTEST_TEST_MARCH, index);
        *word = e->writes ? ~expected : expected;
        found++;
    }
    if (!found) RECORD->unconfirmed++;
    march_bad += found ? found : 1u;
}

/* Run one element over one member. Returns 0 when the deadline stopped it. */
static int march_member(unsigned index, const struct memtest_piece *piece, uint64_t limit)
{
    const struct element *e = &elements[index];
    uint64_t low = block_address(piece->first), high = block_address(piece->first + piece->blocks);
    walk.seed = contract.seed;
    walk.complement = e->complement;
    walk.deadline = limit;
    walk.block_step = e->down ? -(int64_t)MEMTEST_BLOCK_BYTES : (int64_t)MEMTEST_BLOCK_BYTES;
    walk.line_step = e->down ? -64 : 64;
    walk.cursor = e->down ? high - MEMTEST_BLOCK_BYTES : low;
    walk.end = e->down ? low - MEMTEST_BLOCK_BYTES : high;
    for (;;) {
        int status = e->kernel(&walk);
        if (status == MEMTEST_WALK_DONE) break;
        if (status == MEMTEST_WALK_BUDGET) return 0;
        march_rescan(walk.found, index);
        renew();
    }
    march_words += (high - low) / 8u * (index ? 1u : 0u);
    return 1;
}

static void march(uint64_t limit)
{
    uint64_t begin = counter();
    volatile struct memtest_record *r = RECORD;
    r->resume = block_address(contract.slice_start);
    unsigned status = MEMTEST_PASS;
    for (unsigned g = 0; g < plan.groups; ++g) {
        const struct memtest_group *group = &plan.group[g];
        uint64_t mib = 0;
        for (unsigned i = 0; i < group->pieces; ++i) mib += (uint64_t)plan.piece[group->first_piece + i].blocks * 2u;
        uint64_t now = counter();
        if (now >= limit || slowest_per_mib * mib > limit - now) break;
        for (unsigned index = 0; index < 6; ++index) {
            r->phase = (MEMTEST_TEST_MARCH << 4) | index;
            for (unsigned i = 0; i < group->pieces; ++i) {
                unsigned member = elements[index].down ? group->pieces - 1u - i : i;
                if (!march_member(index, &plan.piece[group->first_piece + member], limit)) {
                    /* The group is incomplete; the next run repeats it. */
                    result(MEMTEST_TEST_MARCH, march_bad ? MEMTEST_FAIL : status, march_words, counter() - begin);
                    return;
                }
            }
        }
        uint64_t ticks = counter() - now, per_mib = (ticks + mib - 1u) / mib;
        if (g == 0 || per_mib > slowest_per_mib) slowest_per_mib = per_mib;
        check_digest();
        r->slowest_per_mib = slowest_per_mib;
        r->groups = g + 1u;
        const struct memtest_piece *last = &plan.piece[group->first_piece + group->pieces - 1u];
        r->resume = block_address(last->first + last->blocks);
        if (march_bad) { status = MEMTEST_FAIL; break; }
    }
    result(MEMTEST_TEST_MARCH, status, march_words, counter() - begin);
}

/* 6. Bandwidth and latency harness over the fixed harness and chase
 * extents: read ascending and descending, write and copy, three samples each,
 * then a dependent-load chase. */
static int harness_time(void)
{
    renew();
    uint64_t now = counter();
    return now < deadline && deadline - now >= HARNESS_SAMPLE_LIMIT;
}

static void harness(void)
{
    uint64_t begin = counter(), at = FIXED(MEMTEST_FIXED_HARNESS).address;
    uint64_t bytes = FIXED(MEMTEST_FIXED_HARNESS).bytes, copy = bytes / 3u;
    volatile struct memtest_record *r = RECORD;
    r->phase = MEMTEST_TEST_HARNESS << 4;
    unsigned complete = 1;
    for (unsigned kind = 0; kind < MEMTEST_SAMPLES && complete; ++kind) {
        r->sample[kind].bytes = kind == MEMTEST_COPY ? copy : bytes;
        for (unsigned i = 0; i < 3 && (complete = harness_time()); ++i) {
            uint64_t t0 = counter();
            if (kind == MEMTEST_READ_UP) memtest_read_up(at, bytes);
            else if (kind == MEMTEST_READ_DOWN) memtest_read_down(at, bytes);
            else if (kind == MEMTEST_WRITE) memtest_write(at, bytes);
            else memtest_copy(at + copy, at, copy);
            /* A write sample ends when its stores have completed. */
            if (kind >= MEMTEST_WRITE) barrier_dsb();
            r->sample[kind].ticks[i] = counter() - t0;
        }
    }
    /* Dependent-load chase over 16 KiB to the chase extent in steps of four:
     * each line points to its successor in a full-period linear congruential
     * permutation of the lines. */
    uint64_t chase = FIXED(MEMTEST_FIXED_CHASE).address;
    for (unsigned point = 0; point < MEMTEST_CHASE_POINTS && complete; ++point) {
        uint64_t size = UINT64_C(16384) << (2u * point), lines = size / 64u, loads = UINT64_C(1) << 18;
        if (size > FIXED(MEMTEST_FIXED_CHASE).bytes || !(complete = harness_time())) break;
        for (uint64_t i = 0; i < lines; ++i) {
            uint64_t next = (i * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407)) & (lines - 1u);
            *(volatile uint64_t *)(uintptr_t)(chase + 64u * i) = chase + 64u * next;
        }
        renew();
        uint64_t t0 = counter();
        memtest_chase(chase, loads);
        r->chase[point] = (struct memtest_chase_point){size, loads, counter() - t0};
    }
    result(MEMTEST_TEST_HARNESS, complete ? MEMTEST_PASS : MEMTEST_STOPPED, 0, counter() - begin);
}

static int clock_read(void *unused, uintptr_t address, uint32_t *value)
{
    (void)unused;
    *value = mmio_read(address);
    if (clock_reads < 8) RECORD->clock[clock_reads++] = *value;
    return 1;
}

static uint64_t data_cache_bytes(void)
{
    uint64_t clidr, mmfr2, ccsidr[7] = {0}, saved;
    __asm__ volatile("mrs %0, clidr_el1\n mrs %1, id_aa64mmfr2_el1\n mrs %2, csselr_el1"
                     : "=r"(clidr), "=r"(mmfr2), "=r"(saved));
    for (unsigned level = 0; level < 7; ++level) {
        uint64_t select = (uint64_t)level << 1;
        __asm__ volatile("msr csselr_el1, %1\n isb\n mrs %0, ccsidr_el1" : "=r"(ccsidr[level]) : "r"(select));
    }
    __asm__ volatile("msr csselr_el1, %0\n isb" :: "r"(saved));
    return memtest_dcache_bytes(clidr, ccsidr, ((mmfr2 >> 20) & 15u) != 0);
}

static void admit(void)
{
    volatile struct memtest_record *r = RECORD;
    struct memtest_facts facts = {0};
    facts.writers_sample = r->writers_sample;
    contract = *(volatile struct memtest_contract *)&memtest_contract;
    volatile uint64_t *words = (volatile uint64_t *)r;
    for (unsigned i = 0; i < sizeof(struct memtest_record) / 8u; ++i) words[i] = 0;
    r->magic = MEMTEST_RECORD_MAGIC;
    r->version = MEMTEST_RECORD_VERSION | (sizeof(struct memtest_record) << 16);
    r->sequence = contract.sequence;
    r->contract_digest = memtest_hash(MEMTEST_HASH_SEED, (const uint64_t *)&contract, sizeof contract / 8u);
    r->remote_writers = facts.writers_sample;
    r->start_ticks = start;
    /* SError is taken at EL1 into the platform's exception capture. */
    __asm__ volatile("msr daifclr, #4\n isb" ::: "memory");
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n mrs %1, dczid_el0" : "=r"(daif), "=r"(facts.dczid));
    r->daif = daif;
    r->dczid = facts.dczid;
    facts.dcache_bytes = r->dcache_bytes = data_cache_bytes();
    enum memtest_refusal reason = memtest_admit(&memtest_geometry, &contract, &facts, &plan);
    if (reason != MEMTEST_ADMITTED) refuse(reason);
    r->budget_ticks = plan.budget_ticks;
    deadline = start + plan.budget_ticks;
    /* One write translation per descriptor kind: level-2 block, low and high
     * level-1 blocks, each Normal write-back inner-shareable at its own
     * physical address. */
    static const struct a733_mapping_rule rule = {
        A733_MAPPING_WRITE, 1, {{A733_MAIR_NORMAL_WB, A733_SH_INNER}}, 0, 1};
    static const uint64_t probes[3] = {UINT64_C(0x44000000), UINT64_C(0x80000000), UINT64_C(0x33ffff000)};
    for (unsigned i = 0; i < 3; ++i)
        if (!a733_mapping_check(aarch64_device_translate, (uintptr_t)probes[i], probes[i], &rule))
            refuse(MEMTEST_REFUSE_MAPPING);
    r->digest_admission = recovery_digest();
    struct a733_cpu_clock_report report;
    a733_cpu_clock_report(clock_read, 0, &report);
}

static void run(void *unused)
{
    (void)unused;
    start = counter();
    walk.progress = &RECORD->progress;
    walk.renew_at = 0;
    renew();
    admit();
    window_scan();
    if (contract.mask & MEMTEST_DATA_LINES) data_lines();
    if (contract.mask & MEMTEST_ADDRESS_LINES) address_lines();
    if (contract.mask & MEMTEST_SIGNATURE) signature();
    if (contract.mask & MEMTEST_MARCH)
        march(contract.mask & MEMTEST_HARNESS && deadline - start > HARNESS_RESERVE ? deadline - HARNESS_RESERVE
                                                                                     : deadline);
    if (contract.mask & MEMTEST_HARNESS) harness();
    volatile struct memtest_record *r = RECORD;
    unsigned failed = r->mismatch_count || r->unconfirmed;
    for (unsigned i = 0; i < MEMTEST_TESTS; ++i) failed |= r->test[i].status == MEMTEST_FAIL;
    finish(failed ? MEMTEST_RUN_FAIL : MEMTEST_RUN_PASS, MEMTEST_STOP_NONE);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{ (void)handle; platform_panic(name, 0); }

void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(run, "memtest", sizeof stack / sizeof stack[0], NULL, 3, stack, &task));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
