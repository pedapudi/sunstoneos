// SPDX-License-Identifier: Apache-2.0
// EL3 monitor of the vendor-state diagnostic only. It is the accepted
// returning monitor that the provider-readiness image links, platform/a733/
// monitor.c at SHA-256 08c3a6ad... (evidence cpu-monitor-cleanup-20261024),
// with one addition: before monitor_init returns to the eret into EL1, it
// makes the SPC, R_SPC, R_PRCM and clock-controller writes of the vendor
// BL31's cold-boot security setup, and records each in the EL3 record of
// vendor-state.h. No default image compiles this file.
//
// The writes are those of BL31's sunxi_security_setup (monitor.fex, SHA-256
// 8af16b86...5b2632, loaded at 0x48000000, function at file offset 0xc3b8),
// in its order, at its offsets:
// - SPC ports 0-13 and 16-23: 0xffffffff at 0x02054004 + 0x10 n
//   (0xc3cc-0xc3fc);
// - R_SPC: 0xffffffff at 0x07002004, 0x07002014, 0x07002024 (0xc41c-0xc424);
// - R_PRCM 0x07010290 |= 7 (0xc430-0xc438);
// - CCU 0x02003f00 = 7, CCMU_SEC_SWITCH_REG: PLL, bus and MBUS clock
//   registers non-secure (0xc448; manual 4.1.6.278);
// - SPC port 14: 0xfffffbff at 0x020540e0 (0xc470).
// BL31's other writes there are left out: R_TZMA 0x07003000-0x07003008
// (0xc408-0xc410) governs secure memory, which the image does not use;
// DMA0 0x04601020 and DMA1 0x04024020 (0xc458, 0xc460) concern DMA
// channels, which it does not run; CCU 0x02002800 and 0x02002850 (0xc484,
// 0xc48c) are unidentified; timer 0x03009008 (0xc498) is unused.
//
// Each write is preceded by a read of the word, recorded as its entry
// value, and followed by a read recorded as the value after. The monitor
// restores none of them before its ROM return. The writes only open
// peripherals and clock registers to non-secure software, which the
// Secure-state ROM does not need; the SPC layout is not documented, so a
// word read at the write offset may not be the state that writing it back
// would restore; and the image is the last of its lifetime, which ends with
// a power removal that resets the SPC, R_PRCM and CCU.
#include "boot.h"
#include "vendor-state.h"

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

static const struct vendor_state_el3_write vendor_state_el3_writes[VENDOR_STATE_EL3_WRITES] = {
    {0x02054004, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054014, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054024, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054034, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054044, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054054, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054064, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054074, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054084, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054094, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x020540a4, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x020540b4, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x020540c4, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x020540d4, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054104, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054114, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054124, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054134, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054144, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054154, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x02054164, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x02054174, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x07002004, 0xffffffffU, VENDOR_STATE_EL3_STORE}, {0x07002014, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x07002024, 0xffffffffU, VENDOR_STATE_EL3_STORE},
    {0x07010290, 7, VENDOR_STATE_EL3_OR},
    {VENDOR_STATE_CCU_SWITCH, 7, VENDOR_STATE_EL3_STORE},
    {0x020540e0, 0xfffffbffU, VENDOR_STATE_EL3_STORE},
};

