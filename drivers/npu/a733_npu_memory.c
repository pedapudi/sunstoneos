#include "a733_npu_memory.h"

#define PAGE 4096u
#define TABLE 16384u
#define PA_LIMIT (UINT64_C(1) << 40)
#define VA_LIMIT (UINT64_C(1) << 32)

static int overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return an && bn && (a <= b ? b - a < an : a - b < bn);
}
static int pointer(const void *p, size_t n)
{
    return p && n && n - 1 <= UINTPTR_MAX - (uintptr_t)p;
}
static int translated(const struct a733_npu_memory *m, uintptr_t cpu,
                       uint64_t pa, size_t bytes, int write)
{
    for (size_t offset = 0; offset < bytes; offset += PAGE) {
        struct a733_npu_translation t;
        if (!m->ops.translate(m->cookie, cpu + offset, 0, &t) ||
            t.physical != pa + offset || t.attributes != 0xff || t.shareability != 3)
            return 0;
        if (write && (!m->ops.translate(m->cookie, cpu + offset, 1, &t) ||
            t.physical != pa + offset || t.attributes != 0xff || t.shareability != 3))
            return 0;
    }
    return 1;
}
static int live_mappings(const struct a733_npu_memory *m)
{
    if (!translated(m, m->arena, m->image.physical, m->capacity, 1)) return 0;
    for (size_t i = 0; i < m->count; ++i) {
        const struct a733_npu_buffer *b = &m->buffers[i];
        if (!translated(m, b->cpu, b->physical, b->bytes,
                        b->role == A733_NPU_BUFFER_WRITE)) return 0;
    }
    return 1;
}

int a733_npu_memory_init(struct a733_npu_memory *m, void *arena, size_t capacity,
                         uint64_t physical, const struct a733_npu_buffer *buffers,
                         size_t count, const struct a733_npu_memory_ops *ops, void *cookie)
{
    if (!pointer(m, sizeof(*m)) || m->state != A733_NPU_MEMORY_EMPTY ||
        !count || count > A733_NPU_MEMORY_MAX_BUFFERS ||
        !pointer(buffers, count * sizeof(*buffers)) || !pointer(ops, sizeof(*ops)) ||
        !ops->translate || !ops->cache || !ops->barrier || !pointer(arena, capacity) ||
        ((uintptr_t)arena & (PAGE - 1)) || (physical & (PAGE - 1)) ||
        capacity < PAGE || capacity > A733_NPU_MMU_MAX_BYTES || (capacity & 63u) ||
        physical >= PA_LIMIT || capacity > PA_LIMIT - physical ||
        (physical >> 32) != ((physical + capacity - 1) >> 32)) return 0;
    size_t input_bytes = count * sizeof(*buffers);
    if (overlap((uintptr_t)m, sizeof(*m), (uintptr_t)arena, capacity) ||
        overlap((uintptr_t)m, sizeof(*m), (uintptr_t)buffers, input_bytes) ||
        overlap((uintptr_t)m, sizeof(*m), (uintptr_t)ops, sizeof(*ops)) ||
        overlap((uintptr_t)arena, capacity, (uintptr_t)buffers, input_bytes) ||
        overlap((uintptr_t)arena, capacity, (uintptr_t)ops, sizeof(*ops))) return 0;

    struct a733_npu_memory next = {0};
    struct a733_npu_region regions[A733_NPU_MEMORY_MAX_BUFFERS];
    next.count = count; next.arena = (uintptr_t)arena; next.capacity = capacity;
    next.image.physical = physical; next.ops = *ops; next.cookie = cookie;
    unsigned bootstraps = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct a733_npu_buffer *b = &buffers[i];
        if (!pointer((void *)b->cpu, b->bytes) || !b->device ||
            ((b->cpu | b->physical | b->bytes | b->device) & (PAGE - 1)) ||
            (unsigned)b->role > A733_NPU_BUFFER_CONSTANT ||
            b->physical >= PA_LIMIT || b->bytes > PA_LIMIT - b->physical ||
            b->bytes > VA_LIMIT - b->device ||
            overlap(b->cpu, b->bytes, (uintptr_t)m, sizeof(*m)) ||
            overlap(b->cpu, b->bytes, (uintptr_t)arena, capacity) ||
            overlap(b->physical, b->bytes, physical, capacity) ||
            overlap(b->device, b->bytes, 0x400000, 0x80000)) return 0;
        if (b->role == A733_NPU_BUFFER_BOOTSTRAP) ++bootstraps;
        if ((b->role == A733_NPU_BUFFER_COMMAND || b->role == A733_NPU_BUFFER_BOOTSTRAP) &&
            b->bytes < A733_NPU_FETCH_BYTES) return 0;
        for (size_t j = 0; j < i; ++j) {
            const struct a733_npu_buffer *p = &buffers[j];
            if (overlap(b->cpu, b->bytes, p->cpu, p->bytes) ||
                overlap(b->physical, b->bytes, p->physical, p->bytes) ||
                overlap(b->device, b->bytes, p->device, p->bytes)) return 0;
        }
        /* Insertion sort keeps canonical table validation linear in pages. */
        size_t at = i;
        while (at && next.buffers[at - 1].device > b->device) {
            next.buffers[at] = next.buffers[at - 1]; --at;
        }
        next.buffers[at] = *b;
    }
    if (bootstraps != 1 || !live_mappings(&next)) return 0;
    for (size_t i = 0; i < count; ++i) {
        const struct a733_npu_buffer *b = &next.buffers[i];
        regions[i] = (struct a733_npu_region){b->physical, b->bytes, b->device,
            A733_NPU_PAGE_4K, b->role == A733_NPU_BUFFER_WRITE};
    }
    if (!a733_npu_mmu_build(arena, capacity, physical, regions, count, &next.image)) return 0;
    next.state = A733_NPU_MEMORY_IDLE;
    *m = next;
    return 1;
}

