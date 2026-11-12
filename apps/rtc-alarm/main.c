/* RTC qualification: read the calendar the RTC holds when the application starts (the retention datum: a calendar set by
 * an earlier run survives the return to the host), set a known calendar, read it back, arm an alarm three RTC seconds
 * ahead, receive it in the OS owner task, and read the calendar again. apps/rtc-alarm/host.py decodes the result and
 * states the measured RC rate; no 32.768 kHz accuracy is asserted. */
#include "rtc_alarm-app.h"
#include "sunstone_rtc.h"
#define MAGIC UINT64_C(0x315f4d4c4152435f)
#define SET_DAY 20770u            /* 2026-11-12 */
#define SET_HMS ((4u<<16)|(0u<<8)|0u)
struct report {
    uint64_t magic;
    uint32_t version, failed, step, set_day, set_hms, reserved;
    struct ss_rtc_info info;
    struct ss_rtc_time after_set;
    uint64_t after_set_ticks;
    struct ss_rtc_alarm alarm;
    struct ss_rtc_time final;
    uint64_t final_ticks, set_done_ticks;
};
static struct report report;
#define STEP(call) do{int r_; report.step++; while((r_=(call))==0)ss_rtc_wait(2000); if(r_<0){report.failed=report.step;ss_finish(&report,sizeof report);}}while(0)
void rtc_alarm_main(const struct ss_grant *grant)
{
    (void)grant;
    while(!ss_clock_ready()||ss_throttled()){if(ss_live()<0)ss_fail(0x1c21);ss_progress();ss_usb_wait(1000);}
    report.magic=MAGIC;report.version=1;report.set_day=SET_DAY;report.set_hms=SET_HMS;
    STEP(ss_rtc_open(&report.info));
    STEP(ss_rtc_set(SET_DAY,SET_HMS));
    report.set_done_ticks=ss_counter_ticks();
    STEP(ss_rtc_read(&report.after_set,&report.after_set_ticks));
    ss_progress();
    STEP(ss_rtc_arm(3));
    int r;
    while((r=ss_rtc_alarm(&report.alarm))==0){ss_progress();ss_rtc_wait(500000);}
    if(r<0)report.failed=100;
    STEP(ss_rtc_read(&report.final,&report.final_ticks));
    STEP(ss_rtc_close());
    ss_finish(&report,sizeof report);
}
