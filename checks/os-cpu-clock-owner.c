/* Actual OS owner ordering; driver/device completion is modeled. The real
 * grant decoder and DSU table select the requests below. */
#include <string.h>
#define a733_cpu_clock_init model_init
#define a733_cpu_clock_inspect model_inspect
#define a733_cpu_clock_burst model_burst
#define a733_cpu_clock_request model_request
#include "../apps/os/cpu.c"
#undef a733_cpu_clock_init
#undef a733_cpu_clock_inspect
#undef a733_cpu_clock_burst
#undef a733_cpu_clock_request
struct board_application board;
struct board_provider providers;
static uint32_t fail_site,fail_value;static unsigned fail_notes;
void board_fail_note(uint32_t site,uint32_t value,const void *caller){(void)caller;fail_site=site;fail_value=value;++fail_notes;}
static struct os_session session;
static struct a733_thermal_policy thermal_policy;
static uint64_t time_us=100;
static unsigned inspections,requests,domains[3],targets[3],pending,selected;
volatile unsigned os_check_result;
static void check(int ok,unsigned code)
{if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
const struct os_session *os_session_admitted(void){return &session;}
int os_granted(unsigned mask){return mask&&(session.granted&mask)==mask;}
/* The performance service (checks/perf-os-model.c models it): a grant without perf never reaches it, in FOLLOW or
 * before. With perf its first step answers "not yet" and its second a terminal failure, whose state word the refusal
 * must record. */
static unsigned perf_steps;
#define PERF_WORD (UINT32_C(0x80000000)|UINT32_C(0x2a)<<4)
int os_perf_step(struct a733_cpu_clock *c,const struct a733_thermal_policy *p,int burst)
{(void)c;(void)p;(void)burst;check(session.cpu_clock.perf!=0,90);return ++perf_steps==1 ? 0 : -1;}
uint32_t os_perf_fault_word(int burst){check(session.cpu_clock.perf!=0,92);return PERF_WORD|(uint32_t)(burst&7);}
/* The supervised lifetime: the returning units (apps/os/cpu-return*.c) are outside this owner model, and the entry guard
 * that the returning lifetime adds is never reached. */
unsigned board_session_lifetime(void){return A733_LIFETIME_SUPERVISED;}
int os_cpu_return_requested(void){return 0;}
int os_cpu_return_failing(void){return 0;}
/* The readiness publication wakes the application's main task once (apps/os/cpu.c publish_ready). */
static unsigned ready_wakes;
void os_application_wake_main(void){++ready_wakes;}
uint32_t os_cpu_return_entry_refusal;
int os_cpu_return_entry_guard(const struct a733_cpu_clock *c,const struct a733_thermal_policy *p,uint64_t now,
    int (*read)(void *,uintptr_t,uint32_t *),void *context){(void)c;(void)p;(void)now;(void)read;(void)context;check(0,91);return -1;}
int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=++time_us;return 1;}
int model_init(struct a733_cpu_clock *c,const struct a733_cpu_clock_config *config,
    const struct a733_cpu_clock_ops *ops,void *cookie,struct a733_clock_leases *clocks,
    struct a733_rail_leases *rails,struct a733_pmic *pmic,const struct a733_thermal_policy *policy)
{
    (void)ops;(void)cookie;(void)clocks;(void)rails;(void)pmic;(void)policy;
    c->config=*config;c->report.binned=1;c->report.bin=4;
    c->applied_mhz[0]=494;c->applied_mhz[1]=1014;return 1;
}
enum a733_cpu_clock_status model_inspect(struct a733_cpu_clock *c,enum a733_cpu_domain d,uint64_t end)
{(void)c;check(end>time_us&&d==inspections++,2);pending=1;return A733_CPU_CLOCK_PENDING;}
enum a733_cpu_clock_status model_burst(struct a733_cpu_clock *c,uint64_t budget)
{
    (void)c;check(budget==200,3);
    if(pending){pending=0;return A733_CPU_CLOCK_COMPLETE;}
    return A733_CPU_CLOCK_READY;
}
enum a733_cpu_clock_status model_request(struct a733_cpu_clock *c,enum a733_cpu_domain d,uint32_t target,uint64_t end)
{
    check(inspections==2&&requests<3&&end>time_us&&!os_cpu_clock_ready(),4);
    domains[requests]=d;targets[requests++]=target;
    if(selected==2&&requests==1)return A733_CPU_CLOCK_RETAINED;
    if(d<2)c->applied_mhz[d]=target;
    pending=1;return A733_CPU_CLOCK_PENDING;
}
void os_cpu_owner_check(unsigned mode)
{
    selected=mode;
    session.board.epoch=1;session.board.generation=2;session.granted=OS_GRANT_CPU_CLOCK;
    struct a733_cpu_clock_grant_record *g=&session.cpu_clock;
    g->magic=A733_CPU_CLOCK_GRANT_MAGIC;g->version=1;g->bytes=sizeof(*g);g->owner=10;
    if(mode!=1){g->epoch=1;g->generation=2;g->clusters=3;g->max_mhz[0]=1794;g->max_mhz[1]=2002;
        g->changes=4;}
    if(mode==3)g->perf=1;
    thermal_policy.initialized=1;atomic_init(&thermal_policy.published_seen,0);
    check(os_cpu_clock_step(&thermal_policy)==1,5);
    check(os_cpu_clock_step(&thermal_policy)==1,6);
    check(os_cpu_clock_step(&thermal_policy)==1&&inspections==2&&!requests,7);
    if(mode==1){check(os_cpu_clock_ready(),8);goto done;}
    if(mode==3){
        /* With perf the inspections lead straight to FOLLOW and the service; its failure ends the owner with the
         * service's word in the record, in phase FOLLOW. */
        check(os_cpu_clock_step(&thermal_policy)==1&&perf_steps==1&&!requests,20);
        check(os_cpu_clock_step(&thermal_policy)==-1&&perf_steps==2&&fail_notes==1,21);
        check(fail_value==(PERF_WORD|A733_CPU_CLOCK_READY)&&(fail_site>>24&3u)==FOLLOW,22);
        goto done;
    }
    check(!os_cpu_clock_ready(),9);
    atomic_store_explicit(&thermal_policy.published_seen,1,memory_order_release);
    if(mode==2){check(os_cpu_clock_step(&thermal_policy)==-1&&!os_cpu_clock_ready()&&requests==1,10);goto done;}
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==1,11);
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==1&&!os_cpu_clock_ready(),12);
    atomic_store_explicit(&thermal_policy.published_seen,9,memory_order_release);
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==2,13);
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==3&&!os_cpu_clock_ready()&&!ready_wakes,14);
    check(os_cpu_clock_step(&thermal_policy)==1&&os_cpu_clock_ready()&&ready_wakes==1,15);
    check(domains[0]==A733_CPU_BIG&&domains[1]==A733_CPU_LITTLE&&domains[2]==A733_CPU_DSU,16);
    check(targets[0]==2002&&targets[1]==1794&&targets[2]==1274,17);
    check(owner.count==3,18);
    /* FOLLOW without perf: later steps follow the driver and never reach the performance service, so a session
     * without perf makes no readback snapshot (the radio session's board run failed in that service, cpu.c 275). */
    for(unsigned i=0;i<8;++i)check(os_cpu_clock_step(&thermal_policy)==1&&requests==3&&!perf_steps&&!fail_notes,19);
done:
    os_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
