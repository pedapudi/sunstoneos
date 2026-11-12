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
enum a733_rtc_read_source {A733_RTC_READ_EXTERNAL=1,A733_RTC_READ_RC_NORMAL};
enum a733_rtc_state {A733_RTC_EMPTY,A733_RTC_READY,A733_RTC_ARMED,
    A733_RTC_FIRED,A733_RTC_CANCELLED,A733_RTC_RETAINED};
struct a733_rtc {
    struct a733_rtc_ops ops;void *context;uint64_t deadline,last;
    struct a733_rtc_snapshot snapshot;
    uint32_t saved_enable,saved_irq_enable;
    atomic_uint owned,irq_seen,irq_fault;
    enum a733_rtc_state state;
    /* Read-only capability and source observations are outside snapshot ABI. */
    uint32_t read_only,read_source,prescaler,calibration,oscillator;
    uint32_t rc_mode;
};
/* Own RTC registers/IRQ exclusively, with stable source and existing calendar.
 * Caller masks the SPI and confirms it inactive before every operation.
 * A single owner task performs all RTC operations, including irq().
 * Failed operations retain every reservation and never attempt rollback. */
int a733_rtc_open(struct a733_rtc *,const struct a733_rtc_ops *,void *,uint64_t);
/* Writable open while the RTC runs from the inherited internal RC oscillator
 * (normal RC, uncalibrated), the state the board is in. Same checks and ownership
 * as a733_rtc_open, with the RC source accepted and its control, prescaler,
 * calibration and oscillator words required stable. No clock-source write is
 * ever made. The RC rate is not asserted: the caller measures it against its
 * own time base (the board's RTC second took about 1.1 s). */
int a733_rtc_open_rc(struct a733_rtc *,const struct a733_rtc_ops *,void *,uint64_t);
/* Select the external 32.768 kHz crystal for an RC-mode instance in READY state, with the sequence of
 * a733_rtc_initialize's source step (manual 9.1.7): keep the crystal gate and the auto-switch field, require four
 * consecutive healthy samples (source status bit 2 clear, 3 us apart, within 1 s), select the external source (control
 * bits 1:0 = 01), clear the switch-pending bit, and wait for control & 0x193 == 0x11 with status & 7 == 1. Only the
 * control word (+0x00, with the key) and the status word (+0x04, bit 1) are written. If the crystal is not healthy or the
 * source does not settle, the control word is restored to its value on entry and the instance stays on the RC
 * (result A733_RTC_SWITCH_FALLBACK, the health observations in the record). The calendar is read coherently before
 * and after; the caller judges continuity. Returns 1 when the instance is usable (switched or fell back), 0 after a
 * failure that retains it. */
enum {A733_RTC_SWITCH_DONE=1,A733_RTC_SWITCH_FALLBACK=2};
struct a733_rtc_switch {
    uint32_t result,control_before,status_before,control_after,status_after,samples,healthy_run,writes;
    struct a733_rtc_time before,after;
    uint64_t before_us,after_us;
};
int a733_rtc_select_crystal(struct a733_rtc *,int (*pause)(void *,uint64_t),struct a733_rtc_switch *);
/* Set the calendar of an RC-mode instance (READY state): HMS then DAY, each
 * followed by the busy-bit wait and the 2 ms settle of the pinned rtc-sunxi.c
 * settime. pause(context, until_us) must return 1 after waiting without
 * holding any lock. The new value is verified by a coherent read: not before
 * the request and not later than it plus the elapsed time (rounded up);
 * the saved initial reading is replaced. Failure retains the instance. */
int a733_rtc_set_time(struct a733_rtc *,uint32_t day,uint32_t hms,int (*pause)(void *,uint64_t));
/* Explicit read-only adoption: external or normal (uncalibrated) RC only.
 * RC selection additionally reads +0x08/+0x0c/+0x160 through the supplied
 * callback. Caller establishes bus accessibility and exclusive source/calendar
 * ownership. No rate/accuracy claim is implied by RC adoption.
 * The retained instance has ops.write == NULL. It cannot arm/ack/finish alarms;
 * initialize/set_calendar reject these instance ops before any MMIO. Caller
 * raw ops, if retained independently, remain separate privileged authority. */
int a733_rtc_open_readonly(struct a733_rtc *,const struct a733_rtc_ops *,void *,uint64_t);
/* Coherent read by the same owner; permitted during its own active alarm and after it has finished
 * (fired or cancelled), when the saved alarm registers are restored. A retained instance refuses. */
int a733_rtc_read(struct a733_rtc *,struct a733_rtc_time *);
int a733_rtc_arm(struct a733_rtc *,unsigned seconds);
int a733_rtc_irq(struct a733_rtc *);
/* After masking and confirming GIC inactive and mask propagation complete. Cancellation
 * may acknowledge only the alarm started by this instance. Inherited pending
 * state is refused by open(). Restore saved disabled targets only after quiet
 * readback; clock, calendar, wake output and general-purpose words stay intact. */
int a733_rtc_finish(struct a733_rtc *,int cancel);
#endif
