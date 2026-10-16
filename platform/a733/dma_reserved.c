#include "a733_dma_reserved.h"

static int outside(const struct device_memory_pool *p, const void *ptr, size_t n)
{
    uintptr_t a = (uintptr_t)ptr;
    return ptr && n && a <= UINTPTR_MAX - n &&
        !(a < p->backing.cpu + p->backing.bytes && p->backing.cpu < a + n);
}

static int disjoint(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= UINTPTR_MAX - an && y <= UINTPTR_MAX - bn &&
        !(x < y + bn && y < x + an);
}
static int metadata(const struct a733_dma_reserved *s, const void *p, size_t n)
{
    return outside(s->pool, p, n) && disjoint(p, n, s, sizeof(*s)) &&
        disjoint(p, n, s->pool, sizeof(*s->pool)) &&
        disjoint(p, n, s->controller, sizeof(*s->controller));
}

int a733_dma_reserved_init(struct a733_dma_reserved *s, struct device_memory_pool *p,
                           struct a733_dma_controller *c, unsigned channel, uint64_t owner)
{
    if (!s || s->initialized || !p || !p->initialized || !c || !c->initialized || !owner ||
        channel >= A733_DMA_CHANNELS || !outside(p, s, sizeof(*s)) ||
        !outside(p, c, sizeof(*c)) || !disjoint(s, sizeof(*s), c, sizeof(*c)) ||
        !disjoint(s, sizeof(*s), p, sizeof(*p)) || !disjoint(c, sizeof(*c), p, sizeof(*p)) ||
        !c->channels[channel].claimed ||
        c->channels[channel].transfer || c->channels[channel].status != A733_DMA_CONTROL_OK) return 0;
    *s = (struct a733_dma_reserved){.self = (uintptr_t)s,
        .binding = {p, c, owner, channel}, .pool = p, .controller = c, .owner = owner,
                                  .channel = channel, .initialized = 1};
    return 1;
}

static int bound(const struct a733_dma_reserved *s)
{
    return s->self == (uintptr_t)s && s->pool == s->binding.pool &&
        s->controller == s->binding.controller && s->owner == s->binding.owner &&
        s->channel == s->binding.channel;
}
static enum a733_dma_control_status retain(struct a733_dma_reserved *s)
{
    s->retained = 1;
    for (size_t i = 0; i < s->lease_count && i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i) device_memory_retain(s->binding.pool, s->leases[i]);
    return A733_DMA_RECOVERY;
}

static int active_ownership(struct a733_dma_reserved *s)
{
    if (!bound(s) || s->retained || !s->lease_count ||
        s->lease_count > DEVICE_MEMORY_MAX_ALLOCATIONS ||
        s->controller->channels[s->channel].transfer != &s->transfer) return 0;
    for (size_t i = 0; i < s->lease_count; ++i) {
        struct device_memory_allocation *a = device_memory_lookup(s->pool, s->leases[i]);
        if (!a || a->state != DEVICE_MEMORY_DEVICE) return 0;
    }
    return 1;
}

static enum a733_dma_control_status settle(struct a733_dma_reserved *s,
                                            enum a733_dma_control_status status, int submitting)
{
    if (!bound(s) || status == A733_DMA_RECOVERY) return retain(s);
    if (status == A733_DMA_ACTIVE || status == A733_DMA_STOPPING) return status;
    int refused = submitting && (status == A733_DMA_INVALID || status == A733_DMA_CONFLICT);
    int terminal = status == A733_DMA_COMPLETE || status == A733_DMA_CANCELED || status == A733_DMA_FAILED;
    struct a733_dma_channel *channel = &s->controller->channels[s->channel];
    if ((!refused && !terminal) || a733_dma_owns(s->controller, &s->transfer) ||
        channel->transfer || (!refused && channel->status != status) ||
        channel->status == A733_DMA_RECOVERY) return retain(s);
    for (size_t i = 0; i < s->lease_count; ++i) {
        struct device_memory_allocation *a = device_memory_lookup(s->pool, s->leases[i]);
        if (!a || a->state != DEVICE_MEMORY_DEVICE) return retain(s);
    }
    /* The actual controller detached this copied transfer only after its idle,
     * pending-event, descriptor-restoration and destination-cache sequence.
     * This relies on that controller's physical completion contract. */
    for (size_t i = 0; i < s->lease_count; ++i)
        device_memory_lookup(s->pool, s->leases[i])->state = DEVICE_MEMORY_CPU;
    s->active = 0;
    return status;
}

