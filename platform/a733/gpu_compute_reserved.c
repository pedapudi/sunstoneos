#include "a733_gpu_compute_reserved.h"
#include "aarch64_device_memory.h"
#include <string.h>

extern unsigned char __device_pool_start[];
static struct a733_gpu_compute_reserved *compute_owner;
enum { NEW, ACQUIRED, VM_BUILT, PROGRAMS_BUILT, CONTEXT_BUILT, JOB_BUILT, BOUND, FAILED };
static const uint32_t context_bytes[6] = {232, 4, 32, 1024, 32, 64};
/* Fixed firmware PDS/USC bases and the general heap from the pinned native
 * heap profile. PDS/USC offsets are relative to globally programmed bases. */
static const uint64_t heap_base[3] = {UINT64_C(0xda00000000), UINT64_C(0xe000000000), UINT64_C(0x8000000000)};

static int apart(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{ return a + an <= b || b + bn <= a; }
static size_t rounded(size_t n) { return (n + 4095) & ~(size_t)4095; }
static int mapped(uintptr_t cpu, size_t bytes, unsigned attr, int write)
{
    if (!bytes || cpu > UINTPTR_MAX - bytes) return 0;
    uintptr_t last = (cpu + bytes - 1) & ~(uintptr_t)4095;
    for (uintptr_t page = cpu & ~(uintptr_t)4095; ; page += 4096) {
        struct aarch64_translation r, w;
        if (!aarch64_device_translate(page, 0, &r) || r.physical != page || r.attributes != attr ||
            (r.shareability != 3 && (attr != 0x44 || r.shareability != 2))) return 0;
        if (write && (!aarch64_device_translate(page, 1, &w) || w.physical != page ||
            w.attributes != attr || w.shareability != r.shareability)) return 0;
        if (page == last) return 1;
    }
}
static int private_image(uintptr_t cpu, size_t bytes, int write)
{
    return cpu >= UINT32_C(0x41000000) && cpu < (uintptr_t)__device_pool_start &&
        bytes <= (uintptr_t)__device_pool_start - cpu && mapped(cpu, bytes, 0xff, write);
}
static int lease(struct device_memory_pool *pool, struct device_memory_lease token,
                  struct device_memory_extent expected, uint64_t owner, unsigned attr)
{
    struct device_memory_extent actual; enum device_memory_state state;
    return token.owner == owner && device_memory_describe(pool, token, &actual, &state) &&
        state == DEVICE_MEMORY_DEVICE && actual.cpu == expected.cpu &&
        actual.physical == expected.physical && actual.bytes == expected.bytes &&
        actual.cpu == actual.physical && mapped(actual.cpu, actual.bytes, attr, 1);
}
static int descriptor(const struct pvr_mips_buffer *b, struct device_memory_extent parent,
                       size_t off, size_t bytes, const uint64_t *pages)
{
    if (off > parent.bytes || bytes > parent.bytes - off || (bytes & 4095) ||
        (uintptr_t)b->storage.cpu != parent.cpu + off || b->storage.bytes != bytes ||
        b->pages != pages || b->page_count != bytes / 4096) return 0;
    for (unsigned i = 0; i < b->page_count; ++i)
        if (pages[i] != parent.physical + off + i * 4096u) return 0;
    return 1;
}
static struct pvr_mips_buffer child(struct device_memory_extent parent, size_t off,
                                    size_t bytes, uint64_t *pages)
{
    for (unsigned i = 0; i < bytes / 4096; ++i) pages[i] = parent.physical + off + i * 4096u;
    return (struct pvr_mips_buffer){{(void *)(parent.cpu + off), bytes}, pages, (uint32_t)(bytes / 4096)};
}
static uint64_t address(unsigned program, unsigned i)
{
    if (program) return heap_base[i < 2 ? 1 : i < 9 ? 0 : 2] + 0x10000 + i * 0x50000u;
    return heap_base[i == 0 ? 1 : i < 3 ? 0 : 2] + 0x1000000 + i * 0x50000u;
}
static int firmware_owned(const struct a733_gpu_reserved *g)
{
    if (!g->prepared || !g->admitted ||
        (g->memory.phase != A733_GPU_MEMORY_OFFLINE && g->memory.phase != A733_GPU_MEMORY_PUBLISHED) ||
        g->wb_pool != a733_memory_reservations_init() || g->nc_pool != a733_memory_uncached_init() ||
        g->wb_extent.bytes != A733_GPU_RESERVED_WB_BYTES || g->nc_extent.bytes != A733_GPU_RESERVED_NC_BYTES ||
        !lease(g->wb_pool, g->wb_lease, g->wb_extent, g->wb_lease.owner, 0xff) ||
        !lease(g->nc_pool, g->nc_lease, g->nc_extent, g->nc_lease.owner, 0x44)) return 0;
    static const size_t sizes[] = {102400, 20480, 16384};
    size_t off = 0;
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        if (i == 3) { if (off != g->wb_extent.bytes) return 0; off = 0; }
        unsigned id = i < 3 ? i : i - 3;
        size_t n = i < 3 ? sizes[id] : pvr_fw_state_layout(id)->allocation_bytes;
        const struct pvr_mips_buffer *a = i < 3 ? &g->config.firmware_buffers[id] : &g->config.resources[id];
        const struct pvr_mips_buffer *b = i < 3 ? &g->memory.firmware[id] : &g->memory.resources[id];
        struct device_memory_extent parent = i < 3 ? g->wb_extent : g->nc_extent;
        const uint64_t *pages = (i < 3 ? g->wb_pages : g->nc_pages) + off / 4096;
        if (!descriptor(a, parent, off, n, pages) || !descriptor(b, parent, off, n, pages)) return 0;
        off += n;
    }
    if (off != g->nc_extent.bytes) return 0;
    const volatile unsigned char *system = g->config.resources[PVR_FW_SYSTEM_INIT].storage.cpu;
    for (unsigned heap = 0; heap < 2; ++heap) {
        uint64_t value = 0;
        for (unsigned byte = 0; byte < 8; ++byte)
            value |= (uint64_t)system[8 + heap * 8 + byte] << (byte * 8);
        if (value != heap_base[heap]) return 0;
    }
    return 1;
}
static int logical(const struct pvr_compute_program_span *l, const struct pvr_mips_buffer *b,
                    size_t bytes, uint64_t gpu)
{ return l->cpu == b->storage.cpu && l->bytes == bytes && l->gpu == gpu; }

