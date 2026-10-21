#include "dma_memory.h"
#include "aarch64_device_memory.h"

#define LIMIT (UINT64_C(1) << 35)
#define ROLES (DMA_MEMORY_ROLE(A733_DMA_READ) | DMA_MEMORY_ROLE(A733_DMA_WRITE) | DMA_MEMORY_ROLE(A733_DMA_DESCRIPTORS))
#define WIDTHS ((1u << 1) | (1u << 2) | (1u << 4))
#define BURSTS ((1u << 1) | (1u << 4) | (1u << 8) | (1u << 16))

int dma_memory_init(struct dma_memory_policy *p, const struct dma_memory_window *windows,
                    size_t count, const struct dma_fifo_port *ports, size_t port_count,
                    dma_translate_fn translate, void *cookie)
{
    if (!p || p->initialized || !windows || !count || count > DMA_MEMORY_MAX_WINDOWS ||
        port_count > DMA_MEMORY_MAX_PORTS || (port_count && !ports) || !translate) return 0;
    for (size_t i = 0; i < count; ++i) {
        const struct dma_memory_window *w = &windows[i];
        if (!w->cpu || !w->bytes || (w->cpu & 63) || (w->physical & 63) || (w->bytes & 63) ||
            w->cpu > UINTPTR_MAX - w->bytes || w->physical >= LIMIT || w->bytes > LIMIT - w->physical ||
            !w->roles || (w->roles & ~ROLES) ||
            a733_dma_overlap(w->cpu, w->bytes, (uintptr_t)p, sizeof(*p))) return 0;
        if ((w->roles & DMA_MEMORY_ROLE(A733_DMA_DESCRIPTORS)) &&
            (w->physical >= UINT64_C(0xfffff800) || w->bytes > UINT64_C(0xfffff800) - w->physical)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (a733_dma_overlap(w->cpu, w->bytes, windows[j].cpu, windows[j].bytes) ||
                a733_dma_overlap(w->physical, w->bytes, windows[j].physical, windows[j].bytes)) return 0;
    }
    for (size_t i = 0; i < port_count; ++i) {
        const struct dma_fifo_port *f = &ports[i];
        if (f->address >= LIMIT || !f->request || f->request > 63 ||
            (f->access != A733_DMA_READ && f->access != A733_DMA_WRITE) ||
            !f->widths || (f->widths & ~WIDTHS) || !f->bursts || (f->bursts & ~BURSTS)) return 0;
        unsigned width = f->widths & (1u << 4) ? 4 : f->widths & (1u << 2) ? 2 : 1;
        if (f->address % width || width > LIMIT - f->address) return 0;
        for (size_t j = 0; j < count; ++j)
            if (a733_dma_overlap(f->address, width, windows[j].physical, windows[j].bytes)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (f->request == ports[j].request && f->access == ports[j].access) return 0;
    }
    /* Copy through a temporary so input tables may reside in the output object. */
    struct dma_memory_policy result = {0};
    for (size_t i = 0; i < count; ++i) result.windows[i] = windows[i];
    for (size_t i = 0; i < port_count; ++i) result.ports[i] = ports[i];
    result.window_count = count;
    result.port_count = port_count;
    result.translate = translate;
    result.cookie = cookie;
    result.initialized = 1;
    *p = result;
    return 1;
}

int dma_memory_accept(const struct dma_memory_policy *p, const struct a733_dma_mapping *m)
{
    if (!p || !p->initialized || !m || !a733_dma_mapping_shape(m) ||
        (m->access != A733_DMA_READ && m->access != A733_DMA_WRITE && m->access != A733_DMA_DESCRIPTORS)) return 0;
    const struct dma_memory_window *window = NULL;
    for (size_t i = 0; i < p->window_count; ++i) {
        const struct dma_memory_window *w = &p->windows[i];
        uintptr_t cpu = (uintptr_t)m->cpu;
        if (cpu >= w->cpu && cpu - w->cpu < w->bytes && m->bytes <= w->bytes - (cpu - w->cpu) &&
            m->bus == w->physical + (cpu - w->cpu) && (w->roles & DMA_MEMORY_ROLE(m->access))) {
            window = w;
            break;
        }
    }
    if (!window) return 0;
    uintptr_t start = (uintptr_t)m->cpu, end = start + m->bytes;
    /* Probe every covered 4 KiB granule, including partial endpoint pages. */
    for (uintptr_t cpu = start; cpu < end;) {
        struct dma_translation translation;
        if (!p->translate(p->cookie, cpu, m->access != A733_DMA_READ, &translation) ||
            translation.physical != m->bus + (cpu - start) ||
            translation.attributes != 0xff || translation.shareability != 3) return 0;
        size_t step = 4096 - (cpu & 4095);
        if (step >= end - cpu) break;
        cpu += step;
    }
    return 1;
}

int dma_memory_fifo(const struct dma_memory_policy *p, const struct a733_dma_endpoint *e,
                    enum a733_dma_access access)
{
    if (!p || !p->initialized || !e || (access != A733_DMA_READ && access != A733_DMA_WRITE) ||
        (e->width != 1 && e->width != 2 && e->width != 4) ||
        (e->burst != 1 && e->burst != 4 && e->burst != 8 && e->burst != 16) ||
        e->address % e->width) return 0;
    for (size_t i = 0; i < p->port_count; ++i) {
        const struct dma_fifo_port *f = &p->ports[i];
        if (f->address == e->address && f->request == e->request && f->access == access &&
            (f->widths & (1u << e->width)) && (f->bursts & (1u << e->burst))) return 1;
    }
    return 0;
}
