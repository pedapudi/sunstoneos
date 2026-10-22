/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_MEMTEST_H
#define SUNSTONE_MEMTEST_H
/* Run contract, result record, geometry and planner of the returning memory
 * qualification image. The image and the host checks compile this header;
 * docs/memory-qualification.md describes the tests and their order.
 *
 * Addresses are physical. A block is 2 MiB; a block index counts blocks from
 * DRAM_BASE. The protected window is blocks 0-31 (64 MiB); the tested map is
 * blocks 32-6143 less the contract's excluded ranges. */

/* Values the kernels in kernels.S also use. */
#define MEMTEST_RENEW_TICKS 4800000 /* 200 ms of the 24 MHz counter */
#define MEMTEST_WALK_CURSOR 0
#define MEMTEST_WALK_END 8
#define MEMTEST_WALK_BLOCK_STEP 16
#define MEMTEST_WALK_LINE_STEP 24
#define MEMTEST_WALK_SEED 32
#define MEMTEST_WALK_DEADLINE 40
#define MEMTEST_WALK_RENEW_AT 48
#define MEMTEST_WALK_COMPLEMENT 56
#define MEMTEST_WALK_PROGRESS 64
#define MEMTEST_WALK_FOUND 72
#define MEMTEST_WALK_RENEWALS 80
#define MEMTEST_WALK_MATCH_BASE 88
#define MEMTEST_WALK_MATCH_BYTES 96
#define MEMTEST_WALK_DONE 0
#define MEMTEST_WALK_HIT 1
#define MEMTEST_WALK_BUDGET 2

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#define MEMTEST_DRAM_BASE UINT64_C(0x40000000)
#define MEMTEST_BLOCK_SHIFT 21u
#define MEMTEST_BLOCK_BYTES (UINT64_C(1) << MEMTEST_BLOCK_SHIFT)
#define MEMTEST_WINDOW_BLOCKS 32u
#define MEMTEST_DRAM_BLOCKS 6144u
#define MEMTEST_RECORD_ADDRESS UINT64_C(0x42003000)
#define MEMTEST_COUNTER_HZ UINT64_C(24000000)
/* Admission clamps the contract budget to 20 s, inside the host's 30 s
 * return window. */
#define MEMTEST_BUDGET_LIMIT (20u * MEMTEST_COUNTER_HZ)
/* The secure timer fires 2 s after the last renewal; the kernels renew it
 * once MEMTEST_RENEW_TICKS have passed. */
_Static_assert(MEMTEST_RENEW_TICKS == MEMTEST_COUNTER_HZ / 5u, "renewal after 200 ms");

/* Beacon constant: a recovery-region beacon word holds its own physical
 * address XOR K. Bits 0-33 of K are zero, so a value v read anywhere names a
 * recovery word exactly when v - (K | region start) < region bytes. */
#define MEMTEST_K UINT64_C(0xa5c35a3c00000000)
#define MEMTEST_HIGH_MASK ((UINT64_C(1) << 30) - 1u)
/* A value v the image writes can match a beacon only when its bits 34-63
 * equal K's. Values whose bits 34-63, or the complement's, lie within one of
 * K's are refused: a + seed carries at most one into bit 34. */
#define MEMTEST_NEAR_K(v) \
    ((((MEMTEST_K >> 34) - ((uint64_t)(v) >> 34) + 1u) & MEMTEST_HIGH_MASK) <= 2u)
#define MEMTEST_CLEAR_OF_K(v) (!MEMTEST_NEAR_K(v) && !MEMTEST_NEAR_K(~(uint64_t)(v)))

/* Contract mask. The window scan runs on every run and has no bit. */
enum {
    MEMTEST_DATA_LINES = 1u << 0,
    MEMTEST_ADDRESS_LINES = 1u << 1,
    MEMTEST_SIGNATURE = 1u << 2,
    MEMTEST_MARCH = 1u << 3,
    MEMTEST_HARNESS = 1u << 4,
    MEMTEST_DMA_CACHE = 1u << 5,
    MEMTEST_DMA_REACH = 1u << 6,
};
#define MEMTEST_MASK_KNOWN 0x7fu
#define MEMTEST_MASK_DMA (MEMTEST_DMA_CACHE | MEMTEST_DMA_REACH)
/* Tests this image implements. DMA0 cases are not built yet, so admission
 * refuses their bits. */
