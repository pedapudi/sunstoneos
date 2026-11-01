/* Retained native compute queue backing and immutable GPU address space. */
#include "a733_gpu_queue.h"
#include "a733_range.h"
#include "a733_gpu_mapping.h"
#include "pvr_compute_context.h"
#include "a733_gpu_backend.h"
#include <string.h>

extern unsigned char __device_pool_start[], __device_pool_end[], __uncached_pool_start[], __uncached_pool_end[];
enum { NEW, ACQUIRED, BUILT, BOUND, FAILED, REBUILDING };
#define PAGE 4096u
#define BLOCK (UINT64_C(2) << 20)
static const uint32_t context_bytes[PVR_COMPUTE_QUEUE_OBJECTS] = {232, 4, 32, 1024u, 32, 64};
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
static struct pvr_compute_queue *failed(struct a733_gpu_queue *s, struct a733_gpu_reserved *g)
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
    struct a733_gpu_queue *s = ctx;
    return s && p == &s->programs && (s->stage == ACQUIRED || s->stage == REBUILDING);
}

struct pvr_compute_queue *a733_gpu_queue_prepare(struct a733_gpu_queue *s, struct a733_gpu_reserved *g,
    struct pvr_gpu_service *service, uint32_t owner, const struct ss_region *regions, uint32_t count)
{
    if (!s || !g || !service || !owner || s->stage || !regions || !count || count > SS_REGIONS ||
        (uintptr_t)s % _Alignof(struct a733_gpu_queue) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !private_image((uintptr_t)s, sizeof(*s)) || !private_image((uintptr_t)service, sizeof(*service)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)g, sizeof(*g)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)service, sizeof(*service)) ||
        !g->prepared || g->memory.phase != A733_GPU_MEMORY_OFFLINE || g->config.compute || g->config.mimus ||
        g->config.compute_queue || g->config.graphics || g->config.free_lists.count || service->initialized) return NULL;
    if (!acquire(g->wb_pool, owner, A733_GPU_QUEUE_PAYLOAD_PAGES * PAGE, &s->payload_lease, &s->payload_extent))
        return failed(s, g);
    s->stage = ACQUIRED;
    if (!acquire(g->nc_pool, owner, PVR_COMPUTE_QUEUE_OBJECTS * PAGE, &s->firmware_lease, &s->firmware_extent))
        return failed(s, g);
    s->firmware = g; s->owner = owner; s->region_count = count;
    memcpy(s->regions, regions, count * sizeof(*regions));
    struct pvr_compute_queue *q = &s->component;
    q->owner = s;
    q->job_cpu = (unsigned char *)s->payload_extent.cpu;
    for (unsigned i = 0; i < PVR_COMPUTE_QUEUE_OBJECTS; ++i) {
        q->firmware[i] = child(s->firmware_extent, i * PAGE, PAGE, s->firmware_pages + i);
        q->firmware_address[i] = 0xc0800000u + i * 0x10000u;
    }
    if (!pvr_compute_queue_bind(q, service, &g->config)) return failed(s, g);
    g->config.queue_owner = s;
    s->stage = BOUND;
    return q;
}

static int build_context(struct a733_gpu_queue *s)
{
    struct pvr_compute_buffer contexts[5];
    for (unsigned i = 0; i < 5; ++i)
        contexts[i] = (struct pvr_compute_buffer){s->component.firmware[i].storage.cpu, context_bytes[i],
                                                s->component.firmware_address[i]};
    if (!pvr_compute_programs_build(&s->programs, &s->program_result)) return 0;
    struct pvr_compute_config c = {.bvnc=PVR_FW_A733_BVNC, .priority=1, .context_id=1, .owner_id=s->owner};
    memcpy(c.switch_registers, s->program_result.switch_registers, sizeof(c.switch_registers));
    if (!pvr_compute_context_build(&c, contexts)) return 0;
    s->component.resume = s->program_result.cdm_resume_address;
    return 1;
}

