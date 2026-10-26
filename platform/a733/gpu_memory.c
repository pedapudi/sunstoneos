#include "a733_gpu_memory.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "a733_gpu_mapping.h"
#include <string.h>

static const struct pvr_mips_buffer *buffer(const struct a733_gpu_memory *m, unsigned i)
{ return i < 3 ? &m->firmware[i] : &m->resources[i - 3]; }
static int mapped(const struct pvr_mips_buffer *b, size_t off, size_t n, unsigned attributes, int write)
{
    if (!n || off > b->storage.bytes || n > b->storage.bytes - off) return 0;
    unsigned first = (unsigned)(off / 4096), last = (unsigned)((off + n - 1) / 4096);
    for (unsigned page = first; page <= last; ++page)
        if (!a733_gpu_page_mapped((uintptr_t)b->storage.cpu + page * 4096u, b->pages[page], attributes, write))
            return 0;
    return 1;
}
static int external(const struct a733_gpu_memory *m, const void *p, size_t n)
{
    if (!a733_span_excl_nonempty(p, n) ||
        !a733_extents_disjoint_prechecked((uintptr_t)m, sizeof(*m), (uintptr_t)p, n)) return 0;
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *b = buffer(m, i);
        if (!a733_extents_disjoint_prechecked((uintptr_t)p, n, (uintptr_t)b->storage.cpu, b->storage.bytes) ||
            !a733_extents_disjoint_prechecked((uintptr_t)p, n, (uintptr_t)b->pages, b->page_count * sizeof(uint64_t))) return 0;
    }
    return 1;
}
static int fail(struct a733_gpu_memory *m)
{ if (m) m->phase = A733_GPU_MEMORY_FAILED; return 0; }
static int ready(struct a733_gpu_memory *m)
{
    return m && (m->phase == A733_GPU_MEMORY_OFFLINE || m->phase == A733_GPU_MEMORY_PUBLISHED) &&
        aarch64_device_cache_stride();
}
int a733_gpu_memory_init(struct a733_gpu_memory *m, const struct pvr_mips_buffer fw[3],
                         const struct pvr_mips_buffer resources[PVR_FW_RESOURCE_COUNT])
{
    if (!a733_span_excl_nonempty(m, sizeof(*m)) || m->phase != A733_GPU_MEMORY_NEW ||
        !a733_span_excl_nonempty(fw, 3 * sizeof(*fw)) ||
        !a733_span_excl_nonempty(resources, PVR_FW_RESOURCE_COUNT * sizeof(*resources)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)m, sizeof(*m), (uintptr_t)fw, 3 * sizeof(*fw)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)m, sizeof(*m), (uintptr_t)resources, PVR_FW_RESOURCE_COUNT * sizeof(*resources)) ||
        !aarch64_device_cache_stride()) return 0;
    struct a733_gpu_memory candidate = {0};
    memcpy(candidate.firmware, fw, sizeof(candidate.firmware));
    memcpy(candidate.resources, resources, sizeof(candidate.resources));
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *b = buffer(&candidate, i);
        if (!a733_span_excl_nonempty(b->storage.cpu, b->storage.bytes) || ((uintptr_t)b->storage.cpu & 4095) ||
            !b->page_count || b->page_count > PVR_FW_MAX_ALLOCATION / 4096 ||
            b->storage.bytes != (size_t)b->page_count * 4096 ||
            !a733_span_excl_nonempty(b->pages, b->page_count * sizeof(uint64_t)) || ((uintptr_t)b->pages & 7) ||
            !a733_extents_disjoint_prechecked((uintptr_t)m, sizeof(*m), (uintptr_t)b->pages, b->page_count * sizeof(uint64_t)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, b->storage.bytes, (uintptr_t)m, sizeof(*m)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, b->storage.bytes, (uintptr_t)fw, 3 * sizeof(*fw)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, b->storage.bytes, (uintptr_t)resources, PVR_FW_RESOURCE_COUNT * sizeof(*resources))) return 0;
        if ((i == 2 && b->storage.bytes != PVR_MIPS_TABLE_BYTES) ||
            (i >= 3 && b->storage.bytes != pvr_fw_state_layout((enum pvr_fw_resource)(i - 3))->allocation_bytes)) return 0;
    }
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *b = buffer(&candidate, i);
        for (unsigned j = 0; j < 3 + PVR_FW_RESOURCE_COUNT; ++j) {
            const struct pvr_mips_buffer *a = buffer(&candidate, j);
            if (!a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, b->storage.bytes, (uintptr_t)a->pages, a->page_count * sizeof(uint64_t)) ||
                (j < i && !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, b->storage.bytes, (uintptr_t)a->storage.cpu, a->storage.bytes))) return 0;
        }
        for (unsigned p = 0; p < b->page_count; ++p) {
            uint64_t pa = b->pages[p];
            if (!pa || (pa & 4095) || pa >= (UINT64_C(1) << 36) ||
                (pa >= 0x1fc00000 && pa < 0x1fc04000)) return 0;
            for (unsigned j = 0; j <= i; ++j) {
                const struct pvr_mips_buffer *a = buffer(&candidate, j);
                for (unsigned q = 0; q < (j == i ? p : a->page_count); ++q)
                    if (a->pages[q] == pa) return 0;
            }
        }
        if (!mapped(b, 0, b->storage.bytes, i < 3 ? 0xff : 0x44, 1)) return 0;
    }
    candidate.phase = A733_GPU_MEMORY_OFFLINE; *m = candidate; return 1;
}
/* Each access rechecks the pages it touches: reads their read translation,
 * writes both translations. */
