#include "a733_gpu_platform_native.h"
#include "a733_mmio.h"
#include "boot.h"

int a733_gpu_platform_native_now(void *context, uint64_t *out)
{ (void)context; *out = counter() / 24; return 1; }
static uintptr_t address(enum a733_gpu_power_bank bank, uint32_t off)
{
    if (bank == A733_GPU_CCU && (off == 0x5c0 || off == 0x5e0 || off == 0xb20 || off == 0xb24))
        return 0x02002000u + off;
    if (bank == A733_GPU_R_CCU && off == 0x1ac) return 0x07010000u + off;
    if (bank == A733_GPU_PCK && (off == 0x5000 || off == 0x5008 || off == 0x5170 ||
        off == 0x5174 || off == 0x5c00 || off == 0x5c04 || off == 0x5c10)) return 0x07060000u + off;
    return 0;
}
static int read_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off, uint32_t *out)
{
    (void)context;
    uintptr_t addr = address(bank, off);
    if (!addr || !out) return 0;
    *out = a733_mmio_read32_dsb_sy_both(addr);
    return 1;
}
static int enter(struct a733_gpu_platform_native *n, uint64_t deadline)
{
    uint64_t time;
    a733_gpu_platform_native_now(n, &time);
    if (time >= deadline || !n->locks.register_enter(n->locks.context, deadline)) return 0;
    a733_gpu_platform_native_now(n, &time);
    if (time < deadline) return 1;
    n->locks.register_leave(n->locks.context);
    return 0;
}
static int write_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off,
                      uint32_t value, uint64_t deadline)
{
    struct a733_gpu_platform_native *n = context;
    if (bank != A733_GPU_PCK || !((off == 0x5170 && value == 0x1f1f1f) ||
        (off == 0x5174 && value == 0x1f1f) || (off == 0x5c00 && value == 0x08080808) ||
        (off == 0x5c04 && value == 0x0808) || (off == 0x5c10 && value == 8))) return 0;
    if (!enter(n, deadline)) return 0;
    a733_mmio_write32_dsb_sy_both(address(bank, off), value);
    n->locks.register_leave(n->locks.context);
    return 1;
}
static int update_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off,
                       uint32_t clear, uint32_t set, uint64_t deadline)
{
    struct a733_gpu_platform_native *n = context;
    int allowed = (bank == A733_GPU_PCK && off == 0x5000 && clear == 15 && (set == 0 || set == 8)) ||
        (bank == A733_GPU_CCU && off == 0xb20 &&
         ((clear == 0x8700000f && set == 0x0b000000) || (clear == 0 && set == 0x88000000) ||
          (clear == 0x80000000 && set == 0x08000000))) ||
        (bank == A733_GPU_CCU && off == 0xb24 &&
         ((clear == 0 && (set == 1 || set == 0x10000)) || (set == 0 && (clear == 1 || clear == 0x10000))));
    if (!allowed || !enter(n, deadline)) return 0;
    uint32_t old;
    (void)read_reg(n, bank, off, &old);
    a733_mmio_write32_dsb_sy_after(address(bank, off), (old & ~clear) | set);
    n->locks.register_leave(n->locks.context);
    return 1;
}
int a733_gpu_platform_native_init(struct a733_gpu_platform_native *n,
                                 const struct a733_gpu_resource_locks *locks)
{
    if (!n || !locks || n->initialized || !locks->register_enter || !locks->register_leave ||
        !locks->i2c_enter || !locks->i2c_leave) return 0;
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (frequency != COUNTER_HZ) return 0;
    *n = (struct a733_gpu_platform_native){.operations = {a733_gpu_platform_native_now, read_reg, write_reg, update_reg},
                                          .locks = *locks, .initialized = 1};
    return 1;
}
