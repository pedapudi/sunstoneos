/* Returning scheduler fixture: declared timers, one copied result, no device
 * access or attached application input. */
#include "event_waits-app.h"
#include <stdint.h>
#include "sunstone_bg.h"
#include "sunstone_tlbi.h"
#if !defined(SUNSTONE_BG_LOAD) || !defined(SUNSTONE_TLBI_ISSUER) || SUNSTONE_TLBI_ISSUER != 0 || !defined(SUNSTONE_TLBI_CORE) || SUNSTONE_TLBI_CORE != 5
#error "Select background measurement and the TLBI control form"
#endif
struct result {
    uint64_t magic,elapsed_us,stopping_us;
    uint64_t bursts[2],busy_us[2];
    uint32_t version,waits,stopped,tlbi_wakes,tlbi_issued,tlbi_core;
};
static struct result result;
_Static_assert(sizeof(struct result)<=128,"Result fits the declared output");
static uint64_t now(void){return ss_counter_ticks()/24;}
static uint64_t finish_by;
static void wait_ready(void)
{
    while(!ss_clock_ready()||ss_throttled()){
        uint64_t time=now();
        if(time>=finish_by||ss_live()<0)ss_fail(0x7c01);
        ss_progress();++result.waits;ss_wait_until(finish_by);
    }
}
static void wait_until(uint64_t until)
{
    for(uint64_t time=now();time<until;time=now()){
        if(time>=finish_by||ss_live()<0)ss_fail(0x7c01);
        ss_progress();++result.waits;ss_wait_until(until);
    }
}
static int at_top(void)
{
    char line[256];const char prefix[]="CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    if(ss_clock_info(line,sizeof(line))<(int)(sizeof(prefix)-1))return 0;
    for(unsigned i=0;i<sizeof(prefix)-1;++i)if(line[i]!=prefix[i])return 0;
    return 1;
}
void event_waits_main(const struct ss_grant *grant)
{
    if(!grant||grant->main_core!=6||grant->worker_cores!=0xbf||grant->devices||!grant->finish_by_us)ss_fail(0x7c02);
    finish_by=grant->finish_by_us;
    wait_ready();
    if(!at_top())ss_fail(0x7c03);
    struct sunstone_tlbi_sample first,last;
    if(!sunstone_tlbi_sample(&first)||first.form)ss_fail(0x7c07);
    uint64_t start=now();vSunstoneBgStart();
    wait_until(start+600000);vSunstoneBgPhase(2);wait_until(start+1200000);
    uint64_t stopping=now();vSunstoneBgStop();
    while(ulSunstoneBgStopped()!=SUNSTONE_BG_TASKS){
        uint64_t time=now();if(time>=finish_by)ss_fail(0x7c04);
        ss_progress();++result.waits;
        ss_wait_until(finish_by);
    }
    result.stopping_us=now()-stopping;result.elapsed_us=now()-start;
    for(unsigned p=0;p<2;++p){result.bursts[p]=ulSunstoneBgValue(p+1,0,0);result.busy_us[p]=ulSunstoneBgValue(p+1,1,0);}
    result.version=1;result.stopped=ulSunstoneBgStopped();
    if(!sunstone_tlbi_sample(&last)||last.form)ss_fail(0x7c07);
    result.tlbi_wakes=last.wakes-first.wakes;result.tlbi_issued=last.issued;result.tlbi_core=last.core;
    result.magic=UINT64_C(0x3153544941575645);
    if(!result.bursts[0]||!result.bursts[1]||!result.tlbi_wakes||result.tlbi_issued||!at_top())ss_fail(0x7c05);
    ss_finish(&result,sizeof(result));
}