static int output_private(const struct a733_npu_memory *m, const void *p, size_t n)
{
    if (!pointer(p, n) || overlap((uintptr_t)p, n, (uintptr_t)m, sizeof(*m)) ||
        overlap((uintptr_t)p, n, m->arena, m->capacity)) return 0;
    for (size_t i = 0; i < m->count; ++i)
        if (overlap((uintptr_t)p, n, m->buffers[i].cpu, m->buffers[i].bytes)) return 0;
    return 1;
}
int a733_npu_memory_command(const struct a733_npu_memory *m, uint32_t device,
                            struct a733_npu_command *cmd)
{
    if (!m || m->state != A733_NPU_MEMORY_IDLE || !output_private(m, cmd, sizeof(*cmd))) return 0;
    for (size_t i = 0; i < m->count; ++i) {
        const struct a733_npu_buffer *b = &m->buffers[i];
        if (b->device != device || (b->role != A733_NPU_BUFFER_COMMAND &&
            b->role != A733_NPU_BUFFER_BOOTSTRAP)) continue;
        *cmd = (struct a733_npu_command){device, b->bytes, (void *)b};
        return 1;
    }
    return 0;
}
int a733_npu_memory_group(struct a733_npu_memory *m, uint32_t device, unsigned group)
{
    if (!m || m->state != A733_NPU_MEMORY_IDLE || m->booted || !group || group > UINT8_MAX)
        return 0;
    for (size_t i = 0; i < m->count; ++i) {
        if (m->buffers[i].device != device) continue;
        if (m->buffers[i].role == A733_NPU_BUFFER_BOOTSTRAP) return 0;
        m->groups[i] = (uint8_t)group;
        return 1;
    }
    return 0;
}
/* Job maintenance scope: shared group 0 plus the active command's group. */
static int maintained(const struct a733_npu_memory *m, size_t i, size_t command)
{
    return !m->groups[i] || m->groups[i] == m->groups[command];
}
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int canonical(const struct a733_npu_memory *m)
{
    /* Independently inspect every hardware-visible byte, including unused
     * entries and root padding. Do not trust a hash or an image descriptor. */
    const uint8_t *arena = (const void *)m->arena;
    size_t used = PAGE, region = 0;
    unsigned tables = 0;
    for (unsigned root = 0; root < 256; ++root) {
        uint64_t first = (uint64_t)root << 24, end = first + (1u << 24);
        while (region < m->count &&
               (uint64_t)m->buffers[region].device + m->buffers[region].bytes <= first) ++region;
        if (region == m->count || m->buffers[region].device >= end) {
            if (word(arena + root * 4u) != 2) return 0;
            continue;
        }
        if (used > m->capacity || TABLE > m->capacity - used ||
            word(arena + root * 4u) != ((uint32_t)(m->image.physical + used) | 1u)) return 0;
        for (unsigned slot = 0; slot < PAGE; ++slot) {
            uint64_t va = first + (uint64_t)slot * PAGE;
            while (region < m->count &&
                   (uint64_t)m->buffers[region].device + m->buffers[region].bytes <= va) ++region;
            uint32_t expected = 2;
            if (region < m->count && m->buffers[region].device <= va) {
                const struct a733_npu_buffer *b = &m->buffers[region];
                uint64_t pa = b->physical + va - b->device;
                expected = (uint32_t)pa | (uint32_t)((pa >> 32) << 4) | 3u |
                           (b->role == A733_NPU_BUFFER_WRITE ? 4u : 0);
            }
            if (word(arena + used + slot * 4u) != expected) return 0;
        }
        used += TABLE; ++tables;
    }
    for (size_t i = 1024; i < PAGE; ++i) if (arena[i]) return 0;
    return m->image.bytes == used && m->image.tables == tables &&
        m->image.pd_entry == ((uint32_t)((m->image.physical >> 12) << 4) | 1u);
}
static int same_command(const struct a733_npu_command *a, const struct a733_npu_command *b)
{
    return a && b && a->device == b->device && a->readable_bytes == b->readable_bytes &&
           a->token == b->token;
}
static int fault(struct a733_npu_memory *m)
{
    m->state = A733_NPU_MEMORY_FAULT;
    return 0;
}
int a733_npu_memory_publish(struct a733_npu_memory *m, const struct a733_npu_mmu_image *image,
                            const struct a733_npu_command *cmd, int bootstrap)
{
    if (!m || m->state == A733_NPU_MEMORY_EMPTY || m->state == A733_NPU_MEMORY_FAULT) return 0;
    if (m->state != A733_NPU_MEMORY_IDLE || !image || !cmd ||
        (bootstrap != 0 && bootstrap != 1) || image->physical != m->image.physical ||
        image->bytes != m->image.bytes || image->pd_entry != m->image.pd_entry ||
        image->tables != m->image.tables) return fault(m);
    size_t found = m->count;
    for (size_t i = 0; i < m->count; ++i)
        if (cmd->token == &m->buffers[i]) { found = i; break; }
    if (found == m->count) return fault(m);
    const struct a733_npu_buffer *b = &m->buffers[found];
    if (b->role != (bootstrap ? A733_NPU_BUFFER_BOOTSTRAP : A733_NPU_BUFFER_COMMAND) ||
        cmd->device != b->device || cmd->readable_bytes != b->bytes ||
        (!bootstrap && !m->booted) || !live_mappings(m) || !canonical(m)) return fault(m);
    m->active = *cmd; m->active_index = found; m->state = A733_NPU_MEMORY_DEVICE;
    /* Validate everything before publishing any range. A partial failure
     * retains the complete session even if the controller never triggers.
     * Immutable tables, streams and CONSTANT data were cleaned by the
     * bootstrap publication; canonical() still checks the tables per job. */
    if (bootstrap && !m->ops.cache(m->cookie, A733_NPU_CACHE_CLEAN, m->arena, m->image.bytes))
        return fault(m);
    for (size_t i = 0; i < m->count; ++i) {
        b = &m->buffers[i];
        if (!bootstrap && ((b->role != A733_NPU_BUFFER_READ && b->role != A733_NPU_BUFFER_WRITE) ||
                           !maintained(m, i, found)))
            continue;
        enum a733_npu_cache_op op = b->role == A733_NPU_BUFFER_WRITE ?
            A733_NPU_CACHE_CLEAN_INVALIDATE : A733_NPU_CACHE_CLEAN;
        if (!m->ops.cache(m->cookie, op, b->cpu, b->bytes)) return fault(m);
    }
    m->ops.barrier(m->cookie);
    if (bootstrap) m->booted = 1;
    return 1;
}
int a733_npu_memory_finish(struct a733_npu_memory *m, const struct a733_npu_command *cmd)
{
    if (!m || m->state == A733_NPU_MEMORY_EMPTY || m->state == A733_NPU_MEMORY_FAULT) return 0;
    if (m->state != A733_NPU_MEMORY_DEVICE || !same_command(cmd, &m->active) ||
        !live_mappings(m)) return fault(m);
    const struct a733_npu_buffer *active = m->active.token;
    int bootstrap = active->role == A733_NPU_BUFFER_BOOTSTRAP;
    for (size_t i = 0; i < m->count; ++i) {
        const struct a733_npu_buffer *b = &m->buffers[i];
        if (b->role == A733_NPU_BUFFER_WRITE && (bootstrap || maintained(m, i, m->active_index)) &&
            !m->ops.cache(m->cookie, A733_NPU_CACHE_INVALIDATE, b->cpu, b->bytes)) return fault(m);
    }
    m->ops.barrier(m->cookie);
    m->active = (struct a733_npu_command){0}; m->state = A733_NPU_MEMORY_IDLE;
    return 1;
}
