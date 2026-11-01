#ifndef SUNSTONE_NPU_IDLE_H
#define SUNSTONE_NPU_IDLE_H
#include <stdint.h>

#define NPU_IDLE_DELAY_US UINT64_C(10000)
#define NPU_IDLE_RESUME_US UINT64_C(100)
enum npu_idle_phase { NPU_IDLE_ON, NPU_IDLE_SUSPENDING, NPU_IDLE_SUSPENDED,
    NPU_IDLE_RESUMING, NPU_IDLE_RETAINED };
enum npu_idle_site { NPU_IDLE_HEALTH=1, NPU_IDLE_READ, NPU_IDLE_WRITE,
    NPU_IDLE_SETTLE, NPU_IDLE_GATE_VALUE, NPU_IDLE_SHADER_VALUE,
    NPU_IDLE_FULL_IDLE, NPU_IDLE_UNEXPECTED_IRQ, NPU_IDLE_OWNER };
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
    unsigned site, phase;
    uint32_t offset, value, expected;
};
struct npu_idle_counters {
    uint64_t gates, resumes, controls, writes, gated_ticks, resume_ticks,
        resume_max_ticks, owner_cpu_ticks, owner_wakes;
};
struct npu_idle {
    struct npu_idle_ops ops;
    void *context;
    uint64_t frequency, idle_since, generation, gated_since;
    uint32_t gate_value, shader_value;
    enum npu_idle_phase phase;
    unsigned initialized, enabled;
    struct npu_idle_failure failure;
    struct npu_idle_counters counters;
};
int npu_idle_init(struct npu_idle *, const struct npu_idle_ops *, void *, uint64_t frequency, unsigned enabled);
/* Serialize these calls with the queue's raw local IRQ lock. idle_since is
 * reset on successful resume, so the 10 ms delay also satisfies minimum on time.
 * A completed receipt still held by a caller does not prevent this update. */
void npu_idle_completed(struct npu_idle *, uint64_t now);
int npu_idle_eligible(const struct npu_idle *, uint64_t now);
uint64_t npu_idle_deadline_ticks(const struct npu_idle *);
/* Caller rechecks full queue/ring idle, current generation, STOP, lifetime
 * health and GIC pending/active quiet under that lock immediately before gate.
 * Gate performs no register read after its final suspend write. */
int npu_idle_gate(struct npu_idle *);
int npu_idle_resume(struct npu_idle *);
void npu_idle_retain(struct npu_idle *, unsigned site, uint32_t offset, uint32_t value, uint32_t expected);
#endif
