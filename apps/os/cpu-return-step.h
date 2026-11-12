#ifndef SUNSTONE_OS_CPU_RETURN_STEP_H
#define SUNSTONE_OS_CPU_RETURN_STEP_H
#include <stdint.h>
/* One bound shared by all registered owners, for both return modes. */
static inline int os_cpu_return_expired(uint64_t now_us,uint64_t since_us,uint64_t limit_us)
{return now_us-since_us>=limit_us;}
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
