#include "a733_gpu_graphics_memory.h"
#include "aarch64_device_memory.h"
#include <string.h>

static struct a733_gpu_graphics_memory *graphics_owner;
#define FW_BASE UINT32_C(0xc0900000)
#define GPU_BASE UINT64_C(0x8004000000)
#define TABLE_BASE UINT64_C(0x8006000000)
static size_t rounded(size_t n) { return (n + 4095) & ~(size_t)4095; }
static int apart(uintptr_t a, size_t n, uintptr_t b, size_t m)
{ return a + n <= b || b + m <= a; }
static int mapped_access(uintptr_t cpu, size_t bytes, unsigned attr, int write)
{
    if (!bytes || cpu > UINTPTR_MAX - bytes) return 0;
    uintptr_t last = (cpu + bytes - 1) & ~(uintptr_t)4095;
    for (uintptr_t page = cpu & ~(uintptr_t)4095; ; page += 4096) {
        struct aarch64_translation r, w;
        if (!aarch64_device_translate(page, 0, &r) || r.physical != page || r.attributes != attr ||
            (r.shareability != 3 && (attr != 0x44 || r.shareability != 2)) ||
            (write && (!aarch64_device_translate(page, 1, &w) || w.physical != page ||
                       w.attributes != attr || w.shareability != r.shareability))) return 0;
        if (page == last) return 1;
    }
}
static int mapped(uintptr_t cpu, size_t bytes, unsigned attr)
{ return mapped_access(cpu, bytes, attr, 1); }
static int private_image(uintptr_t cpu, size_t bytes, uintptr_t pool_start, int write)
{ return cpu >= UINT32_C(0x41000000) && cpu < pool_start &&
    bytes <= pool_start - cpu && mapped_access(cpu, bytes, 0xff, write); }
static int lease(struct device_memory_pool *pool, struct device_memory_lease token,
                  struct device_memory_extent expected, uint64_t owner)
{
    struct device_memory_extent actual; enum device_memory_state state;
    return token.owner == owner && device_memory_describe(pool, token, &actual, &state) &&
        state == DEVICE_MEMORY_DEVICE && actual.cpu == expected.cpu && actual.cpu == actual.physical &&
        actual.physical == expected.physical && actual.bytes == expected.bytes;
}
static int live(const struct a733_gpu_graphics_memory *s)
{ return s && s == graphics_owner && s->self == (uintptr_t)s &&
    s->phase >= A733_GPU_GRAPHICS_BOUND && s->phase < A733_GPU_GRAPHICS_FAILED; }
