#include "a733_dma_native.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_range.h"

int a733_dma_native_translate(void *unused, uintptr_t address, int write,
                              struct dma_translation *result)
{
    (void)unused;
    struct aarch64_translation t;
    if (!result || !aarch64_device_translate(address, write, &t)) return 0;
    *result = (struct dma_translation){t.physical, t.attributes, t.shareability};
    return 1;
}

static _Noreturn void fail(struct a733_dma_native *n)
{
    if (n && n->fatal) n->fatal(n->fatal_cookie);
    __builtin_trap();
}

int a733_dma_native_init(struct a733_dma_native *n, uintptr_t register_cpu,
                         uint64_t register_physical, const struct dma_memory_window *windows,
                         size_t count, const struct dma_fifo_port *ports, size_t port_count,
                         void (*fatal)(void *), void *cookie)
{
    if (!n || n->initialized || !fatal || !windows || !count || count > DMA_MEMORY_MAX_WINDOWS ||
        (register_cpu & 4095) || (register_physical & 4095) ||
        register_cpu > UINTPTR_MAX - 0x2000 || register_physical > UINT64_MAX - 0x2000 ||
        !aarch64_device_cache_stride()) return 0;
    for (size_t i = 0; i < count; ++i) {
        const struct dma_memory_window *w = &windows[i];
        if (!w->bytes || w->cpu > UINTPTR_MAX - w->bytes ||
            w->physical > UINT64_MAX - w->bytes) return 0;
        if (a733_extents_overlap_prechecked(w->cpu, w->bytes, (uintptr_t)n, sizeof(*n)) ||
            a733_extents_overlap_prechecked(w->cpu, w->bytes, register_cpu, 0x2000) ||
            a733_extents_overlap_prechecked(w->physical, w->bytes, register_physical, 0x2000)) return 0;
    }
    /* Both register pages are Device memory at the expected physical address. */
    static const struct a733_mapping_rule registers = {
        A733_MAPPING_WRITE, 2, {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}, {A733_MAIR_DEVICE_NGNRE, A733_SH_ANY}}, 0, 1
    };
    if (!a733_mapping_check_stride(aarch64_device_translate, register_cpu, register_physical,
                                   0x2000, 4096, &registers)) return 0;
    if (!dma_memory_init(&n->memory, windows, count, ports, port_count,
                         a733_dma_native_translate, NULL)) return 0;
    n->registers = register_cpu;
    n->fatal = fatal;
    n->fatal_cookie = cookie;
    n->initialized = 1;
    return 1;
}

static void check_register(struct a733_dma_native *n, uintptr_t address)
{
    if (!n || !n->initialized || (address & 3) || address < n->registers ||
        address - n->registers >= 0x2000) fail(n);
}
static uint32_t native_read(void *cookie, uintptr_t address)
{
    struct a733_dma_native *n = cookie;
    check_register(n, address);
    return a733_mmio_read32_dsb_sy_after(address);
}
static void native_write(void *cookie, uintptr_t address, uint32_t value)
{
    struct a733_dma_native *n = cookie;
    check_register(n, address);
    a733_mmio_write32_dsb_sy_both(address, value);
}
static void native_barrier(void *cookie)
{ (void)cookie; a733_mmio_dsb_sy(); }
static uint64_t native_ticks(void *cookie)
{
    (void)cookie;
    uint64_t result;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(result) :: "memory");
    return result;
}
static int native_mapping(void *cookie, const struct a733_dma_mapping *m)
{
    struct a733_dma_native *n = cookie;
    return n && n->initialized && aarch64_device_cache_stride() && dma_memory_accept(&n->memory, m);
}
static int native_fifo(void *cookie, const struct a733_dma_endpoint *e, enum a733_dma_access access)
{
    struct a733_dma_native *n = cookie;
    return n && n->initialized && dma_memory_fifo(&n->memory, e, access);
}
static void native_cache(void *cookie, enum a733_dma_cache operation, void *cpu, size_t bytes)
{
    struct a733_dma_native *n = cookie;
    enum aarch64_cache_op op;
    switch (operation) {
    case A733_DMA_CLEAN: op = AARCH64_CACHE_CLEAN; break;
    case A733_DMA_CLEAN_INVALIDATE: op = AARCH64_CACHE_CLEAN_INVALIDATE; break;
    case A733_DMA_INVALIDATE: op = AARCH64_CACHE_INVALIDATE; break;
    default: fail(n);
    }
    if (!n || !n->initialized || !aarch64_device_cache(op, (uintptr_t)cpu, bytes)) fail(n);
}

const struct a733_dma_operations a733_dma_native_operations = {
    native_read, native_write, native_cache, native_barrier, native_ticks, native_mapping, native_fifo
};
