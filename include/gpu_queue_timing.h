#ifndef SUNSTONE_GPU_QUEUE_TIMING_H
#define SUNSTONE_GPU_QUEUE_TIMING_H
#include <stdint.h>
#include "gpu_queue.h"
#ifdef SUNSTONE_GPU_NSI_PMU
#include "a733_gpu_nsi_pmu.h"
#endif
#define GPU_QUEUE_TIMING_CAPACITY 192u
/* Architectural counter ticks (24 MHz), except explicitly named raw fields. */
enum gpu_queue_timing_stamp {
    GPU_TIMING_SUBMIT_BEGIN, GPU_TIMING_SUBMIT_END, GPU_TIMING_TAKE,
    GPU_TIMING_CCCB_BEGIN, GPU_TIMING_CCB_BEGIN, GPU_TIMING_CCB_END,
    GPU_TIMING_KICK_BEGIN, GPU_TIMING_KICK_END, GPU_TIMING_ACCEPT,
    GPU_TIMING_IRQ_FIRST, GPU_TIMING_IRQ_LAST, GPU_TIMING_WORKER_WAKE,
    GPU_TIMING_ACQUIRE_BEGIN, GPU_TIMING_ACQUIRE_END, GPU_TIMING_COMPLETE,
    GPU_TIMING_WAIT_BEGIN, GPU_TIMING_WAIT_END,
    GPU_TIMING_WORKER_RECEIVE_BEGIN, GPU_TIMING_WORKER_BLOCK,
    GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE, GPU_TIMING_BLOCKED_WAKE_IRQ, GPU_TIMING_STAMPS
};
/* State slots: before kick, first observed execution, latest observed execution,
 * completion. valid bits: 0 PLL, 1 clock, 2 BGR, 3 CORE policy/status,
 * 4 firmware power/runtime, 5 utilization lastword unchanged, 6 local idle registers. */
struct gpu_queue_timing_state {
    uint64_t cpu_before, cpu_after, firmware_pre_raw, firmware_post_raw;
    uint64_t utilization_last_raw, utilization_raw[3], firmware_idle_start_raw;
    uint32_t valid, pll_raw, clock_raw, bgr_raw, core_policy_raw, core_status_raw;
    uint32_t fw_power_raw, fw_flags_raw, fw_core_hz, fw_apm_latency_ms;
    uint32_t fw_apm_flags, fw_apm_persistent, driver_hz, driver_uv;
    uint32_t utilization_sequence, utilization_flags, sidekick_idle_raw, slc_idle_raw;
    uint64_t pmic_rail_id, pmic_request_id;
    uint32_t pmic_observed, pmic_retained, pmic_selector_raw, pmic_enable_raw, pmic_programmed_uv;
};
struct gpu_queue_timing_record {
#ifdef SUNSTONE_GPU_NSI_PMU
    struct a733_gpu_nsi_record nsi;
#endif
    uint64_t ticks[GPU_TIMING_STAMPS];
    uint64_t sync_bytes, clean_ticks, clean_bytes, invalidate_ticks, invalidate_bytes;
    uint64_t firmware_pre_raw, firmware_post_raw;
    uint64_t cr_raw[3], cr_cpu_before[3], cr_cpu_after[3];
    uint32_t sequence, client, groups[3], kick_to_done_us;
    uint32_t irq_count, worker_irq_wakes, result, timestamp_injected;
    uint32_t worker_blocks, worker_blocked_irq_wakes;
    struct gpu_queue_timing_state state[4];
    uint32_t mid_samples;
};
#ifdef SUNSTONE_GPU_TIMING
#ifdef __cplusplus
extern "C" {
#endif
struct pvr_compute_queue;
void gpu_queue_timing_bind(struct pvr_compute_queue *);
void gpu_queue_timing_crumb(uint32_t, uint32_t);
void gpu_queue_timing_stamp(enum gpu_queue_timing_stamp);
int gpu_queue_timing_active(void);
int gpu_queue_timing_kicking(void);
void gpu_queue_timing_irq(void);
void gpu_queue_timing_block_receive(void *);
void gpu_queue_timing_cr(unsigned, uint64_t, uint64_t, uint64_t);
void a733_gpu_backend_timing_sample(unsigned);
#ifdef SUNSTONE_GPU_NSI_PMU
void a733_gpu_backend_nsi_sample(unsigned, struct a733_gpu_nsi_record *);
#endif
int a733_gpu_backend_timing_state(struct gpu_queue_timing_state *);
void gpu_queue_timing_state_sample(unsigned);
#ifdef __cplusplus
}
#endif
#endif
#endif
