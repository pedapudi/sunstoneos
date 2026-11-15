/* SPDX-License-Identifier: Apache-2.0 */
/* Live NPU startup and GPU power composition over one actual PMIC provider,
 * rail ledger, clock registry, resource manager and power-domain owner.
 * Reuse the accepted register/clock/task model without rerunning its cases.
 * Cases 0-7 interleave the tasks deterministically on one thread. Cases 8
 * and 9, built with MODEL_THREADS, run the two retirements on real threads,
 * one per core: the power service (core 0), the NPU bootstrap (core 1) and
 * the GPU retirement (core 6); see the comment above threaded_case(). None
 * of the cases is a hardware test.
 * Local stop assumes the caller already established controller quiescence;
 * neither firmware drain nor OS thermal policy is modeled here. */
#define main npu_startup_existing_cases
#include "npu-startup-host.c"
#undef main
#include "a733_pmic_consumer.h"
#include "a733_cpu_clock.h"

static struct a733_gpu_power gpu;
static struct a733_pmic_consumer supply;
static struct a733_gpu_power_resources ready_gpu;
static uint64_t gpu_token, gpu_deadline;
static unsigned delegated, core_pending, both_pending;
static uint32_t core[0xc14/4]; /* policy..PWCR and the delay row, by word */

MODEL_UNSANITIZED static int shared_read(void *v,uintptr_t a,uint32_t *out)
{
    MODEL_GUARD();uint32_t *w=level_word(a);
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
MODEL_UNSANITIZED static int power_read(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t *out)
{
    (void)v;assert(gpu_token && delegated);MODEL_GUARD();
    if(bank==A733_GPU_CORE) {
        /* CORE6's PPU is always on; reads come before and after TOP5 ON. */
        assert(off==0 || off==8 || off==0x14 || off==0x20 || off==0xfb0);
        if(off==8 && core_pending){core[2]=core[0];core_pending=0;}
        *out=off==0xfb0 ? 0 : core[off/4];return 1;
    }
    uint32_t *w=level_word((bank==A733_GPU_CCU ? CCU_BASE : RCCU_BASE)+off);
    assert(w);
    /* PLL_GPU0 locks once enabled with its LDO and lock enable. */
    if(bank==A733_GPU_CCU && off==0xe0)*w=(*w&0xe0000000)==0xe0000000 ? *w|(1u<<28) : *w&~(1u<<28);
    *out=*w;return 1;
}
MODEL_UNSANITIZED static int power_write(enum a733_gpu_power_bank bank,uint32_t off,uint32_t clear,uint32_t set)
{
    MODEL_GUARD();
    if(bank==A733_GPU_CORE) {
        /* The row's answer is read first: it orders the service's write of
         * counted before this read. */
        assert(off==0x20 || (a733_power_domain_state(&gpu.row)==A733_POWER_ROW_ON && gpu.row.counted));
        assert(clear==UINT32_MAX && ((off==0 && (set==0 || set==8)) || (off==0x20 && !set)));
        core[off/4]=set;if(!off)core_pending=1;return 1;
    }
#ifdef MODEL_MUTANT_GPU_WRITES
    /* Only the mutants of drivers/gpu/a733_gpu_power.c that
     * checks/accelerator-shared-owner.py builds write these shared words. */
    if((bank==A733_GPU_R_CCU && off==0x1ac) || (bank==A733_GPU_CCU && off==0x5c0)) {
        uint32_t *w=level_word((bank==A733_GPU_CCU ? CCU_BASE : RCCU_BASE)+off);
        *w=(*w&~clear)|set;return 1;
    }
#endif
    assert(bank==A733_GPU_CCU && (off==0xb20 || off==0xb24 || off==0xe0));
    if(gpu.phase==A733_GPU_POWER_STOPPING)assert(!core[0] && !core[2]);
    uint32_t *w=level_word(CCU_BASE+off);*w=(*w&~clear)|set;
    if(off==0xb20)*w&=~(1u<<27);
    return 1;
}
/* Production makes these writes under the platform register lock; under
 * MODEL_THREADS the update takes it, with the GPU operation's deadline. */
static int power_update(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t clear,uint32_t set)
{
    (void)v;assert(gpu_token && delegated && !REGISTER_MINE);
#ifdef MODEL_THREADS
    if(!enter(&r,gpu_deadline))return 0;
    const int written=power_write(bank,off,clear,set);
    leave(&r);return written;
#else
    return power_write(bank,off,clear,set);
#endif
}
static void composed_setup(unsigned mode)
{
    setup();memset(&gpu,0,sizeof(gpu));memset(&supply,0,sizeof(supply));
    if(mode>=5) {
        /* CPU OPPs use the admitted 26 MHz source; REF remains 24 MHz. Set
         * the same physical words in each adapter before provider polling. */
        r.osc=0x10008002;r.ccu[0]=0xf8000000|(95u<<8)|(103u<<16);
        *pmic_model_word(&m,0x07090160)=r.osc;
        *pmic_model_word(&m,0x02002000)=r.ccu[0];
        *pmic_model_word(&m,0x07010010)=0x04000000;
        m.pmic[0x14]=m.pmic[0x16]=30;
    }
    m.pmic[0x15]=0x2e; /* DCDC4 at 960 mV, as the GPU platform raises it */
    memset(core,0,sizeof(core));core[8]=0x010101ff;
    gpu_token=0;delegated=core_pending=both_pending=0;gpu_deadline=100000;
    /* Only the callbacks change; all callers retain the same manager address. */
    m.gpu.ops.read32=shared_read;m.gpu.ops.rail=shared_rail;
    const struct a733_pmic_consumer_config c={&m.provider,m.lifetime,9,A733_RAIL_DCDC4};
    assert(a733_pmic_consumer_init(&supply,&c));
    supply.recipient=&gpu;supply.destination=&m.gpu.rail;
    /* The first sample reserves the ledger token and yields, as the native
     * platform does; the later samples read DCDC4 through the provider,
     * which completes before the NPU starts so that both rows are later
     * pending together. */
    assert(a733_pmic_consumer_sample(&supply,gpu_deadline)==0);
    const struct a733_gpu_power_ops ops={gpu_now,delegate,undelegate,power_read,power_update};
    assert(a733_gpu_power_init(&gpu,&ops,&r));
    assert(a733_power_domain_row_init(&gpu.row,&d.owner,5,9,&supply.rail));
    int sampled;unsigned n=0;
    while((sampled=a733_pmic_consumer_sample(&supply,gpu_deadline))==0){assert(++n<40);service_step();m.now+=TICK_US;}
    assert(sampled==1);
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
    assert(!supply.armed && supply.request.id);
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
        /* Both consumers start after the whole supply verification, so that
         * their domain rows are pending together; the NPU startup alone could
         * proceed on DCDC2 verified while the owner still reads DCDC4. */
        const unsigned done=a733_power_authority_load(&m.rails.authority)&A733_POWER_SUPPLY_DONE;
        if(nr==A733_NPU_PLATFORM_PENDING && (done || s.stage<A733_NPU_STARTUP_SUPPLY))nr=step();
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
    /* The row's pin and the consumer's completed sample. */
    assert(m.rails.pins[supply.rail.id&(A733_RAIL_CLIENTS-1)]==2);
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
    assert(m.rails.pins[supply.rail.id&(A733_RAIL_CLIENTS-1)]==3);
    assert(a733_pmic_consumer_health(&supply)==-1);
    struct a733_pmic_request refused={0};
    assert(a733_pmic_submit(&m.provider,9,&supply.rail,A733_RAIL_DCDC4,m.now+10000,&refused)==A733_PMIC_RETAINED);
    assert(!refused.id && gpu_token && delegated && s.row.counted && gpu.row.counted);
    assert(d.ppu_writes==ppu && r.writes==npu_writes);
    /* No consumer hardware polls, local stop, or resource release after the
     * global fault. The board supervisor must retain the whole session. */
}
/* Case 4: the NPU startup alone, ungated, while the owner still reads DCDC4,
 * with the PMIC bus at 100 kHz.
 * The owner publishes DCDC2 verified first, admits the domain-4 row on it
 * before the done bit and DCDC4 verified, and the NPU reaches READY; the
 * owner then completes and publishes DCDC4. */
static int dcdc2_first(void)
{
    /* At 100 kHz the DCDC4 snapshot spans several service steps. */
    setup_bus_100khz=1;composed_setup(0);
    /* The startup model's event-driven boot: the bootstrap runs when the
     * service notifies it. The service step that answers the row ON ends
     * before the one that sets the done bit. */
    struct run b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_READY && s.row.counted && tokens_held());
    assert(b.row_on_us && (!b.supply_done_us || b.row_on_us<b.supply_done_us));
    unsigned w=a733_power_authority_load(&m.rails.authority);
    const unsigned done_at_ready=!!(w&A733_POWER_SUPPLY_DONE);
    for(unsigned i=0;i<200 && !(w&A733_POWER_SUPPLY_DONE);++i) {
        if(d.wake_at>m.now)m.now=d.wake_at;
        service_step();w=a733_power_authority_load(&m.rails.authority);
    }
    assert((w&A733_POWER_SUPPLY_DONE) && (w&A733_POWER_DCDC4_VERIFIED) && (w&A733_POWER_DCDC2_VERIFIED));
    printf("{\"case\":4,\"bus\":\"100kHz\",\"row_on_us\":%llu,\"ready_us\":%llu,\"done_before_ready\":%u,\"pass\":true}\n",
        (unsigned long long)b.row_on_us,(unsigned long long)b.elapsed_us,done_at_ready);
    return 0;
}
/* CPU/GPU coexistence adds no scheduler or device abstraction. The actual
 * CPU controller shares the existing provider model; only its PLL register
 * responses are modeled. Existing CPU checks establish ramp/settle timing. */
