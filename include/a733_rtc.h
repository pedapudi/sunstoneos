/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_RTC_H
#define SUNSTONE_A733_RTC_H
#include <stdint.h>
#include <stdatomic.h>
/* Fixed rtc-v201 TypeA: day1 is1970-01-01. No calendar/clock/reset writes. */
struct a733_rtc_time {uint32_t day,hour,minute,second,year,month,date;};
struct a733_rtc_snapshot {
    struct a733_rtc_time initial,terminal;
    uint32_t target_day,target_hms,saved_day,saved_hms,clock_control,clock_status;
    uint32_t alarm_seen,cancelled;
};
struct a733_rtc_ops {
    int (*now_us)(void *,uint64_t *);
    int (*read)(void *,uint32_t,uint32_t *);
    int (*write)(void *,uint32_t,uint32_t);
};
enum a733_rtc_state {A733_RTC_EMPTY,A733_RTC_READY,A733_RTC_ARMED,
    A733_RTC_FIRED,A733_RTC_CANCELLED,A733_RTC_RETAINED};
struct a733_rtc {
    struct a733_rtc_ops ops;void *context;uint64_t deadline,last;
    struct a733_rtc_snapshot snapshot;
    uint32_t saved_enable,saved_irq_enable;
    atomic_uint owned,irq_seen,irq_fault;
    enum a733_rtc_state state;
};
/* Own RTC registers/IRQ exclusively, with stable source and existing calendar.
 * Caller masks the SPI and confirms it inactive before every operation.
 * A single owner task performs all RTC operations, including irq().
 * Failed operations retain every reservation and never attempt rollback. */
int a733_rtc_open(struct a733_rtc *,const struct a733_rtc_ops *,void *,uint64_t);
int a733_rtc_arm(struct a733_rtc *,unsigned seconds);
int a733_rtc_irq(struct a733_rtc *);
/* After masking and confirming GIC inactive and mask propagation complete. Cancellation
 * may acknowledge only the alarm started by this instance. Inherited pending
 * state is refused by open(). Restore saved disabled targets only after quiet
 * readback; clock, calendar, wake output and general-purpose words stay intact. */
int a733_rtc_finish(struct a733_rtc *,int cancel);
#endif
