/* SPDX-License-Identifier: Apache-2.0 */
/* Live NPU startup and GPU power composition over one actual PMIC provider,
 * rail ledger, clock registry, resource manager and power-domain owner.
 * Reuse the accepted register/clock/task model without rerunning its cases.
 * This is deterministic task interleaving, not a multicore or hardware test.
 * Local stop assumes the caller already established controller quiescence;
 * neither firmware drain nor OS thermal policy is modeled here. */
#define main npu_startup_existing_cases
#include "npu-startup-host.c"
#undef main
#include "a733_pmic_consumer.h"

static struct a733_gpu_power gpu;
static struct a733_pmic_consumer supply;
static struct a733_gpu_power_resources ready_gpu;
static uint64_t gpu_token, gpu_deadline;
static unsigned delegated, core_pending, both_pending;
static uint32_t core[9];

static int shared_read(void *v,uintptr_t a,uint32_t *out)
{
    uint32_t *w=level_word(a);
    if(w){*out=*w;return 1;}
    if(a==A733_POWER_PCK_DOMAIN(5)+8)return owner_read(&d,a,out);
    if(a==0x07066000 || a==0x07066008){*out=core[(a-0x07066000)/4];return 1;}
    return gpu_read(v,a,out);
}
static int shared_rail(void *v,struct a733_gpu_resource_rail *out,uint64_t deadline)
{
    assert(v==&r && atomic_load(&m.gpu.busy) && m.gpu.leases[9].token==gpu_token);
    return a733_pmic_consumer_take(&supply,&gpu,deadline,out);
}
/* As in a733_gpu_platform.c: the actual manager lease precedes controller
 * delegation, and manager release follows the controller's completed OFF. */
