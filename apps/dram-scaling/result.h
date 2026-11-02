/* SPDX-License-Identifier: GPL-2.0-only */
/* Result layout of the DRAM scaling application (docs/designs/dram-bandwidth.md). */
#ifndef DRAM_SCALING_RESULT_H
#define DRAM_SCALING_RESULT_H
#include <stddef.h>
#include <stdint.h>
#define DS_MAGIC "SSDRAMSC"
#define DS_CORES 8u
#define DS_SLOT_BYTES (5u << 20)            /* per-core slot; eight slots fill the 40 MiB buffer */
#define DS_BUFFER_BYTES (DS_CORES * DS_SLOT_BYTES)
#define DS_REGION_BYTES (3u << 20)          /* bytes a core streams, inside its slot at the variant's offset */
#define DS_STAGGER_WRAP (1u << 20)          /* core c starts at (c * stagger) modulo this inside its slot */
#define DS_WINDOW_TICKS 288000u             /* 12 ms at 24 MHz */
#define DS_WARM 1u
#define DS_REPS 3u
#define DS_CLOCK_BYTES 256u
/* Access patterns of one core. A chunk is read with 128-byte loads.
 * 0: sequential, 64 KiB chunks.   1: sequential, 4 KiB chunks (control for the chunk size).
 * 2: two sequential streams half a region apart, alternating 4 KiB chunks.
 * 3: 4 KiB chunks in the order (7 * j) modulo the chunk count: page-granular strides.
 * 4: as 0, with a software prefetch into the L2 cache 2 KiB ahead of the loads.
 * 5: one shared stream: in round r the core of rank k (its position in the core set, n cores) reads
 *    the 16 KiB chunk (r * n + k) modulo 2048 of the buffer, and all cores wait for each other at a
 *    barrier after every round, so the cores read neighbouring addresses at the same time.
 * 6: as 5 with each core reading its own region, 16 KiB per round: the barrier cost without the
 *    shared addresses. */
#define DS_MODES 7u
#define DS_CHUNK_BYTES_0 (64u << 10)
#define DS_CHUNK_BYTES_N (4u << 10)
#define DS_CHUNK_BYTES_S (16u << 10)
#define DS_SHARED_CHUNKS 2048u
/* Core sets: 0x01 one A55, 0x03 two, 0x0f four, 0x3f six A55; 0x40 one A76, 0xc0 two A76;
 * 0xff all eight; 0xcf four A55 and both A76. */
struct ds_variant { uint32_t mask, mode, stagger; };
#define DS_VARIANTS { \
    {0x01,0,0},{0x03,0,0},{0x0f,0,0},{0x3f,0,0},{0x40,0,0},{0xc0,0,0},{0xcf,0,0},{0xff,0,0}, \
    {0x3f,0,4096},{0xc0,0,4096},{0xff,0,4096}, \
    {0x3f,0,65536},{0xc0,0,65536},{0xff,0,65536}, \
    {0x3f,0,262144},{0xc0,0,262144},{0xff,0,262144}, \
    {0x01,1,0},{0x40,1,0},{0x3f,1,0},{0xc0,1,0},{0xff,1,0}, \
    {0x01,2,0},{0x40,2,0},{0x3f,2,0},{0xc0,2,0},{0xff,2,0}, \
    {0x01,3,0},{0x40,3,0},{0x3f,3,0},{0xc0,3,0},{0xff,3,0}, \
    {0x01,4,0},{0x03,4,0},{0x0f,4,0},{0x3f,4,0},{0x40,4,0},{0xc0,4,0},{0xcf,4,0},{0xff,4,0}, \
    {0x01,5,0},{0x03,5,0},{0x0f,5,0},{0x3f,5,0},{0x40,5,0},{0xc0,5,0},{0xcf,5,0},{0xff,5,0}, \
    {0x0f,6,0},{0x3f,6,0},{0xc0,6,0},{0xff,6,0} }
#define DS_CONFIGS 52u
struct ds_core_sample { uint32_t chunks; uint32_t ticks; };   /* chunks read; ticks from the common gate to the core's last chunk */
struct ds_config {
    uint32_t mask, mode, stagger, reserved;
    struct ds_core_sample sample[DS_REPS][DS_CORES];
    uint64_t digest[DS_CORES];              /* XOR of the digests of the chunks core c read in the last timed window */
};
struct ds_result {
    char magic[8];
    uint64_t counter_hz;
    uint64_t buffer_base;
    uint32_t window_ticks, warm, reps, readiness;
    int32_t millicelsius_before[5], millicelsius_after[5];  /* CPUB, DDR, NPU, CPUL, GPU */
    uint64_t fill_digest[DS_CORES];         /* digest of each slot as the fill wrote it */
    char clock_before[DS_CLOCK_BYTES], clock_after[DS_CLOCK_BYTES];
    struct ds_config config[DS_CONFIGS];
};
/* Streams `bytes` (a multiple of 128) from `p` with 128-byte loads; returns the XOR of all 64-bit words. */
uint64_t ds_read(const uint64_t *p, size_t bytes);
uint64_t ds_read_pf(const uint64_t *p, size_t bytes);
/* The word at index i of the whole buffer. */
static inline uint64_t ds_word(uint64_t i) { return i * UINT64_C(0x9e3779b97f4a7c15) + UINT64_C(0x1234567); }
/* Chunk geometry and visit order shared by the image, the host model and the decoder. */
static inline uint32_t ds_chunk_bytes(uint32_t mode) { return (mode == 0 || mode == 4) ? DS_CHUNK_BYTES_0 : mode >= 5 ? DS_CHUNK_BYTES_S : DS_CHUNK_BYTES_N; }
static inline uint32_t ds_chunk_count(uint32_t mode) { return DS_REGION_BYTES / ds_chunk_bytes(mode); }
static inline uint32_t ds_chunk_index(uint32_t mode, uint32_t j)
{
    const uint32_t n = ds_chunk_count(mode);
    if (mode == 2) return ((j >> 1) % (n / 2)) + (j & 1u) * (n / 2);
    if (mode == 3) return (uint32_t)(((uint64_t)j * 7u) % n);
    return j % n;
}
static inline uint32_t ds_region_offset(uint32_t core, uint32_t stagger)
{
    return (uint32_t)(((uint64_t)core * stagger) % DS_STAGGER_WRAP);
}
#endif
