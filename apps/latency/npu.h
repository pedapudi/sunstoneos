/* SPDX-License-Identifier: GPL-2.0-only */
/* The NPU load of stage 3b (docs/designs/concurrent-accel.md): stage 1b's relu graph (16 rows of 176 int16 values,
 * tools/npu-fixtures.json) runs pipelined at eight jobs in flight while the latency diagnostic samples wakes. The NPU's owner tasks, its interrupt and
 * the producer task that submits the jobs all run on CPU0, the service core, so cores 1-7 keep only their wake tasks and
 * streams. The NPU runs alone for one window before the streams start, then in the middle two of four busy sub-phases
 * (off, on, on, off). The producer opens the queue and registers the graph before the idle phase and closes it after the
 * busy phase, so neither cost falls inside a measured window. */
#ifndef SUNSTONE_LATENCY_NPU_H
#define SUNSTONE_LATENCY_NPU_H

#include <stdint.h>

#define LATENCY_NPU_MAGIC UINT64_C(0x5353414c4e505531)        /* "SSALNPU1" */
#define LATENCY_NPU_FOOTER_MAGIC UINT64_C(0x5353464c4e505531) /* "SSFLNPU1" */
#define LATENCY_NPU_VERSION 1u
#define LATENCY_NPU_SUBPHASES 4u      /* off, on, on, off */
#define LATENCY_NPU_SUBPHASE_WAKES 32u
#define LATENCY_NPU_ON_MASK 0x6u      /* sub-phases 1 and 2 */
#define LATENCY_NPU_CORE 0u
#define LATENCY_NPU_IN_FLIGHT 8u      /* the manifest's npu.in_flight */
#define LATENCY_NPU_RUNS 2u           /* the NPU-alone window's run, then the busy phase's */
/* The NPU-alone window, and the completions a run reaches before a measured window opens on it. */
#define LATENCY_NPU_ALONE_MS 200u
#define LATENCY_NPU_WARM_JOBS 64u
/* Nested deadlines inside the run budget (the manifest's run_us, 8 s), each several times its expected time: the queue's open
 * and registration (the 96-graph encoder's open took 285 ms, docs/status/large-regions.md), one job (accel-overlap's bound),
 * the warm-up of a run (64 jobs, 0.4 ms at stage 1's 177,768 jobs/s), its drain (eight jobs and one output compare), one busy
 * sub-phase (32 wakes at 1 ms) and the close. The worst case, with every deadline reached, comes to about 5 s.
 *
 * The QEMU model runs without icount, so its counter follows the host's clock and a host that starves its vCPUs stretches
 * every step. Its sub-phases with the NPU on are the longest: the stand-in completes a job every 250 us on CPU0, and CPU0's
 * wake task then misses ticks (a mean gap of 4.3 ms between its wakes against 2.5 ms with the NPU off, and 1.9 ms on CPU1
 * in both; with a 1 ms stand-in period the gaps were 3.9 and 3.6 ms), so CPU0 is the last core to reach its 32 wakes.
 * The model's bounds are about twice the longest of 16 runs made four at once (checks/latency-npu.py runs as a timing step,
 * tools/gate-slots.json), on 2026-11-15 at load averages 17-25: a sub-phase 1.92 s, a warm-up 225 ms, a drain 51 ms. */
#define LATENCY_NPU_OPEN_US 2000000u
#define LATENCY_NPU_JOB_US 1000000u
#define LATENCY_NPU_CLOSE_US 1000000u
#ifdef SUNSTONE_LATENCY_MODEL
#define LATENCY_NPU_WARM_US 500000u
#define LATENCY_NPU_DRAIN_US 100000u
#define LATENCY_NPU_SUBPHASE_US 4000000u
#else
#define LATENCY_NPU_WARM_US 100000u
#define LATENCY_NPU_DRAIN_US 100000u
#define LATENCY_NPU_SUBPHASE_US 250000u
#endif

