/* SPDX-License-Identifier: GPL-2.0-only */
/* Result layout of the DRAM interconnect study (docs/designs/dram-bandwidth.md, "Interconnect words").
 * The worker tasks run at EL1: they read the nine NSI words that boot0 writes after DRAM
 * initialization, optionally write boot0's values, and sample the two DRAM target-agent counters
 * (NSI ports 32 and 33) around every read window. */
#ifndef DRAM_NSI_RESULT_H
#define DRAM_NSI_RESULT_H
#include <stddef.h>
#include <stdint.h>
#define DN_MAGIC "SSDRAMNS"
#define DN_CORES 8u
#define DN_SLOT_BYTES (5u << 20)
#define DN_BUFFER_BYTES (DN_CORES * DN_SLOT_BYTES)
#define DN_REGION_BYTES (3u << 20)
#define DN_STAGGER_WRAP (1u << 20)
#define DN_WINDOW_TICKS 288000u             /* 12 ms at 24 MHz */
#define DN_WARM 1u
#define DN_REPS 3u
#define DN_CLOCK_BYTES 256u
#define DN_CHUNK_BYTES_0 (64u << 10)
#define DN_CHUNK_BYTES_N (4u << 10)
#define DN_CHUNK_BYTES_S (16u << 10)
#define DN_SHARED_CHUNKS 2048u
/* Modes as in apps/dram-scaling: 0 sequential 64 KiB chunks, 3 page strides, 5 shared stream with a
 * barrier per round, 6 own regions with the same barrier. */
struct dn_variant { uint32_t mask, mode, stagger; };
#define DN_VARIANTS { \
    {0x01,0,0},{0x03,0,0},{0x0f,0,0},{0x3f,0,0},{0x40,0,0},{0xc0,0,0},{0xcf,0,0},{0xff,0,0}, \
    {0x01,3,0},{0x40,3,0},{0xff,3,0},{0x3f,5,0},{0xff,5,0},{0x3f,6,0},{0xff,6,0} }
#define DN_CONFIGS 15u
#define DN_PHASES 2u                        /* 0: words as inherited; 1: after the write (images with DN_WRITE) */
/* The nine NSI words of boot0 routine 0x48f38, in boot0's write order. */
#define DN_WORDS 9u
#define DN_WORD_ADDRESSES { 0x0202446c, 0x0202466c, 0x02023a14, 0x02023c14, 0x02023e14, 0x02024014, 0x02024214, 0x0202200c, 0x0202220c }
#define DN_WORD_VALUES    { 0x00040a05, 0x00040a05, 0x0000003f, 0x0000007f, 0x0000000f, 0x000000f1, 0x000000f1, 0x00000001, 0x00000001 }
/* Registers read once, in this order: CCU NSI clock, NSI bus gate and reset, MBUS clock, and for each of
 * the target-agent ports 32 and 33 the words +0x10, +0x14, +0x18, +0xc0, +0xc8. */
#define DN_EXTRA 13u
struct dn_core_sample { uint32_t chunks; uint32_t ticks; };
/* Counters of one window for the target-agent ports 32 and 33. */
struct dn_counters { uint32_t ticks; uint32_t rq_rd[2], rq_wr[2], dt_rd[2], dt_wr[2], la_rd[2]; };
struct dn_config {
    uint32_t mask, mode, stagger, reserved;
    struct dn_core_sample sample[DN_REPS][DN_CORES];
    struct dn_counters counters[DN_REPS];
    uint64_t digest[DN_CORES];
};
struct dn_probe {
    uint32_t words_before[DN_WORDS];        /* as inherited */
    uint32_t words_written[DN_WORDS];       /* read back after the write; zero when the image does not write */
    uint32_t extra[DN_EXTRA];
    uint32_t write_enabled, reserved[2];
};
struct dn_result {
    char magic[8];
    uint64_t counter_hz;
    uint64_t buffer_base;
    uint32_t window_ticks, warm, reps, readiness;
    int32_t millicelsius_before[5], millicelsius_after[5];
    uint64_t fill_digest[DN_CORES];
    char clock_before[DN_CLOCK_BYTES], clock_after[DN_CLOCK_BYTES];
    struct dn_probe probe;
    struct dn_config config[DN_PHASES][DN_CONFIGS];
};
uint64_t dn_read(const uint64_t *p, size_t bytes);
static inline uint64_t dn_word(uint64_t i) { return i * UINT64_C(0x9e3779b97f4a7c15) + UINT64_C(0x1234567); }
static inline uint32_t dn_chunk_bytes(uint32_t mode) { return mode == 0 ? DN_CHUNK_BYTES_0 : mode >= 5 ? DN_CHUNK_BYTES_S : DN_CHUNK_BYTES_N; }
static inline uint32_t dn_chunk_count(uint32_t mode) { return DN_REGION_BYTES / dn_chunk_bytes(mode); }
static inline uint32_t dn_chunk_index(uint32_t mode, uint32_t j)
{
    const uint32_t n = dn_chunk_count(mode);
    if (mode == 3) return (uint32_t)(((uint64_t)j * 7u) % n);
    return j % n;
}
static inline uint32_t dn_region_offset(uint32_t core, uint32_t stagger)
{
    return (uint32_t)(((uint64_t)core * stagger) % DN_STAGGER_WRAP);
}
#endif
