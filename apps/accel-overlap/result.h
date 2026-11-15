/* The result of the concurrent GPU, NPU and CPU streaming application (main.c), which host.py decodes. Every field is
 * little-endian; times are virtual counter (CNTVCT) ticks at 24 MHz. */
#ifndef ACCEL_OVERLAP_RESULT_H
#define ACCEL_OVERLAP_RESULT_H
#include <stdint.h>

#define ACCEL_OVERLAP_MAGIC UINT32_C(0x4c564f41) /* "AOVL" */
#define ACCEL_OVERLAP_VERSION 1u
#define ACCEL_OVERLAP_SWEEP_VERSION 2u  /* the streaming sweep build (ACCEL_OVERLAP_SWEEP): struct accel_overlap_sweep_result */
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
/* The streaming sweep build: each phase runs both accelerators pipelined with a set of streaming workers (stream_mask, bit w
 * for the worker on stream_core[w]) and a dependent-load chase on core 0. A chase sample times loads_per_sample loads, each
 * of which takes its address from the line the previous one read, in one cycle through every 64-byte line of the chase
 * region in random order. */
struct accel_overlap_sweep {
    uint32_t stream_mask, loads_per_sample;
    struct accel_overlap_distribution chase;    /* ticks of one sample */
    uint64_t loads, chase_ticks;                /* every load of the phase and the span that timed them */
};
struct accel_overlap_sweep_result {
    struct accel_overlap_result base;           /* version ACCEL_OVERLAP_SWEEP_VERSION; the pll cycles stay zero */
    uint64_t chase_lines;                       /* the cycle's length, walked once before the first phase */
    struct accel_overlap_sweep sweep[ACCEL_OVERLAP_PHASES];
};
_Static_assert(sizeof(struct accel_overlap_sweep) == 56 && sizeof(struct accel_overlap_sweep_result) == 1488,
               "host.py decodes this layout");
/* The larger-job sweep build (ACCEL_OVERLAP_LARGE): nine phases, each with its devices, streaming set and chase. A phase's
 * stream_mode names the worker (bits 7:0, as in stream_mask) that streams over three interleaved 4 MiB areas in one loop,
 * with ACCEL_OVERLAP_INTERLEAVED set. After the pipelined second, every GPU slot runs one serial job with a one-line sync
 * range (plain) and then one with the slot's whole output range (synced), whose output is compared; the difference of the two
 * round trips is the output cache maintenance of a full slot. */
#define ACCEL_OVERLAP_LARGE_VERSION 3u
#define ACCEL_OVERLAP_LARGE_PHASES 9u
/* The four-elements-per-lane GPU run (ACCEL_OVERLAP_WIDTH): the larger-job layout; only phase 0, the GPU alone, runs, and the
 * other phases stay zero. */
#define ACCEL_OVERLAP_WIDTH_VERSION 4u
/* The NSI counter calibration (ACCEL_OVERLAP_COUNTERS): the width build with four counted phases (idle, the GPU alone, the
 * NPU alone, core 2 streaming), NSI ports 6 and 18 counted in each with the longest period, then two window probes (the GPU
 * alone on port 6, the NPU alone on port 18) with a 100 ms period and a sample every 5 ms; the larger-job header and its
 * six phases, then the counter record. */
#define ACCEL_OVERLAP_COUNTERS_VERSION 5u
#define ACCEL_OVERLAP_COUNTER_PHASES 4u
#define ACCEL_OVERLAP_COUNTER_PROBES 2u
#define ACCEL_OVERLAP_PROBE_SAMPLES 128u
#define ACCEL_OVERLAP_INTERLEAVED 0x100u
struct accel_overlap_large_phase {
    struct accel_overlap_phase run;
    struct accel_overlap_sweep sweep;
    uint32_t stream_mode, slot_jobs;
    uint64_t plain_ticks_sum, plain_ticks_max;     /* serial slot jobs with a 64-byte output sync range */
    uint64_t synced_ticks_sum, synced_ticks_max;   /* serial slot jobs with the whole slot as the sync range */
};
struct accel_overlap_large_result {
    uint32_t magic, version, bytes, phases;
    uint64_t clock[2];
    uint32_t initial_top, final_top, stream_workers, stream_bytes_per_worker;
    struct accel_overlap_pll pll;
    uint64_t chase_lines;
    uint32_t gpu_elements, npu_tensor_bytes;
    struct accel_overlap_large_phase phase[ACCEL_OVERLAP_LARGE_PHASES];
};
_Static_assert(sizeof(struct accel_overlap_large_phase) == 312 && sizeof(struct accel_overlap_large_result) == 2944,
               "host.py decodes this layout");
_Static_assert(sizeof(struct accel_overlap_distribution) == 32 && sizeof(struct accel_overlap_device) == 88 &&
               sizeof(struct accel_overlap_phase) == 216 && sizeof(struct accel_overlap_pll) == 72 &&
               sizeof(struct accel_overlap_result) == 1200, "host.py decodes this layout");
/* One phase: per bank (0 port 6, 1 port 18) the CLR readback, the six counters (requests, data, latency; read then write)
 * at half the phase with counting on, just after the disable, and 10 us later. */
struct accel_overlap_counter_phase {
    uint32_t status, clear[2], reserved;
    uint32_t live[2][6], value[2][6], held[2][6];
    uint64_t begin, live_at, end;
};
/* One probe sample: ticks since the probe began, the device's jobs completed so far, and RQ_RD, RQ_WR, DT_RD, DT_WR. */
struct accel_overlap_counter_sample {
    uint32_t ticks, jobs, count[4];
};
/* A window probe: one bank with a short period; the values just after the disable and 10 us later; the samples. */
struct accel_overlap_counter_probe {
    uint32_t status, bank, cycle, samples, clear, reserved;
    uint64_t begin, end;
    uint32_t value[6], held[6];
    struct accel_overlap_counter_sample sample[ACCEL_OVERLAP_PROBE_SAMPLES];
};
struct accel_overlap_counter_result {
    struct accel_overlap_large_result large;
    uint32_t open_status, close_status, fail_address, fail_value;
    uint32_t reset[2][9];                         /* every bank word read before any write */
    struct accel_overlap_counter_phase phase[ACCEL_OVERLAP_COUNTER_PHASES];
    struct accel_overlap_counter_probe probe[ACCEL_OVERLAP_COUNTER_PROBES];
};
_Static_assert(sizeof(struct accel_overlap_counter_phase) == 184 && sizeof(struct accel_overlap_counter_sample) == 24 &&
               sizeof(struct accel_overlap_counter_probe) == 3160 && sizeof(struct accel_overlap_counter_result) == 10088,
               "host.py decodes this layout");
#endif
