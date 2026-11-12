#ifndef SUNSTONE_OS_CPU_RETURN_H
#define SUNSTONE_OS_CPU_RETURN_H
#include <stddef.h>
#include <stdint.h>
#include "a733_board_application.h"
/* The returning lifetime (docs/designs/one-os-image.md): every OS image links its code, and EL1 and EL0 code reaches it
 * only when the sealed session's lifetime copy is the returning one (apps/os/sealed.c, readable at EL0). EL3 code tests
 * a733_smp_lifetime instead. */
unsigned board_session_lifetime(void);
static inline int os_returning(void){return board_session_lifetime()==A733_LIFETIME_RETURN;}
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
/* Published before owner requests. Zero until closing is supervised; otherwise
 * the absolute shared return bound, clipped to the session's final guard. */
uint64_t os_cpu_return_deadline_us(void);
int os_cpu_return_requested(void);
/* Failure return (docs/designs/failure-return.md). */
int os_cpu_return_failing(void);
int os_cpu_return_prescheduler_requested(void);
int os_cpu_return_failure(uint32_t site,uint32_t value,const void *caller);
int os_cpu_return_unexpected_interrupt(unsigned interrupt,const void *caller);
int os_cpu_return_panic(const char *file,unsigned line,const void *caller);
int os_cpu_return_clock_done(void);
int os_cpu_quiesce_register(void);
int os_cpu_return_pmic_fault(uint32_t,uint32_t,const void *);
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
/* One Secure Device snapshot per selected image route. The generated table
 * defines both storage and capacity from its own array declaration. */
struct os_cpu_return_spi_snapshot {
    uint64_t route;
    uint32_t group,modifier,config,priority,valid,intid;
};
extern struct os_cpu_return_spi_snapshot os_cpu_return_spi_saved[];
extern const unsigned os_cpu_return_spi_capacity;
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
