#include "a733_gpu_reserved.h"
#include "aarch64_device_memory.h"

extern unsigned char __device_pool_start[];

static int separate(uintptr_t a, size_t n, uintptr_t b, size_t m)
{ return a + n <= b || b + m <= a; }

/* Identity-only native image storage excludes aliases into either pool and
 * model/recording/result ranges. Translation verifies the complete input span. */
static int private_image(uintptr_t cpu, size_t bytes, int write)
{
    if (!bytes || cpu < UINT32_C(0x41000000) ||
        cpu >= (uintptr_t)__device_pool_start ||
        bytes > (uintptr_t)__device_pool_start - cpu) return 0;
    uintptr_t last = (cpu + bytes - 1) & ~(uintptr_t)4095;
    for (uintptr_t page = cpu & ~(uintptr_t)4095; ; page += 4096) {
        struct aarch64_translation t;
        if (!aarch64_device_translate(page, write, &t) || t.physical != page ||
            t.attributes != 0xff || t.shareability != 3) return 0;
        if (page == last) return 1;
    }
}

static struct pvr_gpu_service_config *failed(struct a733_gpu_reserved *s)
{
    if (s->wb_lease.generation) device_memory_retain(s->wb_pool, s->wb_lease);
    if (s->nc_lease.generation) device_memory_retain(s->nc_pool, s->nc_lease);
    s->memory.phase = A733_GPU_MEMORY_FAILED;
    return NULL;
}

static struct pvr_mips_buffer child(struct device_memory_extent parent,
                                    uint64_t *pages, size_t offset, size_t bytes)
{
    unsigned first = (unsigned)(offset / 4096), count = (unsigned)(bytes / 4096);
    for (unsigned i = 0; i < count; ++i)
        pages[first + i] = parent.physical + offset + i * 4096u;
    return (struct pvr_mips_buffer){{(void *)(parent.cpu + offset), bytes}, pages + first, count};
}

struct pvr_gpu_service_config *a733_gpu_reserved_prepare(struct a733_gpu_reserved *s,
    uint64_t owner, const void *firmware, size_t bytes, uint32_t gpu_clock_hz)
{
    if (!s || ((uintptr_t)s % _Alignof(struct a733_gpu_reserved)) || !owner ||
        !firmware || !gpu_clock_hz || bytes > PVR_FW_MAX_IMAGE ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !private_image((uintptr_t)s, sizeof(*s), 1) ||
        !private_image((uintptr_t)firmware, bytes, 0) ||
        !separate((uintptr_t)s, sizeof(*s), (uintptr_t)firmware, bytes) ||
        s->admitted || s->prepared || s->memory.phase != A733_GPU_MEMORY_NEW) return NULL;
    struct pvr_fw_plan plan;
    if (!pvr_fw_requirements(firmware, bytes, PVR_FW_A733_BVNC, &plan) ||
        plan.allocation_bytes[0] != 102400 || plan.allocation_bytes[1] != 20480) return NULL;
    struct device_memory_pool *wb = a733_memory_reservations_init();
    struct device_memory_pool *nc = a733_memory_uncached_init();
    if (!wb || !nc ||
        !separate((uintptr_t)s, sizeof(*s), (uintptr_t)wb, sizeof(*wb)) ||
        !separate((uintptr_t)s, sizeof(*s), (uintptr_t)nc, sizeof(*nc)) ||
        !separate((uintptr_t)firmware, bytes, (uintptr_t)wb, sizeof(*wb)) ||
        !separate((uintptr_t)firmware, bytes, (uintptr_t)nc, sizeof(*nc))) return NULL;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    if (!device_memory_acquire(wb, owner, A733_GPU_RESERVED_WB_BYTES, &lease, &extent)) return NULL;
    s->admitted = 1; s->wb_pool = wb; s->nc_pool = nc;
    s->wb_lease = lease; s->wb_extent = extent;
    device_memory_lookup(wb, lease)->state = DEVICE_MEMORY_DEVICE;
    if (!device_memory_acquire(nc, owner, A733_GPU_RESERVED_NC_BYTES, &lease, &extent)) return failed(s);
    s->nc_lease = lease; s->nc_extent = extent;
    device_memory_lookup(nc, lease)->state = DEVICE_MEMORY_DEVICE;
    struct pvr_gpu_service_config *c = &s->config;
    c->firmware = firmware; c->firmware_bytes = bytes;
    c->state = (struct pvr_fw_state_config){PVR_FW_A733_BVNC, gpu_clock_hz, 1, 16};
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        size_t n = i < 2 ? plan.allocation_bytes[i] : PVR_MIPS_TABLE_BYTES;
        c->firmware_buffers[i] = child(s->wb_extent, s->wb_pages, offset, n);
        offset += n;
    }
    if (offset != s->wb_extent.bytes) return failed(s);
    offset = 0;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        size_t n = pvr_fw_state_layout(i)->allocation_bytes;
        if (offset > s->nc_extent.bytes || n > s->nc_extent.bytes - offset) return failed(s);
        c->resources[i] = child(s->nc_extent, s->nc_pages, offset, n); offset += n;
    }
    if (offset != s->nc_extent.bytes ||
        !a733_gpu_memory_init(&s->memory, c->firmware_buffers, c->resources) ||
        !pvr_fw_state_prepare(c->resources, &c->state, s->mappings) ||
        !pvr_mips_prepare(firmware, bytes, c->firmware_buffers,
                          s->mappings, PVR_FW_RESOURCE_COUNT, &s->boot)) return failed(s);
    s->prepared = 1;
    return c;
}
