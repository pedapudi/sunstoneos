#include "FreeRTOS.h"
#include "platform.h"

// QEMU virt with GICv3, security=off, virtualization=off, and one CPU.
#define UART 0x09000000UL
#define GICD 0x08000000UL
#define GICR 0x080a0000UL
#define GICR_SGI (GICR + 0x10000UL)
#define TIMER_IRQ 30U

static uint64_t tick_interval;
static uint64_t next_tick;
static uint64_t level1[512] __attribute__((aligned(4096)));
static uint64_t level2[512] __attribute__((aligned(4096)));

static inline uint32_t read32(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}

static inline void write32(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

void platform_puts(const char *text)
{
    while (*text) {
        while (read32(UART + 0x18) & (1U << 5)) {}
        write32(UART, (unsigned char)*text++);
    }
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
    uint64_t block[2] = {0x20026, status};
    register uint64_t operation __asm__("x0") = 0x20;
    register uint64_t *argument __asm__("x1") = block;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(argument) : "memory");
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

_Noreturn void platform_exception(void)
{
    uint64_t esr, elr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    platform_puts("FAIL exception ESR="); platform_hex(esr);
    platform_puts(" ELR="); platform_hex(elr);
    platform_puts(" FAR="); platform_hex(far);
    platform_puts("\n");
    platform_exit(1);
}

void platform_init(void)
{
    // Identity-map peripheral space as Device-nGnRnE, and the 128 MiB RAM
    // region as normal, inner-shareable, write-back memory. Boot code/data
    // share a mapping here; separate permissions belong to the full VM layer.
    level1[0] = (1UL << 54) | (1UL << 53) | (1UL << 10) | 1;
    level1[1] = (uintptr_t)level2 | 3;
    for (unsigned i = 0; i < 64; ++i)
        level2[i] = (0x40000000UL + i * 0x200000UL) | (1UL << 10) |
                    (3UL << 8) | (1UL << 2) | 1;
    uint64_t mair = 0xff00;
    uint64_t tcr = (2UL << 32) | (1UL << 23) | (3UL << 12) |
                   (1UL << 10) | (1UL << 8) | 25;
    __asm__ volatile("dsb sy\n msr mair_el1, %0\n msr tcr_el1, %1\n"
                     "msr ttbr0_el1, %2\n isb\n tlbi vmalle1\n dsb sy\n isb"
                     :: "r"(mair), "r"(tcr), "r"((uintptr_t)level1) : "memory");
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 12) | (1UL << 2) | 1;
    __asm__ volatile("msr sctlr_el1, %0\n isb" :: "r"(sctlr) : "memory");
    write32(UART + 0x30, 0);
    write32(UART + 0x24, 13);  // 24 MHz, 115200 baud.
    write32(UART + 0x28, 1);
    write32(UART + 0x2c, (3U << 5) | (1U << 4));
    write32(UART + 0x30, (1U << 9) | (1U << 8) | 1);
    // Single security state: enable affinity routing and Group 1 interrupts.
    configASSERT(read32(GICD) & (1U << 6));
    write32(GICD, (1U << 6) | (1U << 4));
    while (read32(GICD) & (1U << 31)) {}
    write32(GICR + 0x14, read32(GICR + 0x14) & ~(1U << 1));
    while (read32(GICR + 0x14) & (1U << 2)) {}
    write32(GICR_SGI + 0x80, 0xffffffffU);  // IGROUPR0: Group 1.
    write32(GICR_SGI + 0xd00, 0);          // IGRPMODR0.
    write32(GICR_SGI + 0x180, 0xffffffffU);
    write32(GICR_SGI + 0x280, 0xffffffffU);
    *(volatile uint8_t *)(GICR_SGI + 0x400 + TIMER_IRQ) =
        portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT;
    write32(GICR_SGI + 0x100, 1U << TIMER_IRQ);
    while (read32(GICR) & (1U << 3)) {}
    write32(GICD, (1U << 6) | (1U << 4) | (1U << 1));
    while (read32(GICD) & (1U << 31)) {}
    uint64_t value = 1;
    __asm__ volatile("msr ICC_SRE_EL1, %0\n isb" :: "r"(value) : "memory");
    value = 0;
    __asm__ volatile("msr ICC_CTLR_EL1, %0\n msr ICC_BPR1_EL1, %0" :: "r"(value));
    value = 0xff;
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(value));
    value = 1;
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0\n isb" :: "r"(value) : "memory");
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