void a733_gpu_graphics_memory_abort(struct a733_gpu_graphics_memory *s)
{
    if (s && s == graphics_owner && s->self == (uintptr_t)s) {
        s->phase = A733_GPU_GRAPHICS_FAILED; s->vm.phase = A733_GPU_VM_MEMORY_FAILED;
    }
}
static int fault(struct a733_gpu_graphics_memory *s)
{ a733_gpu_graphics_memory_abort(s); return -1; }
static int parents(const struct a733_gpu_graphics_memory *s)
{
    return s->firmware->wb_pool == s->compute->binding.wb &&
        s->firmware->nc_pool == s->compute->binding.nc &&
        lease(s->firmware->wb_pool, s->backing_lease, s->backing_extent, s->owner) &&
        lease(s->firmware->wb_pool, s->payload_lease, s->payload_extent, s->owner) &&
        lease(s->firmware->nc_pool, s->shared_lease, s->shared_extent, s->owner) &&
        lease(s->firmware->wb_pool, s->table_lease, s->table_extent, s->owner);
}
static unsigned list_index(const struct a733_gpu_graphics_memory *s, uint64_t token)
{ return token == s->lists[0].resource.token ? 0 : token == s->lists[1].resource.token ? 1 : 2; }
static int allocate(void *ctx, uint64_t epoch, uint64_t owner, uint64_t token,
                    unsigned slot, struct pvr_free_list_page *out)
{
    struct a733_gpu_graphics_memory *s = ctx;
    if (!live(s)) return -1;
    unsigned i = list_index(s, token);
    if (i > 1 || !out || owner != s->owner || !epoch || epoch != s->service->epoch ||
        (s->epoch && s->epoch != epoch) || slot != s->acquired[i] ||
        slot >= s->lists[i].maximum_pages || !parents(s)) return fault(s);
    size_t offset = (i ? PVR_RENDER_LOCAL_PAGES : 0) * 4096u + slot * 4096u;
    if (offset >= s->backing_extent.bytes || !mapped(s->backing_extent.cpu + offset, 4096, 0xff)) return fault(s);
    /* Admit the child before handing it to the manager. No pool mutation occurs
     * in the live worker; the entire parent already belongs to this lifetime. */
    s->epoch = epoch; ++s->acquired[i];
    *out = (struct pvr_free_list_page){(void *)(s->backing_extent.cpu + offset),
        s->backing_extent.physical + offset, ((uint64_t)i << 32) | (slot + 1)};
    return 1;
}
static int publish_page(void *ctx, const struct pvr_free_list_page *page)
{
    struct a733_gpu_graphics_memory *s = ctx;
    if (!live(s)) return 0;
    unsigned i = page ? (unsigned)(page->lease >> 32) : 2;
    unsigned slot = page ? (uint32_t)page->lease : 0;
    if (i > 1 || !slot || slot > s->acquired[i] || !parents(s)) return fault(s);
    size_t offset = ((i ? PVR_RENDER_LOCAL_PAGES : 0) + slot - 1) * 4096u;
    if ((uintptr_t)page->cpu != s->backing_extent.cpu + offset ||
        page->physical != s->backing_extent.physical + offset ||
        !mapped((uintptr_t)page->cpu, 4096, 0xff) ||
        !aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)page->cpu, 4096)) return fault(s);
    return 1;
}
static uintptr_t memory(struct a733_gpu_graphics_memory *s, uint64_t token,
    enum pvr_free_list_memory kind, uint32_t offset, size_t bytes)
{
    if (!live(s)) return 0;
    unsigned i = list_index(s, token);
    if (i > 1 || !bytes || (offset & 3) || (bytes & 3) || bytes > 256 ||
        (kind != PVR_FREE_LIST_TABLE && kind != PVR_FREE_LIST_FIRMWARE) || !parents(s)) { fault(s); return 0; }
    size_t limit = kind == PVR_FREE_LIST_TABLE ? s->lists[i].maximum_pages * 4u : 64;
    size_t base = kind == PVR_FREE_LIST_FIRMWARE ? i * 64u :
        4096u + (i ? 4096u : 0);
    if (offset > limit || bytes > limit - offset) { fault(s); return 0; }
    uintptr_t cpu = s->shared_extent.cpu + base + offset;
    if (!mapped(cpu, bytes, 0x44)) { fault(s); return 0; }
    return cpu;
}
static int read_memory(void *ctx, uint64_t token, enum pvr_free_list_memory kind,
                        uint32_t off, void *out, size_t bytes)
{
    struct a733_gpu_graphics_memory *s = ctx;
    uintptr_t cpu = memory(s, token, kind, off, bytes);
    if (!cpu || !out) return 0;
    unsigned char *p = out;
    for (size_t i = 0; i < bytes; i += 4) {
        uint32_t word = __atomic_load_n((uint32_t *)(cpu + i), __ATOMIC_ACQUIRE);
        memcpy(p + i, &word, 4);
    }
    return 1;
}
static int write_memory(void *ctx, uint64_t token, enum pvr_free_list_memory kind,
                         uint32_t off, const void *in, size_t bytes)
{
    struct a733_gpu_graphics_memory *s = ctx;
    uintptr_t cpu = memory(s, token, kind, off, bytes);
    if (!cpu || !in) return 0;
    const unsigned char *p = in;
    for (size_t i = 0; i < bytes; i += 4) {
        uint32_t word; memcpy(&word, p + i, 4);
        __atomic_store_n((uint32_t *)(cpu + i), word, __ATOMIC_RELEASE);
    }
    return 1;
}
static int publish(void *ctx, uint64_t token, enum pvr_free_list_memory kind,
                    uint32_t off, size_t bytes)
{
    if (!memory(ctx, token, kind, off, bytes)) return 0;
    __asm__ volatile("dsb sy" ::: "memory"); return 1;
}
static int recover(void *ctx, uint64_t token)
{
    struct a733_gpu_graphics_memory *s = ctx;
    if (!live(s)) return 0;
    unsigned i = list_index(s, token);
    if (i > 1 || !parents(s)) return fault(s);
    /* The reference links both HWRT records to the local list, not the global
     * list. Preserve every other firmware-owned byte during reconstruction. */
    if (i == 1) return 1;
    if (!mapped(s->shared_extent.cpu, 4096, 0x44)) return fault(s);
    for (unsigned j = 0; j < 2; ++j) {
        uintptr_t p = s->shared_extent.cpu + 256 + j * 384u;
        uint32_t flags = __atomic_load_n((uint32_t *)(p + 168), __ATOMIC_ACQUIRE);
        __atomic_store_n((uint32_t *)(p + 172), 9, __ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(p + 168), flags & ~4u, __ATOMIC_RELEASE);
    }
    __asm__ volatile("dsb sy" ::: "memory"); return 1;
}
static const struct pvr_free_list_ops operations = {allocate, publish_page, read_memory, write_memory, publish, recover};

