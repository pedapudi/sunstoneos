#ifndef SUNSTONE_OS_CPU_RETURN_STEP_H
#define SUNSTONE_OS_CPU_RETURN_STEP_H
/* What the supervisor (os_cpu_return_supervise, a task pinned to core 0) decides about a return, as functions of the flags the rest
 * of the OS published. Host checks (checks/os-failure-step.py) drive them.
 *
 * A failure return of an application that holds the NPU follows docs/designs/failure-return.md: the claim marks the application
 * finished (ss_live() is -1, board.fatal stays clear), the application's NPU owner closes its queue when it sees that, the NPU
 * client then publishes its report, and the supervisor waits for that for OS_FAILURE_RETURN_US before it ends the failure at the
 * watchdog. The supervisor does not stop the NPU itself: the queue is the application's to close, and a stop forced from outside
 * ends in the NPU client's retained path, which is a device failure (board.fatal) that only the watchdog releases. */
#include <stdint.h>
enum os_failure_step {
    OS_FAILURE_STEP_NONE,       /* nothing to do about the failure or the devices */
    OS_FAILURE_STEP_TIMEOUT,    /* the failure return has run longer than the limit: end at the watchdog (0x742) */
    OS_FAILURE_STEP_WAIT,       /* a failure return waits for the application's NPU owner to close its queue */
    OS_FAILURE_STEP_STOP_NPU,   /* never returned: the supervisor stopping the NPU from outside (6eb9a01e), which ended in a device failure */
    OS_FAILURE_STEP_DEVICES     /* a normal return found the devices not ready: stop at EL3 (0x741) */
};
static inline enum os_failure_step os_cpu_return_failure_step(int failing,int requested,int devices_ready,
                                                              uint64_t now_us,uint64_t since_us,uint64_t limit_us)
{
    if(failing&&now_us-since_us>=limit_us)return OS_FAILURE_STEP_TIMEOUT;
    if(!requested||devices_ready)return OS_FAILURE_STEP_NONE;
    return failing?OS_FAILURE_STEP_WAIT:OS_FAILURE_STEP_DEVICES;
}
/* The thermal owner's step once the return is requested and the CPU clock is lowered (os_thermal_task): it stops its converter only when the
 * devices are retired. A normal return starts after the application retired its NPU, so not ready there is a defect (exit). A failure return
 * starts at the failure: the application's NPU owner closes its queue only after it sees ss_live() == -1, so the owner waits for it; the supervisor
 * ends a return that takes longer than OS_FAILURE_RETURN_US (0x742). Exiting at once was the setter of board.fatal in the NPU failure test
 * (thermal.c line 381, found by the retained second record). */
enum os_thermal_step { OS_THERMAL_STEP_STOP, OS_THERMAL_STEP_WAIT, OS_THERMAL_STEP_EXIT };
static inline enum os_thermal_step os_cpu_return_thermal_step(int devices_ready,int failing)
{
    if(devices_ready)return OS_THERMAL_STEP_STOP;
    return failing?OS_THERMAL_STEP_WAIT:OS_THERMAL_STEP_EXIT;
}
/* bounded(): the supervisor's check that the board is live. board_live() is the three terms below (apps/board/main.c; live_until is the
 * session deadline in this lifetime). The EL3 stop detail says which one refused, so that a stop is told apart from the record's
 * first failure: 0x736 is kept for the session deadline, 0x744 for board.fatal without a device record, and 0x800 | the source line of
 * the NPU service's first failure (os_npu_first_failure) when board.fatal is set and the NPU recorded one. 0 means live. */
#define OS_BOUNDED_DEADLINE 0x736u
#define OS_BOUNDED_FATAL 0x744u
#define OS_BOUNDED_FATAL_NPU 0x800u
static inline unsigned os_cpu_return_bounded_detail(int fatal,uint64_t now_us,uint64_t live_until_us,uint64_t deadline_us,
                                                    int late,unsigned npu_line)
{
    if(fatal)return npu_line?OS_BOUNDED_FATAL_NPU|(npu_line&0x3ffu):OS_BOUNDED_FATAL;
    if(now_us>=live_until_us||now_us>=deadline_us||late)return OS_BOUNDED_DEADLINE;
    return 0;
}
#endif
