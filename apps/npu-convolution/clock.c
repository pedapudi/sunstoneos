#include "clock.h"
#include "a733_clock_reference.h"
#include "a733_npu_platform_native.h"
#define CCU UINT32_C(0x02002000)
#define BIT(n) (UINT32_C(1)<<(n))
static int context(struct npu_application_clock *p)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    return p && p->initialized && el==4 && a733_npu_application_health() &&
        xTaskGetSchedulerState()==taskSCHEDULER_RUNNING && xTaskGetCurrentTaskHandle()==p->task;
}
/* Ownership is checked once per locked pass, by acquire, enter and leave.
 * Clock samples and register accesses need only the lock that enter took. */
static int now(void *cookie,uint64_t *out)
{
    struct npu_application_clock *p=cookie;
    return a733_counter_clock_now(p->counter,out);
}
static int enter(void *cookie,uint64_t deadline)
{
    struct npu_application_clock *p=cookie;
    if(!context(p) || p->held || deadline!=p->admitted.deadline_us)return -1;
    int r=a733_platform_lock_enter(p->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
    if(r==1)p->held=1;
    return r;
}
static int leave(void *cookie)
{
    struct npu_application_clock *p=cookie;
    if(!context(p) || !p->held)return 0;
    a733_platform_lock_leave(p->locks,A733_PLATFORM_REGISTER_LOCK);p->held=0;return 1;
}
static void retain(void *cookie,const struct a733_npu_pll_resources *a,
    const struct a733_npu_pll_resources *b,unsigned held)
{
    struct npu_application_clock *p=cookie;(void)a;(void)b;(void)held;
    /* Publish failure without waiting for a contended ledger. All acquired
     * records remain allocated; no unlock, register write or release occurs. */
    atomic_store_explicit(&p->registry->failed,1,memory_order_release);
    if(p->held)atomic_fetch_or_explicit(&p->locks->retained,1u<<A733_PLATFORM_REGISTER_LOCK,memory_order_release);
    a733_npu_application_fail(18);
}
static int read_reg(void *cookie,uintptr_t address,uint32_t *out)
{
    struct npu_application_clock *p=cookie;
    return p->held && a733_npu_pll_native_read(NULL,address,out);
}
static int write_reg(void *cookie,uintptr_t address,uint32_t value)
{
    struct npu_application_clock *p=cookie;
    return p->held && a733_npu_pll_native_write(NULL,address,value);
}
static int parent_read(struct npu_application_clock *p,unsigned off,uint32_t *out)
{return p->held && a733_npu_platform_native_read(NULL,CCU+off,out);}
static int parents(struct npu_application_clock *p,a733_clock_mask_t shared)
{
    uint32_t osc,ref,pll,a,b,automatic,status;
    if(!read_reg(p,0x07090160,&osc) || !parent_read(p,0,&ref) ||
       !a733_clock_ref24_valid(osc,ref))return 0;
    unsigned n;
    if(!(shared&A733_CLOCK_PERI0))return 1;
    /* Same fixed parent policy as the local startup controller, checked here
     * before PLL writes. The local controller checks it again before use. */
    if(!parent_read(p,0xa0,&pll) || (pll&0xfe000000)!=0xfe000000 || (pll&BIT(1)) ||
       !parent_read(p,0xa8,&a) || (a&BIT(31)) || !parent_read(p,0xac,&b) || (b&BIT(27)) ||
       !parent_read(p,0x1908,&automatic) || !parent_read(p,0x1988,&status))return 0;
    n=((pll>>8)&255)+1;if(n<53 || n>105)return 0;
    static const unsigned mhz[3][8]={{24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},
        {24,600,0,480,400,0,0,0}};
    for(unsigned i=0;i<3;++i) {
        unsigned rate=mhz[i][(p->admitted.consumers[i]>>24)&7],divisor,mask;
        switch(rate) {
        case 24:continue;
        case 600:divisor=(((pll>>20)&7)+1)*2;mask=BIT(11)|BIT(9);break;
        case 400:divisor=(((pll>>20)&7)+1)*3;mask=BIT(11)|BIT(2)|BIT(1);break;
        case 800:divisor=((pll>>16)&7)+1;mask=BIT(10);break;
        case 480:divisor=((pll>>2)&7)+1;mask=BIT(8)|BIT(7);break;
        default:return 0;
        }
        unsigned gates=mask<<16;
        if((automatic&(mask|gates))!=(mask|gates) || (status&gates)!=gates || 24*n!=rate*divisor)return 0;
    }
    return 1;
}
static int same_config(const struct a733_npu_pll_config *a,const struct a733_npu_pll_config *b)
{
    if(!a || a->owner!=b->owner || a->handoff!=b->handoff || a->deadline_us!=b->deadline_us ||
       a->mode!=b->mode || a->initial_control!=b->initial_control)return 0;
    for(unsigned i=0;i<A733_NPU_PLL_SNAPSHOTS;++i)if(a->consumers[i]!=b->consumers[i])return 0;
    return 1;
}
static enum a733_npu_pll_result acquire(void *cookie,const struct a733_npu_pll_config *config,
    a733_clock_mask_t shared,a733_clock_mask_t exclusive,struct a733_npu_pll_resources *out)
{
    struct npu_application_clock *p=cookie;a733_clock_mask_t expected_shared,expected_exclusive;
    if(!context(p) || !same_config(config,&p->admitted) ||
       !a733_npu_pll_scopes(&p->admitted,&expected_shared,&expected_exclusive) ||
       shared!=expected_shared || exclusive!=expected_exclusive ||
       (out->whole_platform && out->whole_platform!=p->whole_platform))return A733_NPU_PLL_RETAINED;
    if(!out->clock.id) {
        enum a733_clock_result r=a733_clock_acquire(p->registry,config->owner,shared,exclusive,
            config->deadline_us,&out->clock);
        if(r==A733_CLOCK_BUSY)return A733_NPU_PLL_BUSY;
        if(r!=A733_CLOCK_OK)return A733_NPU_PLL_RETAINED;
        out->whole_platform=p->whole_platform;
    } else {
        enum a733_clock_result r=a733_clock_token_status(p->registry,&out->clock);
        if(r==A733_CLOCK_BUSY)return A733_NPU_PLL_PENDING;
        if(r!=A733_CLOCK_OK)return A733_NPU_PLL_RETAINED;
    }
    int locked=enter(p,config->deadline_us);
    if(!locked)return A733_NPU_PLL_PENDING;
    if(locked!=1 || !parents(p,shared))return A733_NPU_PLL_RETAINED;
    if(!leave(p))return A733_NPU_PLL_RETAINED;
    return A733_NPU_PLL_READY;
}
int npu_application_clock_init(struct npu_application_clock *p,const struct npu_session *s,
    struct a733_clock_leases *registry,struct a733_platform_locks *locks,struct a733_counter_clock *counter,
    TaskHandle_t task,uint64_t deadline)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(!p || p->initialized || !s || s!=npu_session_get() || !npu_session_valid(s) || !registry || !locks || !counter ||
       !task || !deadline || el!=4 || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
       !registry->initialized || !locks->initialized || registry->now!=a733_counter_clock_now ||
       registry->context!=counter || locks->now!=a733_counter_clock_now || locks->context!=counter)return 0;
    p->admitted=s->pll;p->admitted.deadline_us=deadline;
    p->registry=registry;p->locks=locks;p->counter=counter;p->task=task;
    p->whole_platform=s->whole_platform;
    const struct a733_npu_pll_operations ops={acquire,enter,leave,retain,now,read_reg,write_reg};
    if(!a733_npu_pll_init(&p->pll,&p->admitted,&ops,p) || !a733_npu_pll_bind_registry(&p->pll,registry))return 0;
    p->initialized=1;return 1;
}
