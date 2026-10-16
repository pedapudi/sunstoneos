#ifndef SUNSTONE_QEMU_SMP_BOOT_H
#define SUNSTONE_QEMU_SMP_BOOT_H
#include <stdint.h>
static inline uint64_t counter(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
    return value;
}
static inline uint64_t counter_frequency(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}
#define COUNTER_HZ counter_frequency()
#endif
