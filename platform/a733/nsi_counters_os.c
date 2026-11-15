/* SPDX-License-Identifier: Apache-2.0
 * The platform side of the NSI counter owner (drivers/dram/a733_nsi_counters_guard.c): 32-bit MMIO with a DSB SY on
 * both sides, each on a page checked to be mapped as Device-nGnRnE, and the IRQ mask its accesses run under. */
#include "a733_nsi_counters.h"
#include "a733_mmio.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"

static int device_page(uintptr_t address)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    return a733_mapping_check(aarch64_device_translate, address, address, &device);
}
static int read32(void *context, uintptr_t address, uint32_t *v)
{
    (void)context;
    if (!a733_nsi_counter_readable(address) || !device_page(address)) return 0;
    *v = a733_mmio_read32_dsb_sy_both(address);
    return 1;
}
static int write32(void *context, uintptr_t address, uint32_t v)
{
    (void)context;
    if (!a733_nsi_counter_writable(address) || !device_page(address)) return 0;
    a733_mmio_write32_dsb_sy_both(address, v);
    return 1;
}
static uint64_t ticks(void *context)
{
    (void)context;
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t)::"memory");
    return t;
}
static const struct a733_nsi_counter_ops ops = {read32, write32, ticks};
uint64_t os_nsi_counters_mask(void)
{
    uint64_t flags;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(flags) :: "memory");
    return flags;
}
void os_nsi_counters_unmask(uint64_t flags) { __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory"); }
const struct a733_nsi_counter_ops *os_nsi_counters_ops(void) { return &ops; }