int a733_gpu_queue_create(struct a733_gpu_queue *s, const struct gpu_queue_context_config *c)
{
    if (!s || s->stage != BOUND || s->component.phase != PVR_COMPUTE_QUEUE_BOUND || !c ||
        !c->lease_count || c->lease_count > GPU_QUEUE_LEASES || c->reserved) return 0;
    struct pvr_compute_queue *q = &s->component;
    unsigned count = 0;
    for (unsigned i = 0; i < c->lease_count; ++i) {
        const struct gpu_queue_lease_request *r = &c->lease[i];
        if (r->region >= s->region_count || r->reserved) return 0;
        const struct ss_region *region = &s->regions[r->region];
        uint64_t bytes = (region->bytes + BLOCK - 1) & ~(BLOCK - 1);
        if (r->bytes) { if (r->bytes > bytes || (r->bytes & (BLOCK - 1))) return 0; bytes = r->bytes; }
        unsigned a = region->access;
        if (!bytes || !region->bytes || region->bytes > (UINT64_C(1) << 36) || (region->address & (BLOCK - 1)) ||
            region->address >= (UINT64_C(1) << 36) || bytes > (UINT64_C(1) << 36) - region->address ||
            !(a & SS_ACCESS_READ) || !(a & (SS_ACCESS_GPU_READ | SS_ACCESS_GPU_WRITE)) ||
            ((a & SS_ACCESS_WRITE) && (a & SS_ACCESS_GPU_WRITE)) ||
            (a & ~(SS_ACCESS_READ | SS_ACCESS_WRITE | SS_ACCESS_GPU_READ | SS_ACCESS_GPU_WRITE))) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (!a733_extents_disjoint_checked(region->address, bytes, q->lease[j].address, q->lease[j].bytes)) return 0;
        if (!a733_extents_disjoint_checked(region->address,bytes,UINT64_C(0x41000000),
                (uintptr_t)__device_pool_end-UINT64_C(0x41000000)) ||
            !a733_extents_disjoint_checked(region->address,bytes,(uintptr_t)__uncached_pool_start,
                (uintptr_t)__uncached_pool_end-(uintptr_t)__uncached_pool_start) ||
            !a733_gpu_identity_mapped(region->address, bytes, 0xff, !!(a & SS_ACCESS_WRITE))) return 0;
        /* GPU output must have no CPU write translation, including at EL1. */
        if (a & SS_ACCESS_GPU_WRITE) for (uint64_t off=0;off<bytes;off+=PAGE) {
            struct aarch64_translation writable;
            if (aarch64_device_translate((uintptr_t)(region->address+off),1,&writable)) return 0;
        }
        q->lease[i] = (struct gpu_queue_lease_info){region->address, bytes, a, 0};
        /* BRN 71317 forbids data pages larger than 256 KiB on this BVNC. */
        s->maps[count++] = (struct pvr_gpu_mapping){region->address, region->address, bytes, 18,
            (a & SS_ACCESS_GPU_WRITE) ? 0 : PVR_GPU_VM_READ_ONLY};
    }
    q->lease_count = c->lease_count;
    struct pvr_compute_program_config *p = &s->programs;
    *p = (struct pvr_compute_program_config){.bvnc=PVR_FW_A733_BVNC,
        .pds={heap_base[0],UINT64_C(1)<<32}, .usc={heap_base[1],UINT64_C(1)<<32},
        .general={heap_base[2],UINT64_C(0x2000000000)}, .excluded_count=7, .execution_cores=1,
        .admission_token=s->payload_lease.generation, .admission_context=s, .admit=admit_programs};
    for (unsigned i=0; i<5; ++i)
        p->excluded[i]=(struct pvr_compute_program_span){q->firmware[i].storage.cpu,context_bytes[i],q->firmware_address[i]};
    unsigned char *payload=(unsigned char *)s->payload_extent.cpu;
    p->excluded[5]=(struct pvr_compute_program_span){payload,PAGE,JOB_GPU};
    p->excluded[6]=(struct pvr_compute_program_span){payload+PAGE,2*PAGE,PROGRAM_GPU};
    for (unsigned i=0; i<3; ++i)
        s->maps[count++]=(struct pvr_gpu_mapping){i ? PROGRAM_GPU+(i-1)*PAGE : JOB_GPU,
            s->payload_extent.physical+i*PAGE,PAGE,12,PVR_GPU_VM_READ_ONLY};
    size_t off=3*PAGE;
    for (unsigned i=0; i<PVR_PROGRAM_REGIONS; ++i) {
        size_t n=pvr_compute_program_bytes[i];
        if (off+rounded(n)>s->payload_extent.bytes) return 0;
        q->publish[i]=child(s->payload_extent,off,rounded(n),s->payload_pages+off/PAGE);
        p->buffers[i]=(struct pvr_compute_program_span){q->publish[i].storage.cpu,n,program_address(i)};
        for (unsigned k=0; k<q->publish[i].page_count; ++k)
            s->maps[count++]=(struct pvr_gpu_mapping){program_address(i)+k*PAGE,q->publish[i].pages[k],PAGE,12,
                i<PVR_PROGRAM_SHARED_A ? PVR_GPU_VM_READ_ONLY : 0};
        off+=rounded(n);
    }
    if (off!=s->payload_extent.bytes) return 0;
    for (unsigned i=1; i<count; ++i) {
        struct pvr_gpu_mapping m=s->maps[i]; unsigned j=i;
        while (j && s->maps[j-1].address>m.address) { s->maps[j]=s->maps[j-1]; --j; }
        s->maps[j]=m;
    }
    if (!pvr_gpu_vm_require(s->maps,count,&s->requirements) || !s->requirements.table_pages ||
        s->requirements.table_pages>A733_GPU_QUEUE_TABLE_PAGES ||
        !acquire(s->firmware->wb_pool,s->owner,s->requirements.table_bytes,&s->table_lease,&s->table_extent))
        return failed(s,s->firmware),0;
    for (unsigned i=0; i<s->requirements.table_pages; ++i) s->table_pages[i]=s->table_extent.physical+i*PAGE;
    const struct pvr_gpu_vm_tables tables={(void *)s->table_extent.cpu,s->table_extent.bytes,s->table_pages,s->requirements.table_pages};
    s->stage=ACQUIRED;
    if (!a733_gpu_vm_memory_init(&s->vm,&tables,q->firmware[4].storage.cpu,s->firmware_pages[4]) ||
        a733_gpu_vm_memory_build(&s->vm,s->maps,count)!=A733_GPU_VM_MEMORY_OK || !build_context(s))
        return failed(s,s->firmware),0;
    s->stage=BUILT;
    return 1;
}
static int held(struct device_memory_pool *pool, struct device_memory_lease lease,
                const struct device_memory_extent *expected)
{
    struct device_memory_extent actual;
    enum device_memory_state state;
    return device_memory_describe(pool, lease, &actual, &state) && state == DEVICE_MEMORY_DEVICE &&
        actual.cpu == expected->cpu && actual.physical == expected->physical && actual.bytes == expected->bytes;
}