static int delegate(void *v,struct a733_gpu_power_resources *out)
{(void)v;assert(gpu_token && !delegated);delegated=1;*out=ready_gpu;return 1;}
static int undelegate(void *v)
{(void)v;assert(gpu_token && delegated && !gpu.row.counted);delegated=0;return 1;}
static int power_read(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t *out)
{
    (void)v;assert(gpu_token && delegated);
    if(bank==A733_GPU_CORE) {
        assert(gpu.row.counted && (d.ppu[5][2]&15)==8);
        assert(off==0 || off==8 || off==0x20);
        if(off==8 && core_pending){core[2]=core[0];core_pending=0;}
        *out=core[off/4];return 1;
    }
    uint32_t *w=level_word((bank==A733_GPU_CCU ? CCU_BASE : RCCU_BASE)+off);
    assert(w);*out=*w;return 1;
}
static int power_update(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t clear,uint32_t set)
{
    (void)v;assert(gpu_token && delegated && !r.held);
    if(bank==A733_GPU_CORE) {
        assert(gpu.row.counted && a733_power_domain_state(&gpu.row)==A733_POWER_ROW_ON);
        assert(clear==UINT32_MAX && ((off==0 && (set==0 || set==8)) || (off==0x20 && !set)));
        core[off/4]=set;if(!off)core_pending=1;return 1;
    }
    assert(bank==A733_GPU_CCU && (off==0xb20 || off==0xb24));
    if(gpu.phase==A733_GPU_POWER_STOPPING)assert(!core[0] && !core[2]);
    uint32_t *w=level_word(CCU_BASE+off);*w=(*w&~clear)|set;
    if(off==0xb20)*w&=~(1u<<27);
    return 1;
}
static void composed_setup(unsigned mode)
{
    setup();memset(&gpu,0,sizeof(gpu));memset(&supply,0,sizeof(supply));
    memset(core,0,sizeof(core));core[8]=0x010101ff;
    gpu_token=0;delegated=core_pending=both_pending=0;gpu_deadline=100000;
    /* Only the callbacks change; all callers retain the same manager address. */
    m.gpu.ops.read32=shared_read;m.gpu.ops.rail=shared_rail;
    const struct a733_pmic_consumer_config c={&m.provider,m.lifetime,9,A733_RAIL_DCDC4};
    assert(a733_pmic_consumer_init(&supply,&c));
    supply.recipient=&gpu;supply.destination=&m.gpu.rail;
    /* The first sample reserves the ledger token and yields, as the native
     * platform does; the later sample observes completed verification. */
    assert(a733_pmic_consumer_sample(&supply,gpu_deadline)==0);
    const struct a733_gpu_power_ops ops={gpu_now,delegate,undelegate,power_read,power_update};
    assert(a733_gpu_power_init(&gpu,&ops,&r));
    assert(a733_power_domain_row_init(&gpu.row,&d.owner,5,9,&supply.rail));
    if(mode==1)d.stall[5]=1;
    if(mode==2)d.stall[4]=1;
}
static void begin_gpu(void)
{
    assert(a733_pmic_consumer_sample(&supply,gpu_deadline)==1);
    assert(a733_pmic_consumer_health(&supply)==1);
    assert(a733_pmic_consumer_arm(&supply,gpu_deadline)==1);
    assert(a733_gpu_resources_acquire(&m.gpu,9,A733_GPU_RESOURCE_ALL,gpu_deadline,
        &gpu_token,&ready_gpu)==A733_GPU_RESOURCE_OK);
    assert(!supply.armed && !supply.request.id);
    assert(a733_gpu_power_on(&gpu,gpu_deadline)==A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_poll(&gpu)==A733_GPU_POWER_AGAIN);
    assert(a733_power_domain_state(&gpu.row)==A733_POWER_ROW_PENDING);
    assert(a733_power_domain_state(&s.row)==A733_POWER_ROW_PENDING);
    ++both_pending;
}
static void shared_conflicts(void)
{
    struct a733_rail_token rail={0};uint64_t change=0;
    struct a733_clock_token clock={0};
    assert(a733_rail_acquire(&m.rails,80,0,A733_RAIL_BIT(A733_RAIL_DCDC2),m.now+10000,&rail)==A733_RAIL_BUSY);
    assert(a733_rail_acquire(&m.rails,80,0,A733_RAIL_BIT(A733_RAIL_DCDC4),m.now+10000,&rail)==A733_RAIL_BUSY);
    assert(!rail.id);
    assert(a733_gpu_resources_change(&m.gpu,8,A733_GPU_RESOURCE_PCK|A733_GPU_RESOURCE_PARENT,&change)==A733_GPU_RESOURCE_BUSY);
    assert(!change);
    assert(a733_clock_acquire(&m.clocks,80,0,A733_CLOCK_PLL_NPU|A733_CLOCK_PERI0,m.now+10000,&clock)==A733_CLOCK_BUSY);
    assert(!clock.id);
}
static unsigned boot_pair(unsigned mode)
{
    enum a733_npu_platform_result nr=A733_NPU_PLATFORM_PENDING;
    enum a733_gpu_power_result gr=A733_GPU_POWER_AGAIN;
    unsigned tick;
    for(tick=0;tick<100;++tick) {
        if(nr==A733_NPU_PLATFORM_PENDING)nr=step();
        if(!gpu_token && s.stage==A733_NPU_STARTUP_ROW)begin_gpu();
        else if(gpu_token && gr==A733_GPU_POWER_AGAIN)gr=a733_gpu_power_poll(&gpu);
        if(nr!=A733_NPU_PLATFORM_PENDING && gr!=A733_GPU_POWER_AGAIN)break;
        service_step();m.now+=TICK_US;
    }
    assert(tick<100 && both_pending==1 && tokens_held());
    assert(nr==(mode==2 ? A733_NPU_PLATFORM_RETAINED : A733_NPU_PLATFORM_READY));
    assert(gr==(mode==1 ? A733_GPU_POWER_FAULT : A733_GPU_POWER_OK));
    assert(s.row.counted && gpu.row.counted && delegated && gpu_token);
    assert(m.rails.pins[s.rail.id&(A733_RAIL_CLIENTS-1)]==1);
    assert(m.rails.pins[supply.rail.id&(A733_RAIL_CLIENTS-1)]==1);
    const unsigned faults=a733_power_authority_load(&m.rails.authority);
    assert(!(faults&A733_POWER_PMIC_TRANSFER) && !d.owner.failed && !m.gpu.failed);
    assert(!!(faults&A733_POWER_DOMAIN4_FAULT)==(mode==2));
    assert(!!(faults&A733_POWER_DOMAIN5_FAULT)==(mode==1));
    shared_conflicts();return tick;
}
static void local_stop(void)
{
    const unsigned ppu=d.ppu_writes,wire=m.writes;const uint64_t token=s.pck.token;
    const uint32_t pll=r.ccu[0x2a0/4],gpu_gate=d.ccu_b20,gpu_reset=d.ccu_b24;
    bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
    assert(s.stage==A733_NPU_STARTUP_STOPPED && tokens_held() && s.pck.token==token);
    assert(s.row.counted && a733_power_domain_state(&s.row)==A733_POWER_ROW_ON);
    assert(d.ppu_writes==ppu && m.writes==wire && r.ccu[0x2a0/4]==pll);
    assert(d.ccu_b20==gpu_gate && d.ccu_b24==gpu_reset);
    assert(!(r.ccu[0xb00/4]&(1u<<31)) && (r.ccu[0xb04/4]&0xf0001)==(1u<<19));
    assert(a733_rail_release(&m.rails,&s.rail,m.now+10000)==A733_RAIL_BUSY);
}
static void gpu_stop(void)
{
    assert(a733_pmic_consumer_health(&supply)==1);
    assert(a733_gpu_power_off(&gpu,m.now+30000)==A733_GPU_POWER_AGAIN);
    unsigned i=0;enum a733_gpu_power_result result;
    while((result=a733_gpu_power_poll(&gpu))==A733_GPU_POWER_AGAIN) {
        assert(++i<30);service_step();m.now+=TICK_US;
    }
    assert(result==A733_GPU_POWER_OK && !delegated && !gpu.row.counted);
    assert(a733_gpu_resources_release(&m.gpu,9,gpu_token,m.now+10000)==A733_GPU_RESOURCE_OK);gpu_token=0;
    assert(tokens_held() && s.row.counted && a733_power_domain_state(&s.row)==A733_POWER_ROW_ON);
    /* Production keeps the GPU supply consumer's stable parent token. */
    assert(a733_pmic_consumer_health(&supply)==1);
    shared_conflicts();
}
static void shared_fault(void)
{
    struct a733_pmic_request q[2]={{0}};struct a733_pmic_snapshot out={0};
    assert(a733_pmic_submit(&m.provider,s.config.owner,&s.rail,A733_RAIL_DCDC2,m.now+10000,&q[0])==A733_PMIC_PENDING);
    assert(a733_pmic_submit(&m.provider,9,&supply.rail,A733_RAIL_DCDC4,m.now+10000,&q[1])==A733_PMIC_PENDING);
    assert(q[0].id && q[1].id);
    const unsigned ppu=d.ppu_writes,npu_writes=r.writes,stops=m.stops;
    m.fail_phase=2;m.fail_status=0x38; /* Arbitration loss in the first read. */
    unsigned i=0;while(!m.provider.retained){assert(++i<10);service_step();m.now+=TICK_US;}
    assert(m.wire==2 && m.stops==stops && m.provider.failed && m.retained && m.i2c_held);
    for(unsigned j=0;j<2;++j) {
        assert(a733_pmic_result(&m.provider,&q[j],&out)==A733_PMIC_RETAINED);
        assert(a733_pmic_reap(&m.provider,&q[j])==A733_PMIC_RETAINED);
    }
    assert(m.rails.pins[s.rail.id&(A733_RAIL_CLIENTS-1)]==2);
    assert(m.rails.pins[supply.rail.id&(A733_RAIL_CLIENTS-1)]==2);
    assert(a733_pmic_consumer_health(&supply)==-1);
    struct a733_pmic_request refused={0};
    assert(a733_pmic_submit(&m.provider,9,&supply.rail,A733_RAIL_DCDC4,m.now+10000,&refused)==A733_PMIC_RETAINED);
    assert(!refused.id && gpu_token && delegated && s.row.counted && gpu.row.counted);
    assert(d.ppu_writes==ppu && r.writes==npu_writes);
    /* No consumer hardware polls, local stop, or resource release after the
     * global fault. The board supervisor must retain the whole session. */
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned mode=(unsigned)strtoul(argv[1],0,10);assert(mode<4);
    composed_setup(mode);unsigned ticks=boot_pair(mode);
    const unsigned wire=m.writes;const uint32_t ahb=d.ccu_5c0,mbus=d.ccu_5e0,pck=d.rccu_1ac;
    if(mode==0){local_stop();gpu_stop();}
    if(mode==1)local_stop(); /* Local domain failure, not board-fatal policy. */
    if(mode==3)shared_fault();
    assert(d.ccu_5c0==ahb && d.ccu_5e0==mbus && d.rccu_1ac==pck);
    assert(m.pmic[0x13]==30 && m.pmic[0x15]==30);
    if(mode!=3)assert(m.writes==wire);
    printf("{\"case\":%u,\"startup_ticks\":%u,\"service_steps\":%u,\"ppu_reads\":%u,\"ppu_writes\":%u,"
        "\"pmic_bus_writes\":%u,\"simultaneous_pending_rows\":%u,\"pass\":true}\n",
        mode,ticks,d.steps,d.ppu_reads,d.ppu_writes,m.writes,both_pending);
    return 0;
}
