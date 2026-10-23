#include "FreeRTOS.h"
#include "platform.h"
#include "boot.h"
#include "a733_memory_boot.h"
#include "a733_cpu_coherence.h"
#include "a733_device_memory_layout.h"

#ifdef SUNSTONE_MEMTEST
// The memory qualification record occupies the result block's last page.
#define LOG_END 0x3000U
#else
#define LOG_END RESULT_SIZE
#endif
#define TIMER_IRQ TICK_IRQ

static uint64_t tick_interval;
static uint64_t next_tick;
static uint64_t level1[512] __attribute__((aligned(4096)));
static uint64_t level2[512] __attribute__((aligned(4096)));

void platform_puts(const char *text)
{
    volatile char *log = (volatile char *)(RESULT_BASE + LOG_OFFSET);
    uint64_t size = RESULT->log_length;
    // A corrupt length must not turn the terminator into a wild store.
    if (size > LOG_END - LOG_OFFSET - 1)
        return;
    while (*text && size < LOG_END - LOG_OFFSET - 1)
        log[size++] = *text++;
    log[size] = 0;
    RESULT->log_length = size;
}

void platform_hex(uint64_t value)
{
    char buffer[19] = "0x";
    for (unsigned i = 0; i < 16; ++i)
        buffer[2 + i] = "0123456789abcdef"[(value >> (60 - i * 4)) & 15];
    platform_puts(buffer);
}

_Noreturn void platform_exit(unsigned status)
{
    register uint64_t operation __asm__("x0") = 0x733;
    register uint64_t argument __asm__("x1") = status;
    RESULT->stage = 4;
    __asm__ volatile("dsb sy\n smc #0" : "+r"(operation) : "r"(argument) : "memory");
    for (;;) __asm__ volatile("wfe");
}

_Noreturn void platform_panic(const char *file, unsigned line)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    platform_puts("FAIL ");
    platform_puts(file);
    platform_puts(":");
    platform_hex(line);
    platform_puts("\n");
    platform_exit(1);
}

/* Included here rather than at the top so that the platform.patch files of
 * other images keep their context. */
