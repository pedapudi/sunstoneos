/* Host shim of apps/os/os.h for checks/perf-os-model.c: only what
 * apps/os/perf.c uses. */
#ifndef SUNSTONE_OS_H
#define SUNSTONE_OS_H
#include <stddef.h>
#include <stdint.h>
#include "a733_counter_clock.h"
#include "a733_cpu_clock.h"
/* The granted set and the admitted session, as far as perf.c reads them: the CPU clock grant's perf and the NPU. */
enum { OS_GRANT_NPU=1u<<0, OS_GRANT_CPU_CLOCK=1u<<2 };
struct os_session { struct a733_cpu_clock_grant_record cpu_clock; unsigned granted; };
extern struct os_session perf_model_session;
static inline const struct os_session *os_session_admitted(void){return &perf_model_session;}
static inline int os_granted(unsigned mask){return mask&&(perf_model_session.granted&mask)==mask;}
#endif
