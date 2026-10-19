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
#if defined(SUNSTONE_NPU_NN_POOL_GRAPH)
    static const size_t bytes[]={24576,892928,524288,151552,4096,524288,1093632,0};
    static const uint32_t device[]={0xe0000000,0xe0007000,0xe01ee000,0xe026f000,0xe0295000,
        0xe0297000,0xe00e2000,0};
    static const size_t offsets[]={0,0x6000,0x1eb000,0x26b000,0x290000,0x291000,0xe0000,0};
    static const enum a733_npu_buffer_role role[]={A733_NPU_BUFFER_READ,A733_NPU_BUFFER_READ,
        A733_NPU_BUFFER_COMMAND,A733_NPU_BUFFER_READ,A733_NPU_BUFFER_WRITE,A733_NPU_BUFFER_BOOTSTRAP,
        A733_NPU_BUFFER_WRITE,A733_NPU_BUFFER_READ};
#elif defined(SUNSTONE_NPU_LARGE_GRAPH)
    static const size_t bytes[]={4096,0,524288,524288,266240,524288,1048576,4096};
    static const uint32_t device[]={0xe0000000,0,0xe0105000,0xe0186000,0xe0207000,
        0xe0249000,0xe0004000,0xe0002000};
    static const size_t offsets[]={0,0,0x102000,0x182000,0x202000,0x243000,0x2000,0x1000};
    static const enum a733_npu_buffer_role role[]={A733_NPU_BUFFER_READ,A733_NPU_BUFFER_READ,
        A733_NPU_BUFFER_COMMAND,A733_NPU_BUFFER_READ,A733_NPU_BUFFER_WRITE,A733_NPU_BUFFER_BOOTSTRAP,
        A733_NPU_BUFFER_WRITE,A733_NPU_BUFFER_READ};
#else
    static const size_t bytes[] = {4096, 4096, 524288, 4096, 4096, 524288};
    static const uint32_t device[] = {0xe0000000, 0xe0002000, 0xe0004000,
                                     0xe0085000, 0xe0087000, 0xe0089000};
    static const enum a733_npu_buffer_role role[] = {A733_NPU_BUFFER_READ, A733_NPU_BUFFER_READ,
        A733_NPU_BUFFER_COMMAND, A733_NPU_BUFFER_READ, A733_NPU_BUFFER_WRITE, A733_NPU_BUFFER_BOOTSTRAP};
#endif
    size_t offset = 0;
    for (unsigned i = 0; i < NPU_CONVOLUTION_BUFFERS; ++i) {
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
        if(!bytes[i])continue;
        offset=offsets[i];
#endif
        config.buffers[i] = (struct a733_npu_buffer){extent.cpu + offset,
            extent.physical + offset, bytes[i], device[i], role[i]};
        offset += bytes[i];
    }
#if defined(SUNSTONE_NPU_NN_POOL_GRAPH)
    offset=0x311000;
#elif defined(SUNSTONE_NPU_LARGE_GRAPH)
    offset=0x2c3000;
#endif
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
