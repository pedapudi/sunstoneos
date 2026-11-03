#ifndef SUNSTONE_OS_CPU_RETURN_H
#define SUNSTONE_OS_CPU_RETURN_H
#include <stddef.h>
#include <stdint.h>
#define OS_CPU_RETURN_RESERVE_US UINT64_C(5000000)
/* The application's NPU retirement wait in ss_finish gives up this many counter ticks (one second) before the supervisor's
 * cutoff, so its failure, which names the unmet conditions, is recorded first. */
#define OS_NPU_RETIREMENT_MARGIN_TICKS UINT64_C(24000000)
/* Longest a failure return may take before the failure ends at the watchdog. */
#define OS_FAILURE_RETURN_US UINT64_C(3000000)
uint64_t a733_smp_counter(void);
#ifdef SUNSTONE_A733_OS_RADIO_SCAN
/* The scanning radio station: the radio owner reports each poll result (apps/os/radio_scan.c). */
struct board_radio;
void os_radio_scan_step(struct board_radio *,int result);
#endif
uint64_t os_cpu_return_cutoff(void);
int os_cpu_return_requested(void);
/* Failure return (docs/designs/failure-return.md). */
int os_cpu_return_failing(void);
int os_cpu_return_prescheduler_requested(void);
int os_cpu_return_failure(uint32_t site,uint32_t value,const void *caller);
int os_cpu_return_unexpected_interrupt(unsigned interrupt,const void *caller);
int os_cpu_return_panic(const char *file,unsigned line,const void *caller);
int os_cpu_return_clock_done(void);
int os_cpu_return_start(void);
int os_cpu_return_task_privileged(void (*)(void *),void *);
void os_cpu_return_begin(const uint8_t *,size_t);
void os_cpu_return_thermal_done(void);
void os_cpu_return_pmic(void);
void os_cpu_return_supervise(void);
int os_cpu_return_ready(void);
int os_cpu_return_devices_ready(void);
void os_cpu_return_spi_save(void);
int os_cpu_return_spi_restore(void);
struct a733_cpu_clock;
struct a733_thermal_policy;
int os_cpu_return_thermal_ready(void);
uint32_t os_cpu_return_thermal_state(void);
int os_cpu_return_entry_guard(const struct a733_cpu_clock *,const struct a733_thermal_policy *,
    uint64_t,int (*)(void *,uintptr_t,uint32_t *),void *);
/* The last guard refusal: detail << 12 | source line in cpu-return-entry.c;
 * 0 after a guard that did not refuse. */
extern uint32_t os_cpu_return_entry_refusal;
#endif
