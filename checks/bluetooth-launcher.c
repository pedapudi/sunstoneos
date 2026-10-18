/* Only the Bluetooth wrapper's additional immutable admission. The existing
 * supervisor is an explicit host stub; its ARM launcher evidence is reused. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include "a733_board_application.h"
#include "../apps/bluetooth/launch.c"
int main(void)
{
    struct a733_board_session *session=mmap((void *)A733_BOARD_SESSION_ADDRESS,4096,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(session==(void *)A733_BOARD_SESSION_ADDRESS);
    struct a733_board_session baseline={.magic=A733_BOARD_SESSION_MAGIC,.version=A733_BOARD_SESSION_VERSION,
        .bytes=sizeof(*session),.epoch=31,.generation=37,.lease=41,.workload=2,.sid_authority=7,.sid_evidence={1}};
    request_model[3]=31;request_model[4]=37;request_model[6]=41;
    *session=baseline;assert(a733_smp_launch_prepare()==1&&supervisor_calls==1);
    uint64_t *identity[]={&session->magic,&session->version,&session->bytes,&session->epoch,
        &session->generation,&session->lease,&session->sid_authority};
    for(unsigned i=0;i<sizeof(identity)/sizeof(*identity);++i){*session=baseline;*identity[i]=0;
        assert(!a733_smp_launch_prepare()&&refusal_model==14&&supervisor_calls==1);}
    *session=baseline;session->workload=0;assert(!a733_smp_launch_prepare()&&supervisor_calls==1);
    *session=baseline;session->sid_evidence[0]=0;assert(!a733_smp_launch_prepare()&&supervisor_calls==1);
    uint64_t *watchdog[]={&session->minimum_reload_us,&session->maximum_recovery_us,&session->guard_us,
        &session->reset_by_us,
        &session->deadline_us,&session->session_start_ticks,&session->reload_after_ticks};
    for(unsigned i=0;i<sizeof(watchdog)/sizeof(*watchdog);++i){*session=baseline;*watchdog[i]=1;
        assert(!a733_smp_launch_prepare()&&supervisor_calls==1);}
    uint32_t *rtc[]={&session->rtc_control,&session->rtc_divider,&session->rtc_source};
    for(unsigned i=0;i<sizeof(rtc)/sizeof(*rtc);++i){*session=baseline;*rtc[i]=1;
        assert(!a733_smp_launch_prepare()&&supervisor_calls==1);}
    assert(!munmap(session,4096));puts("PASS Bluetooth launcher: actual wrapper validates session, workload, SID and absent watchdog authority before unchanged supervisor");
    return 0;
}
