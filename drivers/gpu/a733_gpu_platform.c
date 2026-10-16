#include "a733_gpu_platform.h"
#include <stddef.h>

static int fail(struct a733_gpu_platform *p)
{
    if (p->power.phase == A733_GPU_POWER_STARTING || p->power.phase == A733_GPU_POWER_STOPPING)
        (void)a733_gpu_power_cancel(&p->power);
    p->phase = A733_GPU_PLATFORM_FAILED;
    return -1;
}
static int clock_ok(struct a733_gpu_platform *p)
{
    uint64_t now;
    if (!p->ops.now(p->context, &now) || now < p->last_now || now < p->power.last_now || now >= p->deadline)
        return fail(p), 0;
    p->last_now = now;
    return 1;
}
static int clock_read(void *context, uint64_t *out)
{ struct a733_gpu_platform *p = context; return p->ops.now(p->context, out); }
static int acquire(void *context, struct a733_gpu_power_resources *out)
{
    struct a733_gpu_platform *p = context;
    if (!p->token || p->delegated || p->phase != A733_GPU_PLATFORM_STARTING) return 0;
    *out = p->ready; p->delegated = 1;
    return 1;
}
static int release(void *context)
{
    struct a733_gpu_platform *p = context;
    if (!p->token || !p->delegated || p->phase != A733_GPU_PLATFORM_STOPPING) return 0;
    /* The parent platform retains the actual lease while the controller returns
     * its delegated handle. Actual manager release is a separate polling step. */
    p->delegated = 0;
    return 1;
}
static int read_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off, uint32_t *out)
{
    struct a733_gpu_platform *p = context;
    return p->token && p->delegated && p->ops.read(p->context, bank, off, out);
}
static int write_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off, uint32_t value)
{
    struct a733_gpu_platform *p = context;
    return p->token && p->delegated && p->ops.write(p->context, bank, off, value, p->deadline);
}
static int update_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off,
                       uint32_t clear, uint32_t set)
{
    struct a733_gpu_platform *p = context;
    return p->token && p->delegated &&
        p->ops.update(p->context, bank, off, clear, set, p->deadline);
}
static int disjoint(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= UINTPTR_MAX - as && y <= UINTPTR_MAX - bs && (x + as <= y || y + bs <= x);
}
int a733_gpu_platform_init(struct a733_gpu_platform *p, struct a733_gpu_resource_manager *m,
    unsigned client, const struct a733_gpu_platform_ops *ops, void *context)
{
    if (!p || !m || !ops || p->initialized || !m->initialized || client >= A733_GPU_RESOURCE_CLIENTS ||
        !ops->now || !ops->read || !ops->write || !ops->update ||
        !disjoint(p, sizeof(*p), m, sizeof(*m)) || !disjoint(p, sizeof(*p), ops, sizeof(*ops))) return 0;
    *p = (struct a733_gpu_platform){.manager = m, .ops = *ops, .context = context,
                                   .client = client, .initialized = 1};
    const struct a733_gpu_power_ops power_ops = {clock_read, acquire, release, read_reg, write_reg, update_reg};
    return a733_gpu_power_init(&p->power, &power_ops, p);
}
int a733_gpu_platform_begin(void *context, uint64_t deadline)
{
    struct a733_gpu_platform *p = context;
    if (!p || !p->initialized || (p->phase != A733_GPU_PLATFORM_NEW && p->phase != A733_GPU_PLATFORM_OFF))
        return 0;
    p->deadline = deadline;
    if (!clock_ok(p)) return 0;
    p->phase = A733_GPU_PLATFORM_ADMIT;
    return 1;
}
int a733_gpu_platform_poll(void *context)
{
    struct a733_gpu_platform *p = context;
    if (!p || !p->initialized || p->phase == A733_GPU_PLATFORM_FAILED) return -1;
    if (p->phase == A733_GPU_PLATFORM_ON) return 1;
    if (p->phase != A733_GPU_PLATFORM_ADMIT && p->phase != A733_GPU_PLATFORM_STARTING) return -1;
    if (!clock_ok(p)) return -1;
    if (p->phase == A733_GPU_PLATFORM_ADMIT) {
        p->resource_result = a733_gpu_resources_acquire(p->manager, p->client, A733_GPU_RESOURCE_ALL,
                                                       p->deadline, &p->token, &p->ready);
        if (!clock_ok(p)) return -1;
        if (p->resource_result == A733_GPU_RESOURCE_BUSY) return 0;
        if (p->resource_result != A733_GPU_RESOURCE_OK) return fail(p);
        p->phase = A733_GPU_PLATFORM_STARTING;
        if (a733_gpu_power_on(&p->power, p->deadline) != A733_GPU_POWER_AGAIN) return fail(p);
        return 0;
    }
    enum a733_gpu_power_result result = a733_gpu_power_poll(&p->power);
    if (!clock_ok(p)) return -1;
    if (result == A733_GPU_POWER_AGAIN) return 0;
    if (result != A733_GPU_POWER_OK) return fail(p);
    p->phase = A733_GPU_PLATFORM_ON;
    return 1;
}
int a733_gpu_platform_off_begin(struct a733_gpu_platform *p, uint64_t deadline)
{
    if (!p || !p->initialized || p->phase != A733_GPU_PLATFORM_ON) return 0;
    p->deadline = deadline;
    if (!clock_ok(p)) return 0;
    p->phase = A733_GPU_PLATFORM_STOPPING;
    if (a733_gpu_power_off(&p->power, deadline) != A733_GPU_POWER_AGAIN) { fail(p); return 0; }
    return 1;
}
int a733_gpu_platform_off_poll(struct a733_gpu_platform *p)
{
    if (!p || !p->initialized || p->phase == A733_GPU_PLATFORM_FAILED) return -1;
    if (p->phase == A733_GPU_PLATFORM_OFF) return 1;
    if (p->phase != A733_GPU_PLATFORM_STOPPING && p->phase != A733_GPU_PLATFORM_RELEASE) return -1;
    if (!clock_ok(p)) return -1;
    if (p->phase == A733_GPU_PLATFORM_STOPPING) {
        enum a733_gpu_power_result result = a733_gpu_power_poll(&p->power);
        if (!clock_ok(p)) return -1;
        if (result == A733_GPU_POWER_AGAIN) return 0;
        if (result != A733_GPU_POWER_OK) return fail(p);
        p->phase = A733_GPU_PLATFORM_RELEASE;
        return 0;
    }
    p->resource_result = a733_gpu_resources_release(p->manager, p->client, p->token, p->deadline);
    /* Record completed release even if the following clock read fails. */
    if (p->resource_result == A733_GPU_RESOURCE_OK) p->token = 0;
    if (!clock_ok(p)) return -1;
    if (p->resource_result == A733_GPU_RESOURCE_BUSY) return 0;
    if (p->resource_result != A733_GPU_RESOURCE_OK) return fail(p);
    p->phase = A733_GPU_PLATFORM_OFF;
    return 1;
}
