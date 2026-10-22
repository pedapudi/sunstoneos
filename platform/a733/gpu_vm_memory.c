#include "a733_gpu_vm_memory.h"
#include "a733_range.h"
#include "aarch64_device_memory.h"
#include "a733_gpu_mapping.h"

#define PAGE 4096u
#define PA_LIMIT (UINT64_C(1) << 36)
#define MAX_TABLE_PAGES (1u + 1024u + 524288u)

static int physical(uint64_t p)
{ return p && p < PA_LIMIT && !(p >= 0x1fc00000 && p < 0x1fc04000); }
static int mapped(uintptr_t cpu, uint64_t pa, unsigned attributes)
{ return a733_gpu_page_mapped(cpu, pa, attributes, 1); }
static int context_mapped(const struct a733_gpu_vm_memory *m)
{ return mapped((uintptr_t)m->context_cpu, m->context_physical, 0x44); }
static int table_mapped(const struct a733_gpu_vm_memory *m, unsigned i)
{
    return mapped((uintptr_t)m->tables.cpu + (size_t)i * PAGE,
                   m->tables.physical_pages[i], 0xff);
}
static int all_mapped(const struct a733_gpu_vm_memory *m)
{
    if (!context_mapped(m)) return 0;
    for (unsigned i = 0; i < m->tables.page_count; ++i) if (!table_mapped(m, i)) return 0;
    return 1;
}
static enum a733_gpu_vm_memory_result fail(struct a733_gpu_vm_memory *m)
{ m->phase = A733_GPU_VM_MEMORY_FAILED; return A733_GPU_VM_MEMORY_FAULT; }