/* The callback context is only the disjoint binding record, never an output
 * record. This proof covers native boot allocations and selected offline layout.
 * It does not grant live core mode or establish exclusion of external masters. */
static int admitted(struct a733_gpu_compute_binding *b)
{
    if (!b || b->self != (uintptr_t)b || !b->session || &b->session->binding != b) return 0;
    struct a733_gpu_compute_reserved *s = b->session;
    if (compute_owner != s || s->stage < VM_BUILT || s->stage >= FAILED || !firmware_owned(b->firmware) ||
        b->wb != b->firmware->wb_pool || b->nc != b->firmware->nc_pool ||
        !lease(b->wb, s->payload_lease, s->payload_extent, b->owner, 0xff) ||
        !lease(b->nc, s->firmware_lease, s->firmware_extent, b->owner, 0x44) ||
        !lease(b->wb, s->table_lease, s->table_extent, b->owner, 0xff) ||
        s->payload_extent.bytes != A733_GPU_COMPUTE_PAYLOAD_PAGES * 4096u ||
        s->firmware_extent.bytes != 6 * 4096u ||
        s->requirements.table_pages > PVR_COMPUTE_SERVICE_TABLE_PAGES ||
        s->table_extent.bytes != s->requirements.table_bytes ||
        s->vm.tables.cpu != (void *)s->table_extent.cpu || s->vm.tables.bytes != s->table_extent.bytes ||
        s->vm.tables.physical_pages != s->table_pages || s->vm.tables.page_count != s->requirements.table_pages ||
        s->vm.context_cpu != (void *)(s->firmware_extent.cpu + 4 * 4096u) ||
        s->vm.context_physical != s->firmware_extent.physical + 4 * 4096u ||
        (s->vm.phase != A733_GPU_VM_MEMORY_PREPARED && s->vm.phase != A733_GPU_VM_MEMORY_PUBLISHING &&
         s->vm.phase != A733_GPU_VM_MEMORY_PUBLISHED)) return 0;
    for (unsigned i = 0; i < s->requirements.table_pages; ++i)
        if (s->table_pages[i] != s->table_extent.physical + i * 4096u) return 0;
    if (s->stage >= JOB_BUILT &&
        (s->config.job != &s->job || s->config.image != &s->image ||
         s->config.vm != &s->vm || s->config.maps != s->maps ||
         s->config.map_count != A733_GPU_COMPUTE_PAYLOAD_PAGES)) return 0;
    if (s->stage == BOUND &&
        (b->firmware->config.compute != &s->compute || s->compute.config != &s->config ||
         s->compute.service != b->service || s->compute.phase == PVR_COMPUTE_SERVICE_FAILED)) return 0;
    struct pvr_compute_program_config *p = &s->programs;
    if (p->bvnc != PVR_FW_A733_BVNC || p->execution_cores != 1 ||
        p->admission_token != s->payload_lease.generation || p->admission_context != b ||
        p->excluded_count != 5 || p->pds.base != heap_base[0] || p->usc.base != heap_base[1] ||
        p->general.base != heap_base[2] || p->pds.bytes != (UINT64_C(1) << 32) ||
        p->usc.bytes != p->pds.bytes || p->general.bytes != UINT64_C(0x2000000000)) return 0;
    for (unsigned i = 0; i < 6; ++i) {
        const struct pvr_mips_buffer *f = &s->config.firmware[i];
        uint32_t va = 0xc0800000u + i * 0x10000u;
        if (!descriptor(f, s->firmware_extent, i * 4096u, 4096, s->firmware_pages + i) ||
            s->config.firmware_address[i] != va ||
            (i < 5 && !logical(&p->excluded[i], f, context_bytes[i], va))) return 0;
    }
    size_t off = 0; unsigned maps = 0;
    for (unsigned i = 0; i < PVR_PROGRAM_REGIONS + PVR_JOB_REGIONS; ++i) {
        int program = i < PVR_PROGRAM_REGIONS;
        unsigned j = program ? i : i - PVR_PROGRAM_REGIONS;
        if (!program && j == PVR_JOB_UFO) continue;
        const struct pvr_mips_buffer *d = program ? &s->config.programs[j] : &s->config.buffers[j];
        size_t n = program ? pvr_compute_program_bytes[j] : pvr_compute_job_bytes[j];
        uint64_t va = address(program, j);
        if (!descriptor(d, s->payload_extent, off, rounded(n), s->payload_pages + off / 4096) ||
            !logical(program ? &p->buffers[j] : &s->job.buffers[j], d, n, va)) return 0;
        unsigned flags = (program ? j < PVR_PROGRAM_SHARED_A : j != PVR_JOB_OUTPUT) ? PVR_GPU_VM_READ_ONLY : 0;
        for (unsigned k = 0; k < d->page_count; ++k) {
            unsigned found = 0;
            for (unsigned m = 0; m < A733_GPU_COMPUTE_PAYLOAD_PAGES; ++m) {
                const struct pvr_gpu_mapping *map = &s->maps[m];
                if (map->address == va + k * 4096u && map->physical == d->pages[k] &&
                    map->bytes == 4096 && map->page_shift == 12 && map->flags == flags) ++found;
            }
            if (found != 1) return 0;
            ++maps;
        }
        off += rounded(n);
    }
    struct pvr_gpu_vm_requirements requirements;
    if (off != s->payload_extent.bytes || maps != A733_GPU_COMPUTE_PAYLOAD_PAGES ||
        !pvr_gpu_vm_require(s->maps, maps, &requirements) ||
        memcmp(&requirements, &s->requirements, sizeof(requirements))) return 0;
    const struct pvr_mips_buffer *u = &s->config.buffers[PVR_JOB_UFO];
    return descriptor(u, s->firmware_extent, 5 * 4096u, 4096, s->firmware_pages + 5) &&
        logical(&s->job.buffers[PVR_JOB_UFO], u, 64, s->config.firmware_address[5]);
}
static int admit_program(void *ctx, const struct pvr_compute_program_config *p)
{
    struct a733_gpu_compute_binding *b = ctx;
    return admitted(b) && p == &b->session->programs && p->admit == admit_program;
}
static int admit_job(void *ctx, const struct pvr_compute_job_config *j)
{
    struct a733_gpu_compute_binding *b = ctx;
    if (!admitted(b)) return 0;
    struct a733_gpu_compute_reserved *s = b->session;
    return s->stage >= CONTEXT_BUILT && j == &s->job && j->admit == admit_job &&
        j->admission_context == b && j->programs == &s->programs && j->program_result == &s->program_result &&
        j->firmware_context == s->config.firmware_address[0] && j->firmware_ufo == s->config.firmware_address[5] &&
        j->seed == 17 && j->factor == 3 && j->job_id == 1 && s->programs.admit == admit_program &&
        s->compute.client.phase == PVR_CCCB_NEW && s->compute.job.phase == PVR_JOB_NEW;
}
static struct pvr_compute_service *failed(struct a733_gpu_compute_reserved *s)
{
    if (s->payload_lease.generation) device_memory_retain(s->binding.wb, s->payload_lease);
    if (s->firmware_lease.generation) device_memory_retain(s->binding.nc, s->firmware_lease);
    if (s->table_lease.generation) device_memory_retain(s->binding.wb, s->table_lease);
    s->stage = FAILED;
    pvr_compute_service_abort(&s->compute, PVR_COMPUTE_SERVICE_ADMISSION);
    return NULL;
}
static int acquire(struct device_memory_pool *pool, uint64_t owner, size_t bytes,
                    struct device_memory_lease *token, struct device_memory_extent *extent)
{
    if (!device_memory_acquire(pool, owner, bytes, token, extent)) return 0;
    device_memory_lookup(pool, *token)->state = DEVICE_MEMORY_DEVICE;
    memset((void *)extent->cpu, 0, extent->bytes);
    return 1;
}