static struct a733_cpu_clock cpu_owner;
static struct a733_thermal_policy cpu_policy;
static uint32_t cpu_words[3][8];
static unsigned cpu_stuck,cpu_writes,cpu_gpu_pending;
static int cpu_notify(void *v,enum a733_thermal_level level){(void)v;(void)level;return 1;}
static int cpu_time(void *v,uint64_t *out)
{assert(v==&cpu_owner);++m.now;return pmic_model_now(&m,out);}
static uint32_t *cpu_word(uintptr_t a)
{
    assert(a>=0x08871000 && a<0x08874000 && ((a&0xfff)<=0x1c) && !(a&3));
    return &cpu_words[(a-0x08871000)/0x1000][(a&0xfff)/4];
}
static int cpu_read(void *v,uintptr_t a,uint32_t *out)
{
    assert(v==&cpu_owner && a733_cpu_clock_address(a,0));
    *out=a==0x07090160 ? r.osc : a==0x0300624c ? r.sid : *cpu_word(a);
    return 1;
}
static int cpu_write(void *v,uintptr_t a,uint32_t value)
{
    assert(v==&cpu_owner && a733_cpu_clock_address(a,1));++cpu_writes;
    if(!(a&0xfff))value=(value&~((1u<<26)|(1u<<28)))|(cpu_stuck?0:1u<<28);
    *cpu_word(a)=value;return 1;
}
static void cpu_setup(void)
{
    memset(&cpu_owner,0,sizeof(cpu_owner));memset(&cpu_policy,0,sizeof(cpu_policy));
    memset(cpu_words,0,sizeof(cpu_words));cpu_stuck=cpu_writes=cpu_gpu_pending=0;
    cpu_words[0][0]=0xf8802600;cpu_words[0][7]=0x03010000;
    cpu_words[1][0]=0xf8002700;cpu_words[1][7]=0x03000000;
    cpu_words[2][0]=0xf8001e00;cpu_words[2][7]=0x03000000;
    assert(a733_thermal_policy_init(&cpu_policy,m.now,cpu_notify,0));
    struct a733_thermal_sample sample={.observed_us=m.now,.sequence=1,.valid=31};
    for(unsigned i=0;i<5;++i)sample.millicelsius[i]=50000;
    assert(a733_thermal_policy_update(&cpu_policy,m.now,A733_THERMAL_SAMPLE,&sample)==A733_THERMAL_NORMAL);
    const struct a733_cpu_clock_config config={4242,{77,3,{1794,2002},8,0}};
    const struct a733_cpu_clock_ops ops={cpu_time,cpu_read,cpu_write};
    assert(a733_cpu_clock_init(&cpu_owner,&config,&ops,&cpu_owner,&m.clocks,&m.rails,&m.provider,&cpu_policy));
}
static void cpu_gpu_on(void)
{
    const uint64_t deadline=m.now+1000000;
    /* Native reopen retains the sampled supply and checks its health; it
     * does not begin a new sample under a different operation deadline. */
    assert(a733_pmic_consumer_health(&supply)==1);
    assert(a733_pmic_consumer_arm(&supply,deadline)==1);
    assert(a733_gpu_resources_acquire(&m.gpu,9,A733_GPU_RESOURCE_GPU,deadline,
        &gpu_token,&ready_gpu)==A733_GPU_RESOURCE_OK);
    assert(a733_gpu_power_on(&gpu,deadline)==A733_GPU_POWER_AGAIN);
}
static void cpu_gpu_run(unsigned fault)
{
    enum a733_cpu_clock_status cr=A733_CPU_CLOCK_PENDING;
    enum a733_gpu_power_result gr=A733_GPU_POWER_AGAIN;
    for(unsigned i=0;i<300;++i) {
        if(cr==A733_CPU_CLOCK_PENDING)cr=a733_cpu_clock_burst(&cpu_owner,200);
        if(cpu_owner.request.id && supply.request.id)++cpu_gpu_pending;
        if(fault==6 && cpu_owner.request.id && !m.fail_phase) {
            m.fail_phase=2;m.fail_status=0x38;
        }
        if(cr==A733_CPU_CLOCK_RETAINED) {
            assert(fault && cpu_owner.clock.id && cpu_owner.rail.id && gpu_token && delegated && supply.rail.id);
            /* The OS closes board admission here. No GPU stop or further
             * device poll can establish completion of retained work. */
            return;
        }
        assert(cr==A733_CPU_CLOCK_PENDING || cr==A733_CPU_CLOCK_COMPLETE);
        if(gr==A733_GPU_POWER_AGAIN)gr=a733_gpu_power_poll(&gpu);
        if(gr==A733_GPU_POWER_FAULT) {
            /* Either task can observe a shared-provider fault first. Close
             * admission on that observation, retaining both owners. */
            assert(fault && m.provider.retained && cpu_owner.clock.id &&
                cpu_owner.rail.id && gpu_token && delegated && supply.rail.id);
            return;
        }
        assert(gr==A733_GPU_POWER_AGAIN || gr==A733_GPU_POWER_OK);
        if(cr==A733_CPU_CLOCK_COMPLETE && gr==A733_GPU_POWER_OK){assert(!fault);return;}
        service_step();m.now+=TICK_US;
    }
    assert(0);
}
static int cpu_gpu_case(unsigned mode)
{
    composed_setup(mode);cpu_setup();cpu_gpu_on();
    assert(a733_cpu_clock_request(&cpu_owner,A733_CPU_BIG,2002,m.now+120000)==A733_CPU_CLOCK_PENDING);
    if(mode==7)cpu_stuck=1;
    cpu_gpu_run(mode==5 ? 0 : mode);
    if(mode==5) {
        assert(cpu_owner.applied_mhz[A733_CPU_BIG]==2002 && m.pmic[0x14]==55);
        /* Quiescence is an external precondition, as in the earlier power
         * cases. This is not a firmware-drain or thermal-lifecycle test. */
        assert(a733_gpu_power_off(&gpu,m.now+1000000)==A733_GPU_POWER_AGAIN);
        assert(a733_cpu_clock_request(&cpu_owner,A733_CPU_LITTLE,1794,m.now+120000)==A733_CPU_CLOCK_PENDING);
        cpu_gpu_run(0);
        assert(cpu_owner.applied_mhz[A733_CPU_LITTLE]==1794 && m.pmic[0x16]==50);
        assert(!delegated && !gpu.row.counted && supply.rail.id);
        assert(a733_gpu_resources_release(&m.gpu,9,gpu_token,m.now+10000)==A733_GPU_RESOURCE_OK);gpu_token=0;
        cpu_gpu_on();
        assert(a733_cpu_clock_request(&cpu_owner,A733_CPU_DSU,1274,m.now+120000)==A733_CPU_CLOCK_PENDING);
        cpu_gpu_run(0);
        assert(cpu_owner.applied_mhz[A733_CPU_DSU]==1274 && delegated && gpu.row.counted);
        assert(!cpu_owner.clock.id && !cpu_owner.rail.id && cpu_gpu_pending);
    } else if(mode==6) {
        struct a733_pmic_snapshot snapshot;
        assert(m.provider.retained && m.i2c_held && m.retained);
        assert(cpu_owner.request.id && supply.request.id && !cpu_writes);
        assert(a733_pmic_result(&m.provider,&cpu_owner.request,&snapshot)==A733_PMIC_RETAINED);
        assert(a733_pmic_result(&m.provider,&supply.request,&snapshot)==A733_PMIC_RETAINED);
    } else assert(cpu_writes && !m.provider.retained);
    assert(m.pmic[0x15]==0x2e && supply.rail.id && gpu_token);
    printf("{\"case\":%u,\"pass\":true,\"cpu_register_writes\":%u,\"pmic_bus_writes\":%u,"
        "\"cpu_requests_with_gpu_snapshot_held\":%u,\"retained\":%s}\n",
        mode,cpu_writes,m.writes,cpu_gpu_pending,mode==5?"false":"true");
    return 0;
}
#ifdef MODEL_THREADS
#include <time.h>
#include <string.h>
#include <unistd.h>
/* Cases 8 and 9 run the GPU retirement and the NPU retirement on real
 * threads after the deterministic boot of case 0, one thread for each core
 * of the image's placement. The host scheduler places the threads; the
 * result counts the runs in which the three threads ran on three distinct
 * host CPUs at their recorded events.
 * - The power service (core 0) runs service_step() when notified or when its
 *   ticks elapse, as the blocked task wakes; otherwise it advances model time
 *   by SERVICE_STEP_US and yields. It advances only after each running NPU
 *   and GPU thread has passed one of its loops since the previous advance,
 *   so that model deadlines count the threads' own progress: a thread that
 *   the host does not schedule stops model time instead of expiring a
 *   deadline. A host watchdog fails the run when no thread progresses for
 *   HOST_WATCHDOG_US of host time.
 * - The NPU bootstrap (core 1) has a job in flight from its start. Until the
 *   job completes it checks the job's invariant continuously
 *   (npu_invariant()); it then calls a733_npu_startup_stop, retrying BUSY.
 * - The GPU retirement (core 6) starts once the job is in flight:
 *   a733_gpu_power_off, polls until OK, then a733_gpu_resources_release of
 *   client 9, retrying BUSY as production does.
 * Case 8 retires the GPU while the job is in flight: the job completes a
 * random delay after the GPU release. Case 9 stops the NPU while the GPU
 * retirement runs: the job completes a random delay after the GPU
 * retirement starts. The delays are model microseconds drawn from a recorded
 * seed. Each run starts from a fresh setup and boot; after both retirements
 * the main thread checks the end state that case 0 checks. One atomic
 * counter orders the events of each run. A run overlaps when, in case 8, the
 * whole GPU retirement lies inside the NPU job and, in case 9, the local
 * stop call intersects the GPU retirement. The result also counts the
 * invariant samples taken inside the GPU retirement and, in case 9, the runs
 * whose local stop call lies inside the GPU retirement; the minimum applies
 * to the sampled runs in case 8 and to those runs in case 9. A failed condition
 * prints one JSON line with its "violation" text and aborts. */
