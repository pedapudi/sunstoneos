#ifndef SUNSTONE_NPU_FINITE_WATCHDOG_H
#define SUNSTONE_NPU_FINITE_WATCHDOG_H
#include <stdint.h>
#define NPU_FINITE_WATCHDOG_MODE UINT32_C(0xb1)
#ifdef SUNSTONE_NPU_WATCHDOG_TEST
uint32_t npu_finite_watchdog_read(uintptr_t);
void npu_finite_watchdog_write(uintptr_t,uint32_t);
#else
static inline uint32_t npu_finite_watchdog_read(uintptr_t address)
{
    uint32_t value=*(volatile uint32_t *)address;
    __asm__ volatile("dsb sy":::"memory");return value;
}
static inline void npu_finite_watchdog_write(uintptr_t address,uint32_t value)
{
    *(volatile uint32_t *)address=value;
    __asm__ volatile("dsb sy":::"memory");
}
#endif
int npu_finite_watchdog_admitted(void);
int npu_finite_watchdog_refresh(uint64_t now,uint64_t deadline,unsigned failed);
uint32_t npu_finite_watchdog_disarm(void);
void npu_finite_watchdog_stop(void);
int npu_finite_watchdog_start(void);
#endif
