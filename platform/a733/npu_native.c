#include "a733_npu_native.h"
#include "aarch64_device_memory.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "a733_mapping_check.h"

static int ready(const struct a733_npu_native *n)
{ return n && n->initialized && !n->failed && aarch64_device_cache_stride(); }
static int refuse(struct a733_npu_native *n)
{
    if (n) {
        n->failed = 1;
        if (n->initialized) n->memory.state = A733_NPU_MEMORY_FAULT;
    }
    return 0;
}
static _Noreturn void fatal(struct a733_npu_native *n)
{
    refuse(n);
    if (n && n->fatal) n->fatal(n->fatal_cookie);
    __builtin_trap();
}
static int translate(void *unused, uintptr_t cpu, int write, struct aarch64_translation *out)
{
    (void)unused;
    return aarch64_device_translate(cpu, write, out);
}
static int cache(void *cookie, enum aarch64_cache_op operation, uintptr_t cpu, size_t bytes)
{
    struct a733_npu_native *n = cookie;
    return ready(n) && aarch64_device_cache(operation, cpu, bytes) ? 1 : refuse(n);
}
static void barrier(void *cookie)
{
    struct a733_npu_native *n = cookie;
    if (!ready(n)) fatal(n);
    a733_mmio_dsb_sy();
}
static uint64_t frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}
static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(value) :: "memory");
    return value;
}
static int device_mapping(uintptr_t cpu, uint64_t physical, int write)
{
    static const struct a733_mapping_rule read = {A733_MAPPING_READ, 2,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}, {A733_MAIR_DEVICE_NGNRE, A733_SH_ANY}}, 0, 1};
    static const struct a733_mapping_rule written = {A733_MAPPING_WRITE, 2,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}, {A733_MAIR_DEVICE_NGNRE, A733_SH_ANY}}, 0, 1};
    return a733_mapping_check(aarch64_device_translate, cpu, physical, write ? &written : &read);
}

int a733_npu_native_init(struct a733_npu_native *n, uintptr_t register_cpu,
                         uint64_t register_physical, void *arena, size_t capacity,
                         uint64_t arena_physical, const struct a733_npu_buffer *buffers,
                         size_t count, void (*on_fatal)(void *), void *cookie)
{
    if (!n || n->initialized || n->failed || !on_fatal || !buffers || !count ||
        count > A733_NPU_MEMORY_MAX_BUFFERS || (uintptr_t)n > UINTPTR_MAX - sizeof(*n) ||
        (uintptr_t)buffers > UINTPTR_MAX - count * sizeof(*buffers) ||
        !register_cpu || (register_cpu & 4095) || register_cpu > UINTPTR_MAX - 4096 ||
        (register_physical & 4095) || register_physical > (UINT64_C(1) << 40) - 4096 ||
        !aarch64_device_cache_stride()) return 0;
    if (a733_extents_intersect((uintptr_t)n, sizeof(*n), (uintptr_t)arena, capacity) ||
        a733_extents_intersect((uintptr_t)n, sizeof(*n), (uintptr_t)buffers, count * sizeof(*buffers)) ||
        a733_extents_intersect((uintptr_t)n, sizeof(*n), register_cpu, 4096) ||
        a733_extents_intersect((uintptr_t)arena, capacity, register_cpu, 4096) ||
        a733_extents_intersect(arena_physical, capacity, register_physical, 4096)) return 0;
    for (size_t i = 0; i < count; ++i) {
        const struct a733_npu_buffer *b = &buffers[i];
        if (a733_extents_intersect(b->cpu, b->bytes, (uintptr_t)n, sizeof(*n)) ||
            a733_extents_intersect(b->cpu, b->bytes, register_cpu, 4096) ||
            a733_extents_intersect(b->physical, b->bytes, register_physical, 4096)) return 0;
    }
    if (!device_mapping(register_cpu, register_physical, 0) ||
        !device_mapping(register_cpu, register_physical, 1)) return 0;
    uint64_t hz = frequency();
    struct counter_clock clock = {0};
    if (!hz || hz > UINT32_MAX || !counter_clock_start(&clock, (uint32_t)hz, counter())) return 0;
    const struct a733_npu_memory_ops ops = {translate, cache, barrier};
    if (!a733_npu_memory_init(&n->memory, arena, capacity, arena_physical, buffers, count, &ops, n)) return 0;
    n->clock = clock; n->registers = register_cpu; n->register_physical = register_physical;
    n->fatal = on_fatal; n->fatal_cookie = cookie; n->initialized = 1;
    return 1;
}

static int register_access(struct a733_npu_native *n, unsigned offset, int write)
{
    if (!ready(n) || (offset & 3) || offset >= 4096) return refuse(n);
    return device_mapping(n->registers + offset, n->register_physical + offset, write) ?
           1 : refuse(n);
}
static int read32(void *cookie, unsigned offset, uint32_t *out)
{
    struct a733_npu_native *n = cookie;
    if (!out) return refuse(n);
    if (!register_access(n, offset, 0)) return 0;
    *out = a733_mmio_read32_dsb_sy_after(n->registers + offset);
    return 1;
}
static int write32(void *cookie, unsigned offset, uint32_t value)
{
    struct a733_npu_native *n = cookie;
    if (!register_access(n, offset, 1)) return 0;
    a733_mmio_write32_dsb_sy_both(n->registers + offset, value);
    return 1;
}
static uint64_t microseconds(void *cookie)
{
    struct a733_npu_native *n = cookie;
    if (!ready(n) || frequency() != n->clock.frequency ||
        !counter_clock_sample(&n->clock, counter())) fatal(n);
    return n->clock.microseconds;
}
static void delay(void *cookie, unsigned us)
{
    struct a733_npu_native *n = cookie;
    if (us > 1000) fatal(n);
    uint64_t start = microseconds(n);
    for (unsigned samples = 0; samples < 1048576; ++samples) {
        if (microseconds(n) - start >= us) return;
        __asm__ volatile("yield" ::: "memory");
    }
    fatal(n);
}
static int publish(void *cookie, const struct a733_npu_mmu_image *image,
                   const struct a733_npu_command *cmd, int bootstrap)
{
    struct a733_npu_native *n = cookie;
    return ready(n) && a733_npu_memory_publish(&n->memory, image, cmd, bootstrap) ? 1 : refuse(n);
}
static int finish(void *cookie, const struct a733_npu_command *cmd, int canceled)
{
    (void)canceled; /* Both completion paths already require full bus idle. */
    struct a733_npu_native *n = cookie;
    return ready(n) && a733_npu_memory_finish(&n->memory, cmd) ? 1 : refuse(n);
}
const struct a733_npu_ops a733_npu_native_operations = {
    read32, write32, microseconds, delay, barrier, publish, finish
};
