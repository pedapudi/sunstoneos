#ifndef SUNSTONE_A733_BOOT_H
#define SUNSTONE_A733_BOOT_H
#include <stdint.h>
#ifdef MONITOR_QEMU
#define GICD_BASE 0x08000000UL
#define GICR_BASE 0x080a0000UL
#else
#define GICD_BASE 0x03400000UL
#define GICR_BASE 0x03460000UL
#endif
#define GICR_SGI (GICR_BASE + 0x10000UL)
#define RESULT_BASE 0x42000000UL
#define RESULT_SIZE 16384U
#define LOG_OFFSET 256U
#define RESULT_MAGIC 0x534b4552UL
#define TICK_IRQ 30U
#define RECOVERY_IRQ 29U
#define COUNTER_HZ 24000000UL

struct boot_result {
    uint64_t magic, stage, status, log_length;
    uint64_t esr, elr, far, interrupt;
    uint64_t start_counter, end_counter, mode;
    uint64_t gicd_ctlr, gicd_typer, gicr_typer, priority_mask, reserved;
};
#define RESULT ((volatile struct boot_result *)RESULT_BASE)
static inline uint32_t mmio_read(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}
static inline void mmio_write(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}
static inline uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
    return value;
}
#endif
