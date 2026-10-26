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
static struct os_session session;
static struct a733_thermal_policy thermal_policy;
static uint64_t time_us=100;
static unsigned inspections,requests,domains[3],targets[3],pending,selected;
volatile unsigned os_check_result;
static void check(int ok,unsigned code)
{if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
const struct os_session *os_session_admitted(void){return &session;}
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
    session.board.epoch=1;session.board.generation=2;
    struct a733_cpu_clock_grant_record *g=&session.cpu_clock;
    g->magic=A733_CPU_CLOCK_GRANT_MAGIC;g->version=1;g->bytes=sizeof(*g);g->owner=10;
    if(mode!=1){g->epoch=1;g->generation=2;g->clusters=3;g->max_mhz[0]=1794;g->max_mhz[1]=2002;
        g->changes=4;memset(g->approval,1,32);}
    thermal_policy.initialized=1;atomic_init(&thermal_policy.published_seen,0);
    check(os_cpu_clock_step(&thermal_policy)==1,5);
    check(os_cpu_clock_step(&thermal_policy)==1,6);
    check(os_cpu_clock_step(&thermal_policy)==1&&inspections==2&&!requests,7);
    if(mode==1){check(os_cpu_clock_ready(),8);goto done;}
    check(!os_cpu_clock_ready(),9);
    atomic_store_explicit(&thermal_policy.published_seen,1,memory_order_release);
    if(mode==2){check(os_cpu_clock_step(&thermal_policy)==-1&&!os_cpu_clock_ready()&&requests==1,10);goto done;}
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==1,11);
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==1&&!os_cpu_clock_ready(),12);
    atomic_store_explicit(&thermal_policy.published_seen,9,memory_order_release);
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==2,13);
    check(os_cpu_clock_step(&thermal_policy)==1&&requests==3&&!os_cpu_clock_ready(),14);
    check(os_cpu_clock_step(&thermal_policy)==1&&os_cpu_clock_ready(),15);
    check(domains[0]==A733_CPU_BIG&&domains[1]==A733_CPU_LITTLE&&domains[2]==A733_CPU_DSU,16);
    check(targets[0]==2002&&targets[1]==1794&&targets[2]==1274,17);
    check(owner.count==3,18);
done:
    os_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
