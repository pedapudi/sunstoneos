/* SPDX-License-Identifier: GPL-2.0-only */
/* Result layout of the DRAM bandwidth application (docs/designs/dram-bandwidth.md). */
#ifndef DRAM_BANDWIDTH_RESULT_H
#define DRAM_BANDWIDTH_RESULT_H
#include <stddef.h>
#include <stdint.h>
#define BW_MAGIC "SSDRAMBW"
#define BW_CORES 8u
#define BW_SLICES 8u
#define BW_SLICE_BYTES (5u << 20)           /* per-core slice; eight slices fill the 40 MiB buffer */
#define BW_BUFFER_BYTES (BW_SLICES * BW_SLICE_BYTES)
#define BW_CONFIGS 5u
#define BW_WARM 2u
#define BW_REPS 8u
/* Core masks of the configurations: one A55, one A76, both A76, six A55, all eight. */
#define BW_CONFIG_MASKS { 0x01u, 0x40u, 0xc0u, 0x3fu, 0xffu }
#define BW_CLOCK_BYTES 256u
struct bw_sample {
    uint64_t ticks;                         /* last finish minus first start over the participating cores, 24 MHz ticks */
    uint64_t digest;                        /* XOR over the participating cores of the slice digests, rotated per pass */
};
struct bw_config {
    uint32_t mask, cores;
    uint32_t reserved[2];
    struct bw_sample samples[BW_REPS];      /* timed passes, in order */
};
struct bw_result {
    char magic[8];
    uint64_t counter_hz;
    uint32_t slice_bytes, slices, warm, reps;
    uint32_t readiness;                     /* bit 0 clock ready and bit 1 unthrottled before, bits 2 and 3 after */
    uint32_t reserved;
    int32_t millicelsius_before[5], millicelsius_after[5];  /* CPUB, DDR, NPU, CPUL, GPU */
    uint64_t fill_digest[BW_SLICES];        /* digest of each slice as the fill wrote it */
    char clock_before[BW_CLOCK_BYTES], clock_after[BW_CLOCK_BYTES];
    struct bw_config config[BW_CONFIGS];
};
/* Streams `bytes` (a multiple of 128) from `p` with 128-byte loads; returns the XOR of all 64-bit words. */
uint64_t bw_read(const uint64_t *p, size_t bytes);
/* The word at index i of the whole buffer. */
static inline uint64_t bw_word(uint64_t i) { return i * UINT64_C(0x9e3779b97f4a7c15) + UINT64_C(0x1234567); }
#endif
