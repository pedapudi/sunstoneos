#ifndef SUNSTONE_OS_SUPERVISOR_WAIT_H
#define SUNSTONE_OS_SUPERVISOR_WAIT_H
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_deadline.h"

#define OS_SUPERVISOR_OBSERVATION_US UINT64_C(20000)
/* Zero once before initialization in the supervisor task. Retain this object
 * and its registered owner for the image's lifetime, including after failure. */
struct os_supervisor_wait {
    TaskHandle_t owner;
    uint64_t next_us,last_us;
    unsigned failed;
};
/* Register one precise-deadline slot and anchor the first phase at now+20ms.
 * Returns 1 on success, 0 on refusal. A registered object cannot reinitialize. */
int os_supervisor_wait_init(struct os_supervisor_wait *);
/* Perform the existing supervisor checks before each call and after each
 * returned event. Returns SUNSTONE_DEADLINE_NOTIFIED (0), EXPIRED (1), or
 * ERROR (-1). Early hints preserve the phase. After delayed checks, skip elapsed
 * phases and block until the first future phase. Clock/wait errors latch. */
int os_supervisor_wait_next(struct os_supervisor_wait *);
#endif