static int bind(struct a733_dma_reserved *s, size_t index, struct device_memory_lease lease,
                enum a733_dma_access access, struct a733_dma_mapping *mapping)
{
    struct device_memory_extent e;
    enum device_memory_state state;
    if (lease.owner != s->owner || !device_memory_describe(s->pool, lease, &e, &state) ||
        state != DEVICE_MEMORY_CPU) return 0;
    for (size_t i = 0; i < index; ++i) if (lease.slot == s->leases[i].slot) return 0;
    s->leases[index] = lease;
    *mapping = (struct a733_dma_mapping){(void *)e.cpu, e.physical, e.bytes, access};
    return 1;
}

enum a733_dma_control_status a733_dma_reserved_submit(struct a733_dma_reserved *s,
    struct device_memory_lease descriptors, const struct a733_dma_reserved_buffer *buffers,
    size_t count, const struct a733_dma_segment *segments, size_t segment_count, uint64_t timeout)
{
    if (!s || !s->initialized) return A733_DMA_INVALID;
    if (!bound(s) || s->retained) return retain(s);
    if (!count || count > A733_DMA_RESERVED_MAPPINGS ||
        (uintptr_t)buffers % _Alignof(struct a733_dma_reserved_buffer) ||
        (uintptr_t)segments % _Alignof(struct a733_dma_segment) ||
        !segment_count || segment_count > A733_DMA_MAX_SEGMENTS || !timeout || timeout > INT64_MAX ||
        !metadata(s, buffers, count * sizeof(*buffers)) ||
        !metadata(s, segments, segment_count * sizeof(*segments))) return A733_DMA_INVALID;
    if (s->retained) return A733_DMA_RECOVERY;
    if (s->active) return A733_DMA_CONFLICT;
    if (!bind(s, 0, descriptors, A733_DMA_DESCRIPTORS, &s->transfer.descriptors)) return A733_DMA_INVALID;
    for (size_t i = 0; i < count; ++i) {
        if ((buffers[i].access != A733_DMA_READ && buffers[i].access != A733_DMA_WRITE) ||
            !bind(s, i + 1, buffers[i].lease, buffers[i].access, &s->mappings[i])) return A733_DMA_INVALID;
    }
    for (size_t i = 0; i < segment_count; ++i) s->segments[i] = segments[i];
    s->transfer.segments = s->segments;
    s->transfer.count = segment_count;
    s->transfer.mappings = s->mappings;
    s->transfer.mapping_count = count;
    s->lease_count = count + 1;
    for (size_t i = 0; i < s->lease_count; ++i)
        device_memory_lookup(s->pool, s->leases[i])->state = DEVICE_MEMORY_DEVICE;
    s->active = 1;
    return settle(s, a733_dma_submit(s->controller, s->channel, &s->transfer, timeout), 1);
}

enum a733_dma_control_status a733_dma_reserved_poll(struct a733_dma_reserved *s)
{
    if (!s || !s->initialized || !s->active) return A733_DMA_INVALID;
    if (!active_ownership(s)) return retain(s);
    return settle(s, a733_dma_poll(s->controller, s->channel), 0);
}
enum a733_dma_control_status a733_dma_reserved_cancel(struct a733_dma_reserved *s)
{
    if (!s || !s->initialized || !s->active) return A733_DMA_INVALID;
    if (!active_ownership(s)) return retain(s);
    return settle(s, a733_dma_cancel(s->controller, s->channel), 0);
}