#define MEMTEST_MASK_BUILT 0x1fu

#define MEMTEST_CONTRACT_MAGIC UINT64_C(0x314e4f4354534d4d) /* "MMSTCON1" */
#define MEMTEST_EXCLUDED_MAX 10u
struct memtest_range { uint32_t start, end; }; /* blocks [start, end) */
/* Written by the host at the linker-placed symbol memtest_contract after the
 * image upload. The image's identity digest covers the zero bytes the build
 * leaves there. */
struct memtest_contract {
    uint64_t magic;
    uint64_t sequence;
    uint32_t slice_start, slice_end; /* march blocks [start, end) */
    uint32_t mask, excluded_count;
    uint64_t budget_ticks;
    uint64_t seed;
    struct memtest_range excluded[MEMTEST_EXCLUDED_MAX];
};

/* Remote-writer sample: the memtest monitor stores
 * (MEMTEST_WRITERS_MAGIC << 32) | mask in the record's writers_sample word at
 * EL3 before EL1 entry. The image accepts only OR100, CE and USB0 bits. */
#define MEMTEST_WRITERS_MAGIC UINT64_C(0x57525452)
#define MEMTEST_WRITERS_ACCEPTED ((UINT32_C(1) << 1) | (UINT32_C(1) << 4) | (UINT32_C(1) << 12))

enum memtest_refusal {
    MEMTEST_ADMITTED,
    MEMTEST_REFUSE_CONTRACT,       /* magic */
    MEMTEST_REFUSE_MASK,           /* unknown bit */
    MEMTEST_REFUSE_MASK_UNBUILT,   /* DMA0 case bit */
    MEMTEST_REFUSE_SLICE_EMPTY,
    MEMTEST_REFUSE_SLICE_OUTSIDE,
    MEMTEST_REFUSE_SLICE_EXCLUDED,
    MEMTEST_REFUSE_EXCLUDED_COUNT,
    MEMTEST_REFUSE_EXCLUDED_EMPTY,
    MEMTEST_REFUSE_EXCLUDED_ORDER,
    MEMTEST_REFUSE_EXCLUDED_OUTSIDE,
    MEMTEST_REFUSE_FIXED_OUTSIDE,
    MEMTEST_REFUSE_FIXED_EXCLUDED,
    MEMTEST_REFUSE_GROUPING,       /* slice cannot form march groups */
    MEMTEST_REFUSE_SEED,
    MEMTEST_REFUSE_BUDGET,
    MEMTEST_REFUSE_DCZID,
    MEMTEST_REFUSE_DCACHE,
    MEMTEST_REFUSE_WRITERS_UNSAMPLED,
    MEMTEST_REFUSE_WRITERS,
    MEMTEST_REFUSE_MAPPING,
    MEMTEST_REFUSE_RECORD,
    MEMTEST_REFUSALS
};

/* Values the image reads from the CPU and the monitor for admission. */
struct memtest_facts {
    uint64_t dczid;          /* DCZID_EL0 */
    uint64_t dcache_bytes;   /* from CLIDR_EL1 and CCSIDR_EL1 */
    uint64_t writers_sample; /* record word written by the monitor */
};

/* Physical extents the image writes at fixed places in the tested map. */
enum { MEMTEST_FIXED_DATA, MEMTEST_FIXED_ADDRESS, MEMTEST_FIXED_HARNESS, MEMTEST_FIXED_CHASE,
       MEMTEST_FIXED_COUNT };
