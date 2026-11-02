#ifndef SUNSTONE_NPU_IDLE_H
#define SUNSTONE_NPU_IDLE_H
#include <stdint.h>

/* The state machine needs no knowledge of a particular NPU. A part descriptor
 * supplies the three register offsets it may touch and the bit patterns of the
 * clock-gate pulse protocol. The part also fixes the idle delay before gating
 * and the resume budget. */
struct npu_idle_part {
    uint32_t clock, status, shader;  /* register offsets */
    uint32_t clock_pulse, clock_gate; /* pulse bit reloads; gate bit suspends */
    uint32_t clock_field_mask, clock_field_value; /* scaler fields held across gating */
    uint32_t shader_pulse, shader_field_mask, shader_field_value;
    uint32_t full_idle;              /* status value that proves every unit idle */
    uint64_t delay_us;               /* continuous idle time before gating */
    uint64_t resume_us;              /* budget from resume request to ready */
    uint64_t hard_us;                /* bound on any single settle wait */
};
enum npu_idle_phase { NPU_IDLE_ON, NPU_IDLE_SUSPENDED, NPU_IDLE_RETAINED };
/* The transition in progress when a failure is recorded. */
enum npu_idle_step { NPU_IDLE_STEP_NONE, NPU_IDLE_STEP_SUSPEND, NPU_IDLE_STEP_RESUME };
enum npu_idle_site { NPU_IDLE_HEALTH=1, NPU_IDLE_READ, NPU_IDLE_WRITE,
    NPU_IDLE_SETTLE, NPU_IDLE_GATE_VALUE, NPU_IDLE_SHADER_VALUE,
    NPU_IDLE_FULL_IDLE, NPU_IDLE_UNEXPECTED_IRQ, NPU_IDLE_OWNER,
    NPU_IDLE_RESUME_IRQ, NPU_IDLE_RESUME_BUDGET };
/* Each write completes its DSB before returning. now returns counter ticks,
 * including MMIO latency. health checks lifetime/window authority without MMIO.
 * Every callback is bounded and returns 1 on success, except now. */
struct npu_idle_ops {
    int (*read)(void *, uint32_t offset, uint32_t *);
    int (*write)(void *, uint32_t offset, uint32_t);
    uint64_t (*now)(void *);
    int (*health)(void *);
};
struct npu_idle_failure {
    unsigned site, phase, step;
    uint32_t offset, value, expected;
};
/* Present when gate and resume record unexpected register values instead of refusing. */
#define NPU_IDLE_VALUE_RECORDING 1
struct npu_idle_counters {
    uint64_t gates, resumes, controls, writes, gated_ticks, resume_ticks,
        resume_max_ticks, resume_over_budget, owner_cpu_ticks, owner_wakes,
        value_deviations; /* cycles with a register value other than the one expected */
};
/* The register values one gate and resume cycle read. Values that differ from
 * the expectation are recorded and do not refuse: the vendor reads and writes
 * whatever it finds. The first cycle with a deviation is kept (deviation 1);
 * until then the latest cycle is kept. */
struct npu_idle_observed {
    uint32_t gate_clock, gate_shader, gate_status;
    uint32_t resume_entry_clock, resume_clock, resume_shader, resume_clock_after, resume_shader_after,
        resume_status;
    uint32_t deviation; /* bit mask of NPU_IDLE_DEV_* */
};
enum { NPU_IDLE_DEV_GATE_CLOCK=1, NPU_IDLE_DEV_GATE_SHADER=2, NPU_IDLE_DEV_RESUME_ENTRY=4,
    NPU_IDLE_DEV_RESUME_CLOCK=8, NPU_IDLE_DEV_RESUME_SHADER=16, NPU_IDLE_DEV_RESUME_STATUS=32 };
struct npu_idle {
    struct npu_idle_ops ops;
    const struct npu_idle_part *part;
    void *context;
    uint64_t frequency, idle_since, generation, gated_since;
    uint32_t gate_value, shader_value;
    enum npu_idle_phase phase;
    enum npu_idle_step step;
    unsigned initialized, enabled, strict;
    struct npu_idle_failure failure;
    struct npu_idle_counters counters;
    struct npu_idle_observed observed, first_deviation;
};
/* enabled 0 performs every decision and no register write (the control).
 * strict retains the queue when a resume exceeds the part's budget; otherwise
 * the overrun is only counted. The elapsed time is recorded in both modes. */
int npu_idle_init(struct npu_idle *, const struct npu_idle_part *, const struct npu_idle_ops *,
                  void *, uint64_t frequency, unsigned enabled, unsigned strict);
/* Serialize these calls with the queue's raw local IRQ lock. idle_since is
 * reset on successful resume, so the idle delay also bounds the minimum on
 * time. A completed receipt still held by a caller does not prevent this
 * update. */
void npu_idle_completed(struct npu_idle *, uint64_t now);
int npu_idle_eligible(const struct npu_idle *, uint64_t now);
uint64_t npu_idle_deadline_ticks(const struct npu_idle *);
/* The full-idle proof compares with the part's mask as the vendor does
 * (vipdrv_hw_wait_idle): bits outside the mask, such as bit 31, are ignored. */
/* Caller rechecks full queue/ring idle, current generation, STOP, lifetime
 * health and GIC pending/active quiet under that lock immediately before gate.
 * Gate performs no register read after its final suspend write. */
int npu_idle_gate(struct npu_idle *);
int npu_idle_resume(struct npu_idle *);
void npu_idle_retain(struct npu_idle *, unsigned site, uint32_t offset, uint32_t value, uint32_t expected);
#endif