int a733_gpu_queue_reopen(struct a733_gpu_queue *s)
{
    struct pvr_compute_queue *q=s ? &s->component : NULL;
    struct pvr_gpu_service *g=q ? q->service : NULL;
    if (!g || s->stage!=BUILT || g->phase!=PVR_GPU_SERVICE_STOPPED ||
        q->phase!=PVR_COMPUTE_QUEUE_READY || q->final_close || q->owed || atomic_load(&q->lost) ||
        atomic_load(&q->slot_state)!=PVR_COMPUTE_QUEUE_CLOSED ||
        q->client_owed || uxQueueMessagesWaiting(q->completions) ||
        s->vm.phase!=A733_GPU_VM_MEMORY_PUBLISHED ||
        !a733_gpu_backend_stopped(g->config.context,g) ||
        s->firmware->memory.phase!=A733_GPU_MEMORY_OFFLINE ||
        !held(s->firmware->wb_pool,s->payload_lease,&s->payload_extent) ||
        !held(s->firmware->wb_pool,s->table_lease,&s->table_extent) ||
        !held(s->firmware->nc_pool,s->firmware_lease,&s->firmware_extent)) return 0;
    uint64_t now;
    if (pvr_gpu_service_now(g,&now)!=1 || now>=g->deadline) return -1;
    memset(s->vm.context_cpu,0,PVR_GPU_VM_CONTEXT_BYTES);
    memcpy(s->vm.context_cpu,&s->table_pages[0],8);
    const uint32_t marker=UINT32_MAX;
    memcpy((unsigned char *)s->vm.context_cpu+8,&marker,4);
    s->stage=REBUILDING;
    if (!build_context(s) || pvr_gpu_service_now(g,&now)!=1 || now>=g->deadline)
        return failed(s,s->firmware),-1;
    q->client=(struct pvr_cccb){0};
    q->phase=PVR_COMPUTE_QUEUE_CREATED; q->held=0;
    s->stage=BUILT;
    return 1;
}