struct memtest_extent { uint64_t address, bytes; };
struct memtest_geometry {
    uint32_t first_block, end_block; /* tested map before exclusions */
    uint32_t piece_blocks;           /* largest march group member */
    uint32_t others_blocks;          /* members other than any one total at least this */
    struct memtest_extent fixed[MEMTEST_FIXED_COUNT];
};
#define MEMTEST_DATA_ADDRESS UINT64_C(0x44000000)
#define MEMTEST_ADDRESS_BASE UINT64_C(0x4c000000)
#define MEMTEST_HARNESS_ADDRESS UINT64_C(0x100000000)
#define MEMTEST_HARNESS_BYTES (UINT64_C(192) << 20)
#define MEMTEST_COPY_BYTES (UINT64_C(64) << 20)
#define MEMTEST_CHASE_ADDRESS UINT64_C(0x120000000)
#define MEMTEST_CHASE_BYTES (UINT64_C(256) << 20)
#define MEMTEST_GEOMETRY_BOARD { MEMTEST_WINDOW_BLOCKS, MEMTEST_DRAM_BLOCKS, 128u, 128u, { \
    { MEMTEST_DATA_ADDRESS, 64u }, { MEMTEST_ADDRESS_BASE, UINT64_C(1) << 26 }, \
    { MEMTEST_HARNESS_ADDRESS, MEMTEST_HARNESS_BYTES }, { MEMTEST_CHASE_ADDRESS, MEMTEST_CHASE_BYTES } } }

/* A march plan: the slice cut into members of at most piece_blocks, in
 * address order, and consecutive groups of two or three members. */
#define MEMTEST_PIECES_MAX 64u
struct memtest_piece { uint32_t first, blocks; };
struct memtest_group { uint32_t first_piece, pieces; };
struct memtest_plan {
    uint32_t pieces, groups;
    uint64_t budget_ticks; /* clamped */
    struct memtest_piece piece[MEMTEST_PIECES_MAX];
    struct memtest_group group[MEMTEST_PIECES_MAX / 2u];
};

/* Pure admission over the contract and the facts: returns MEMTEST_ADMITTED
 * and fills plan, or the first refusal. Called by the image before any write
 * outside the window and by checks/memtest-plan.py on the host. */
enum memtest_refusal memtest_admit(const struct memtest_geometry *, const struct memtest_contract *,
                                   const struct memtest_facts *, struct memtest_plan *);
/* Data-cache bytes summed over every data or unified level that CLIDR lists.
 * ccsidr[i] is CCSIDR_EL1 for level i + 1 with CSSELR.InD = 0;
 * extended selects the FEAT_CCIDX layout. */
uint64_t memtest_dcache_bytes(uint64_t clidr, const uint64_t ccsidr[7], unsigned extended);
/* FNV-1a over 64-bit words, used for the record checksum, the contract
 * digest and the recovery-region digest. */
static inline uint64_t memtest_hash(uint64_t hash, const volatile uint64_t *words, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        hash = (hash ^ words[i]) * UINT64_C(0x100000001b3);
    return hash;
}
#define MEMTEST_HASH_SEED UINT64_C(0xcbf29ce484222325)

/* Per-test results in the record. */
enum { MEMTEST_NOT_RUN, MEMTEST_PASS, MEMTEST_FAIL, MEMTEST_INSENSITIVE, MEMTEST_STOPPED };
enum { MEMTEST_TEST_SCAN, MEMTEST_TEST_DATA, MEMTEST_TEST_ADDRESS, MEMTEST_TEST_SIGNATURE,
       MEMTEST_TEST_MARCH, MEMTEST_TEST_HARNESS, MEMTEST_TEST_DMA_CACHE, MEMTEST_TEST_DMA_REACH,
       MEMTEST_TESTS };
/* Overall status. */
enum { MEMTEST_RUNNING, MEMTEST_RUN_PASS, MEMTEST_RUN_FAIL, MEMTEST_RUN_REFUSED,
       MEMTEST_RUN_STOPPED };
/* Stop reasons for MEMTEST_RUN_STOPPED. */
enum { MEMTEST_STOP_NONE, MEMTEST_STOP_SCAN, MEMTEST_STOP_DECODE, MEMTEST_STOP_DIGEST,
       MEMTEST_STOP_BUDGET };

struct memtest_result { uint32_t status, reserved; uint64_t words, ticks; };
struct memtest_mismatch { uint64_t address, expected, observed; uint32_t test, element; };
struct memtest_sample { uint64_t bytes, ticks[3]; };
struct memtest_chase_point { uint64_t bytes, loads, ticks; };
#define MEMTEST_MISMATCHES 32u
#define MEMTEST_CHASE_POINTS 8u
enum { MEMTEST_READ_UP, MEMTEST_READ_DOWN, MEMTEST_WRITE, MEMTEST_COPY, MEMTEST_SAMPLES };

