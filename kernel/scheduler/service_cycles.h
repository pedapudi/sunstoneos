/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SERVICE_CYCLES_H
#define SS_SERVICE_CYCLES_H
/* Measurement build only (SUNSTONE_LATENCY_SERVICE_CYCLES, which needs the
 * latency cause build and its cycle counter). Each service pass and each
 * control drain is timed in CPU cycles. The drain's empty return is switched
 * on and off by bit SS_SERVICE_CYCLES_SWITCH_SHIFT of the CPU's own generic
 * counter, so both behaviours run interleaved under the same load; a drain
 * inside a service pass takes the pass's setting. Without the option the
 * names below select the production functions and nothing else changes. */
#ifdef SUNSTONE_LATENCY_SERVICE_CYCLES
#include <stdbool.h>
#include <stdint.h>
#ifndef SUNSTONE_LATENCY_CAUSE
#error "Service cycle measurement needs the latency cause build"
#endif
#define SS_SERVICE_CYCLES_MAGIC UINT64_C(0x5353535643594331)
#define SS_SERVICE_CYCLES_FOOTER_MAGIC UINT64_C(0x5353535643594631)
#define SS_SERVICE_CYCLES_VERSION 1u
#define SS_SERVICE_CYCLES_CORES 8u
#define SS_SERVICE_CYCLES_PHASES 2u /* the latency idle and busy phases, by the span's end */
#define SS_SERVICE_CYCLES_KINDS 2u  /* service pass, control drain */
#define SS_SERVICE_CYCLES_STATES 2u /* empty return off (bringup), on (fast path) */
/* 14: the setting changes every 16,384 counts, 683 us at 24 MHz, so CPU0's
 * 1 ms tick falls in both settings. */
#define SS_SERVICE_CYCLES_SWITCH_SHIFT 14u
/* Bins 0-7 hold 0-63 cycles in steps of 8. Bins 8-103 divide each octave
 * [2^e, 2^(e+1)) for e = 6..17 into eight equal parts. Bin 104 holds 2^18
 * cycles and more. */
#define SS_SERVICE_CYCLES_BINS 105u
enum ss_service_cycles_kind { SS_SERVICE_CYCLES_SERVICE, SS_SERVICE_CYCLES_DRAIN };
/* A span during which the CPU took an interrupt is counted in interrupted and
 * interrupted_sum only, since its cycles include the nested handler. */
struct ss_service_cycles_cell {
    uint64_t count, sum, max, interrupted, interrupted_sum;
    uint32_t bins[SS_SERVICE_CYCLES_BINS], pad;
};
_Static_assert(sizeof(struct ss_service_cycles_cell) == 464, "service cycle cell layout");
struct ss_service_cycles_report {
    uint64_t magic, version, bytes, cores, phases, kinds, states, bins;
    uint64_t switch_shift, complete, reserved[6];
    struct ss_service_cycles_cell cell[SS_SERVICE_CYCLES_PHASES][SS_SERVICE_CYCLES_CORES]
        [SS_SERVICE_CYCLES_KINDS][SS_SERVICE_CYCLES_STATES];
};
struct ss_service_cycles_span {
    uint64_t cycles, irqs;
    unsigned kind, core, state, previous, recording;
};
void ss_service_cycles_begin(struct ss_service_cycles_span *span, unsigned kind, unsigned core);
void ss_service_cycles_end(const struct ss_service_cycles_span *span, unsigned core);
bool ss_service_cycles_fast(unsigned core);
bool ss_service_cycles_snapshot(struct ss_service_cycles_report *out);
#ifdef SS_FREERTOS_TASKS_TRANSLATION
/* kernel/scheduler/freertos.c defines the bodies under these names; its last
 * lines define the timed ssKernelService and ssKernelControlDrain. */
#define SS_SERVICE_DEFINITION ssKernelServiceBody
#define SS_CONTROL_DRAIN_DEFINITION ssKernelControlDrainBody
#define SS_CONTROL_EMPTY(c) (ss_service_cycles_fast(c) && !ssKernelControlDue(c))
static void ssKernelService(void);
#endif
#else
#define SS_SERVICE_DEFINITION ssKernelService
#define SS_CONTROL_DRAIN_DEFINITION ssKernelControlDrain
#define SS_CONTROL_EMPTY(c) (!ssKernelControlDue(c))
#endif
#endif
