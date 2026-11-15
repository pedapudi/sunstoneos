/* The result of the concurrent GPU, NPU and CPU streaming application (main.c), which host.py decodes. Every field is
 * little-endian; times are virtual counter (CNTVCT) ticks at 24 MHz. */
#ifndef ACCEL_OVERLAP_RESULT_H
#define ACCEL_OVERLAP_RESULT_H
#include <stdint.h>

#define ACCEL_OVERLAP_MAGIC UINT32_C(0x4c564f41) /* "AOVL" */
#define ACCEL_OVERLAP_VERSION 1u
#define ACCEL_OVERLAP_VECTORS_MAGIC UINT32_C(0x56564f41) /* "AOVV" */
#define ACCEL_OVERLAP_PHASES 5u
/* Phase devices (struct accel_overlap_phase.devices). */
#define ACCEL_OVERLAP_GPU 1u
#define ACCEL_OVERLAP_NPU 2u
#define ACCEL_OVERLAP_STREAM 4u
#define ACCEL_OVERLAP_PLL_CYCLES 8u
/* The second input region: this header, then the relu graph's input tensor and its simulator reference output. */
struct accel_overlap_vectors {
    uint32_t magic, version;
    uint32_t input_offset, input_bytes, reference_offset, reference_bytes;
};
/* A distribution of interval samples in ticks. count samples entered the histogram; missing counts the completions with no
 * sample (the waiter did not block, or a stamp was absent); overflow counts samples above the histogram, which only max
 * bounds. A percentile that falls in the overflow reads UINT32_MAX. */
struct accel_overlap_distribution {
    uint32_t count, missing, overflow, min, p50, p99, p999, max;
};
struct accel_overlap_device {
    struct accel_overlap_distribution wake;  /* completion interrupt handler entry to the waiting task running */
    struct accel_overlap_distribution trip;  /* submission to the wait's return, serial jobs */
    uint32_t serial_jobs, pipelined_jobs, depth, mismatches;
    uint64_t pipelined_ticks;
};
struct accel_overlap_phase {
    uint32_t devices, throttled;
    struct accel_overlap_device gpu, npu;
    uint64_t overlap;       /* NPU completions observed while a GPU job was in flight */
    uint64_t ticks;         /* the phase from its start to the last device's end */
    uint64_t sweeps;        /* streaming sweeps of all workers, each 4 MiB read and rewritten */
    uint64_t stream_ticks;  /* the streaming span */
};
/* PLL_NPU around PLL_GPU0 writes (struct ss_npu_queue_pll_watch) and the GPU close and reopen cycles of the last phase. */
struct accel_overlap_pll {
    uint32_t reference, diff, last, unlocked;
    uint64_t writes, in_flight, completions;
    uint32_t first_site, first_word;
    uint32_t cycles, cycle_mismatches;
    uint64_t close_ticks_max, open_ticks_max;
};
struct accel_overlap_result {
    uint32_t magic, version, bytes, phases;
    uint64_t clock[2];      /* the CLOCK line's CPU points, as apps/npu-timing packs them */
    uint32_t initial_top, final_top, stream_workers, stream_bytes_per_worker;
    struct accel_overlap_pll pll;
    struct accel_overlap_phase phase[ACCEL_OVERLAP_PHASES];
};
_Static_assert(sizeof(struct accel_overlap_distribution) == 32 && sizeof(struct accel_overlap_device) == 88 &&
               sizeof(struct accel_overlap_phase) == 216 && sizeof(struct accel_overlap_pll) == 72 &&
               sizeof(struct accel_overlap_result) == 1200, "host.py decodes this layout");
#endif
