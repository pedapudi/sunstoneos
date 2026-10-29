#ifndef SUNSTONE_OS_CPU_RETURN_H
#define SUNSTONE_OS_CPU_RETURN_H
#include <stddef.h>
#include <stdint.h>
#define OS_CPU_RETURN_RESERVE_US UINT64_C(5000000)
uint64_t a733_smp_counter(void);
uint64_t os_cpu_return_cutoff(void);
int os_cpu_return_requested(void);
int os_cpu_return_clock_done(void);
int os_cpu_return_start(void);
int os_cpu_return_task_privileged(void (*)(void *),void *);
void os_cpu_return_begin(const uint8_t *,size_t);
void os_cpu_return_thermal_done(void);
void os_cpu_return_pmic(void);
void os_cpu_return_supervise(void);
int os_cpu_return_ready(void);
void os_cpu_return_spi_save(void);
int os_cpu_return_spi_restore(void);
#endif
