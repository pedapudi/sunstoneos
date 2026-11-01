#include "npu_idle.h"
#include <stddef.h>

static uint64_t us_ticks(const struct npu_idle *p, uint64_t us)
{ return (p->frequency * us + UINT64_C(999999)) / UINT64_C(1000000); }
void npu_idle_retain(struct npu_idle *p, unsigned site, uint32_t offset, uint32_t value, uint32_t expected)
{
    if (!p || p->phase == NPU_IDLE_RETAINED) return;
    p->failure = (struct npu_idle_failure){site, p->phase, offset, value, expected};
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
 * conversion preserves at least one microsecond at fractional MHz rates. */
static int settle(struct npu_idle *p, uint64_t start, uint64_t budget_start)
{
    const uint64_t hold = us_ticks(p, 1), budget = us_ticks(p, NPU_IDLE_RESUME_US);
    for (unsigned i = 0; i < 100000; ++i) {
        const uint64_t now = p->ops.now(p->context);
        if (now - budget_start > budget) break;
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
int npu_idle_init(struct npu_idle *p, const struct npu_idle_ops *ops, void *context,
                  uint64_t frequency, unsigned enabled)
{
    if (!p || p->initialized || !ops || !ops->read || !ops->write || !ops->now ||
        !ops->health || !context || !frequency || frequency > UINT64_C(1000000000) || enabled > 1) return 0;
    p->ops = *ops; p->context = context; p->frequency = frequency;
    p->enabled = enabled; p->phase = NPU_IDLE_ON; p->initialized = 1;
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
{ return p->idle_since + us_ticks(p, NPU_IDLE_DELAY_US); }
int npu_idle_eligible(const struct npu_idle *p, uint64_t now)
{
    return p && p->initialized && p->phase == NPU_IDLE_ON &&
        now - p->idle_since >= us_ticks(p, NPU_IDLE_DELAY_US);
}
int npu_idle_gate(struct npu_idle *p)
{
    if (!p || !p->initialized || p->phase == NPU_IDLE_RETAINED) return -1;
    if (!npu_idle_eligible(p, p->ops.now(p->context))) return 0;
    if (!health(p)) return -1;
    if (!p->enabled) {
        ++p->counters.controls; return 0;
    }
    uint32_t value, shader;
    if (!read_reg(p, 0, &value) || !read_reg(p, 0x10c, &shader)) return -1;
    if (value & ((1u << 9) | 1u)) {
        npu_idle_retain(p, NPU_IDLE_GATE_VALUE, 0, value, value & ~((1u << 9) | 1u)); return -1;
    }
    if ((value & 0x1fcu) != 0x100u) {
        npu_idle_retain(p, NPU_IDLE_GATE_VALUE, 0, value, (value & ~0x1fcu) | 0x100u); return -1;
    }
    if (shader & 1u) { npu_idle_retain(p, NPU_IDLE_SHADER_VALUE, 0x10c, shader, shader & ~1u); return -1; }
    if ((shader & 0xfeu) != 0x80u) {
        npu_idle_retain(p, NPU_IDLE_SHADER_VALUE, 0x10c, shader, (shader & ~0xfeu) | 0x80u); return -1;
    }
    if (!matches(p, 4, UINT32_C(0x7fffffff), NPU_IDLE_FULL_IDLE)) return -1;
    p->gate_value = value; p->shader_value = shader;
    p->phase = NPU_IDLE_SUSPENDING;
    const uint64_t began = p->ops.now(p->context);
    if (!write_reg(p, 0, value | (1u << 9) | 1u) ||
        !write_reg(p, 0, value | 1u)) return -1;
    const uint64_t completed = p->ops.now(p->context);
    if (!settle(p, completed, began)) return -1;
    p->phase = NPU_IDLE_SUSPENDED; p->gated_since = p->ops.now(p->context);
    ++p->counters.gates; return 1;
}
int npu_idle_resume(struct npu_idle *p)
{
    if (!p || !p->initialized || p->phase == NPU_IDLE_RETAINED) return -1;
    if (p->phase == NPU_IDLE_ON) return 1;
    if (p->phase != NPU_IDLE_SUSPENDED) return 0;
    const uint64_t began = p->ops.now(p->context);
    p->phase = NPU_IDLE_RESUMING;
    if (!health(p)) return -1;
    if (p->enabled) {
        /* +0 is the vendor's only read while the clocks remain suspended. */
        if (!matches(p, 0, p->gate_value | 1u, NPU_IDLE_GATE_VALUE) ||
            !write_reg(p, 0, p->gate_value | (1u << 9)) ||
            !write_reg(p, 0, p->gate_value)) return -1;
        if (!settle(p, p->ops.now(p->context), began)) return -1;
        if (!matches(p, 0, p->gate_value, NPU_IDLE_GATE_VALUE)) return -1;
        uint32_t shader;
        if (!read_reg(p, 0x10c, &shader)) return -1;
        if (shader != p->shader_value) {
            npu_idle_retain(p, NPU_IDLE_SHADER_VALUE, 0x10c, shader, p->shader_value); return -1;
        }
        if (!write_reg(p, 0x10c, shader | 1u) || !write_reg(p, 0x10c, shader)) return -1;
        if (!matches(p, 0, p->gate_value, NPU_IDLE_GATE_VALUE) ||
            !matches(p, 0x10c, shader, NPU_IDLE_SHADER_VALUE) ||
            !matches(p, 4, UINT32_C(0x7fffffff), NPU_IDLE_FULL_IDLE)) return -1;
    }
    const uint64_t now = p->ops.now(p->context), elapsed = now - began;
    if (elapsed > us_ticks(p, NPU_IDLE_RESUME_US)) {
        npu_idle_retain(p, NPU_IDLE_SETTLE, 0, (uint32_t)elapsed,
            (uint32_t)us_ticks(p, NPU_IDLE_RESUME_US)); return -1;
    }
    p->counters.gated_ticks += began - p->gated_since;
    p->counters.resume_ticks += elapsed;
    if (elapsed > p->counters.resume_max_ticks) p->counters.resume_max_ticks = elapsed;
    ++p->counters.resumes; p->phase = NPU_IDLE_ON;
    npu_idle_completed(p, now); return 1;
}