#define MEMTEST_RECORD_MAGIC UINT64_C(0x3144524f4345524d) /* "MRECORD1" */
#define MEMTEST_RECORD_VERSION 1u
/* The last 4 KiB page of the platform result block, Normal non-cacheable.
 * Only core 0 writes it. checksum is memtest_hash over every earlier word. */
struct memtest_record {
    uint64_t magic, version;
    uint64_t writers_sample;    /* monitor sample, cleared by admission */
    uint64_t sequence, contract_digest;
    uint64_t status, refusal, stop, phase;
    uint64_t progress;          /* last block address the current pass finished */
    uint64_t resume;            /* first march block address not yet run */
    uint64_t remote_writers, daif, dcache_bytes, dczid;
    uint64_t address_tested, address_failed; /* address bits 6-33 */
    uint64_t scan_page, scan_value;
    uint64_t digest_admission, digest_last;
    uint64_t groups, slowest_per_mib, mismatch_count, unconfirmed, renewals;
    uint64_t budget_ticks, start_ticks, end_ticks;
    uint64_t dma_phase, dma_polls[8];
    uint32_t clock[8];          /* oscillator, SID, then PLL and mux words of CPU-L, CPU-B, DSU */
    struct memtest_result test[MEMTEST_TESTS];
    struct memtest_sample sample[MEMTEST_SAMPLES];
    struct memtest_chase_point chase[MEMTEST_CHASE_POINTS];
    struct memtest_mismatch mismatch[MEMTEST_MISMATCHES];
    uint64_t checksum;
};
_Static_assert(sizeof(struct memtest_contract) == 128, "contract is 128 bytes");
_Static_assert(sizeof(struct memtest_record) <= 4096, "record fits its page");
_Static_assert(offsetof(struct memtest_record, writers_sample) == 16, "monitor sample word");

/* Kernel walk state shared with kernels.S. Every field is 64 bits wide. */
struct memtest_walk {
    uint64_t cursor;        /* next block address in travel order */
    uint64_t end;           /* block address that stops the walk */
    int64_t block_step;     /* +/- MEMTEST_BLOCK_BYTES */
    int64_t line_step;      /* +/- 64 */
    uint64_t seed;
    uint64_t deadline;      /* counter value that stops the walk */
    uint64_t renew_at;      /* counter value of the next renewal */
    uint64_t complement;    /* march: expected words are ~P */
    volatile uint64_t *progress;
    uint64_t found;         /* HIT: batch or block address */
    uint64_t renewals;
    uint64_t match_base;    /* scan: K | recovery start */
    uint64_t match_bytes;   /* scan: recovery bytes */
};
_Static_assert(offsetof(struct memtest_walk, match_bytes) == MEMTEST_WALK_MATCH_BYTES, "walk layout");
_Static_assert(offsetof(struct memtest_walk, renewals) == MEMTEST_WALK_RENEWALS, "walk layout");
_Static_assert(offsetof(struct memtest_walk, progress) == MEMTEST_WALK_PROGRESS, "walk layout");
_Static_assert(offsetof(struct memtest_walk, complement) == MEMTEST_WALK_COMPLEMENT, "walk layout");

/* Leaf kernels in kernels.S. Each walk kernel processes whole blocks from
 * cursor to end and, after each block, stores the block address to
 * *progress, reads the counter, renews the secure timer when renew_at has
 * passed and stops at deadline. */
int memtest_scan(struct memtest_walk *);      /* HIT: found = 32 KiB batch */
int memtest_sig_write(struct memtest_walk *);
int memtest_sig_read(struct memtest_walk *);  /* HIT: found = 32 KiB batch */
int memtest_march_w(struct memtest_walk *);
int memtest_march_r(struct memtest_walk *);   /* HIT: found = block */
int memtest_march_rw(struct memtest_walk *);  /* HIT: found = block */
void memtest_read_up(uint64_t start, uint64_t bytes);
void memtest_read_down(uint64_t start, uint64_t bytes);
void memtest_write(uint64_t start, uint64_t bytes);
void memtest_copy(uint64_t destination, uint64_t source, uint64_t bytes);
uint64_t memtest_chase(uint64_t start, uint64_t loads); /* loads: multiple of 8 */
#endif
#endif
