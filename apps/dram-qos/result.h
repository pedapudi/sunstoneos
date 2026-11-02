/* SPDX-License-Identifier: GPL-2.0-only */
/* Result layout of the CPU memory-latency study under device load (docs/designs/dram-bandwidth.md,
 * "CPU memory latency under device load"). */
#ifndef DRAM_QOS_RESULT_H
#define DRAM_QOS_RESULT_H
#include <stddef.h>
#include <stdint.h>
#define DQ_MAGIC "SSDRAMQS"
#define DQ_CORES 8u
#define DQ_CHASE_BYTES (16u << 20)          /* pointer-chase buffer: 262144 nodes of 64 bytes */
#define DQ_NODES (DQ_CHASE_BYTES / 64u)
#define DQ_SLOT_BYTES (5u << 20)            /* streamer slot, one per core, after the chase buffer */
#define DQ_BUFFER_BYTES (DQ_CHASE_BYTES + DQ_CORES * DQ_SLOT_BYTES)
#define DQ_CONTROL_HZ 10000u
#define DQ_CONTROL_LOADS 16u                /* dependent loads per control iteration */
#define DQ_SETTINGS 3u                      /* 0 as found, 1 CPU ports priority 3, 2 priority 3 and high-priority reads */
#define DQ_PROBES 3u                        /* 0 chase on an A76 core (7), 1 chase on an A55 core (0), 2 control loop on core 7 */
#define DQ_BINS 256u
#define DQ_WINDOW_TICKS 14400000u           /* 0.6 s of the 24 MHz counter for a chase */
#define DQ_CONTROL_TICKS 28800000u          /* 1.2 s: 12000 iterations at 10 kHz */
#define DQ_NPU_JOBS 64u
/* Load of the image: 0 none, 1 streaming reads on the other worker cores, 2 NPU job stream, 3 DMA0 copies. */
struct dq_probe {
    uint32_t hist[DQ_BINS];                 /* cycles per timed event, log-linear bins (dq_bin) */
    uint32_t max_cycles, samples, overhead, reserved;
    uint32_t misses;                        /* control loop: iterations that ended after the next release */
    uint32_t late_max;                      /* control loop: largest start delay after the release, in counter ticks */
    uint64_t stream_bytes;                  /* bytes read by the streaming cores during the probe */
    uint64_t ticks;                         /* elapsed counter ticks of the probe */
};
struct dq_setting {
    uint32_t port_words[2][4];              /* ports 19 and 20: +0xc, +0x14, +0x10, +0x18 as applied */
    uint32_t npu_port_words[2];             /* port 18 (NPU): +0xc, +0x14 */
    struct dq_probe probe[DQ_PROBES];
    uint32_t npu_jobs, npu_failed;
    uint64_t npu_ticks_sum, npu_ticks_max, npu_ticks_min, npu_ticks_window;
    uint32_t dma_copies, dma_failed;
    uint64_t dma_ticks_window;
};
struct dq_result {
    char magic[8];
    uint64_t counter_hz;
    uint64_t buffer_base;
    uint32_t load, readiness, a76_mhz, a55_mhz;
    int32_t millicelsius_before[5], millicelsius_after[5];
    char clock_before[256];
    struct dq_setting setting[DQ_SETTINGS];
};
uint64_t dq_read(const uint64_t *p, size_t bytes);
static inline uint32_t dq_bin(uint32_t v)
{
    if (v < 256u) return v >> 2;
    const unsigned msb = 31u - (unsigned)__builtin_clz(v);
    unsigned b = 64u + (msb - 8u) * 16u + ((v >> (msb - 4u)) & 15u);
    return b > DQ_BINS - 1u ? DQ_BINS - 1u : b;
}
#endif