int a733_gpu_vm_memory_init(struct a733_gpu_vm_memory *m, const struct pvr_gpu_vm_tables *t,
                            void *context, uint64_t pa)
{
    if (!a733_span_excl_nonempty(m, sizeof(*m)) || m->phase != A733_GPU_VM_MEMORY_NEW ||
        !a733_span_excl_nonempty(t, sizeof(*t)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)m, sizeof(*m), (uintptr_t)t, sizeof(*t)) ||
        !t->page_count || t->page_count > MAX_TABLE_PAGES ||
        t->bytes != (size_t)t->page_count * PAGE || !a733_span_excl_nonempty(t->cpu, t->bytes) ||
        ((uintptr_t)t->cpu & (PAGE - 1)) ||
        !a733_span_excl_nonempty(t->physical_pages, (size_t)t->page_count * sizeof(uint64_t)) ||
        ((uintptr_t)t->physical_pages & 7) || !a733_span_excl_nonempty(context, PVR_GPU_VM_CONTEXT_BYTES) ||
        ((uintptr_t)context & 7) || ((uintptr_t)context & (PAGE - 1)) > PAGE - PVR_GPU_VM_CONTEXT_BYTES ||
        !physical(pa & ~(uint64_t)(PAGE - 1)) ||
        (pa & (PAGE - 1)) != ((uintptr_t)context & (PAGE - 1)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)t->cpu, t->bytes, (uintptr_t)context, PVR_GPU_VM_CONTEXT_BYTES) ||
        !a733_extents_disjoint_prechecked((uintptr_t)t->cpu, t->bytes, (uintptr_t)m, sizeof(*m)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)t->cpu, t->bytes, (uintptr_t)t, sizeof(*t)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)t->cpu, t->bytes, (uintptr_t)t->physical_pages, (size_t)t->page_count * sizeof(uint64_t)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)context, PVR_GPU_VM_CONTEXT_BYTES, (uintptr_t)m, sizeof(*m)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)context, PVR_GPU_VM_CONTEXT_BYTES, (uintptr_t)t, sizeof(*t)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)context, PVR_GPU_VM_CONTEXT_BYTES, (uintptr_t)t->physical_pages, (size_t)t->page_count * sizeof(uint64_t)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)m, sizeof(*m), (uintptr_t)t->physical_pages, (size_t)t->page_count * sizeof(uint64_t))) return 0;
    for (unsigned i = 0; i < t->page_count; ++i) {
        uint64_t p = t->physical_pages[i];
        if (!physical(p) || (p & (PAGE - 1)) || (i && p <= t->physical_pages[i - 1]) ||
            p == (pa & ~(uint64_t)(PAGE - 1))) return 0;
    }
    struct a733_gpu_vm_memory candidate = {.tables = *t, .context_cpu = context,
        .context_physical = pa, .phase = A733_GPU_VM_MEMORY_OFFLINE};
    if (!all_mapped(&candidate)) return 0;
    *m = candidate;
    return 1;
}
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_build(struct a733_gpu_vm_memory *m,
    const struct pvr_gpu_mapping *maps, size_t count)
{
    if (!m || m->phase == A733_GPU_VM_MEMORY_FAILED) return A733_GPU_VM_MEMORY_FAULT;
    if (m->phase != A733_GPU_VM_MEMORY_OFFLINE || count > SIZE_MAX / sizeof(*maps) ||
        (count && (!a733_span_excl_nonempty(maps, count * sizeof(*maps)) ||
        ((uintptr_t)maps % _Alignof(struct pvr_gpu_mapping)) ||
                   !a733_extents_disjoint_prechecked((uintptr_t)m, sizeof(*m), (uintptr_t)maps, count * sizeof(*maps))))) return A733_GPU_VM_MEMORY_INVALID;
    struct pvr_gpu_vm_requirements required;
    if (!pvr_gpu_vm_require(maps, count, &required) || required.table_bytes != m->tables.bytes)
        return A733_GPU_VM_MEMORY_INVALID;
    uint64_t context_page = m->context_physical & ~(uint64_t)(PAGE - 1);
    for (size_t i = 0; i < count; ++i)
        if (maps[i].physical < context_page + PAGE &&
            context_page < maps[i].physical + maps[i].bytes) return A733_GPU_VM_MEMORY_INVALID;
    if (!all_mapped(m)) return fail(m);
    if (!pvr_gpu_vm_build(maps, count, &m->tables, m->context_cpu, PVR_GPU_VM_CONTEXT_BYTES))
        return A733_GPU_VM_MEMORY_INVALID;
    m->phase = A733_GPU_VM_MEMORY_PREPARED;
    return A733_GPU_VM_MEMORY_OK;
}
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_publish_begin(struct a733_gpu_vm_memory *m)
{
    if (!m || m->phase == A733_GPU_VM_MEMORY_FAILED) return A733_GPU_VM_MEMORY_FAULT;
    if (m->phase != A733_GPU_VM_MEMORY_PREPARED) return A733_GPU_VM_MEMORY_INVALID;
    m->cursor = 0; m->phase = A733_GPU_VM_MEMORY_PUBLISHING;
    return A733_GPU_VM_MEMORY_AGAIN;
}
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_reclaim_begin(struct a733_gpu_vm_memory *m)
{
    if (!m || m->phase == A733_GPU_VM_MEMORY_FAILED) return A733_GPU_VM_MEMORY_FAULT;
    if (m->phase != A733_GPU_VM_MEMORY_PUBLISHED) return A733_GPU_VM_MEMORY_INVALID;
    m->cursor = 0; m->phase = A733_GPU_VM_MEMORY_RECLAIMING;
    return A733_GPU_VM_MEMORY_AGAIN;
}
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_poll(struct a733_gpu_vm_memory *m)
{
    if (!m || m->phase == A733_GPU_VM_MEMORY_FAILED) return A733_GPU_VM_MEMORY_FAULT;
    if (m->phase != A733_GPU_VM_MEMORY_PUBLISHING && m->phase != A733_GPU_VM_MEMORY_RECLAIMING)
        return A733_GPU_VM_MEMORY_INVALID;
    if (m->cursor < m->tables.page_count) {
        if (!table_mapped(m, m->cursor) ||
            !aarch64_device_cache(m->phase == A733_GPU_VM_MEMORY_PUBLISHING ?
                AARCH64_CACHE_CLEAN : AARCH64_CACHE_INVALIDATE,
                (uintptr_t)m->tables.cpu + (size_t)m->cursor * PAGE, PAGE)) return fail(m);
        ++m->cursor;
        return A733_GPU_VM_MEMORY_AGAIN;
    }
    if (!context_mapped(m)) return fail(m);
    __asm__ volatile("dsb sy" ::: "memory");
    m->phase = m->phase == A733_GPU_VM_MEMORY_PUBLISHING ?
        A733_GPU_VM_MEMORY_PUBLISHED : A733_GPU_VM_MEMORY_OFFLINE;
    return A733_GPU_VM_MEMORY_OK;
}
