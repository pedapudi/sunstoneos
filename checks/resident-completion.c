/* Production resident supervision with modeled service progress and WDT0.
 * The FreeRTOS shim supplies task context; no hardware is accessed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "host_rtos.h"
#include "os.h"
#include "application.h"
static uint64_t ticks=12000000;
static unsigned reloads,finished,succeeded;
uint64_t a733_fault_pending;
uint64_t board_runtime_deadline_us(void){return UINT64_MAX;}
int os_watchdog_reload(void *owner){assert(owner==&ticks);++reloads;return 1;}
int os_application_finished(void){return finished;}
int os_application_succeeded(void){return succeeded;}
int os_npu_complete(void){return 0;}
static const struct ss_manifest manifest={0};
const struct ss_application ss_application={.manifest=&manifest};
const struct os_session *os_session_admitted(void)
{
#define EVERY(name,bit,with) |OS_GRANT_##name
    static const struct os_session s={.board={.workload=4},.granted=0 OS_GRANT_ROWS(EVERY)};
#undef EVERY
    return &s;
}
const struct os_service os_services[OS_SERVICES]={
    {"pmic",1000000},{"usb0",1000000},{"typec",1000000},
    {"power",1000000},{"usb2",1000000},{"application",1000000},OS_SERVICE_TABLE};
#include SERVICES_SOURCE
static void task(void *argument)
{
    unsigned mode=*(unsigned *)argument;
    host_advance_us(1000000);ticks=host_counter();
    if(mode==0){ /* board_live refusal bypasses renewal entirely. */
        assert(!board_resident_completion_stop());
#ifdef SUNSTONE_A733_OS_RESIDENT_STOP_ON_FINISH
        assert(board_resident_stop_detail()==0xfff);
#endif
        return;
    }
    if(mode==1){ /* Completion cannot hide selected services that never report. */
        finished=succeeded=1;
        assert(board_resident_renew(&ticks,4*24000000)==1);
        assert(!reloads&&!board_resident_completion_stop());return;
    }
    for(unsigned i=0;i<OS_SERVICES;++i)board_service_progress(i);
    assert(board_resident_renew(&ticks,4*24000000)==1&&reloads==1);
    finished=1;succeeded=mode!=2;
    if(mode==3)a733_fault_pending=1;
    if(mode==4){host_advance_us(2000000);ticks=host_counter();} /* Stale service. */
    unsigned before=reloads;
    int result=board_resident_renew(&ticks,4*24000000);
    if(mode==3||mode==4){
        assert(result==0&&reloads==before&&!board_resident_completion_stop());
        if(mode==4)assert(board_resident_stop_detail()!=0);
        return;
    }
#ifdef SUNSTONE_A733_OS_RESIDENT_STOP_ON_FINISH
    if(succeeded){
        assert(result==0&&reloads==before&&board_resident_completion_stop());
        assert(board_resident_stop_detail()==0);
        a733_fault_pending=1; /* A later fault cannot retain a successful detail. */
        assert(board_resident_stop_detail()!=0);
    }else assert(result==1&&!board_resident_completion_stop());
#else
    assert(result==1&&!board_resident_completion_stop());
    host_advance_us(1000000);ticks=host_counter();
    for(unsigned i=0;i<OS_SERVICES;++i)board_service_progress(i);
    assert(board_resident_renew(&ticks,4*24000000)==1&&reloads==before+1);
#endif
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned mode=(unsigned)atoi(argv[1]);
    host_rtos_run(task,&mode,2,10000000);
    puts("PASS resident completion");return 0;
}
