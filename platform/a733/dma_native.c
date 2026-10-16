#include "a733_dma_native.h"

static unsigned cache_stride(void)
{
    uint64_t level, control, ctr;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, sctlr_el1\n mrs %2, ctr_el0"
                     : "=r"(level), "=r"(control), "=r"(ctr));
    if (level != 4 || (control & 5) != 5) return 0;
    return dma_cache_stride(ctr);
}

int a733_dma_native_translate(void *unused, uintptr_t address, int write,
                              struct dma_translation *result)
{
    (void)unused;
    if (!result) return 0;
    uint64_t saved_flags, saved_par, par;
#define TRANSLATE(operation) \
    __asm__ volatile("mrs %0, daif\n msr daifset, #0xf\n mrs %1, par_el1\n " \
                     operation ", %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0" \
                     : "=&r"(saved_flags), "=&r"(saved_par), "=&r"(par) : "r"(address) : "memory")
    if (write) { TRANSLATE("at s1e1w"); }
    else { TRANSLATE("at s1e1r"); }
#undef TRANSLATE
    if (par & 1) return 0;
    result->physical = (par & UINT64_C(0x000ffffffffff000)) | (address & 4095);
    result->attributes = (unsigned)(par >> 56);
    result->shareability = (unsigned)(par >> 7) & 3;
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
        !cache_stride()) return 0;
    for (size_t i = 0; i < count; ++i) {
        const struct dma_memory_window *w = &windows[i];
        if (!w->bytes || w->cpu > UINTPTR_MAX - w->bytes ||
            w->physical > UINT64_MAX - w->bytes) return 0;
        if (w->cpu < (uintptr_t)n + sizeof(*n) && (uintptr_t)n < w->cpu + w->bytes) return 0;
        if (w->cpu < register_cpu + 0x2000 && register_cpu < w->cpu + w->bytes) return 0;
        if (w->physical < register_physical + 0x2000 && register_physical < w->physical + w->bytes) return 0;
    }
    for (unsigned offset = 0; offset < 0x2000; offset += 4096) {
        struct dma_translation t;
        if (!a733_dma_native_translate(NULL, register_cpu + offset, 1, &t) ||
            t.physical != register_physical + offset || (t.attributes != 0 && t.attributes != 4)) return 0;
    }
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
    uint32_t result = *(volatile uint32_t *)address;
    __asm__ volatile("dsb sy" ::: "memory");
    return result;
}
static void native_write(void *cookie, uintptr_t address, uint32_t value)
{
    struct a733_dma_native *n = cookie;
    check_register(n, address);
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void native_barrier(void *cookie)
{ (void)cookie; __asm__ volatile("dsb sy" ::: "memory"); }
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
    return n && n->initialized && cache_stride() && dma_memory_accept(&n->memory, m);
}
static int native_fifo(void *cookie, const struct a733_dma_endpoint *e, enum a733_dma_access access)
{
    struct a733_dma_native *n = cookie;
    return n && n->initialized && dma_memory_fifo(&n->memory, e, access);
}
static void native_cache(void *cookie, enum a733_dma_cache operation, void *cpu, size_t bytes)
{
    struct a733_dma_native *n = cookie;
    unsigned stride = cache_stride();
    if (!n || !n->initialized || !stride || !cpu || !bytes ||
        ((uintptr_t)cpu & 63) || (bytes & 63) || (uintptr_t)cpu > UINTPTR_MAX - bytes ||
        (operation != A733_DMA_CLEAN && operation != A733_DMA_CLEAN_INVALIDATE &&
         operation != A733_DMA_INVALIDATE)) fail(n);
    /* Only the controller invokes this callback, after successful admission.
     * Mappings and allocation ownership must remain pinned until completion. */
    __asm__ volatile("dsb sy" ::: "memory");
    uintptr_t end = (uintptr_t)cpu + bytes;
    for (uintptr_t address = (uintptr_t)cpu; address < end; address += stride) {
        if (operation == A733_DMA_CLEAN)
            __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
        else if (operation == A733_DMA_CLEAN_INVALIDATE)
            __asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

const struct a733_dma_operations a733_dma_native_operations = {
    native_read, native_write, native_cache, native_barrier, native_ticks, native_mapping, native_fifo
};