int a733_gpu_graphics_memory_validate(struct a733_gpu_graphics_memory *s,
    const struct pvr_gpu_service_config *c, const struct pvr_gpu_service *service)
{
    if (!live(s) || s->phase != A733_GPU_GRAPHICS_BOUND || !c || service != s->service ||
        c->graphics != s || c->compute != &s->compute->compute || c->free_lists.count != 2 ||
        c->free_lists.lists != s->lists || c->free_lists.minimum_pages != PVR_RENDER_LOCAL_PAGES ||
        c->free_lists.operation_timeout_us != s->profile.operation_timeout_us || !parents(s) ||
        s->compute->compute.service != service || s->compute->compute.phase != PVR_COMPUTE_SERVICE_BOUND ||
        !a733_gpu_compute_reserved_admitted(s->compute, s->firmware, s->service) ||
        s->firmware->config.graphics != s || s->firmware->config.compute != c->compute ||
        c->firmware != s->firmware->config.firmware || c->firmware_bytes != s->firmware->config.firmware_bytes ||
        memcmp(c->firmware_buffers, s->firmware->config.firmware_buffers, sizeof(c->firmware_buffers)) ||
        memcmp(c->resources, s->firmware->config.resources, sizeof(c->resources))) return 0;
    uintptr_t boundary = s->firmware->wb_pool->backing.cpu;
    if (!private_image((uintptr_t)s, sizeof(*s), boundary, 1) ||
        (c != &s->firmware->config && c != &service->config)) return 0;
    const void *metadata[] = {s->firmware, s->compute, service, c->firmware,
        s->firmware->wb_pool, s->firmware->nc_pool};
    const size_t sizes[] = {sizeof(*s->firmware), sizeof(*s->compute), sizeof(*service), c->firmware_bytes,
        sizeof(*s->firmware->wb_pool), sizeof(*s->firmware->nc_pool)};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        if (!apart((uintptr_t)s, sizeof(*s), (uintptr_t)metadata[i], sizes[i])) return 0;
    const struct device_memory_extent extents[] = {s->backing_extent, s->payload_extent,
        s->shared_extent, s->table_extent, s->firmware->wb_extent, s->firmware->nc_extent,
        s->compute->payload_extent, s->compute->firmware_extent, s->compute->table_extent};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = i + 1; j < sizeof(extents) / sizeof(extents[0]); ++j)
            if (!apart(extents[i].cpu, extents[i].bytes, extents[j].cpu, extents[j].bytes)) return 0;
        if (extents[i].cpu < boundary) return 0;
    }
    struct pvr_render_requirements r;
    if (!pvr_render_target_require(&s->profile.render, &r) || memcmp(&r, &s->render, sizeof(r)) ||
        s->backing_extent.bytes != ((size_t)s->profile.render.global_maximum_pages + 28) * 4096 ||
        s->payload_extent.bytes != r.payload_bytes ||
        s->shared_extent.bytes != 8192 + rounded(s->profile.render.global_maximum_pages * 4u) ||
        s->table_extent.bytes != s->requirements.table_bytes ||
        s->object.pages != &s->firmware_page || s->object.page_count != 1 || s->object.address != FW_BASE ||
        s->object.cache_policy != PVR_MIPS_UNCACHED || s->firmware_page != s->shared_extent.physical ||
        s->vm.context_cpu != (void *)(s->shared_extent.cpu + 1024) ||
        s->vm.context_physical != s->shared_extent.physical + 1024 ||
        s->vm.tables.cpu != (void *)s->table_extent.cpu || s->vm.tables.bytes != s->table_extent.bytes ||
        s->vm.tables.physical_pages != s->table_pages || s->vm.tables.page_count != s->requirements.table_pages ||
        s->vm.phase != A733_GPU_VM_MEMORY_PREPARED) return 0;
    size_t off = 0;
    for (unsigned i = 0; i < PVR_RENDER_REGIONS; ++i) {
        struct pvr_gpu_mapping map = {GPU_BASE + off, s->payload_extent.physical + off, r.allocation[i], 12, PVR_GPU_VM_SLC_BYPASS};
        if (memcmp(&map, s->maps + i, sizeof(map)) || s->target.gpu[i] != GPU_BASE + off) return 0;
        off += r.allocation[i];
    }
    for (unsigned i = 0; i < 2; ++i) {
        unsigned maximum = i ? s->profile.render.global_maximum_pages : PVR_RENDER_LOCAL_PAGES;
        struct pvr_free_list_config f = {{PVR_GPU_RESOURCE_FREELIST, i + 1, s->owner, i + 1},
            TABLE_BASE + i * 4096u, FW_BASE + i * 64u,
            i ? s->profile.initial_pages : 28, maximum,
            i ? s->profile.grow_pages : 0, i ? s->profile.threshold : 0,
            i ? s->global_pages : s->local_pages};
        struct pvr_gpu_mapping map = {f.gpu_address, s->shared_extent.physical + 4096 + i * 4096u,
            rounded(maximum * 4u), 12, PVR_GPU_VM_FW_PROTECT | PVR_GPU_VM_SLC_BYPASS};
        const struct pvr_free_list_config *actual = s->lists + i;
        if (actual->resource.kind != f.resource.kind || actual->resource.id != f.resource.id ||
            actual->resource.owner != f.resource.owner || actual->resource.token != f.resource.token ||
            actual->gpu_address != f.gpu_address || actual->firmware_address != f.firmware_address ||
            actual->initial_pages != f.initial_pages || actual->maximum_pages != f.maximum_pages ||
            actual->grow_pages != f.grow_pages || actual->threshold != f.threshold || actual->pages != f.pages ||
            memcmp(&map, s->maps + PVR_RENDER_REGIONS + i, sizeof(map))) return 0;
    }
    struct pvr_gpu_vm_requirements req;
    if (!pvr_gpu_vm_require(s->maps, A733_GPU_GRAPHICS_MAPS, &req) ||
        memcmp(&req, &s->requirements, sizeof(req)) || req.table_pages > A733_GPU_GRAPHICS_TABLE_PAGES) return 0;
    for (unsigned i = 0; i < req.table_pages; ++i)
        if (s->table_pages[i] != s->table_extent.physical + i * 4096u) return 0;
    return s->target.common == (void *)(s->shared_extent.cpu + 128) &&
        s->target.common_firmware == FW_BASE + 128 && s->target.local_firmware == FW_BASE &&
        s->target.global_firmware == FW_BASE + 64 &&
        s->target.data[0] == (void *)(s->shared_extent.cpu + 256) &&
        s->target.data[1] == (void *)(s->shared_extent.cpu + 640) &&
        s->target.data_firmware[0] == FW_BASE + 256 && s->target.data_firmware[1] == FW_BASE + 640;
}
static int acquire(struct device_memory_pool *pool, uint64_t owner, size_t bytes,
                    struct device_memory_lease *token, struct device_memory_extent *extent, int zero)
{
    if (!device_memory_acquire(pool, owner, bytes, token, extent)) return 0;
    device_memory_lookup(pool, *token)->state = DEVICE_MEMORY_DEVICE;
    if (zero) memset((void *)extent->cpu, 0, extent->bytes);
    return 1;
}
static int construction_failed(struct a733_gpu_graphics_memory *s)
{
    struct device_memory_lease leases[] = {s->backing_lease, s->payload_lease, s->shared_lease, s->table_lease};
    for (unsigned i = 0; i < 4; ++i) if (leases[i].generation)
        device_memory_retain(i == 2 ? s->firmware->nc_pool : s->firmware->wb_pool, leases[i]);
    s->phase = A733_GPU_GRAPHICS_FAILED; return 0;
}
int a733_gpu_graphics_memory_prepare(struct a733_gpu_graphics_memory *s,
    struct a733_gpu_reserved *g, struct a733_gpu_compute_reserved *compute,
    struct pvr_gpu_service *service, uint64_t owner, const struct a733_gpu_graphics_profile *profile)
{
    if (graphics_owner || !s || !g || !compute || !service || !profile || !owner ||
        (uintptr_t)s % _Alignof(struct a733_gpu_graphics_memory) ||
        (uintptr_t)g % _Alignof(struct a733_gpu_reserved) ||
        (uintptr_t)compute % _Alignof(struct a733_gpu_compute_reserved) ||
        (uintptr_t)service % _Alignof(struct pvr_gpu_service) ||
        (uintptr_t)profile % _Alignof(struct a733_gpu_graphics_profile) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !mapped((uintptr_t)g, sizeof(*g), 0xff) || !mapped((uintptr_t)compute, sizeof(*compute), 0xff) ||
        !a733_gpu_compute_reserved_admitted(compute, g, service)) return 0;
    /* Existing native compute admission proves this is the actual boot pool,
     * whose backing begins at the native linker private-image boundary. */
    uintptr_t boundary = g->wb_pool->backing.cpu;
    if (!private_image((uintptr_t)s, sizeof(*s), boundary, 1) ||
        !private_image((uintptr_t)g, sizeof(*g), boundary, 1) ||
        !private_image((uintptr_t)compute, sizeof(*compute), boundary, 1) ||
        !private_image((uintptr_t)service, sizeof(*service), boundary, 1) ||
        !private_image((uintptr_t)profile, sizeof(*profile), boundary, 0)) return 0;
    const void *p[] = {s, g, compute, service, profile, g->config.firmware};
    const size_t n[] = {sizeof(*s), sizeof(*g), sizeof(*compute), sizeof(*service), sizeof(*profile), g->config.firmware_bytes};
    for (unsigned i = 0; i < 6; ++i) for (unsigned j = 0; j < i; ++j)
        if (!apart((uintptr_t)p[i], n[i], (uintptr_t)p[j], n[j])) return 0;
    if (s->self || s->phase || service->initialized || g->config.graphics || g->config.free_lists.count ||
        g->config.context || g->config.compute != &compute->compute || compute->binding.firmware != g ||
        compute->binding.service != service || compute->compute.phase != PVR_COMPUTE_SERVICE_BOUND ||
        !a733_gpu_compute_reserved_admitted(compute, g, service) ||
        g->memory.phase != A733_GPU_MEMORY_OFFLINE || !g->prepared || !g->admitted ||
        !profile->operation_timeout_us || profile->operation_timeout_us > INT64_MAX ||
        profile->initial_pages < 28 || profile->initial_pages > profile->render.global_maximum_pages ||
        (profile->initial_pages & 3) || (profile->grow_pages & 3) || profile->threshold > 100 ||
        profile->grow_pages > profile->render.global_maximum_pages ||
        (profile->initial_pages < profile->render.global_maximum_pages ?
         !profile->grow_pages : profile->grow_pages != 0)) return 0;
    struct pvr_render_requirements r;
    if (!pvr_render_target_require(&profile->render, &r)) return 0;
    for (unsigned i = 0; i < 5; ++i)
        if (!apart((uintptr_t)p[i], n[i], (uintptr_t)g->wb_pool, sizeof(*g->wb_pool)) ||
            !apart((uintptr_t)p[i], n[i], (uintptr_t)g->nc_pool, sizeof(*g->nc_pool))) return 0;
    struct device_memory_lease first; struct device_memory_extent extent;
    if (!device_memory_acquire(g->wb_pool, owner,
        ((size_t)profile->render.global_maximum_pages + 28) * 4096, &first, &extent)) return 0;
    graphics_owner = s; s->self = (uintptr_t)s; s->firmware = g; s->compute = compute; s->service = service;
    s->owner = owner; s->profile = *profile; s->render = r; s->backing_lease = first; s->backing_extent = extent;
    s->phase = A733_GPU_GRAPHICS_ACQUIRED;
    device_memory_lookup(g->wb_pool, first)->state = DEVICE_MEMORY_DEVICE;
    if (!acquire(g->wb_pool, owner, r.payload_bytes, &s->payload_lease, &s->payload_extent, 1) ||
        !acquire(g->nc_pool, owner, 8192 + rounded(profile->render.global_maximum_pages * 4u),
                 &s->shared_lease, &s->shared_extent, 1)) return construction_failed(s);
    size_t off = 0;
    for (unsigned i = 0; i < PVR_RENDER_REGIONS; ++i) {
        s->target.gpu[i] = GPU_BASE + off;
        s->maps[i] = (struct pvr_gpu_mapping){GPU_BASE + off, s->payload_extent.physical + off,
            r.allocation[i], 12, PVR_GPU_VM_SLC_BYPASS};
        off += r.allocation[i];
    }
    for (unsigned i = 0; i < 2; ++i) {
        unsigned maximum = i ? profile->render.global_maximum_pages : 28;
        s->lists[i] = (struct pvr_free_list_config){{PVR_GPU_RESOURCE_FREELIST, i + 1, owner, i + 1},
            TABLE_BASE + i * 4096u, FW_BASE + i * 64u,
            i ? profile->initial_pages : 28, maximum, i ? profile->grow_pages : 0,
            i ? profile->threshold : 0, i ? s->global_pages : s->local_pages};
        s->maps[PVR_RENDER_REGIONS + i] = (struct pvr_gpu_mapping){TABLE_BASE + i * 4096u,
            s->shared_extent.physical + 4096 + i * 4096u, rounded(maximum * 4u), 12,
            PVR_GPU_VM_FW_PROTECT | PVR_GPU_VM_SLC_BYPASS};
    }
    if (!pvr_gpu_vm_require(s->maps, A733_GPU_GRAPHICS_MAPS, &s->requirements) ||
        s->requirements.table_pages > A733_GPU_GRAPHICS_TABLE_PAGES ||
        !acquire(g->wb_pool, owner, s->requirements.table_bytes, &s->table_lease, &s->table_extent, 0)) return construction_failed(s);
    for (unsigned i = 0; i < s->requirements.table_pages; ++i)
        s->table_pages[i] = s->table_extent.physical + i * 4096u;
    struct pvr_gpu_vm_tables tables = {(void *)s->table_extent.cpu, s->table_extent.bytes,
        s->table_pages, s->requirements.table_pages};
    if (!a733_gpu_vm_memory_init(&s->vm, &tables, (void *)(s->shared_extent.cpu + 1024), s->shared_extent.physical + 1024) ||
        a733_gpu_vm_memory_build(&s->vm, s->maps, A733_GPU_GRAPHICS_MAPS) != A733_GPU_VM_MEMORY_OK) return construction_failed(s);
    s->target.common = (void *)(s->shared_extent.cpu + 128); s->target.common_firmware = FW_BASE + 128;
    s->target.local_firmware = FW_BASE; s->target.global_firmware = FW_BASE + 64;
    for (unsigned i = 0; i < 2; ++i) {
        s->target.data[i] = (void *)(s->shared_extent.cpu + 256 + i * 384);
        s->target.data_firmware[i] = FW_BASE + 256 + i * 384;
    }
    if (!pvr_render_target_build(&s->profile.render, &s->target)) return construction_failed(s);
    s->firmware_page = s->shared_extent.physical;
    s->object = (struct pvr_mips_object){&s->firmware_page, 1, FW_BASE, PVR_MIPS_UNCACHED};
    struct pvr_mips_object objects[PVR_FW_RESOURCE_COUNT + PVR_COMPUTE_SERVICE_OBJECTS + 1];
    memcpy(objects, g->mappings, sizeof(g->mappings));
    memcpy(objects + PVR_FW_RESOURCE_COUNT, compute->compute.objects, sizeof(compute->compute.objects));
    objects[PVR_FW_RESOURCE_COUNT + PVR_COMPUTE_SERVICE_OBJECTS] = s->object;
    struct pvr_mips_boot boot;
    if (!pvr_mips_prepare(g->config.firmware, g->config.firmware_bytes, g->config.firmware_buffers,
                          objects, sizeof(objects) / sizeof(objects[0]), &boot)) return construction_failed(s);
    g->config.free_lists = (struct pvr_gpu_service_free_list_config){s->lists, 2, 28, profile->operation_timeout_us, operations};
    g->config.context = s; g->config.graphics = s; s->phase = A733_GPU_GRAPHICS_BOUND;
    if (!a733_gpu_graphics_memory_validate(s, &g->config, service)) return construction_failed(s);
    return 1;
}
int a733_gpu_graphics_memory_begin(struct a733_gpu_graphics_memory *s)
{
    if (!live(s) || s->phase != A733_GPU_GRAPHICS_BOUND) return -1;
    if (!parents(s) || s->service->free_lists.phase != PVR_FREE_LIST_IDLE ||
        s->service->free_lists.count != 2 || s->acquired[0] != 28 ||
        s->acquired[1] != s->profile.initial_pages) return fault(s);
    s->cursor = 0; s->phase = A733_GPU_GRAPHICS_DATA; return 1;
}
int a733_gpu_graphics_memory_poll(struct a733_gpu_graphics_memory *s)
{
    if (!live(s)) return -1;
    if (s->phase == A733_GPU_GRAPHICS_READY) return 1;
    if (!parents(s)) return fault(s);
    if (s->phase == A733_GPU_GRAPHICS_DATA) {
        if (s->cursor < s->payload_extent.bytes / 4096) {
            uintptr_t cpu = s->payload_extent.cpu + s->cursor * 4096u;
            if (!mapped(cpu, 4096, 0xff) || !aarch64_device_cache(AARCH64_CACHE_CLEAN, cpu, 4096)) return fault(s);
            ++s->cursor; return 0;
        }
        if (!mapped(s->shared_extent.cpu, s->shared_extent.bytes, 0x44)) return fault(s);
        __asm__ volatile("dsb sy" ::: "memory");
        if (a733_gpu_vm_memory_publish_begin(&s->vm) != A733_GPU_VM_MEMORY_AGAIN) return fault(s);
        s->phase = A733_GPU_GRAPHICS_TABLES; return 0;
    }
    if (s->phase != A733_GPU_GRAPHICS_TABLES) return fault(s);
    enum a733_gpu_vm_memory_result r = a733_gpu_vm_memory_poll(&s->vm);
    if (r == A733_GPU_VM_MEMORY_AGAIN) return 0;
    if (r != A733_GPU_VM_MEMORY_OK) return fault(s);
    s->phase = A733_GPU_GRAPHICS_READY; return 1;
}
