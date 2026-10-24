/* Native construction of the mimus GPU session (docs/gpu-mimus-service.md).
 * It follows a733_gpu_compute_reserved: the same firmware heap profile, the
 * same context-switch programs and compute context, and the firmware objects
 * at the same firmware addresses; an image holds either one or the other. */
#include "a733_gpu_mimus.h"
#include "a733_range.h"
#include "a733_gpu_mapping.h"
#include "pvr_compute_context.h"
#include <string.h>

extern unsigned char __device_pool_start[];
enum { NEW, ACQUIRED, BUILT, BOUND, FAILED };
#define PAGE 4096u
#define BLOCK (UINT64_C(2) << 20)
static const uint32_t context_bytes[PVR_MIMUS_SERVICE_OBJECTS] = {232, 4, 32, PVR_MIMUS_SERVICE_RING_BYTES, 32, 64};
/* Firmware PDS and USC heap bases, programmed by the firmware's system
 * initialization, and the general heap of the pinned native profile. */
static const uint64_t heap_base[3] = {UINT64_C(0xda00000000), UINT64_C(0xe000000000), UINT64_C(0x8000000000)};
/* The job page and the program pages share the first 2 MiB region of their
 * heaps with the context-switch programs (PDS programs from 0xb0000, the USC
 * programs at 0x10000 and 0x60000), so they need no leaf table of their own;
 * each 2 MiB region the session maps costs one table page. */
#define JOB_GPU (UINT64_C(0xda00000000) + 0x1e0000u)
#define PROGRAM_GPU (UINT64_C(0xe000000000) + 0x100000u)

static size_t rounded(size_t n) { return (n + PAGE - 1) & ~(size_t)(PAGE - 1); }
static uint64_t program_address(unsigned i)
{ return heap_base[i < 2 ? 1 : i < PVR_PROGRAM_CDM_RESUME ? 0 : 2] + 0x10000 + i * 0x50000u; }
static int private_image(uintptr_t cpu, size_t bytes)
{
    return cpu >= UINT32_C(0x41000000) && cpu < (uintptr_t)__device_pool_start &&
        bytes <= (uintptr_t)__device_pool_start - cpu && a733_gpu_identity_mapped(cpu, bytes, 0xff, 1);
}
static struct pvr_mips_buffer child(struct device_memory_extent parent, size_t off, size_t bytes, uint64_t *pages)
{
    for (unsigned i = 0; i < bytes / PAGE; ++i) pages[i] = parent.physical + off + i * PAGE;
    return (struct pvr_mips_buffer){{(void *)(parent.cpu + off), bytes}, pages, (uint32_t)(bytes / PAGE)};
}
static int acquire(struct device_memory_pool *pool, uint64_t owner, size_t bytes,
                   struct device_memory_lease *token, struct device_memory_extent *extent)
{
    if (!device_memory_acquire(pool, owner, bytes, token, extent)) return 0;
    device_memory_lookup(pool, *token)->state = DEVICE_MEMORY_DEVICE;
    memset((void *)extent->cpu, 0, extent->bytes);
    return extent->cpu == extent->physical;
}
static struct pvr_mimus_service *failed(struct a733_gpu_mimus *s, struct a733_gpu_reserved *g)
{
    if (s->payload_lease.generation) device_memory_retain(g->wb_pool, s->payload_lease);
    if (s->table_lease.generation) device_memory_retain(g->wb_pool, s->table_lease);
    if (s->firmware_lease.generation) device_memory_retain(g->nc_pool, s->firmware_lease);
    s->stage = FAILED;
    return NULL;
}
/* The context-switch programs prove nothing beyond this construction. */
static int admit_programs(void *ctx, const struct pvr_compute_program_config *p)
{
    struct a733_gpu_mimus *s = ctx;
    return s && p == &s->programs && s->stage == ACQUIRED;
}