#include "task.h"
#include "a733_fault.h"
#include "a733_range.h"
_Noreturn void platform_exception(void)
{
    /* The shared fault record (docs/fault-capture.md), written as one line.
     * These images keep their FEL transport and do not use the slots. */
    struct a733_fault_record r;
    uint64_t mpidr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(r.esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(r.elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(r.far));
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(r.spsr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    r.magic = r.crc = r.epoch = r.detail = r.reserved[0] = r.reserved[1] = 0;
    r.info = A733_FAULT_INFO(A733_FAULT_KIND_VECTOR, A733_FAULT_SLOT_UNKNOWN, (mpidr >> 8) & 7);
    r.status = 1;
    r.counter = counter();
    r.task[0] = r.task[1] = 0;
    /* The name is read only inside image RAM, so a corrupt TCB pointer
     * cannot fault again here. */
    extern void *volatile pxCurrentTCB;
    void *tcb = pxCurrentTCB;
    r.tcb = (uintptr_t)tcb;
    if (tcb) {
        const char *name = pcTaskGetName((TaskHandle_t)tcb);
        if (a733_extent_within(0x41000000UL, RESULT_BASE - 0x41000000UL, (uintptr_t)name, 16))
            for (unsigned i = 0; i < 16; ++i)
                r.task[i / 8] |= (uint64_t)(uint8_t)name[i] << (8 * (i % 8));
    }
    char line[A733_FAULT_LINE_BYTES];
    a733_fault_line(&r, line);
    platform_puts(line);
    platform_exit(1);
}

void platform_init(void)
{
    // Identity-map peripheral space as Device-nGnRnE, and the 128 MiB RAM
    // region as normal, inner-shareable, write-back memory. Boot code/data
    // share a mapping here; separate permissions belong to the full VM layer.
    // The walker reads the tables through the data cache (TCR_EL1 IRGN0 =
    // ORGN0 = 1), but this code writes them with the cache off. A line left
    // over a table page would hide a descriptor or later overwrite one, so
    // both pages are cleaned and invalidated before the first write and
    // after the last. EL1 runs here with DAIF masked and SCTLR_EL1.M/C/I clear.
    configASSERT(a733_cpu_coherence_flush((uintptr_t)level1, sizeof level1, counter() + COUNTER_HZ / 100) == 1);
    configASSERT(a733_cpu_coherence_flush((uintptr_t)level2, sizeof level2, counter() + COUNTER_HZ / 100) == 1);
    level1[0] = (1UL << 54) | (1UL << 53) | (1UL << 10) | 1;
    level1[1] = (uintptr_t)level2 | 3;
    for (unsigned i = 0; i < 64; ++i)
        level2[i] = (0x40000000UL + i * 0x200000UL) | (1UL << 10) |
                    (3UL << 8) | (1UL << 2) | 1;
#ifdef SUNSTONE_MEMTEST
    // The memory qualification image maps the rest of DRAM, up to
    // 0x3_4000_0000, as non-executable write-back blocks closed to EL0.
    for (unsigned i = 64; i < 512; ++i)
        level2[i] = A733_EL1_DRAM_BLOCK(0x40000000UL + i * 0x200000UL);
    for (unsigned i = 2; i < 13; ++i)
        level1[i] = A733_EL1_DRAM_BLOCK((uint64_t)i << 30);
#endif
    // Result writes must reach DRAM for the cache-disabled EL3 monitor and ROM.
    // Map this 2 MiB block as normal non-cacheable memory (attribute index 2).
    level2[(RESULT_BASE - 0x40000000UL) / 0x200000UL] =
        RESULT_BASE | (1UL << 10) | (3UL << 8) | (2UL << 2) | 1;
    configASSERT(a733_memory_boot_uncached(level2));
    configASSERT(a733_cpu_coherence_flush((uintptr_t)level1, sizeof level1, counter() + COUNTER_HZ / 100) == 1);
    configASSERT(a733_cpu_coherence_flush((uintptr_t)level2, sizeof level2, counter() + COUNTER_HZ / 100) == 1);
    uint64_t mair = 0x44ff00;
    uint64_t tcr = (2UL << 32) | (1UL << 23) | (3UL << 12) |
                   (1UL << 10) | (1UL << 8) | 25;
    __asm__ volatile("dsb sy\n msr mair_el1, %0\n msr tcr_el1, %1\n"
                     "msr ttbr0_el1, %2\n isb\n tlbi vmalle1\n dsb sy\n isb"
                     :: "r"(mair), "r"(tcr), "r"((uintptr_t)level1) : "memory");
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 12) | (1UL << 2) | 1;
    __asm__ volatile("msr sctlr_el1, %0\n isb" :: "r"(sctlr) : "memory");
    // EL3 owns distributor security and the recovery timer. EL1 owns only
    // the nonsecure physical timer and its Group 1 CPU interface.
    *(volatile uint8_t *)(GICR_SGI + 0x400 + TIMER_IRQ) =
        portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT;
    mmio_write(GICR_SGI + 0x100, 1U << TIMER_IRQ);
    uint64_t deadline = counter() + COUNTER_HZ / 10;
    while (mmio_read(GICR_BASE) & 8)
        configASSERT(counter() < deadline);
    uint64_t value = 1;
    __asm__ volatile("msr ICC_SRE_EL1, %0\n isb" :: "r"(value) : "memory");
    value = 0;
    __asm__ volatile("msr ICC_CTLR_EL1, %0\n msr ICC_BPR1_EL1, %0" :: "r"(value));
    value = 0xff;
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(value));
    value = 1;
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0\n isb" :: "r"(value) : "memory");
    __asm__ volatile("mrs %0, ICC_PMR_EL1" : "=r"(value));
    RESULT->priority_mask = value;
    configASSERT(value == 0xf0);
    RESULT->stage = 3;
    if (RESULT->mode == 1) {
        platform_puts("CHECK recovery: EL1 stalls with exceptions masked\n");
        __asm__ volatile("msr daifset, #0xf" ::: "memory");
        for (;;) __asm__ volatile("wfi");
    }
}

void platform_tick_setup(void)
{
    uint64_t frequency, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    configASSERT(frequency >= configTICK_RATE_HZ);
    tick_interval = frequency / configTICK_RATE_HZ;
    next_tick = now;
    platform_tick_ack();
    uint64_t enable = 1;
    __asm__ volatile("msr cntp_ctl_el0, %0\n isb" :: "r"(enable) : "memory");
}

void platform_tick_ack(void)
{
    next_tick += tick_interval;
    __asm__ volatile("msr cntp_cval_el0, %0\n isb" :: "r"(next_tick) : "memory");
}

void vApplicationFPUSafeIRQHandler(uint64_t interrupt)
{
    if (interrupt == TIMER_IRQ)
        FreeRTOS_Tick_Handler();
    else if (interrupt < 1020)
        platform_panic("unexpected interrupt", (unsigned)interrupt);
}
