/* Host model of the resident reboot path: apps/os/services.c and runtime/deadline.c on
 * the real FreeRTOS kernel under the host shim (checks/host/), with a modeled WDT0, a
 * supervising task every 20 ms and one park of each core at every phase of the
 * renewal second. The counter is the shim's A733 counter, advanced 1 ms per step. checks/reset-latency.py builds it
 * twice, with the changed services.c and SUNSTONE_A733_FAULT_PAGE and with
 * the baseline services.c without it.
 *
 * A park on core c stores byte c of a733_fault_pending, as the EL3 handler
 * does on every entry (checks/fault-capture.py executes that store for each
 * kind of park). The model takes the worst case for the baseline: every
 * supervised service and the supervisor run on other cores and keep running.
 * A park of core 0 also stops the tick, and with it every task.
 *
 *   reset-latency PHASE_STEP_MS   one JSON line per case on stdout */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_rtos.h"
static uint64_t ticks;
#define HZ UINT64_C(24000000)
static uint64_t expires,reloads,last_reload;
int os_watchdog_reload(void *supervisor)
{
    assert(supervisor==&ticks);
    if(ticks>=expires)return 0;
    expires=ticks+4*HZ;++reloads;last_reload=ticks;return 1;
}
#ifdef SUNSTONE_A733_FAULT_PAGE
uint64_t a733_fault_pending;
#endif
/* No service in this model registers for precise deadlines, so every service
 * keeps the periodic-progress path; checks/deadline-host.c covers the
 * registered-owner path. */
#include "sunstone_deadline.h"
#include SERVICES_SOURCE
#ifdef SUNSTONE_OS_APPLICATION_H
/* Keep every service selected: the modeled application is present and live,
 * and workload 4 creates the Type-C, power and USB host service tasks. */
static const struct ss_manifest model_manifest={0};
const struct ss_application ss_application={.manifest=&model_manifest};
int os_application_finished(void){return 0;}
/* The modeled NPU service never finishes, so it keeps the periodic-progress path as every other service. */
int os_npu_complete(void){return 0;}
const struct os_session *os_session_admitted(void)
{
#define EVERY(NAME,bit,with) |OS_GRANT_##NAME
    static const struct os_session session={.board={.workload=4},.granted=0 OS_GRANT_ROWS(EVERY)};
#undef EVERY
    return &session;
}
/* The runtime deadline is the board's own, beyond the longest case here. */
uint64_t board_runtime_deadline_us(void){return UINT64_C(3600)*1000000;}
/* The image's table (tools/sunstone-manifest.py): the board's services, the application, then the declared ones. The
 * model's session grants every service, so the supervisor requires every one of them. */
const struct os_service os_services[OS_SERVICES]={
    {"board-pmic",1000000},{"board-usb0",1000000},{"board-typec",1000000},
    {"board-power",1000000},{"board-usb2",1000000},
    {"application",1000000},OS_SERVICE_TABLE
};
#endif

struct outcome { uint64_t expired,refused,detail,reloads; };
/* One run in 1 ms steps from scheduler start. Services report every 10 ms
 * from 0.3 s; `stalled` stops reporting at stall_at (never reports when
 * stall_at is 0). Core park_core parks at park_at. */
static struct outcome run(int park_core,uint64_t park_at,int stalled,uint64_t stall_at,uint64_t end)
{
    struct outcome o={0,0,0,0};
    int stopped=0;
    ticks=0;expires=4*HZ;reloads=0;last_reload=0;
    for(uint64_t ms=0;ticks<end;++ms){
        if(ms)host_advance_us(1000);
        ticks=host_counter();
        if(ticks>=expires){o.expired=ticks;break;}
        int parked=park_core>=0&&ticks>=park_at;
#ifdef SUNSTONE_A733_FAULT_PAGE
        if(parked)((volatile uint8_t *)&a733_fault_pending)[park_core]=1;
#endif
        if(parked&&park_core==0)continue; /* no tick: no task runs */
        for(int s=0;s<OS_SERVICES;++s){
            if(ticks<HZ*3/10||(s==stalled&&(!stall_at||ticks>=stall_at)))continue;
            if(ms%10==0)board_service_progress((unsigned)s);
        }
        if(!stopped&&ms%20==0&&board_resident_renew(&ticks,4*HZ)!=1){
            /* apps/board/main.c: board_fail, then a733_smp_exit_privileged(5, detail). */
            stopped=1;o.refused=ticks;
#ifdef SUNSTONE_A733_FAULT_PAGE
            o.detail=board_resident_stop_detail();
#endif
        }
    }
    o.reloads=reloads;return o;
}
static void report(const char *name,int core,uint64_t at,struct outcome o)
{
    printf("{\"case\":\"%s\",\"core\":%d,\"at\":%llu,\"expired\":%llu,\"refused\":%llu,\"detail\":%llu,\"reloads\":%llu}\n",
        name,core,(unsigned long long)at,(unsigned long long)o.expired,(unsigned long long)o.refused,
        (unsigned long long)o.detail,(unsigned long long)o.reloads);
    fflush(stdout);
}
/* Each run in its own process: services.c keeps its records in statics, and one
 * scheduler session runs per process. */
struct job{const char *name;int core;uint64_t at;int stalled;uint64_t stall_at,end;};
static void job_task(void *argument)
{
    const struct job *j=argument;
    int core=j->core,stalled=j->stalled;uint64_t at=j->at,stall_at=j->stall_at;
    struct outcome o=run(core,at,stalled,stall_at,j->end);
#ifdef SUNSTONE_A733_FAULT_PAGE
    if(!stall_at&&stalled>=0){
        /* The watchdog fires at the loader's first expiry, before the
         * supervisor's pass there; that pass would refuse and name the
         * silent service. */
        expires=UINT64_MAX;assert(host_counter()==4*HZ);ticks=host_counter();
        assert(board_resident_renew(&ticks,4*HZ)==0);
        o.detail=board_resident_stop_detail();
    }
#endif
    report(j->name,core,at,o);
}
static void fork_run(const char *name,int core,uint64_t at,int stalled,uint64_t stall_at,uint64_t end)
{
    pid_t child=fork();
    if(!child){
        struct job j={name,core,at,stalled,stall_at,end};
        host_rtos_run(job_task,&j,2,UINT64_MAX/2);
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
int main(int argc,char **argv)
{
    unsigned step=argc>1?(unsigned)atoi(argv[1]):1;
    assert(step>=1&&step<=1000);
    /* Renewal settles at a fixed phase after start; parks cover one full
     * second of phases from 10 s. */
    for(int core=0;core<8;++core)
        for(unsigned phase=0;phase<1000;phase+=step){
            uint64_t at=10*HZ+phase*(HZ/1000);
            fork_run("park",core,at,-1,0,at+8*HZ);
        }
    /* The stop detail names the first stale or silent service. */
    for(int s=0;s<OS_SERVICES;++s){
        fork_run("stall",-1,0,s,20*HZ,30*HZ);
        fork_run("silent",-1,0,s,0,10*HZ);
    }
    fork_run("healthy",-1,0,-1,0,30*HZ);
    return 0;
}
