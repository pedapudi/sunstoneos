#include "a733_npu_session_reserved.h"

static int disjoint(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{
    return a && b && an && bn && a <= UINTPTR_MAX - an && b <= UINTPTR_MAX - bn &&
        !(a < b + bn && b < a + an);
}

struct npu_sessions *a733_npu_session_reserve(struct a733_npu_session_reserved *s, uint64_t owner,
    const struct npu_session_model *models, unsigned count, uintptr_t registers,
    void (*fatal)(void *), void *cookie)
{
    if (!s || s->admitted || s->sessions.prepared || atomic_load(&s->sessions.failed) || !owner ||
        !models || !count || count > NPU_SESSION_MODELS || !fatal || !registers ||
        (registers & 4095) || registers > UINTPTR_MAX - 4096 ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !disjoint((uintptr_t)s, sizeof(*s), registers, 4096)) return NULL;
    struct device_memory_pool *pool = a733_memory_reservations_init();
    if (!pool || !disjoint((uintptr_t)s, sizeof(*s), pool->backing.cpu, pool->backing.bytes) ||
        !disjoint((uintptr_t)s, sizeof(*s), (uintptr_t)pool, sizeof(*pool)) ||
        !disjoint(registers, 4096, pool->backing.cpu, pool->backing.bytes)) return NULL;
    for (unsigned m = 0; m < count; ++m)
        if (!disjoint((uintptr_t)models[m].data, models[m].bytes, pool->backing.cpu, pool->backing.bytes) ||
            !disjoint((uintptr_t)models[m].data, models[m].bytes, (uintptr_t)s, sizeof(*s))) return NULL;
    size_t bytes = npu_session_requirements(models, count);
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    if (!bytes || !device_memory_acquire(pool, owner, bytes, &lease, &extent)) return NULL;
    s->pool = pool; s->lease = lease; s->extent = extent; s->admitted = 1;
    /* The session owns this allocation from preparation through its lifetime. */
    device_memory_lookup(pool, lease)->state = DEVICE_MEMORY_DEVICE;
    struct npu_session_config config = {.count = count, .cpu = extent.cpu,
        .physical = extent.physical, .bytes = extent.bytes, .register_cpu = registers,
        .register_physical = 0x03600000, .fatal = fatal, .fatal_cookie = cookie};
    for (unsigned m = 0; m < count; ++m) config.models[m] = models[m];
    if (extent.bytes != bytes || !npu_session_prepare(&s->sessions, &config)) {
        device_memory_retain(pool, lease);
        atomic_store(&s->sessions.failed, 1);
        return NULL;
    }
    return &s->sessions;
}
