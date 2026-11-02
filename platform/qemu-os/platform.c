/* Core, interrupt, timer and console support of the virtual OS board: QEMU
 * virt with a GICv3, eight cores in one affinity group, PL011 console and
 * semihosting exit. It follows platform/qemu-smp/platform.c and adds the
 * pieces that the integrated OS image takes from the A733 cached platform:
 * the virtual timer for precise waits (PPI 27), the architectural counter
 * presented as the A733's 24 MHz counter, and the stop paths of the board
 * application. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "a733_memory_local.h"
#include "sunstone_deadline.h"
#include "irq_table.h"
#include "qemu_os.h"

#define UART 0x09000000UL
#define GICD 0x08000000UL
#define GICR 0x080a0000UL
#define TIMER_IRQ 30U
#define DEADLINE_IRQ 27U
static uint64_t level1[512] __attribute__((aligned(4096)));
static uint64_t level2[512] __attribute__((aligned(4096)));
static uint64_t tick_interval, next_tick;
static uint32_t cpu_online;
extern void _start(void);
extern volatile uint8_t ucPrimaryCoreInitDoneFlag;
extern volatile uint8_t ucSecondaryCoresReadyFlags[];
extern void FreeRTOS_SGI_Handler(void);

static uint32_t read32(uintptr_t address) { return *(volatile uint32_t *)address; }
static void write32(uintptr_t address, uint32_t value) { *(volatile uint32_t *)address = value; }
static uint64_t raw_counter(void) { uint64_t n; __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(n) :: "memory"); return n; }
static uint64_t frequency(void) { uint64_t n; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(n)); return n; }
static unsigned core(void) { uint64_t n; __asm__ volatile("mrs %0, tpidr_el1" : "=r"(n)); return n; }
static void wait_clear(uintptr_t address, uint32_t mask) {
    uint64_t end = raw_counter() + frequency();
    while (read32(address) & mask) configASSERT(raw_counter() < end);
}

/* QEMU's architectural counter runs at a fixed 62.5 MHz. The A733's runs at
 * 24 MHz, and the OS converts counter values to microseconds by dividing by 24
 * and checks CNTFRQ_EL0 for 24 MHz. With no EL2 or EL3 the highest implemented
 * level is EL1, which may write CNTFRQ_EL0, so each core declares 24 MHz and
 * the OS's own conversions run unmodified. Guest time then passes 2.6 times
 * faster than wall-clock time: every duration the OS and its applications
 * measure is guest time. */
uint64_t a733_smp_counter(void) { return raw_counter(); }

