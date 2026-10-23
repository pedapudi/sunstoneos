#include "boot.h"

// State is outside the BSS cleared by EL1 startup. EL1 does not write it, but
// it can: cleanup re-checks the values it uses as bounds.
static struct {
    uint32_t ready, banks, ctlr, group, modifier, enabled, configuration;
    uint32_t priorities, spi_enabled[32];
} saved __attribute__((section(".monitor_state")));
extern _Noreturn void monitor_reset(void);
static _Noreturn void finish(unsigned status);

#ifdef SUNSTONE_MEMTEST
#include "a733_remote_writers.h"
#include "memtest.h"
static int writer_read(void *context, uint64_t address, uint32_t *value)
{
    (void)context;
    *value = mmio_read(address);
    return 1;
}
#endif

static void wait_clear(uintptr_t address, uint32_t bits)
{
    uint64_t deadline = counter() + COUNTER_HZ / 10;
    while (mmio_read(address) & bits) {
        if (counter() >= deadline)
            finish(4);
    }
}

void monitor_init(void)
{
    uint64_t mode = RESULT->mode;
    volatile uint64_t *record = (volatile uint64_t *)RESULT_BASE;
    for (unsigned i = 0; i < sizeof(*RESULT) / 8; ++i)
        record[i] = 0;
    RESULT->mode = mode;
    RESULT->magic = RESULT_MAGIC;
    RESULT->status = ~0UL;
    RESULT->stage = 1;
    RESULT->start_counter = counter();
    __asm__ volatile("msr cntfrq_el0, %0" :: "r"(COUNTER_HZ));
    saved.ready = 0;
    saved.ctlr = mmio_read(GICD_BASE);
    RESULT->gicd_ctlr = saved.ctlr;
    RESULT->gicd_typer = mmio_read(GICD_BASE + 4);
    RESULT->gicr_typer = *(volatile uint64_t *)(GICR_BASE + 8);
    // Keep the two security states and identify the boot CPU redistributor.
    if ((saved.ctlr & (1U << 6)) || (RESULT->gicr_typer >> 32) != 0)
        finish(4);
#ifndef MONITOR_QEMU
    // Returning to ROM cannot undo a newly enabled affinity-routing bit.
    if ((saved.ctlr & 0x30U) != 0x30U)
        finish(4);
#endif
    saved.banks = (RESULT->gicd_typer & 31) + 1;
    saved.group = mmio_read(GICR_SGI + 0x80);
    saved.modifier = mmio_read(GICR_SGI + 0xd00);
    saved.enabled = mmio_read(GICR_SGI + 0x100);
    saved.configuration = mmio_read(GICR_SGI + 0xc04);
    saved.priorities = mmio_read(GICR_SGI + 0x41c);
    for (unsigned i = 1; i < saved.banks; ++i)
        saved.spi_enabled[i] = mmio_read(GICD_BASE + 0x100 + 4 * i);
    saved.ready = 1;
    // Disable groups before changing routing or per-interrupt configuration.
    mmio_write(GICD_BASE, saved.ctlr & ~7U);
    wait_clear(GICD_BASE, 1U << 31);
    for (unsigned i = 1; i < saved.banks; ++i)
        mmio_write(GICD_BASE + 0x180 + 4 * i, 0xffffffffU);
    wait_clear(GICD_BASE, 1U << 31);
    mmio_write(GICD_BASE, (saved.ctlr & ~7U) | 0x30U);
    wait_clear(GICD_BASE, 1U << 31);
    mmio_write(GICR_BASE + 0x14, mmio_read(GICR_BASE + 0x14) & ~2U);
    wait_clear(GICR_BASE + 0x14, 4);
    mmio_write(GICR_SGI + 0x180, 0xffffffffU);
    wait_clear(GICR_BASE, 8);
    mmio_write(GICR_SGI + 0x280, 0xffffffffU);
    mmio_write(GICR_SGI + 0x380, 0xffffffffU);
    mmio_write(GICR_SGI + 0x80, ~(1U << RECOVERY_IRQ));
    mmio_write(GICR_SGI + 0xd00, 0);
    // Both timer PPIs are level-sensitive.
    mmio_write(GICR_SGI + 0xc04, saved.configuration & ~((2U << 26) | (2U << 28)));
    *(volatile uint8_t *)(GICR_SGI + 0x400 + RECOVERY_IRQ) = 0;
    mmio_write(GICR_SGI + 0x100, 1U << RECOVERY_IRQ);
    wait_clear(GICR_BASE, 8);
    uint64_t value = 0xf;
    __asm__ volatile("msr ICC_SRE_EL3, %0\n isb" :: "r"(value) : "memory");
    value = 0;
    __asm__ volatile("msr ICC_CTLR_EL3, %0\n isb" :: "r"(value) : "memory");
    value = 0xff;
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(value));
    value = 1;
    __asm__ volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"(value));
    // Group 0 FIQs target EL3 even if the nonsecure kernel masks exceptions.
    value = 0x505;
    __asm__ volatile("msr scr_el3, %0\n isb" :: "r"(value) : "memory");
    value = 0xf;
    __asm__ volatile("msr ICC_SRE_EL2, %0\n isb\n msr ICC_SRE_EL1, %0\n isb"
                     :: "r"(value) : "memory");
    value = 1UL << 31;
    __asm__ volatile("msr hcr_el2, %0" :: "r"(value));
    value = 3;
    __asm__ volatile("msr cnthctl_el2, %0\n msr cntvoff_el2, xzr\n"
                     "msr cptr_el3, xzr" :: "r"(value));
    value = 0x33ff;
    __asm__ volatile("msr cptr_el2, %0" :: "r"(value));
    mmio_write(GICD_BASE, (saved.ctlr & ~7U) | 0x33U);
    wait_clear(GICD_BASE, 1U << 31);
    value = counter() + 2 * COUNTER_HZ;
    __asm__ volatile("msr cntps_cval_el1, %0" :: "r"(value));
    value = 1;
    __asm__ volatile("msr cntps_ctl_el1, %0\n isb" :: "r"(value) : "memory");
