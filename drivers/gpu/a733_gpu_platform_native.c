#include "a733_gpu_platform_native.h"
#include "a733_mmio.h"
#include "boot.h"

int a733_gpu_platform_native_now(void *context, uint64_t *out)
{ (void)context; *out = counter() / 24; return 1; }
static uintptr_t address(enum a733_gpu_power_bank bank, uint32_t off)
{
    if (bank == A733_GPU_CCU && (off == 0x0e0 || off == 0x5c0 || off == 0x5e0 || off == 0xb20 || off == 0xb24))
        return 0x02002000u + off;
    if (bank == A733_GPU_R_CCU && off == 0x1ac) return 0x07010000u + off;
    if (bank == A733_GPU_CORE && (off == 0 || off == 8 || off == 0x14 || off == 0x20 || off == 0xfb0))
        return 0x07066000u + off;
    /* GPU_TOP (PCK domain 5) belongs to the power-domain owner. */
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
static int update_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off,
                       uint32_t clear, uint32_t set, uint64_t deadline)
{
    struct a733_gpu_platform_native *n = context;
    /* The writes of drivers/gpu/a733_gpu_power.c only. PLL_GPU0: one control
     * bit set or cleared (enable 31, LDO 30, lock enable 29, output gate 27),
     * or N 84 and P0 2 (bits 15:8 and 22:20) for 1008 MHz. The GPU clock:
     * gate with the update bit, the update bit alone, or gate off with it. */
    int allowed = (bank == A733_GPU_CCU && off == 0x0e0 &&
         ((clear == 0 && (set == 0x80000000 || set == 0x40000000 || set == 0x20000000 || set == 0x08000000)) ||
          (set == 0 && (clear == 0x80000000 || clear == 0x40000000 || clear == 0x20000000 || clear == 0x08000000)) ||
          (clear == 0x0070ff00 && set == 0x00105300))) ||
        (bank == A733_GPU_CCU && off == 0xb20 &&
         ((clear == 0 && set == 0x88000000) || (clear == 0 && set == 0x08000000) ||
          (clear == 0x80000000 && set == 0x08000000) ||
          (clear == 0x0700000f && (set == 0x08000000 || set == 0x0a000000)))) ||
        (bank == A733_GPU_CCU && off == 0xb24 &&
         ((clear == 0 && (set == 1 || set == 0x10000)) || (set == 0 && (clear == 1 || clear == 0x10000)))) ||
        (bank == A733_GPU_CORE && clear == UINT32_MAX &&
         ((off == 0 && (set == 0 || set == 8)) || (off == 0x20 && set == 0)));
    if (!allowed || !enter(n, deadline)) return 0;
    uint32_t old = 0;
    if (clear != UINT32_MAX) (void)read_reg(n, bank, off, &old);
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
    *n = (struct a733_gpu_platform_native){.operations = {a733_gpu_platform_native_now, read_reg, update_reg},
                                          .locks = *locks, .initialized = 1};
    return 1;
}
