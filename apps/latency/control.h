#ifndef SUNSTONE_LATENCY_CONTROL_H
#define SUNSTONE_LATENCY_CONTROL_H

#include "sunstone_control.h"
#include <stddef.h>
#include <stdint.h>

#define LATENCY_CONTROL_MAGIC UINT64_C(0x53534354524c3031)
#define LATENCY_CONTROL_VERSION 1u
#define LATENCY_CONTROL_COUNT 7u
#define LATENCY_CONTROL_MIN_SAMPLES UINT64_C(10000)
enum latency_control_mode { LATENCY_CONTROL_BUSY, LATENCY_CONTROL_IDLE };
enum latency_control_flags {
    LATENCY_CONTROL_HISTOGRAM_ONLY = 1u,
    LATENCY_CONTROL_TICKLESS = 2u,
    LATENCY_CONTROL_RESTORE_SPANS = 4u
};
enum latency_control_histogram {
    LATENCY_CONTROL_TASK_LATENCY,
    LATENCY_CONTROL_TASK_GAP,
    LATENCY_CONTROL_IRQ_JITTER,
    LATENCY_CONTROL_HISTOGRAMS
};
struct latency_control_core {
    uint32_t core, rate_hz, paths, error_flags;
    struct ss_control_stats stats[2];
    uint32_t task_entry_max_ticks, task_gap_max_ticks;
    uint32_t task_entry_p9999_ticks, task_gap_p9999_ticks;
    uint64_t published_control, published_task;
    uint32_t histogram[LATENCY_CONTROL_HISTOGRAMS][SS_CONTROL_BINS];
};
struct latency_control_report {
    uint64_t magic, version, frequency_hz, minimum_samples;
    uint32_t requested_mask, mode, ready_mask, quiescent_mask;
    uint32_t consumer_parked, errors, complete_mask, flags;
    struct latency_control_core core[LATENCY_CONTROL_COUNT];
};
_Static_assert(sizeof(struct ss_control_stats) == 72 &&
    offsetof(struct latency_control_core, histogram) == 192 &&
    sizeof(struct latency_control_core) == 2496 &&
    offsetof(struct latency_control_report, core) == 64 &&
    sizeof(struct latency_control_report) == 17536, "control qualification report layout");

/* One start per image. Busy mode measures both paths at 1, 2, 4, 8, 10, 4 and
 * 4 kHz on cores 1 to 7. Idle mode measures only control IRQs, all at 10 kHz.
 * Both modes retain histograms without a ring consumer. Start returns 1 only
 * when every requested core is admitted. */
int latency_control_start(unsigned mode);
int latency_control_ready(void);
/* Complete returns 1 after every selected path has the minimum sample count
 * and control IRQs have one extra entry to supply that many jitter intervals,
 * 0 while collecting, and -1 when a runtime measurement error is published. */
int latency_control_complete(void);
void latency_control_stop(void);
int latency_control_stopped(void);
/* Snapshot returns 0 until all writers acknowledge stop. The report contains
 * raw histogram counts; no missing samples or overflow is converted to a pass. */
int latency_control_snapshot(struct latency_control_report *out);

#endif
