#ifndef NPU_TIMING_H
#define NPU_TIMING_H
/* The NPU job timing application (main.c): every graph of the models region runs alone, one job at a time, then with jobs in
 * flight, and the counter ticks of each are kept. The data region (pack.py) names each graph's inputs and reference outputs;
 * the result is struct npu_timing_result. All numbers are little-endian. */
#include <stdint.h>
#define NPU_TIMING_MAGIC 0x4d49544eu          /* "NTIM" */
#define NPU_TIMING_VERSION 1u
#define NPU_TIMING_RESULT_VERSION 2u
#define NPU_TIMING_GRAPHS 16u
#define NPU_TIMING_JOBS 1000u                 /* the most single jobs of one graph */
struct npu_timing_graph_desc {
    uint32_t jobs;          /* single jobs: each submitted after the one before completed */
    uint32_t depth;         /* jobs in flight in the pipelined pass (0: no such pass), jobs of them in all */
    uint64_t input_offset, input_bytes;           /* the inputs, in the graph's order, from the start of the data region */
    uint64_t reference_offset, reference_bytes;   /* the simulator's outputs, in the graph's order */
};
struct npu_timing_header {
    uint32_t magic, version, count, reserved;
    struct npu_timing_graph_desc graph[NPU_TIMING_GRAPHS];
};
#define NPU_TIMING_RESULT_MAGIC UINT64_C(0x544c534547494d54)    /* "TMIGESLT" */
struct npu_timing_graph_result {
    /* Single jobs: submit to the observed completion, polled, in counter ticks (24 MHz). */
    uint64_t jobs, min, p50, p99, max, sum;
    /* The pipelined pass: jobs with up to depth in flight, from the first submit to the last completion. */
    uint64_t depth, pipelined_ticks;
    /* The outputs after the last job against the reference: bytes compared and bytes that differ. */
    uint64_t output_bytes, differing_bytes;
    uint64_t input_bytes;
    /* Completions of this graph whose event came before all units were idle (bits 31:0) and the idle-word bits seen busy
     * (bits 63:32), from the queue's idle-wait record (struct ss_npu_queue_idle_wait). */
    uint64_t idle_waits_busy;
};
struct npu_timing_result {
    uint64_t magic, version, bytes, count;
    uint64_t clock[2], open_ticks, reserved;
    struct npu_timing_graph_result graph[NPU_TIMING_GRAPHS];
    /* For each graph: the longest wait for full idle (bits 31:0) and all its waits together (bits 63:32), in counter ticks. */
    uint64_t idle_wait_ticks[NPU_TIMING_GRAPHS];
};
_Static_assert(sizeof(struct npu_timing_result) == 1728, "the result fills the output limit");
#endif