#define RUN_LIMIT_US 2000000u /* model-time bound of each run's threads */
#define SERVICE_STEP_US 1u
#define HOST_WATCHDOG_US 60000000u
static atomic_ulong events;
static atomic_int job_active,gpu_active,gpu_done,threads_finished;
static unsigned threaded_mode,run_index;
static int hold_npu; /* test: the NPU thread waits, beating, until the GPU retirement is done (argv[5] "hold-npu") */
static uint64_t run_limit,gpu_delay_us,npu_delay_us;
static uint32_t job_pll;
static struct {
    unsigned long job_begin,job_end,stop_begin,stop_end,gpu_begin,gpu_end;
    uint64_t gpu_begin_us,gpu_end_us;
    int service_cpu,npu_cpu,gpu_cpu; /* host CPUs at the start, the stop call and the GPU OFF call */
    unsigned samples,samples_in_gpu,status_busy,release_busy,stop_busy,polls,service_steps;
} rec;
static unsigned long event(void){return atomic_fetch_add(&events,1)+1;}
MODEL_UNSANITIZED static uint64_t model_now(void){MODEL_GUARD();return m.now;}
static _Noreturn void violation(const char *what,unsigned long value)
{
    fprintf(stderr,"{\"case\":%u,\"run\":%u,\"violation\":\"%s\",\"value\":\"0x%lx\",\"model_us\":%llu,"
        "\"run_limit_us\":%llu,\"gpu_delay_us\":%llu,\"npu_delay_us\":%llu}\n",
        threaded_mode,run_index,what,value,(unsigned long long)model_now(),(unsigned long long)run_limit,
        (unsigned long long)gpu_delay_us,(unsigned long long)npu_delay_us);
    fflush(stderr);abort();
}
/* A host that schedules none of the threads is not a result of the model: the run stops with the inconclusive status
 * (checks/harness.py, INCONCLUSIVE_EXIT), which the check runs once more and then reports as inconclusive. */
