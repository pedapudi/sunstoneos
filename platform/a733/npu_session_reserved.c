#include "a733_npu_session_reserved.h"
#include "a733_range.h"

struct npu_sessions *a733_npu_session_reserve(struct a733_npu_session_reserved *s, uint64_t owner,
    const struct npu_session_model *models, unsigned count, uintptr_t registers,
    void (*fatal)(void *), void *cookie)
{
    if (!s || s->admitted || s->sessions.prepared || atomic_load(&s->sessions.failed) || !owner ||
        !models || !count || count > NPU_SESSION_MODELS || !fatal || !registers ||
        (registers & 4095) || registers > UINTPTR_MAX - 4096 ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !a733_ranges_disjoint_nonempty(s, sizeof(*s), (const void *)registers, 4096)) return NULL;
    struct device_memory_pool *pool = a733_memory_reservations_init();
    if (!pool ||
        !a733_ranges_disjoint_nonempty(s, sizeof(*s),
            (const void *)pool->backing.cpu, pool->backing.bytes) ||
        !a733_ranges_disjoint_nonempty(s, sizeof(*s), pool, sizeof(*pool)) ||
        !a733_ranges_disjoint_nonempty((const void *)registers, 4096,
            (const void *)pool->backing.cpu, pool->backing.bytes)) return NULL;
    for (unsigned m = 0; m < count; ++m)
        if (!a733_ranges_disjoint_nonempty(models[m].data, models[m].bytes,
                (const void *)pool->backing.cpu, pool->backing.bytes) ||
            !a733_ranges_disjoint_nonempty(models[m].data, models[m].bytes, s, sizeof(*s))) return NULL;
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
