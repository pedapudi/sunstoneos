#include "npu_idle.h"
#include <stddef.h>

static uint64_t us_ticks(const struct npu_idle *p, uint64_t us)
{ return (p->frequency * us + UINT64_C(999999)) / UINT64_C(1000000); }
void npu_idle_retain(struct npu_idle *p, unsigned site, uint32_t offset, uint32_t value, uint32_t expected)
{
    if (!p || p->phase == NPU_IDLE_RETAINED) return;
    p->failure = (struct npu_idle_failure){site, p->phase, p->step, offset, value, expected};
    p->phase = NPU_IDLE_RETAINED;
}
static int health(struct npu_idle *p)
{
    if (p->phase == NPU_IDLE_RETAINED) return 0;
    if (p->ops.health(p->context) == 1) return 1;
    npu_idle_retain(p, NPU_IDLE_HEALTH, 0, 0, 1); return 0;
}
static int read_reg(struct npu_idle *p, uint32_t offset, uint32_t *value)
{
    *value = 0;
    if (!health(p)) return 0;
    if (p->ops.read(p->context, offset, value) == 1) return 1;
    npu_idle_retain(p, NPU_IDLE_READ, offset, *value, 0); return 0;
}
static int write_reg(struct npu_idle *p, uint32_t offset, uint32_t value)
{
    if (!health(p)) return 0;
    ++p->counters.writes;
    if (p->ops.write(p->context, offset, value) == 1) return 1;
    npu_idle_retain(p, NPU_IDLE_WRITE, offset, value, 0); return 0;
}
/* The vendor's delay begins after the completed pulse-low write. Ceiling
 * conversion preserves at least one microsecond at fractional MHz rates. The
 * wait is bounded by the resume budget when the budget is strict and by the
 * part's hard bound otherwise. */