// Runs at EL3 with the MMU off, so each access is Device-nGnRnE and in
// program order. The record is written as it goes: a stall leaves the
// count of completed writes, although only a return lets EL1 report it.
static void vendor_state_el3_setup(void)
{
    volatile struct vendor_state_el3 *record = (volatile struct vendor_state_el3 *)VENDOR_STATE_EL3_RECORD;
    record->magic = 0;
    record->done = 0;
    for (unsigned i = 0; i < VENDOR_STATE_EL3_WRITES; ++i) {
        const struct vendor_state_el3_write *w = &vendor_state_el3_writes[i];
        const uint32_t entry = mmio_read(w->address);
        record->address[i] = w->address;
        record->entry[i] = entry;
        mmio_write(w->address, w->kind == VENDOR_STATE_EL3_OR ? entry | w->value : w->value);
        __asm__ volatile("dsb sy" ::: "memory");
        record->after[i] = mmio_read(w->address);
        record->done = i + 1;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    record->magic = VENDOR_STATE_EL3_MAGIC;
    __asm__ volatile("dsb sy" ::: "memory");
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
#ifdef SUNSTONE_MONITOR_COUNTERS
    // Event counters for EL1, as platform/a733/cpu_continue.S sets them:
    // MDCR_EL3 at its TF-A reset value (MTPME, TPM clear), no EL2 trap of
    // CP15 groups, and MDCR_EL2.HPMN = PMCR_EL0.N with TPM, TPMCR and HPME
    // clear. After reset into EL3 the MDCR_EL2 trap bits are UNKNOWN.
    value = 0x10000000;
    __asm__ volatile("msr mdcr_el3, %0\n msr hstr_el2, xzr" :: "r"(value));
    __asm__ volatile("mrs %0, pmcr_el0" : "=r"(value));
    value = (value >> 11) & 0x1f;
    __asm__ volatile("msr mdcr_el2, %0\n isb" :: "r"(value) : "memory");
#endif
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
    // VENDOR-STATE VARIANT: BL31's security writes, the last step before
    // the eret into EL1.
    vendor_state_el3_setup();
    RESULT->stage = 2;
}

static int cleanup_wait(uintptr_t address, uint32_t mask, uint64_t deadline)
{
    for (;;) {
        uint32_t value = mmio_read(address);
        // A completed read at the deadline is already too late.
        if (counter() >= deadline)
            return 0;
        if (!(value & mask))
            return 1;
    }
}

static int restore_interrupts(void)
{
    // Keep the existing three 100 ms windows. Readbacks share their phase's
    // deadline; a stalled register access still needs external recovery.
    uint64_t deadline = counter() + COUNTER_HZ / 10;
    uint32_t live_ctlr = mmio_read(GICD_BASE) & ~(1U << 31);
    mmio_write(GICD_BASE, live_ctlr & ~7U);
    if (!cleanup_wait(GICD_BASE, 1U << 31, deadline) ||
        (mmio_read(GICD_BASE) & 7U) || counter() >= deadline)
        return 0;

    deadline = counter() + COUNTER_HZ / 10;
    mmio_write(GICR_SGI + 0x180, 0xffffffffU);
    if (!cleanup_wait(GICR_BASE, 8, deadline) ||
        mmio_read(GICR_SGI + 0x100) || counter() >= deadline)
        return 0;
    const uint32_t timers = (1U << TICK_IRQ) | (1U << RECOVERY_IRQ);
    mmio_write(GICR_SGI + 0x280, timers);
    mmio_write(GICR_SGI + 0x380, timers);
    __asm__ volatile("dsb sy" ::: "memory");
    if ((mmio_read(GICR_SGI + 0x200) & timers) ||
        (mmio_read(GICR_SGI + 0x300) & timers) || counter() >= deadline)
        return 0;
    mmio_write(GICR_SGI + 0x80, saved.group);
    mmio_write(GICR_SGI + 0xd00, saved.modifier);
    mmio_write(GICR_SGI + 0xc04, saved.configuration);
    mmio_write(GICR_SGI + 0x41c, saved.priorities);
    mmio_write(GICR_SGI + 0x100, saved.enabled);
    __asm__ volatile("dsb sy" ::: "memory");
    // Compare values read from the implemented Secure register view, not
    // assumed all-ones masks or unimplemented priority bits.
    if (!cleanup_wait(GICR_BASE, 8, deadline) ||
        mmio_read(GICR_SGI + 0x80) != saved.group ||
        mmio_read(GICR_SGI + 0xd00) != saved.modifier ||
        mmio_read(GICR_SGI + 0xc04) != saved.configuration ||
        mmio_read(GICR_SGI + 0x41c) != saved.priorities ||
        mmio_read(GICR_SGI + 0x100) != saved.enabled || counter() >= deadline)
        return 0;

    deadline = counter() + COUNTER_HZ / 10;
    // EL1 can write the saved block. Derive the bounds again before using it.
    unsigned banks = (mmio_read(GICD_BASE + 4) & 31) + 1;
    if (banks != saved.banks || counter() >= deadline)
        return 0;
    for (unsigned i = 1; i < banks; ++i)
        mmio_write(GICD_BASE + 0x180 + 4 * i, 0xffffffffU);
    if (!cleanup_wait(GICD_BASE, 1U << 31, deadline))
        return 0;
    for (unsigned i = 1; i < banks; ++i)
        if (mmio_read(GICD_BASE + 0x100 + 4 * i) || counter() >= deadline)
            return 0;
    for (unsigned i = 1; i < banks; ++i)
        mmio_write(GICD_BASE + 0x100 + 4 * i, saved.spi_enabled[i]);
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 1; i < banks; ++i)
        if (mmio_read(GICD_BASE + 0x100 + 4 * i) != saved.spi_enabled[i] ||
            counter() >= deadline)
            return 0;
    // QEMU may enable ARE from cold state; never restore an enabled bit to 0.
    uint32_t restored_ctlr = (saved.ctlr & ~(1U << 31)) | (live_ctlr & 0x30U);
    mmio_write(GICD_BASE, restored_ctlr);
    return cleanup_wait(GICD_BASE, 1U << 31, deadline) &&
           mmio_read(GICD_BASE) == restored_ctlr && counter() < deadline;
}

static _Noreturn void finish(unsigned status)
{
    __asm__ volatile("msr daifset, #0xf\n msr cntps_ctl_el1, xzr\n"
                     "msr cntp_ctl_el0, xzr\n isb" ::: "memory");
    RESULT->end_counter = counter();
    if (saved.ready && !restore_interrupts())
        status = 6; // ROM return is permitted, but interrupt ownership is retained.
    RESULT->status = status;
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
