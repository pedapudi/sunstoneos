/* Crystal selection qualification. Open the RTC on the inherited RC, time five RTC seconds against the system
 * counter, select the 32.768 kHz crystal, read the calendar across the switch, time ten RTC seconds, then repeat the
 * set, alarm and read of rtc-alarm on the new source. apps/rtc-crystal/host.py decodes the result. */
#include "rtc_crystal-app.h"
#include "sunstone_rtc.h"
#define MAGIC UINT64_C(0x315f5359525f4354)
#define SET_DAY 20770u
#define SET_HMS ((5u<<16))
struct report {
    uint64_t magic;
    uint32_t version, failed, step, reserved;
    struct ss_rtc_info info;
    struct ss_rtc_rate rc_rate;
    struct ss_rtc_switch crystal;
    struct ss_rtc_rate crystal_rate;
    struct ss_rtc_time after_set;
    uint64_t after_set_ticks;
    struct ss_rtc_alarm alarm;
    struct ss_rtc_time final;
    uint64_t final_ticks;
};
static struct report report;
#define STEP(call) do{int r_; report.step++; while((r_=(call))==0){ss_progress();ss_rtc_wait(500000);} if(r_<0){report.failed=report.step;ss_finish(&report,sizeof report);}}while(0)
void rtc_crystal_main(const struct ss_grant *grant)
{
    (void)grant;
    while(!ss_clock_ready()||ss_throttled()){if(ss_live()<0)ss_fail(0x1c22);ss_progress();ss_usb_wait(1000);}
    report.magic=MAGIC;report.version=1;
    STEP(ss_rtc_open_with(&report.info,SS_RTC_KEEP_RC));
    ss_progress();
    STEP(ss_rtc_measure(5,&report.rc_rate));
    ss_progress();
    STEP(ss_rtc_select_crystal(&report.crystal));
    ss_progress();
    STEP(ss_rtc_measure(10,&report.crystal_rate));
    ss_progress();
    STEP(ss_rtc_set(SET_DAY,SET_HMS));
    STEP(ss_rtc_read(&report.after_set,&report.after_set_ticks));
    STEP(ss_rtc_arm(3));
    int r;
    while((r=ss_rtc_alarm(&report.alarm))==0){ss_progress();ss_rtc_wait(500000);}
    if(r<0)report.failed=100;
    STEP(ss_rtc_read(&report.final,&report.final_ticks));
    STEP(ss_rtc_close());
    ss_finish(&report,sizeof report);
}