static _Noreturn void host_stalled(const char *what)
{
    fprintf(stderr,"INCONCLUSIVE: case %u run %u: %s\n",threaded_mode,run_index,what);
    fflush(stderr);_exit(75);
}
/* Loop passes of the NPU (0) and GPU (1) threads, and which of them run. */
static atomic_uint beats[2];
static atomic_int alive[2];
static _Thread_local int beat_slot=-1;
static void beat(void){if(beat_slot>=0)atomic_fetch_add_explicit(&beats[beat_slot],1,memory_order_relaxed);}
static void bounded(const char *what){beat();if(model_now()>=run_limit)violation(what,0);}
/* The NPU job's invariant: the PCK clock gate, the NPU AHB and MBUS master
 * gates, PLL_NPU unchanged and locked, the domain-4 row ON and the NPU's PCK
 * lease live. LOCK is read through the model's read path, which reports it. */
static void npu_invariant(void)
{
    const int before=atomic_load(&gpu_active);
    uint32_t pck,ahb,mbus,pll,ppu4,observed;
    {MODEL_GUARD();pck=d.rccu_1ac;ahb=d.ccu_5c0;mbus=d.ccu_5e0;pll=r.ccu[0x2a0/4];ppu4=d.ppu[4][2];}
    assert(reg_read(&r,CCU+0x2a0,&observed));
    if(!(pck&1))violation("PCK clock gate R_CCU+0x1ac bit 0 clear while the NPU job is in flight",pck);
    if(!(ahb&(1u<<6)))violation("NPU AHB gate CCU+0x5c0 bit 6 clear while the NPU job is in flight",ahb);
    if(!(mbus&(1u<<18)))violation("NPU MBUS gate CCU+0x5e0 bit 18 clear while the NPU job is in flight",mbus);
    if(pll!=job_pll)violation("PLL_NPU control word CCU+0x2a0 changed while the NPU job is in flight",pll);
    if(!(observed&(1u<<28)))violation("PLL_NPU LOCK CCU+0x2a0 bit 28 clear while the NPU job is in flight",observed);
    if(a733_power_domain_state(&s.row)!=A733_POWER_ROW_ON || (ppu4&15)!=8)
        violation("domain-4 row not ON while the NPU job is in flight",ppu4);
    enum a733_gpu_resource_result lease;
    while((lease=a733_gpu_resources_status(&m.gpu,s.config.pck_client,&s.pck))==A733_GPU_RESOURCE_BUSY) {
        ++rec.status_busy;bounded("deadline: the NPU PCK lease status stayed BUSY");sched_yield();
    }
    if(lease!=A733_GPU_RESOURCE_OK)violation("NPU PCK lease not live while the NPU job is in flight",lease);
    ++rec.samples;
    if(before && atomic_load(&gpu_active))++rec.samples_in_gpu;
    beat();
}
static uint64_t host_us(void)
{struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;}
static void *service_thread(void *unused)
{
    (void)unused;rec.service_cpu=sched_getcpu();
    unsigned seen[2]={0,0};uint64_t progressed=host_us();
    while(!atomic_load(&threads_finished)) {
        int due;
        {MODEL_GUARD();due=d.notified || m.now>=d.wake_at;}
        if(due){service_step();++rec.service_steps;continue;}
        int advance=1;unsigned now_beats[2];
        for(unsigned i=0;i<2;++i) {
            now_beats[i]=atomic_load_explicit(&beats[i],memory_order_relaxed);
            if(atomic_load(&alive[i]) && now_beats[i]==seen[i])advance=0;
        }
        if(advance) {
            seen[0]=now_beats[0];seen[1]=now_beats[1];progressed=host_us();
            MODEL_GUARD();m.now+=SERVICE_STEP_US;
        } else if(host_us()-progressed>HOST_WATCHDOG_US)
            host_stalled("no thread progressed within the host watchdog");
        sched_yield();
    }
    return 0;
}
static void *npu_thread(void *unused)
{
    (void)unused;bootstrap_running=1;beat_slot=0;
    rec.job_begin=event();atomic_store(&job_active,1);
    if(hold_npu)while(!atomic_load(&gpu_done)){beat();sched_yield();}
    if(threaded_mode==8)
        while(!atomic_load(&gpu_done)){npu_invariant();bounded("deadline: the GPU retirement did not complete during the NPU job");}
    else
#ifdef SS_OLD_CLAIM_DEADLINE
        while(!atomic_load(&gpu_active)){npu_invariant();bounded("deadline: the GPU retirement did not start during the NPU job");}
#else
        /* The retirement has started once it is active or done: a host that schedules the GPU thread long enough to run the whole
         * retirement between two looks of this thread leaves gpu_active clear again. The claim is stated in the model's progress. */
        while(!atomic_load(&gpu_active) && !atomic_load(&gpu_done)){npu_invariant();bounded("deadline: the GPU retirement did not start during the NPU job");}
#endif
    const uint64_t end=model_now()+npu_delay_us;
    do npu_invariant(); while(model_now()<end);
    rec.job_end=event();
    rec.npu_cpu=sched_getcpu();rec.stop_begin=event();
    enum a733_npu_platform_result x;
    while((x=a733_npu_startup_stop(&s))==A733_NPU_PLATFORM_BUSY) {
        ++rec.stop_busy;bounded("deadline: the NPU local stop stayed BUSY");sched_yield();
    }
    rec.stop_end=event();
    if(x!=A733_NPU_PLATFORM_READY)violation("the NPU local stop did not return READY",x);
    atomic_store(&alive[0],0);bootstrap_running=0;return 0;
}
static void *gpu_thread(void *unused)
{
    (void)unused;beat_slot=1;
    while(!atomic_load(&job_active)){bounded("deadline: the NPU job did not start");sched_yield();}
    const uint64_t start=model_now()+gpu_delay_us;
    while(model_now()<start){bounded("deadline: the GPU delay did not end");sched_yield();}
    rec.gpu_cpu=sched_getcpu();rec.gpu_begin=event();rec.gpu_begin_us=model_now();atomic_store(&gpu_active,1);
    if(a733_pmic_consumer_health(&supply)!=1)violation("the GPU supply consumer is not healthy at retirement",0);
    gpu_deadline=run_limit;
    enum a733_gpu_power_result g=a733_gpu_power_off(&gpu,gpu_deadline);
    if(g!=A733_GPU_POWER_AGAIN)violation("a733_gpu_power_off did not start the retirement",g);
    while((g=a733_gpu_power_poll(&gpu))==A733_GPU_POWER_AGAIN) {
        ++rec.polls;bounded("deadline: the GPU retirement did not complete");sched_yield();
    }
    if(g!=A733_GPU_POWER_OK)violation("the GPU retirement failed",gpu.error);
    if(delegated)violation("the GPU controller kept its delegation after OFF",0);
    enum a733_gpu_resource_result released;
    while((released=a733_gpu_resources_release(&m.gpu,9,gpu_token,run_limit))==A733_GPU_RESOURCE_BUSY) {
        ++rec.release_busy;bounded("deadline: the GPU release stayed BUSY");sched_yield();
    }
    if(released!=A733_GPU_RESOURCE_OK)violation("a733_gpu_resources_release of client 9 failed",released);
    rec.gpu_end_us=model_now();rec.gpu_end=event();
    atomic_store(&gpu_active,0);atomic_store(&gpu_done,1);atomic_store(&alive[1],0);
    return 0;
}
static uint64_t random_state;
static uint64_t next_random(void)
{random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;return random_state;}
static void end_state(unsigned wire,uint32_t ahb,uint32_t mbus,uint32_t pck,uint64_t pck_token)
{
    if(d.ccu_5c0!=ahb || d.ccu_5e0!=mbus || d.rccu_1ac!=pck)
        violation("a shared gate word changed across the retirements",(unsigned long)((d.ccu_5c0^ahb)|(d.ccu_5e0^mbus)|(d.rccu_1ac^pck)));
    if(m.pmic[0x13]!=30 || m.pmic[0x15]!=0x2e)violation("a PMIC selector changed across the retirements",m.pmic[0x13]|(m.pmic[0x15]<<8));
    if(m.writes!=wire)violation("PMIC bus writes during the retirements",m.writes-wire);
    if(!tokens_held() || s.pck.token!=pck_token)violation("the NPU tokens are not held after both retirements",0);
    if(s.stage!=A733_NPU_STARTUP_STOPPED || !s.row.counted || a733_power_domain_state(&s.row)!=A733_POWER_ROW_ON)
        violation("the NPU domain-4 row is not ON and counted after its local stop",s.stage);
    if(r.ccu[0x2a0/4]!=job_pll)violation("PLL_NPU changed across the retirements",r.ccu[0x2a0/4]);
    if((r.ccu[0xb00/4]&(1u<<31)) || (r.ccu[0xb04/4]&0xf0001)!=(1u<<19))
        violation("the NPU local stop left its gates or resets open",r.ccu[0xb04/4]);
    if(a733_rail_release(&m.rails,&s.rail,m.now+10000)!=A733_RAIL_BUSY)violation("the NPU rail is not pinned by its row",0);
    if(delegated || gpu.row.counted || gpu.phase!=A733_GPU_POWER_OFF)violation("the GPU row is still counted after its retirement",gpu.phase);
    if(m.gpu.leases[9].mode!=A733_GPU_RESOURCE_FREE)violation("the GPU lease of client 9 is not released",m.gpu.leases[9].mode);
    gpu_token=0;
    if(a733_pmic_consumer_health(&supply)!=1)violation("the GPU supply consumer lost its parent token",0);
    const unsigned faults=a733_power_authority_load(&m.rails.authority);
    if((faults&(A733_POWER_PMIC_TRANSFER|A733_POWER_DOMAIN4_FAULT|A733_POWER_DOMAIN5_FAULT)) || d.owner.failed || m.gpu.failed)
        violation("a provider fault after the retirements",faults);
    shared_conflicts();
}
static int threaded_case(unsigned mode,unsigned runs,uint64_t seed,unsigned minimum)
{
    threaded_mode=mode;random_state=seed ? seed : 1;
    const uint64_t gpu_span=mode==8 ? 200 : 50,npu_span=mode==8 ? 200 : 10;
    unsigned overlapped=0,sampled=0,stop_inside=0,distinct=0,samples=0,in_gpu=0,status_busy=0,release_busy=0,stop_busy=0;
    uint64_t gpu_min_us=UINT64_MAX,gpu_max_us=0;
    register_waits=0;
    for(run_index=0;run_index<runs;++run_index) {
        gpu_delay_us=next_random()%gpu_span;npu_delay_us=next_random()%npu_span;
        composed_setup(0);(void)boot_pair(0);
        const unsigned wire=m.writes;const uint32_t ahb=d.ccu_5c0,mbus=d.ccu_5e0,pck=d.rccu_1ac;
        const uint64_t pck_token=s.pck.token;
        job_pll=r.ccu[0x2a0/4];memset(&rec,0,sizeof(rec));atomic_store(&events,0);
        atomic_store(&job_active,0);atomic_store(&gpu_active,0);atomic_store(&gpu_done,0);atomic_store(&threads_finished,0);
        atomic_store(&beats[0],0);atomic_store(&beats[1],0);atomic_store(&alive[0],1);atomic_store(&alive[1],1);
        run_limit=m.now+RUN_LIMIT_US;
        pthread_t service,npu,gpu_task;
        assert(!pthread_create(&service,0,service_thread,0));
        assert(!pthread_create(&npu,0,npu_thread,0));
        assert(!pthread_create(&gpu_task,0,gpu_thread,0));
        assert(!pthread_join(npu,0) && !pthread_join(gpu_task,0));
        atomic_store(&threads_finished,1);assert(!pthread_join(service,0));
        end_state(wire,ahb,mbus,pck,pck_token);
        if(mode==8 ? rec.job_begin<rec.gpu_begin && rec.gpu_end<rec.job_end :
                     rec.stop_begin<rec.gpu_end && rec.gpu_begin<rec.stop_end)++overlapped;
        if(rec.gpu_begin<rec.stop_begin && rec.stop_end<rec.gpu_end)++stop_inside;
        if(rec.samples_in_gpu)++sampled;
        if(rec.service_cpu!=rec.npu_cpu && rec.service_cpu!=rec.gpu_cpu && rec.npu_cpu!=rec.gpu_cpu)++distinct;
        samples+=rec.samples;in_gpu+=rec.samples_in_gpu;
        status_busy+=rec.status_busy;release_busy+=rec.release_busy;stop_busy+=rec.stop_busy;
        const uint64_t span=rec.gpu_end_us-rec.gpu_begin_us;
        if(span<gpu_min_us)gpu_min_us=span;
        if(span>gpu_max_us)gpu_max_us=span;
    }
    printf("{\"case\":%u,\"ordering\":\"%s\",\"runs_on_distinct_cpus\":%u,"
        "\"runs\":%u,\"seed\":%llu,\"gpu_delay_us\":[0,%llu],\"npu_delay_us\":[0,%llu],"
        "\"overlap\":\"%s\",\"overlapping_runs\":%u,\"minimum\":%u,\"stop_inside_gpu_runs\":%u,"
        "\"runs_sampled_inside_gpu\":%u,\"invariant_samples\":%u,\"samples_inside_gpu\":%u,"
        "\"status_busy\":%u,\"release_busy\":%u,\"stop_busy\":%u,\"register_lock_waits\":%u,"
        "\"gpu_retirement_us\":[%llu,%llu],\"pass\":%s}\n",
        mode,mode==8 ? "gpu-retired-during-npu-job" : "npu-stopped-during-gpu-retirement",
        distinct,runs,(unsigned long long)seed,
        (unsigned long long)gpu_span-1,(unsigned long long)npu_span-1,
        mode==8 ? "GPU retirement inside the NPU job" : "NPU local stop intersects the GPU retirement",
        overlapped,minimum,stop_inside,sampled,samples,in_gpu,status_busy,release_busy,stop_busy,register_waits,
        (unsigned long long)gpu_min_us,(unsigned long long)gpu_max_us,
        (mode==8 ? overlapped==runs && sampled>=minimum : stop_inside>=minimum) ? "true" : "false");
    if(mode==8 && overlapped!=runs)violation("a run without the GPU retirement inside the NPU job",overlapped);
    if(mode==8 && sampled<minimum)violation("too few runs with an invariant sample inside the GPU retirement",sampled);
    if(mode==9 && stop_inside<minimum)violation("too few runs with the NPU local stop inside the GPU retirement",stop_inside);
    return 0;
}
#endif
int main(int argc,char **argv)
{
#ifdef MODEL_THREADS
    /* mode runs seed minimum */
    if(argc==5 || argc==6) {
        const unsigned mode=(unsigned)strtoul(argv[1],0,10);assert(mode==8 || mode==9);
        hold_npu=argc>5 && !strcmp(argv[5],"hold-npu");
        return threaded_case(mode,(unsigned)strtoul(argv[2],0,10),strtoull(argv[3],0,10),(unsigned)strtoul(argv[4],0,10));
    }
#endif
    assert(argc==2);unsigned mode=(unsigned)strtoul(argv[1],0,10);assert(mode<8);
    if(mode>=5)return cpu_gpu_case(mode);
    if(mode==4)return dcdc2_first();
    composed_setup(mode);unsigned ticks=boot_pair(mode);
    const unsigned wire=m.writes;const uint32_t ahb=d.ccu_5c0,mbus=d.ccu_5e0,pck=d.rccu_1ac;
    if(mode==0){local_stop();gpu_stop();}
    if(mode==1)local_stop(); /* Local domain failure, not board-fatal policy. */
    if(mode==3)shared_fault();
    assert(d.ccu_5c0==ahb && d.ccu_5e0==mbus && d.rccu_1ac==pck);
    assert(m.pmic[0x13]==30 && m.pmic[0x15]==0x2e);
    if(mode!=3)assert(m.writes==wire);
    printf("{\"case\":%u,\"startup_ticks\":%u,\"service_steps\":%u,\"ppu_reads\":%u,\"ppu_writes\":%u,"
        "\"pmic_bus_writes\":%u,\"simultaneous_pending_rows\":%u,\"pass\":true}\n",
        mode,ticks,d.steps,d.ppu_reads,d.ppu_writes,m.writes,both_pending);
    return 0;
}