/* The failure details (ss_fail_value) of the NPU load: 0xd00 and up. */
enum latency_npu_fail {
    LATENCY_NPU_FAIL_GRANT = 0xd01,     /* the grant names another NPU core, or the producer runs off CPU0 */
    LATENCY_NPU_FAIL_VECTORS = 0xd02,   /* the vectors region is not stage 1b's version 1 layout */
    LATENCY_NPU_FAIL_OPEN = 0xd03,      /* the queue did not open in LATENCY_NPU_OPEN_US */
    LATENCY_NPU_FAIL_REGISTER = 0xd04,  /* the graph did not register, or its input tensor did not take the vector */
    LATENCY_NPU_FAIL_SUBMIT = 0xd05,
    LATENCY_NPU_FAIL_WAIT = 0xd06,
    LATENCY_NPU_FAIL_RELEASE = 0xd07,
    LATENCY_NPU_FAIL_OUTPUT = 0xd08,    /* the output could not be read back */
    LATENCY_NPU_FAIL_MISMATCH = 0xd09,  /* the last output of a run differs from the simulator reference */
    LATENCY_NPU_FAIL_WARM = 0xd0a,      /* a run did not complete LATENCY_NPU_WARM_JOBS in LATENCY_NPU_WARM_US */
    LATENCY_NPU_FAIL_DRAIN = 0xd0b,     /* a run did not drain in LATENCY_NPU_DRAIN_US */
    LATENCY_NPU_FAIL_SUBPHASE = 0xd0c,  /* a busy sub-phase outlasted LATENCY_NPU_SUBPHASE_US; the value is its index */
    LATENCY_NPU_FAIL_CLOSE = 0xd0d,
    LATENCY_NPU_FAIL_READY = 0xd0e,     /* the producer did not report the queue ready in LATENCY_NPU_OPEN_US */
};

/* One measured window: the NPU-alone window or a busy sub-phase. */
struct latency_npu_window {
    uint64_t start, end;                    /* counter values at the edges */
    uint64_t jobs_start, jobs_end;          /* NPU jobs completed since the queue opened, at the edges */
    uint64_t sweeps_start[8], sweeps_end[8];
    uint32_t in_flight_start, in_flight_end;
    uint32_t run_start, run_end;            /* the producer's run state at the edges: 0 stopped, 1 running */
};
/* One run of continuous jobs, from the control task's start to the producer's drain. */
struct latency_npu_run {
    uint64_t requested;        /* the control task asked the producer to run */
    uint64_t warm;             /* the control task saw LATENCY_NPU_WARM_JOBS completions of this run */
    uint64_t stop_requested;
    uint64_t drained;          /* the producer's last completion of the run */
    uint64_t jobs_before;      /* completions since the open, when the run began */
    uint64_t submitted, completed;
    uint32_t compared, mismatches;
};
struct latency_npu_report {
    uint64_t magic, version, bytes;
    uint32_t subphase_wakes, on_mask, in_flight, alone_ms;
    uint32_t warm_jobs, core, producer_core, complete;
    uint64_t open_ticks, register_ticks, close_ticks;   /* counter ticks of each setup step */
    uint32_t graph_input_bytes, graph_reference_bytes, model, reserved;
    struct latency_npu_window alone;
    struct latency_npu_window sub[LATENCY_NPU_SUBPHASES];
    struct latency_npu_run runs[LATENCY_NPU_RUNS];
};
_Static_assert(sizeof(struct latency_npu_report) <= 4096 - 64, "the section and its footer fit the 4 KiB allowance");

/* Stop the session with a failure detail of enum latency_npu_fail and its value. */
#ifdef SUNSTONE_LATENCY_MODEL
void latency_npu_fail(uint32_t detail, uint32_t value);
#else
__attribute__((noreturn)) void latency_npu_fail(uint32_t detail, uint32_t value);
#endif
/* Create the producer task on CPU0 (from latency_workload_init). It opens the queue and registers the graph at once. */
void latency_npu_init(void);
/* The producer has opened the queue and registered the graph: 1; still opening: 0. A failure stops the session. */
int latency_npu_ready(void);
/* Completions since the queue opened, and jobs submitted and not yet completed. */
uint64_t latency_npu_jobs(void);
unsigned latency_npu_in_flight(void);
/* Start a run of continuous jobs; stop it. latency_npu_drained is 1 once the stopped run's jobs have completed and its
 * last output has been compared. */
void latency_npu_start(struct latency_npu_run *run);
void latency_npu_stop(struct latency_npu_run *run);
int latency_npu_drained(void);
int latency_npu_running(void);
/* Close the queue, after the busy phase; latency_npu_closed is 1 once it has. */
void latency_npu_close(void);
int latency_npu_closed(void);
/* The setup costs and the producer's core, for the report. */
void latency_npu_setup(struct latency_npu_report *report);
#ifdef SUNSTONE_LATENCY_MODEL
/* The QEMU stand-in's completion interrupt (CNTV, PPI 27, on CPU0): platform/qemu-smp/platform.c. */
void latency_npu_model_irq(void);
#endif

#endif