struct pvr_mimus_service *a733_gpu_mimus_prepare(struct a733_gpu_mimus *s, struct a733_gpu_reserved *g,
    struct pvr_gpu_service *service, uint32_t owner, const struct a733_gpu_mimus_layout *l)
{
    if (!s || !g || !service || !l || !owner || s->stage ||
        (uintptr_t)s % _Alignof(struct a733_gpu_mimus) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !private_image((uintptr_t)s, sizeof(*s)) || !private_image((uintptr_t)service, sizeof(*service)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)g, sizeof(*g)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)service, sizeof(*service)) ||
        !g->prepared || g->memory.phase != A733_GPU_MEMORY_OFFLINE || g->config.compute || g->config.mimus ||
        g->config.graphics || g->config.free_lists.count || service->initialized ||
        l->arena != PVR_MIMUS_ARENA_GPU || l->output != PVR_MIMUS_OUTPUT_GPU || !l->tensors || !l->tensor_count)
        return NULL;
    for (unsigned i = 0; i < PVR_MIMUS_MODELS; ++i)
        if (!l->models[i] || (l->models[i] & (BLOCK - 1)) || l->models[i] >= (UINT64_C(1) << 36)) return NULL;
    if (!acquire(g->wb_pool, owner, A733_GPU_MIMUS_PAYLOAD_PAGES * PAGE, &s->payload_lease, &s->payload_extent))
        return failed(s, g);
    s->stage = ACQUIRED;
    if (!acquire(g->nc_pool, owner, PVR_MIMUS_SERVICE_OBJECTS * PAGE, &s->firmware_lease, &s->firmware_extent))
        return failed(s, g);
    struct pvr_mimus_service_config *c = &s->config;
    struct pvr_compute_program_config *p = &s->programs;
    *p = (struct pvr_compute_program_config){.bvnc = PVR_FW_A733_BVNC,
        .pds = {heap_base[0], UINT64_C(1) << 32}, .usc = {heap_base[1], UINT64_C(1) << 32},
        .general = {heap_base[2], UINT64_C(0x2000000000)}, .excluded_count = 7, .execution_cores = 1,
        .admission_token = s->payload_lease.generation, .admission_context = s, .admit = admit_programs};
    struct pvr_compute_buffer contexts[5];
    for (unsigned i = 0; i < PVR_MIMUS_SERVICE_OBJECTS; ++i) {
        c->firmware[i] = child(s->firmware_extent, i * PAGE, PAGE, s->firmware_pages + i);
        c->firmware_address[i] = 0xc0800000u + i * 0x10000u;
        if (i < 5) {
            p->excluded[i] = (struct pvr_compute_program_span){c->firmware[i].storage.cpu, context_bytes[i],
                                                               c->firmware_address[i]};
            contexts[i] = (struct pvr_compute_buffer){c->firmware[i].storage.cpu, context_bytes[i],
                                                      c->firmware_address[i]};
        }
    }
    /* Payload: the job page, the program pages, then the programs. */
    unsigned char *payload = (unsigned char *)s->payload_extent.cpu;
    p->excluded[5] = (struct pvr_compute_program_span){payload, PVR_MIMUS_JOB_PAGE_BYTES, JOB_GPU};
    p->excluded[6] = (struct pvr_compute_program_span){payload + PAGE, PVR_MIMUS_PROGRAM_BYTES, PROGRAM_GPU};
    unsigned count = 0;
    const uint64_t models_gpu[PVR_MIMUS_MODELS] = {PVR_MIMUS_MODEL_GPU, PVR_MIMUS_MODEL_GPU + PVR_MIMUS_MODEL_BYTES};
    for (unsigned i = 0; i < PVR_MIMUS_MODELS; ++i)
        s->maps[count++] = (struct pvr_gpu_mapping){models_gpu[i], l->models[i], PVR_MIMUS_MODEL_BYTES, 21,
                                                    PVR_GPU_VM_READ_ONLY};
    s->maps[count++] = (struct pvr_gpu_mapping){PVR_MIMUS_ARENA_GPU, l->arena, PVR_MIMUS_ARENA_BYTES, 21,
                                                PVR_GPU_VM_READ_ONLY};
    s->maps[count++] = (struct pvr_gpu_mapping){PVR_MIMUS_OUTPUT_GPU, l->output, PVR_MIMUS_OUTPUT_BYTES, 21, 0};
    for (unsigned i = 0; i < 3; ++i)
        s->maps[count++] = (struct pvr_gpu_mapping){(i ? PROGRAM_GPU + (i - 1) * PAGE : JOB_GPU),
            s->payload_extent.physical + i * PAGE, PAGE, 12, PVR_GPU_VM_READ_ONLY};
    size_t off = 3 * PAGE;
    for (unsigned i = 0; i < PVR_PROGRAM_REGIONS; ++i) {
        size_t n = pvr_compute_program_bytes[i];
        if (off + rounded(n) > s->payload_extent.bytes) return failed(s, g);
        c->publish[i] = child(s->payload_extent, off, rounded(n), s->payload_pages + off / PAGE);
        p->buffers[i] = (struct pvr_compute_program_span){c->publish[i].storage.cpu, n, program_address(i)};
        unsigned flags = i < PVR_PROGRAM_SHARED_A ? PVR_GPU_VM_READ_ONLY : 0;
        for (unsigned k = 0; k < c->publish[i].page_count; ++k)
            s->maps[count++] = (struct pvr_gpu_mapping){program_address(i) + k * PAGE, c->publish[i].pages[k],
                                                        PAGE, 12, flags};
        off += rounded(n);
    }
    if (off != s->payload_extent.bytes || count != A733_GPU_MIMUS_MAPS) return failed(s, g);
    for (unsigned i = 1; i < count; ++i) {
        struct pvr_gpu_mapping m = s->maps[i]; unsigned j = i;
        while (j && s->maps[j - 1].address > m.address) { s->maps[j] = s->maps[j - 1]; --j; }
        s->maps[j] = m;
    }
    if (!pvr_gpu_vm_require(s->maps, count, &s->requirements) || !s->requirements.table_pages ||
        s->requirements.table_pages > A733_GPU_MIMUS_TABLE_PAGES ||
        !acquire(g->wb_pool, owner, s->requirements.table_bytes, &s->table_lease, &s->table_extent))
        return failed(s, g);
    for (unsigned i = 0; i < s->requirements.table_pages; ++i) s->table_pages[i] = s->table_extent.physical + i * PAGE;
    const struct pvr_gpu_vm_tables tables = {(void *)s->table_extent.cpu, s->table_extent.bytes, s->table_pages,
                                             s->requirements.table_pages};
    if (!a733_gpu_vm_memory_init(&s->vm, &tables, contexts[4].cpu, s->firmware_pages[4]) ||
        a733_gpu_vm_memory_build(&s->vm, s->maps, count) != A733_GPU_VM_MEMORY_OK ||
        !pvr_compute_programs_build(p, &s->program_result)) return failed(s, g);
    struct pvr_compute_config context = {.bvnc = PVR_FW_A733_BVNC, .priority = 1, .context_id = 1, .owner_id = owner};
    memcpy(context.switch_registers, s->program_result.switch_registers, sizeof(context.switch_registers));
    if (!pvr_compute_context_build(&context, contexts)) return failed(s, g);
    s->stage = BUILT;
    c->session = (struct pvr_mimus_session_config){payload, payload + PAGE, (uintptr_t)l->arena,
        (uintptr_t)l->output, JOB_GPU, PROGRAM_GPU, heap_base[0], heap_base[1], 0};
    c->vm = &s->vm;
    c->tensors = l->tensors;
    c->tensor_count = l->tensor_count;
    c->resume = s->program_result.cdm_resume_address;
    c->max_job_us = l->max_job_us;
    if (!pvr_mimus_service_bind(&s->component, service, &g->config, c)) return failed(s, g);
    s->stage = BOUND;
    return &s->component;
}
