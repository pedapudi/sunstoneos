#ifndef SUNSTONE_RTC_H
#define SUNSTONE_RTC_H
/* RTC class: absolute time and one non-waking alarm. An application declares it with "rtc": true in its manifest.json.
 * The RTC runs from the internal RC oscillator the board starts with; the OS never changes the clock source, and the
 * RC rate is not asserted: ss_rtc_alarm reports the system-counter ticks across the alarm interval, from which the
 * caller derives the measured RTC second. Every call returns at once: 1 done, 0 not yet (repeat the identical call
 * after ss_rtc_wait), -1 refused or failed. One request is in flight. The alarm interrupt (INTID 228) is taken by the
 * OS owner task, which acknowledges it and wakes the application task. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ss_rtc_time {uint32_t day, hour, minute, second, year, month, date;}; /* day 1 is 1970-01-01 */
struct ss_rtc_info {
    struct ss_rtc_time initial;       /* the calendar when the RTC was opened */
    uint32_t clock_control, clock_status, prescaler, calibration, oscillator;
    uint32_t source;                  /* 1 external 32.768 kHz, 2 normal internal RC */
    uint64_t opened_ticks;            /* system counter at the open read */
};
enum {SS_RTC_ALARM_FIRED = 1, SS_RTC_ALARM_TIMEOUT = 2};
struct ss_rtc_alarm {
    int32_t status;
    uint32_t seconds;                 /* requested delay in RTC seconds, 1 to 10 */
    struct ss_rtc_time armed, terminal;
    uint32_t target_day, target_hms;
    uint64_t arm_ticks;               /* counter when the alarm was armed */
    uint64_t irq_ticks;               /* counter in the alarm interrupt handler */
    uint64_t done_ticks;              /* counter when the owner finished the alarm */
    uint32_t irq_count, reserved;
};
int ss_rtc_open(struct ss_rtc_info *info);
/* Set the calendar to day (1 = 1970-01-01) and hms (hour << 16 | minute << 8 | second). */
int ss_rtc_set(uint32_t day, uint32_t hms);
int ss_rtc_read(struct ss_rtc_time *time, uint64_t *ticks);
int ss_rtc_arm(unsigned seconds);
/* 1 once the alarm has fired (or timed out) and the RTC is restored; 0 while armed. */
int ss_rtc_alarm(struct ss_rtc_alarm *alarm);
void ss_rtc_wait(uint32_t wait_us);
int ss_rtc_close(void);
#ifdef __cplusplus
}
#endif
#endif
