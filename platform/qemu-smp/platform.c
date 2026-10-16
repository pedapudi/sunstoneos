#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"

#define UART 0x09000000UL
#define GICD 0x08000000UL
#define GICR 0x080a0000UL
#define TIMER_IRQ 30U
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
static uint64_t counter(void) { uint64_t n; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(n)); return n; }
static uint64_t frequency(void) { uint64_t n; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(n)); return n; }
static unsigned core(void) { uint64_t n; __asm__ volatile("mrs %0, tpidr_el1" : "=r"(n)); return n; }
static void wait_clear(uintptr_t address, uint32_t mask) {
    uint64_t end = counter() + frequency();
    while (read32(address) & mask) configASSERT(counter() < end);
}
void platform_puts(const char *text) {
    while (*text) {
        while (read32(UART + 0x18) & (1U << 5)) {}
        write32(UART, (unsigned char)*text++);
    }
}
void platform_hex(uint64_t value) {
    char text[19] = "0x";
    for (unsigned i = 0; i < 16; ++i) text[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    platform_puts(text);
}
_Noreturn void platform_exit(unsigned status) {
    register uint64_t code __asm__("x0") = status;
    __asm__ volatile("svc #119" : "+r"(code) :: "memory");
    for (;;) {}
}
_Noreturn void platform_exit_privileged(unsigned status) {
    uint64_t block[2] = {0x20026, status};
    register uint64_t operation __asm__("x0") = 0x20;
    register uint64_t *argument __asm__("x1") = block;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(argument) : "memory");
    for (;;) {}
}
_Noreturn void platform_panic(const char *file, unsigned line) {
    platform_puts("FAIL "); platform_puts(file); platform_puts(":"); platform_hex(line);
    platform_puts("\n"); platform_exit(1);
}
_Noreturn void platform_exception(void) {
    uint64_t esr, elr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    platform_puts("FAIL exception ESR="); platform_hex(esr);
    platform_puts(" ELR="); platform_hex(elr); platform_puts(" FAR="); platform_hex(far);
    platform_puts("\n"); platform_exit(1);
}
static void local_init(void) {
    // The non-MPU reference port runs tasks at EL0. Both levels share this
    // development mapping; tasks can access all data and peripherals.
    uint64_t mair = 0xff00, tcr = (2UL << 32) | (1UL << 23) | (3UL << 12) |
        (1UL << 10) | (1UL << 8) | 25;
    __asm__ volatile("dsb sy\n msr mair_el1, %0\n msr tcr_el1, %1\n msr ttbr0_el1, %2\n"
                     "isb\n tlbi vmalle1\n dsb sy\n isb" :: "r"(mair), "r"(tcr), "r"(level1) : "memory");
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 12) | (1UL << 2) | 1;
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
    write32(s + 0x100, 1 | (core() ? 0 : 1U << TIMER_IRQ));
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
    level1[0] = (1UL << 54) | (1UL << 53) | (1UL << 10) | (1UL << 6) | 1;
    level1[1] = (uintptr_t)level2 | 3;
    for (unsigned i = 0; i < 64; ++i)
        level2[i] = (0x40000000UL + i * 0x200000UL) | (1UL << 10) |
            (3UL << 8) | (1UL << 6) | (1UL << 2) | (1UL << 54) | (1UL << 53) | 1;
    // Code is read-only at both EL0 and EL1; writable EL0 pages cannot
    // execute at EL1. The linker puts all data in later blocks.
    level2[8] = 0x41000000UL | (1UL << 10) | (3UL << 8) | (3UL << 6) | (1UL << 2) | 1;
    __asm__ volatile("dsb sy" ::: "memory");
    configASSERT(read32(GICD) & (1U << 6));
    write32(GICD, (1U << 6) | (1U << 4)); wait_clear(GICD, 1U << 31);
    local_init();
    write32(GICD, (1U << 6) | (1U << 4) | 2); wait_clear(GICD, 1U << 31);
    // QEMU uses HVC for PSCI when virtualization=off.
    register uint64_t id __asm__("x0") = 0xc4000003;
    register uint64_t affinity __asm__("x1") = 1;
    register uint64_t entry __asm__("x2") = (uintptr_t)_start;
    register uint64_t context __asm__("x3") = 0;
    __asm__ volatile("dsb sy\n hvc #0" : "+r"(id) : "r"(affinity), "r"(entry), "r"(context) : "memory");
    configASSERT(id == 0);
    uint64_t end = counter() + frequency();
    while (!__atomic_load_n(&cpu_online, __ATOMIC_ACQUIRE)) configASSERT(counter() < end);
}
void platform_secondary(void) {
    local_init();
    __atomic_store_n(&cpu_online, 1, __ATOMIC_RELEASE);
    uint64_t end = counter() + frequency();
    while (!__atomic_load_n(&ucPrimaryCoreInitDoneFlag, __ATOMIC_ACQUIRE)) configASSERT(counter() < end);
    __atomic_store_n(&ucSecondaryCoresReadyFlags[core() - 1], 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb sy\n sev\n svc #106" ::: "memory");
    platform_panic("secondary scheduler returned", 0);
}
void platform_tick_setup(void) {
    tick_interval = frequency() / configTICK_RATE_HZ;
    next_tick = counter(); platform_tick_ack();
    uint64_t enable = 1;
    __asm__ volatile("msr cntp_ctl_el0, %0\n isb" :: "r"(enable) : "memory");
}
void platform_tick_ack(void) {
    next_tick += tick_interval;
    __asm__ volatile("msr cntp_cval_el0, %0\n isb" :: "r"(next_tick) : "memory");
}
void vApplicationIRQHandler(uint64_t interrupt) {
    if (interrupt == TIMER_IRQ) FreeRTOS_Tick_Handler();
    else if (interrupt == 0) FreeRTOS_SGI_Handler();
    else if (interrupt < 1020) platform_panic("unexpected interrupt", interrupt);
}