void qemu_os_puts(const char *text) {
    while (*text) {
        while (read32(UART + 0x18) & (1U << 5)) {}
        write32(UART, (unsigned char)*text++);
    }
}
void qemu_os_hex(uint64_t value) {
    char text[19] = "0x";
    for (unsigned i = 0; i < 16; ++i) text[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    text[18] = 0;
    qemu_os_puts(text);
}
void qemu_os_dec(uint64_t value) {
    char text[21];
    unsigned i = sizeof(text);
    text[--i] = 0;
    do { text[--i] = (char)('0' + value % 10); value /= 10; } while (value);
    qemu_os_puts(text + i);
}
_Noreturn void platform_exit_privileged(unsigned status) {
    uint64_t block[2] = {0x20026, status};
    register uint64_t operation __asm__("x0") = 0x20;
    register uint64_t *argument __asm__("x1") = block;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(argument) : "memory");
    for (;;) {}
}
_Noreturn void platform_exit(unsigned status) {
    register uint64_t code __asm__("x0") = status;
    __asm__ volatile("svc #119" : "+r"(code) :: "memory");
    for (;;) {}
}
/* The board's panic path: named, and with a distinct exit status. */
_Noreturn void platform_panic(const char *name, unsigned code) {
    qemu_os_puts("PANIC "); qemu_os_puts(name); qemu_os_puts(" code=");
    qemu_os_dec(code); qemu_os_puts("\n");
    platform_exit(2);
}
/* Stop of the supervising task (resident lifetime): renewal refused. */
_Noreturn void a733_smp_exit_privileged(unsigned status, uint64_t detail) {
    __asm__ volatile("msr daifset, #15" ::: "memory");
    qemu_os_puts("STOP status="); qemu_os_dec(status);
    qemu_os_puts(" detail="); qemu_os_dec(detail); qemu_os_puts("\n");
    platform_exit_privileged(status ? 3 : 0);
}
_Noreturn void platform_exception(void) {
    uint64_t esr, elr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    qemu_os_puts("EXCEPTION ESR="); qemu_os_hex(esr);
    qemu_os_puts(" ELR="); qemu_os_hex(elr); qemu_os_puts(" FAR="); qemu_os_hex(far);
    qemu_os_puts("\n"); platform_exit_privileged(4);
}
static void local_init(void) {
    uint64_t declared = 24000000;
    __asm__ volatile("msr cntfrq_el0, %0" :: "r"(declared));
    uint64_t mair = 0xff00, tcr = (2UL << 32) | (1UL << 23) | (3UL << 12) |
        (1UL << 10) | (1UL << 8) | 25;
    __asm__ volatile("dsb sy\n msr mair_el1, %0\n msr tcr_el1, %1\n msr ttbr0_el1, %2\n"
                     "isb\n tlbi vmalle1\n dsb sy\n isb" :: "r"(mair), "r"(tcr), "r"(level1) : "memory");
    /* M, C and I, and nTWI so that the EL0 idle tasks may wait in WFI. */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 16) | (1UL << 12) | (1UL << 2) | 1;
    __asm__ volatile("msr sctlr_el1, %0\n isb" :: "r"(sctlr) : "memory");
    uintptr_t r = GICR + core() * 0x20000, s = r + 0x10000;
    write32(r + 0x14, read32(r + 0x14) & ~2U);
    wait_clear(r + 0x14, 4);
    write32(s + 0x80, 0xffffffff);
    write32(s + 0xd00, 0);
    write32(s + 0x180, 0xffffffff);
    write32(s + 0x280, 0xffffffff);
    uint8_t priority = portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT;
    *(volatile uint8_t *)(s + 0x400) = priority;
    *(volatile uint8_t *)(s + 0x400 + TIMER_IRQ) = priority;
    /* The virtual timer wakes precise waits; API calls need this priority or lower. */
    *(volatile uint8_t *)(s + 0x400 + DEADLINE_IRQ) = 0xd8;
    write32(s + 0x100, 1 | (core() ? 0 : (1U << TIMER_IRQ) | (1U << DEADLINE_IRQ)));
    wait_clear(r, 8);
    uint64_t value = 1;
    __asm__ volatile("msr ICC_SRE_EL1, %0\n isb" :: "r"(value) : "memory");
    value = 0;
    __asm__ volatile("msr ICC_CTLR_EL1, %0\n msr ICC_BPR1_EL1, %0" :: "r"(value));
    value = 255;
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(value));
    value = 1;
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0\n isb" :: "r"(value) : "memory");
}
void platform_init(void) {
    /* Below 1 GiB: Device-nGnRnE, which is where the A733 peripherals sit. */
    level1[0] = (1UL << 54) | (1UL << 53) | (1UL << 10) | (1UL << 6) | 1;
    level1[1] = (uintptr_t)level2 | 3;
    for (unsigned i = 0; i < 128; ++i)
        level2[i] = (0x40000000UL + i * 0x200000UL) | (1UL << 10) |
            (3UL << 8) | (1UL << 6) | (1UL << 2) | (1UL << 54) | (1UL << 53) | 1;
    /* Code is read-only at both EL0 and EL1; data lies in later blocks. */
    level2[8] = 0x41000000UL | (1UL << 10) | (3UL << 8) | (3UL << 6) | (1UL << 2) | 1;
    __asm__ volatile("dsb sy" ::: "memory");
    configASSERT(read32(GICD) & (1U << 6));
    write32(GICD, (1U << 6) | (1U << 4)); wait_clear(GICD, 1U << 31);
    local_init();
    write32(GICD, (1U << 6) | (1U << 4) | 2); wait_clear(GICD, 1U << 31);
    for (uint64_t cpu = 1; cpu < configNUMBER_OF_CORES; ++cpu) {
        register uint64_t id __asm__("x0") = 0xc4000003;
        register uint64_t affinity __asm__("x1") = cpu;
        register uint64_t entry __asm__("x2") = (uintptr_t)_start;
        register uint64_t context __asm__("x3") = 0;
        __asm__ volatile("dsb sy\n hvc #0" : "+r"(id) : "r"(affinity), "r"(entry), "r"(context) : "memory");
        configASSERT(id == 0);
    }
    uint64_t end = raw_counter() + frequency();
    while (__atomic_load_n(&cpu_online, __ATOMIC_ACQUIRE) != configNUMBER_OF_CORES - 1)
        configASSERT(raw_counter() < end);
    configASSERT(a733_memory_local_prepare());
}
void platform_secondary(void) {
    local_init();
    __atomic_add_fetch(&cpu_online, 1, __ATOMIC_RELEASE);
    uint64_t end = raw_counter() + frequency();
    while (!__atomic_load_n(&ucPrimaryCoreInitDoneFlag, __ATOMIC_ACQUIRE)) configASSERT(raw_counter() < end);
    __atomic_store_n(&ucSecondaryCoresReadyFlags[core() - 1], 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb sy\n sev\n svc #106" ::: "memory");
    platform_panic("secondary scheduler returned", 0);
}
void platform_tick_setup(void) {
    configASSERT(core() == 0);
    tick_interval = frequency() / configTICK_RATE_HZ;
    next_tick = raw_counter(); platform_tick_ack();
    uint64_t enable = 1;
    __asm__ volatile("msr cntp_ctl_el0, %0\n isb" :: "r"(enable) : "memory");
}
void platform_tick_ack(void) {
    next_tick += tick_interval;
    __asm__ volatile("msr cntp_cval_el0, %0\n isb" :: "r"(next_tick) : "memory");
}
void vApplicationIRQHandler(uint64_t interrupt) {
    unsigned c = core();
    if (interrupt == TIMER_IRQ && !c) FreeRTOS_Tick_Handler();
    else if (interrupt == DEADLINE_IRQ && !c) sunstone_deadline_irq();
    else if (interrupt == 0) { FreeRTOS_SGI_Handler(); if (!c) sunstone_deadline_rearm(); }
    else if (interrupt < 1020 && a733_irq_table_dispatch((unsigned)interrupt, c)) {}
    else if (interrupt < 1020) platform_panic("unexpected interrupt", (unsigned)interrupt);
}