static int shared(struct a733_gpu_memory *m, unsigned id, size_t off, size_t bytes, int write)
{
    return ready(m) && id < PVR_FW_RESOURCE_COUNT &&
        mapped(&m->resources[id], off, bytes, 0x44, write);
}
static void read_bytes(const void *source, void *out, size_t n)
{
    const volatile unsigned char *in = source; unsigned char *p = out;
    for (size_t i = 0; i < n; ++i) p[i] = in[i];
    __asm__ volatile("dsb sy" ::: "memory");
}
static void *address(struct a733_gpu_memory *m, unsigned id, size_t off)
{ return (unsigned char *)m->resources[id].storage.cpu + off; }
int a733_gpu_memory_read(void *ctx, enum pvr_ccb_region r, uint32_t off, void *out, size_t n)
{
    struct a733_gpu_memory *m = ctx;
    size_t size = pvr_ccb_region_bytes(r);
    if (!ready(m) || off > size || n > size - off || (n == 4 && (off & 3)) ||
        !external(m, out, n) || !shared(m, (unsigned)r, off, n, 0)) return fail(m);
    if (n == 4) {
        uint32_t value = a733_mmio_read32_dsb_sy_after((uintptr_t)address(m, (unsigned)r, off));
        memcpy(out, &value, 4);
    } else read_bytes(address(m, (unsigned)r, off), out, n);
    return 1;
}
static int host_write(enum pvr_ccb_region r, uint32_t off, const void *in, size_t n)
{
    switch (r) {
    case PVR_CCB_HOST_CONTROL: return !off && n == 4;
    case PVR_CCB_HOST_COMMANDS: return !(off % PVR_CCB_COMMAND_BYTES) && n == PVR_CCB_COMMAND_BYTES;
    case PVR_CCB_RETURNS: {
        uint32_t v = 1; if (n == 4) memcpy(&v, in, 4); return n == 4 && !v;
    }
    case PVR_CCB_FIRMWARE_CONTROL: return off == 128 && n == 4;
    default: return 0;
    }
}
int a733_gpu_memory_write(void *ctx, enum pvr_ccb_region r, uint32_t off, const void *in, size_t n)
{
    struct a733_gpu_memory *m = ctx; size_t size = pvr_ccb_region_bytes(r);
    if (!ready(m) || off > size || n > size - off || (n == 4 && (off & 3)) ||
        !external(m, in, n) || !shared(m, (unsigned)r, off, n, 1) ||
        (m->phase == A733_GPU_MEMORY_PUBLISHED && !host_write(r, off, in, n))) return fail(m);
    a733_mmio_dsb_sy();
    if (n == 4) { uint32_t v; memcpy(&v, in, 4); a733_mmio_write32((uintptr_t)address(m, (unsigned)r, off), v); }
    else memcpy(address(m, (unsigned)r, off), in, n);
    a733_mmio_dsb_sy(); return 1;
}
int a733_gpu_memory_barrier(void *ctx, enum pvr_ccb_barrier operation)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || (unsigned)operation > PVR_CCB_DEVICE_COMPLETE) return fail(m);
    __asm__ volatile("dsb sy" ::: "memory"); return 1;
}
static int all_mappings(struct a733_gpu_memory *m)
{
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *b = buffer(m, i);
        if (!mapped(b, 0, b->storage.bytes, i < 3 ? 0xff : 0x44, 1)) return 0;
    }
    return 1;
}
static int cached(struct a733_gpu_memory *m, enum aarch64_cache_op op)
{
    for (unsigned i = 0; i < 3; ++i) {
        const struct pvr_fw_buffer *b = &m->firmware[i].storage;
        if (!aarch64_device_cache(op, (uintptr_t)b->cpu, b->bytes)) return 0;
    }
    return 1;
}
int a733_gpu_memory_publish(void *ctx)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || m->phase != A733_GPU_MEMORY_OFFLINE || !all_mappings(m) ||
        !cached(m, AARCH64_CACHE_CLEAN_INVALIDATE)) return fail(m);
    __asm__ volatile("dsb sy" ::: "memory"); m->phase = A733_GPU_MEMORY_PUBLISHED; return 1;
}
int a733_gpu_memory_reclaim(struct a733_gpu_memory *m)
{
    if (!ready(m) || m->phase != A733_GPU_MEMORY_PUBLISHED || !all_mappings(m) ||
        !cached(m, AARCH64_CACHE_INVALIDATE)) return fail(m);
    __asm__ volatile("dsb sy" ::: "memory"); m->phase = A733_GPU_MEMORY_OFFLINE; return 1;
}
int a733_gpu_memory_started(void *ctx, uint32_t *out)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || m->phase != A733_GPU_MEMORY_PUBLISHED || !external(m, out, sizeof(*out)) ||
        !shared(m, PVR_FW_SYSTEM_INIT, 352, 4, 0)) return fail(m);
    /* The whole 32-bit word, as the vendor host polls bFirmwareStarted
     * (rgxpower.c:1039-1052); the upstream bool's padding is zero. */
    *out = a733_mmio_read32_dsb_sy_after((uintptr_t)address(m, PVR_FW_SYSTEM_INIT, 352));
    return 1;
}
int a733_gpu_memory_compat(void *ctx, uint32_t off, void *out, size_t n)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || m->phase != A733_GPU_MEMORY_PUBLISHED || off > 64 || n > 64 - off ||
        !external(m, out, n) || !shared(m, PVR_FW_OS_INIT, 40 + off, n, 0)) return fail(m);
    read_bytes(address(m, PVR_FW_OS_INIT, 40 + off), out, n); return 1;
}
int a733_gpu_memory_sync_write(void *ctx, uint32_t value)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || m->phase != A733_GPU_MEMORY_PUBLISHED || value ||
        !shared(m, PVR_FW_POWER_SYNC, 0, 4, 1)) return fail(m);
    a733_mmio_write32_dsb_sy_both((uintptr_t)address(m, PVR_FW_POWER_SYNC, 0), value);
    return 1;
}
int a733_gpu_memory_sync_read(void *ctx, uint32_t *out)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || m->phase != A733_GPU_MEMORY_PUBLISHED || !external(m, out, sizeof(*out)) ||
        !shared(m, PVR_FW_POWER_SYNC, 0, 4, 0)) return fail(m);
    *out = a733_mmio_read32_dsb_sy_after((uintptr_t)address(m, PVR_FW_POWER_SYNC, 0));
    return 1;
}

int a733_gpu_memory_power_state(void *ctx, uint32_t *out)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || m->phase != A733_GPU_MEMORY_PUBLISHED || !external(m, out, sizeof(*out)) ||
        !shared(m, PVR_FW_SYSTEM_DATA, 8, 4, 0)) return fail(m);
    *out = a733_mmio_read32_dsb_sy_after((uintptr_t)address(m, PVR_FW_SYSTEM_DATA, 8));
    return 1;
}
int a733_gpu_memory_executed(void *ctx, uint32_t *out)
{
    struct a733_gpu_memory *m = ctx;
    if (!ready(m) || m->phase != A733_GPU_MEMORY_PUBLISHED || !external(m, out, sizeof(*out)) ||
        !shared(m, PVR_FW_OS_DATA, 568, 4, 0)) return fail(m);
    *out = a733_mmio_read32_dsb_sy_after((uintptr_t)address(m, PVR_FW_OS_DATA, 568));
    return 1;
}