static int settle(struct npu_idle *p, uint64_t start, uint64_t budget_start)
{
    const uint64_t hold = us_ticks(p, 1),
        limit = us_ticks(p, p->strict ? p->part->resume_us : p->part->hard_us);
    for (unsigned i = 0; i < 100000; ++i) {
        const uint64_t now = p->ops.now(p->context);
        if (now - budget_start > limit) break;
        if (now - start >= hold) return health(p);
    }
    npu_idle_retain(p, NPU_IDLE_SETTLE, 0, (uint32_t)(p->ops.now(p->context) - start), (uint32_t)hold);
    return 0;
}
static int matches(struct npu_idle *p, uint32_t offset, uint32_t expected, unsigned site)
{
    uint32_t value;
    if (!read_reg(p, offset, &value)) return 0;
    if (value == expected) return 1;
    npu_idle_retain(p, site, offset, value, expected); return 0;
}
int npu_idle_init(struct npu_idle *p, const struct npu_idle_part *part, const struct npu_idle_ops *ops,
                  void *context, uint64_t frequency, unsigned enabled, unsigned strict)
{
    if (!p || p->initialized || !part || !ops || !ops->read || !ops->write || !ops->now ||
        !ops->health || !context || !frequency || frequency > UINT64_C(1000000000) ||
        enabled > 1 || strict > 1 || !part->delay_us || !part->resume_us ||
        part->hard_us < part->resume_us) return 0;
    p->ops = *ops; p->part = part; p->context = context; p->frequency = frequency;
    p->enabled = enabled; p->strict = strict; p->phase = NPU_IDLE_ON;
    p->step = NPU_IDLE_STEP_NONE; p->initialized = 1;
    p->idle_since = p->ops.now(context);
    return health(p) ? 1 : -1;
}
void npu_idle_completed(struct npu_idle *p, uint64_t now)
{
    if (!p || !p->initialized || p->phase != NPU_IDLE_ON) return;
    if (p->generation == UINT64_MAX) {
        npu_idle_retain(p, NPU_IDLE_OWNER, 0, UINT32_MAX, 0); return;
    }
    p->idle_since = now; ++p->generation;
}
uint64_t npu_idle_deadline_ticks(const struct npu_idle *p)
{ return p->idle_since + us_ticks(p, p->part->delay_us); }
int npu_idle_eligible(const struct npu_idle *p, uint64_t now)
{
    return p && p->initialized && p->phase == NPU_IDLE_ON &&
        now - p->idle_since >= us_ticks(p, p->part->delay_us);
}
int npu_idle_gate(struct npu_idle *p)
{
    if (!p || !p->initialized || p->phase == NPU_IDLE_RETAINED) return -1;
    if (!npu_idle_eligible(p, p->ops.now(p->context))) return 0;
    if (!health(p)) return -1;
    if (!p->enabled) {
        ++p->counters.controls; return 0;
    }
    const struct npu_idle_part *part = p->part;
    const uint32_t held = part->clock_pulse | part->clock_gate;
    uint32_t value, shader;
    p->step = NPU_IDLE_STEP_SUSPEND;
    if (!read_reg(p, part->clock, &value) || !read_reg(p, part->shader, &shader)) return -1;
    if (value & held) {
        npu_idle_retain(p, NPU_IDLE_GATE_VALUE, part->clock, value, value & ~held); return -1;
    }
    if ((value & part->clock_field_mask) != part->clock_field_value) {
        npu_idle_retain(p, NPU_IDLE_GATE_VALUE, part->clock, value,
            (value & ~part->clock_field_mask) | part->clock_field_value); return -1;
    }
    if (shader & part->shader_pulse) {
        npu_idle_retain(p, NPU_IDLE_SHADER_VALUE, part->shader, shader, shader & ~part->shader_pulse); return -1;
    }
    if ((shader & part->shader_field_mask) != part->shader_field_value) {
        npu_idle_retain(p, NPU_IDLE_SHADER_VALUE, part->shader, shader,
            (shader & ~part->shader_field_mask) | part->shader_field_value); return -1;
    }
    if (!matches(p, part->status, part->full_idle, NPU_IDLE_FULL_IDLE)) return -1;
    p->gate_value = value; p->shader_value = shader;
    const uint64_t began = p->ops.now(p->context);
    if (!write_reg(p, part->clock, value | held) ||
        !write_reg(p, part->clock, value | part->clock_gate)) return -1;
    const uint64_t completed = p->ops.now(p->context);
    if (!settle(p, completed, began)) return -1;
    p->phase = NPU_IDLE_SUSPENDED; p->step = NPU_IDLE_STEP_NONE;
    p->gated_since = p->ops.now(p->context);
    ++p->counters.gates; return 1;
}
int npu_idle_resume(struct npu_idle *p)
{
    if (!p || !p->initialized || p->phase == NPU_IDLE_RETAINED) return -1;
    if (p->phase == NPU_IDLE_ON) return 1;
    const struct npu_idle_part *part = p->part;
    const uint64_t began = p->ops.now(p->context);
    p->step = NPU_IDLE_STEP_RESUME;
    if (!health(p)) return -1;
    /* The clock register is the vendor's only read while the clocks remain
     * suspended. */
    if (!matches(p, part->clock, p->gate_value | part->clock_gate, NPU_IDLE_GATE_VALUE) ||
        !write_reg(p, part->clock, p->gate_value | part->clock_pulse) ||
        !write_reg(p, part->clock, p->gate_value)) return -1;
    if (!settle(p, p->ops.now(p->context), began)) return -1;
    uint32_t shader;
    if (!matches(p, part->clock, p->gate_value, NPU_IDLE_GATE_VALUE) ||
        !read_reg(p, part->shader, &shader)) return -1;
    if (shader != p->shader_value) {
        npu_idle_retain(p, NPU_IDLE_SHADER_VALUE, part->shader, shader, p->shader_value); return -1;
    }
    if (!write_reg(p, part->shader, shader | part->shader_pulse) || !write_reg(p, part->shader, shader)) return -1;
    if (!matches(p, part->clock, p->gate_value, NPU_IDLE_GATE_VALUE) ||
        !matches(p, part->shader, shader, NPU_IDLE_SHADER_VALUE) ||
        !matches(p, part->status, part->full_idle, NPU_IDLE_FULL_IDLE)) return -1;
    const uint64_t now = p->ops.now(p->context), elapsed = now - began,
        budget = us_ticks(p, part->resume_us);
    p->counters.gated_ticks += began - p->gated_since;
    p->counters.resume_ticks += elapsed;
    if (elapsed > p->counters.resume_max_ticks) p->counters.resume_max_ticks = elapsed;
    if (elapsed > budget) {
        ++p->counters.resume_over_budget;
        if (p->strict) {
            npu_idle_retain(p, NPU_IDLE_RESUME_BUDGET, 0, (uint32_t)elapsed, (uint32_t)budget); return -1;
        }
    }
    ++p->counters.resumes; p->phase = NPU_IDLE_ON; p->step = NPU_IDLE_STEP_NONE;
    npu_idle_completed(p, now); return p->phase == NPU_IDLE_ON ? 1 : -1;
}