#ifdef SUNSTONE_MEMTEST
    // One remote-writer sample before EL1 entry (docs/remote-writers.md). The
    // memory qualification image records it and refuses EL1 entry for any
    // writer other than OR100, CE and USB0, which the user accepts as a residual.
    static const struct a733_remote_writers_ops writers = {writer_read, 0};
    uint32_t unproven = a733_remote_writers_excluded(&writers);
    ((volatile struct memtest_record *)MEMTEST_RECORD_ADDRESS)->writers_sample =
        (MEMTEST_WRITERS_MAGIC << 32) | unproven;
    if (unproven & ~MEMTEST_WRITERS_ACCEPTED)
        finish(4);
#endif
    RESULT->stage = 2;
}

static _Noreturn void finish(unsigned status)
{
    __asm__ volatile("msr daifset, #0xf\n msr cntps_ctl_el1, xzr\n"
                     "msr cntp_ctl_el0, xzr\n isb" ::: "memory");
    RESULT->status = status;
    RESULT->end_counter = counter();
    if (saved.ready) {
        // Bounded cleanup: a stalled GIC must not prevent the ROM reset.
        uint32_t live_ctlr = mmio_read(GICD_BASE);
        mmio_write(GICD_BASE, live_ctlr & ~7U);
        uint64_t deadline = counter() + COUNTER_HZ / 10;
        while ((mmio_read(GICD_BASE) & (1U << 31)) && counter() < deadline) {}
        mmio_write(GICR_SGI + 0x180, 0xffffffffU);
        deadline = counter() + COUNTER_HZ / 10;
        while ((mmio_read(GICR_BASE) & 8) && counter() < deadline) {}
        mmio_write(GICR_SGI + 0x280, (1U << TICK_IRQ) | (1U << RECOVERY_IRQ));
        mmio_write(GICR_SGI + 0x380, (1U << TICK_IRQ) | (1U << RECOVERY_IRQ));
        mmio_write(GICR_SGI + 0x80, saved.group);
        mmio_write(GICR_SGI + 0xd00, saved.modifier);
        mmio_write(GICR_SGI + 0xc04, saved.configuration);
        mmio_write(GICR_SGI + 0x41c, saved.priorities);
        mmio_write(GICR_SGI + 0x100, saved.enabled);
        // EL1 can write this image block, so re-derive the bank count from
        // the live distributor instead of trusting the saved loop bound.
        // ISENABLER only sets bits: clear SPIs enabled during the run first.
        unsigned banks = (mmio_read(GICD_BASE + 4) & 31) + 1;
        if (banks == saved.banks) {
            for (unsigned i = 1; i < banks; ++i)
                mmio_write(GICD_BASE + 0x180 + 4 * i, 0xffffffffU);
            deadline = counter() + COUNTER_HZ / 10;
            while ((mmio_read(GICD_BASE) & (1U << 31)) && counter() < deadline) {}
            for (unsigned i = 1; i < banks; ++i)
                mmio_write(GICD_BASE + 0x100 + 4 * i, saved.spi_enabled[i]);
        }
        // QEMU may enable ARE from cold state; cleanup must never clear it.
        mmio_write(GICD_BASE, saved.ctlr | (live_ctlr & 0x30U));
    }
    RESULT->stage = 5;
    __asm__ volatile("dsb sy" ::: "memory");
#ifdef MONITOR_QEMU
    register uint64_t op __asm__("x0") = 4;
    register uint64_t argument __asm__("x1") = RESULT_BASE + LOG_OFFSET;
    __asm__ volatile("hlt #0xf000" : "+r"(op) : "r"(argument) : "memory");
    uint64_t block[2] = {0x20026, status};
    op = 0x20;
    argument = (uintptr_t)block;
    __asm__ volatile("hlt #0xf000" : "+r"(op) : "r"(argument) : "memory");
#endif
    monitor_reset();
}

_Noreturn void monitor_exception(uint64_t operation, uint64_t status, uint64_t kind)
{
    uint64_t esr, elr, far;
    __asm__ volatile("mrs %0, esr_el3\n mrs %1, elr_el3\n mrs %2, far_el3"
                     : "=r"(esr), "=r"(elr), "=r"(far));
    RESULT->esr = esr;
    RESULT->elr = elr;
    RESULT->far = far;
    if (kind == 2) {
        uint64_t irq;
        __asm__ volatile("mrs %0, ICC_IAR0_EL1" : "=r"(irq));
        RESULT->interrupt = irq;
        if (irq < 1020)
            __asm__ volatile("msr ICC_EOIR0_EL1, %0\n isb" :: "r"(irq) : "memory");
        finish(irq == RECOVERY_IRQ ? 2 : 5);
    }
    if (kind == 1 && (esr >> 26) == 0x17 && operation == 0x733)
        finish((unsigned)status);
    finish(3);
}
