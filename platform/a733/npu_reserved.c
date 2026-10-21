#include "a733_npu_reserved.h"

#if defined(SUNSTONE_NPU_LARGE_GRAPH)
_Static_assert(NPU_CONVOLUTION_BUFFERS == 8, "large graph layout");
#else
_Static_assert(NPU_CONVOLUTION_BUFFERS == 6, "reserved convolution layout");
#endif

static int disjoint(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{
    return a && b && an && bn && a <= UINTPTR_MAX - an && b <= UINTPTR_MAX - bn &&
        !(a < b + bn && b < a + an);
}

struct npu_convolution *a733_npu_reserved_prepare(struct a733_npu_reserved *s,
    uint64_t owner, const void *model, size_t model_bytes, uintptr_t registers,
    void (*fatal)(void *), void *cookie)
{
    if (!s || s->admitted || s->convolution.prepared || atomic_load(&s->convolution.failed) ||
        !owner || !model || (!model_bytes || model_bytes > NPU_CONVOLUTION_MODEL_CAPACITY) || !fatal ||
        !registers || (registers & 4095) || registers > UINTPTR_MAX - 4096 ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !disjoint((uintptr_t)s, sizeof(*s), (uintptr_t)model, model_bytes) ||
        !disjoint((uintptr_t)s, sizeof(*s), registers, 4096) ||
        !disjoint((uintptr_t)model, model_bytes, registers, 4096)) return NULL;
    struct device_memory_pool *pool = a733_memory_reservations_init();
    if (!pool ||
        !disjoint((uintptr_t)s, sizeof(*s), pool->backing.cpu, pool->backing.bytes) ||
        !disjoint((uintptr_t)model, model_bytes, pool->backing.cpu, pool->backing.bytes) ||
        !disjoint((uintptr_t)s, sizeof(*s), (uintptr_t)pool, sizeof(*pool)) ||
        !disjoint((uintptr_t)model, model_bytes, (uintptr_t)pool, sizeof(*pool)) ||
        !disjoint(registers, 4096, pool->backing.cpu, pool->backing.bytes)) return NULL;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    if (!device_memory_acquire(pool, owner, A733_NPU_RESERVED_BYTES, &lease, &extent)) return NULL;
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    if(extent.cpu!=UINT32_C(0x41600000)||extent.physical!=UINT32_C(0x41600000)||
       extent.bytes!=A733_NPU_RESERVED_BYTES){device_memory_retain(pool,lease);return NULL;}
#endif
    s->pool = pool; s->lease = lease; s->extent = extent; s->admitted = 1;
    /* The service owns this entire allocation from offline preparation through
     * its final lifetime. CPU access follows the convolution's private protocol. */
    device_memory_lookup(pool, lease)->state = DEVICE_MEMORY_DEVICE;
    struct npu_convolution_config config = {.model = model, .model_bytes = model_bytes,
        .register_cpu = registers, .register_physical = 0x03600000,
        .fatal = fatal, .fatal_cookie = cookie};
    for (unsigned i = 0; i < NPU_CONVOLUTION_BUFFERS; ++i) {
        const struct npu_convolution_slot *l = &npu_convolution_layout[i];
        if (!l->bytes) continue;
        config.buffers[i] = (struct a733_npu_buffer){extent.cpu + l->offset,
            extent.physical + l->offset, l->bytes, l->device, l->role};
    }
    size_t offset = NPU_CONVOLUTION_ARENA_OFFSET;
    config.arena = (void *)(extent.cpu + offset);
    config.arena_physical = extent.physical + offset;
    config.arena_bytes = A733_NPU_RESERVED_TABLE_BYTES;
    if (offset + config.arena_bytes != extent.bytes ||
        !npu_convolution_prepare(&s->convolution, &config)) {
        device_memory_retain(pool, lease);
        atomic_store(&s->convolution.failed, 1);
        return NULL;
    }
    return &s->convolution;
}