struct pvr_compute_service *a733_gpu_compute_reserved_prepare(
    struct a733_gpu_compute_reserved *s, struct a733_gpu_reserved *g,
    struct pvr_gpu_service *service, uint32_t owner)
{
    if (compute_owner || !s || !g || !service || !owner || (uintptr_t)s % _Alignof(struct a733_gpu_compute_reserved) ||
        (uintptr_t)g % _Alignof(struct a733_gpu_reserved) || (uintptr_t)service % _Alignof(struct pvr_gpu_service) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !private_image((uintptr_t)s, sizeof(*s), 1) || !private_image((uintptr_t)g, sizeof(*g), 1) ||
        !private_image((uintptr_t)service, sizeof(*service), 1) ||
        !apart((uintptr_t)s, sizeof(*s), (uintptr_t)g, sizeof(*g)) ||
        !apart((uintptr_t)s, sizeof(*s), (uintptr_t)service, sizeof(*service)) ||
        !apart((uintptr_t)g, sizeof(*g), (uintptr_t)service, sizeof(*service)) ||
        s->stage || s->binding.self || service->initialized || g->config.compute ||
        g->config.free_lists.count || g->memory.phase != A733_GPU_MEMORY_OFFLINE ||
        !private_image((uintptr_t)g->config.firmware, g->config.firmware_bytes, 0) ||
        !apart((uintptr_t)s, sizeof(*s), (uintptr_t)g->config.firmware, g->config.firmware_bytes) ||
        !apart((uintptr_t)service, sizeof(*service), (uintptr_t)g->config.firmware, g->config.firmware_bytes) ||
        !firmware_owned(g)) return NULL;
    if (!apart((uintptr_t)s, sizeof(*s), (uintptr_t)g->wb_pool, sizeof(*g->wb_pool)) ||
        !apart((uintptr_t)s, sizeof(*s), (uintptr_t)g->nc_pool, sizeof(*g->nc_pool)) ||
        !apart((uintptr_t)service, sizeof(*service), (uintptr_t)g->wb_pool, sizeof(*g->wb_pool)) ||
        !apart((uintptr_t)service, sizeof(*service), (uintptr_t)g->nc_pool, sizeof(*g->nc_pool))) return NULL;
    struct device_memory_lease first; struct device_memory_extent extent;
    if (!device_memory_acquire(g->wb_pool, owner, A733_GPU_COMPUTE_PAYLOAD_PAGES * 4096u, &first, &extent)) return NULL;
    compute_owner = s;
    s->binding = (struct a733_gpu_compute_binding){(uintptr_t)&s->binding, s, g, service, g->wb_pool, g->nc_pool, owner};
    s->payload_lease = first; s->payload_extent = extent; s->stage = ACQUIRED;
    device_memory_lookup(g->wb_pool, first)->state = DEVICE_MEMORY_DEVICE;
    memset((void *)extent.cpu, 0, extent.bytes);
    if (!acquire(g->nc_pool, owner, 6 * 4096u, &s->firmware_lease, &s->firmware_extent)) return failed(s);
    struct pvr_compute_program_config *p = &s->programs;
    *p = (struct pvr_compute_program_config){.bvnc = PVR_FW_A733_BVNC,
        .pds = {heap_base[0], UINT64_C(1) << 32}, .usc = {heap_base[1], UINT64_C(1) << 32},
        .general = {heap_base[2], UINT64_C(0x2000000000)}, .excluded_count = 5, .execution_cores = 1,
        .admission_token = first.generation, .admission_context = &s->binding, .admit = admit_program};
    struct pvr_compute_buffer contexts[5];
    for (unsigned i = 0; i < 6; ++i) {
        struct pvr_mips_buffer *f = &s->config.firmware[i];
        *f = child(s->firmware_extent, i * 4096u, 4096, s->firmware_pages + i);
        uint32_t va = s->config.firmware_address[i] = 0xc0800000u + i * 0x10000u;
        if (i < 5) {
            p->excluded[i] = (struct pvr_compute_program_span){f->storage.cpu, context_bytes[i], va};
            contexts[i] = (struct pvr_compute_buffer){f->storage.cpu, context_bytes[i], va};
        }
    }
    size_t off = 0; unsigned count = 0;
    for (unsigned i = 0; i < PVR_PROGRAM_REGIONS + PVR_JOB_REGIONS; ++i) {
        int program = i < PVR_PROGRAM_REGIONS; unsigned j = program ? i : i - PVR_PROGRAM_REGIONS;
        if (!program && j == PVR_JOB_UFO) continue;
        size_t n = program ? pvr_compute_program_bytes[j] : pvr_compute_job_bytes[j];
        if (off > extent.bytes || rounded(n) > extent.bytes - off) return failed(s);
        struct pvr_mips_buffer *d = program ? &s->config.programs[j] : &s->config.buffers[j];
        *d = child(extent, off, rounded(n), s->payload_pages + off / 4096);
        uint64_t va = address(program, j);
        struct pvr_compute_program_span l = {d->storage.cpu, n, va};
        if (program) p->buffers[j] = l; else s->job.buffers[j] = l;
        unsigned flags = (program ? j < PVR_PROGRAM_SHARED_A : j != PVR_JOB_OUTPUT) ? PVR_GPU_VM_READ_ONLY : 0;
        for (unsigned k = 0; k < d->page_count; ++k) {
            if (count == A733_GPU_COMPUTE_PAYLOAD_PAGES) return failed(s);
            s->maps[count++] = (struct pvr_gpu_mapping){va + k * 4096u, d->pages[k], 4096, 12, flags};
        }
        off += rounded(n);
    }
    if (off != extent.bytes || count != A733_GPU_COMPUTE_PAYLOAD_PAGES) return failed(s);
    s->config.buffers[PVR_JOB_UFO] = s->config.firmware[5];
    s->job.buffers[PVR_JOB_UFO] = (struct pvr_compute_program_span){s->config.firmware[5].storage.cpu, 64, s->config.firmware_address[5]};
    for (unsigned i = 1; i < count; ++i) {
        struct pvr_gpu_mapping m = s->maps[i]; unsigned j = i;
        while (j && s->maps[j - 1].address > m.address) { s->maps[j] = s->maps[j - 1]; --j; }
        s->maps[j] = m;
    }
    if (!pvr_gpu_vm_require(s->maps, count, &s->requirements) ||
        !s->requirements.table_pages || s->requirements.table_pages > PVR_COMPUTE_SERVICE_TABLE_PAGES ||
        !acquire(g->wb_pool, owner, s->requirements.table_bytes, &s->table_lease, &s->table_extent)) return failed(s);
    for (unsigned i = 0; i < s->requirements.table_pages; ++i) s->table_pages[i] = s->table_extent.physical + i * 4096u;
    struct pvr_gpu_vm_tables tables = {(void *)s->table_extent.cpu, s->table_extent.bytes, s->table_pages, s->requirements.table_pages};
    if (!a733_gpu_vm_memory_init(&s->vm, &tables, contexts[4].cpu, s->firmware_pages[4]) ||
        a733_gpu_vm_memory_build(&s->vm, s->maps, count) != A733_GPU_VM_MEMORY_OK) return failed(s);
    s->stage = VM_BUILT;
    if (!pvr_compute_programs_build(p, &s->program_result)) return failed(s);
    s->stage = PROGRAMS_BUILT;
    struct pvr_compute_config context = {.bvnc = PVR_FW_A733_BVNC, .priority = 1, .context_id = 1, .owner_id = owner};
    memcpy(context.switch_registers, s->program_result.switch_registers, sizeof(context.switch_registers));
    if (!pvr_compute_context_build(&context, contexts)) return failed(s);
    s->stage = CONTEXT_BUILT;
    s->job.programs = p; s->job.program_result = &s->program_result;
    s->job.firmware_context = s->config.firmware_address[0]; s->job.firmware_ufo = s->config.firmware_address[5];
    s->job.seed = 17; s->job.factor = 3; s->job.job_id = 1;
    s->job.admission_context = &s->binding; s->job.admit = admit_job;
    if (!pvr_compute_job_build(&s->job, &s->image)) return failed(s);
    s->config.job = &s->job; s->config.image = &s->image; s->config.vm = &s->vm;
    s->config.maps = s->maps; s->config.map_count = count;
    s->stage = JOB_BUILT;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT + PVR_COMPUTE_SERVICE_OBJECTS; ++i) {
        const struct pvr_mips_buffer *buffer = i < PVR_FW_RESOURCE_COUNT ?
            &g->config.resources[i] : &s->config.firmware[i - PVR_FW_RESOURCE_COUNT];
        uint32_t va = i < PVR_FW_RESOURCE_COUNT ? pvr_fw_state_layout(i)->address :
            s->config.firmware_address[i - PVR_FW_RESOURCE_COUNT];
        s->mappings[i] = (struct pvr_mips_object){buffer->pages, buffer->page_count, va, PVR_MIPS_UNCACHED};
    }
    if (!pvr_mips_prepare(g->config.firmware, g->config.firmware_bytes,
        g->config.firmware_buffers, s->mappings,
        PVR_FW_RESOURCE_COUNT + PVR_COMPUTE_SERVICE_OBJECTS, &s->boot)) return failed(s);
    if (!pvr_compute_service_bind(&s->compute, service, &g->config, &s->config)) return failed(s);
    s->stage = BOUND;
    return &s->compute;
}
