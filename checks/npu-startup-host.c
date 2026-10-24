/* SPDX-License-Identifier: Apache-2.0 */
/* The NPU startup sequencer over the power-domain owner, on the host.
 *
 * The model runs the actual sequencer (drivers/npu/a733_npu_startup.c) and its
 * tables with the actual PMIC provider, rail ledger, clock registry, GPU/PCK
 * resource manager and power-domain owner of checks/power-domains-model.h, at
 * the 400 kHz S_TWI0 timing of checks/power-domains-service.c. The NPU's CCU
 * words are a register model; PLL_NPU reports LOCK lock_us after its enable
 * table; domain 4 is the owner model's PPU, which acknowledges a policy write
 * 10 us later. Time is the PMIC model clock.
 *
 * Scheduling follows the images: the bootstrap task (priority 4) calls the
 * step once per tick until READY; the power service (priority 3) runs after
 * it within a tick when notified or when its own ticks elapse. Both start at
 * the same tick, the service's first step.
 *
 * Cases: boot to READY with the counts of the design's budget table; each
 * refusal before the first NPU write (supply unverified, supply never done,
 * register pass mismatch) releases the tokens and writes nothing; LOCK never
 * seen, a stalled PPU and a readback mismatch retain the tokens; local stop.
 * Output: one JSON line, then PASS. */
#include "power-domains-model.h"
/* The model boots the service itself; it does not use domains_boot(). */
static void unused_boot(void){(void)domains_boot;}
#include "a733_npu_startup.h"

#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07064000)
static struct a733_npu_startup s;
static struct a733_counter_clock counter;
static struct a733_platform_locks locks;
static int bootstrap_running,scheduler_running;
BaseType_t xTaskGetSchedulerState(void){return scheduler_running ? taskSCHEDULER_RUNNING : taskSCHEDULER_NOT_STARTED;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return bootstrap_running ? (TaskHandle_t)&s : (TaskHandle_t)&d;}
/* The sequencer uses its own callbacks here; the native ones must not run. */
int a733_npu_platform_native_read(void *c,uintptr_t a,uint32_t *v){(void)c;(void)a;(void)v;assert(0);return 0;}
int a733_npu_platform_native_write(void *c,uintptr_t a,uint32_t v){(void)c;(void)a;(void)v;assert(0);return 0;}
int a733_counter_clock_now(void *c,uint64_t *o){(void)c;(void)o;assert(0);return 0;}
int a733_platform_lock_enter(struct a733_platform_locks *l,enum a733_platform_lock k,uint64_t t){(void)l;(void)k;(void)t;assert(0);return 0;}
void a733_platform_lock_leave(struct a733_platform_locks *l,enum a733_platform_lock k){(void)l;(void)k;assert(0);}
int a733_platform_lock_retain(struct a733_platform_locks *l,enum a733_platform_lock k){(void)l;(void)k;assert(0);return 0;}

static struct {
    uint32_t ccu[0x2000/4];
    uint64_t lock_us,locked_at;
    uint32_t osc;
    unsigned held,retained,enters,writes,never_lock,flip_b04,pmic_reads_before;
} r;
static uint32_t *word(uintptr_t a)
{assert(a>=CCU && a<CCU+0x2000 && !(a&3));return &r.ccu[(a-CCU)/4];}
static int reg_read(void *v,uintptr_t a,uint32_t *out)
{
    assert(v==&r && bootstrap_running);
    if(a==0x07090160){*out=r.osc;return 1;}
    if(a==PCK || a==PCK+8)return owner_read(&d,a,out);
    *out=*word(a);
    if(a==CCU+0x2a0) {
        if((*out&0xa0000000)==0xa0000000 && !r.never_lock) {
            if(!r.locked_at)r.locked_at=m.now+r.lock_us;
            if(m.now>=r.locked_at)*out|=1u<<28;
        }
    } else if(a==CCU+0x19a0) {
        *out=(r.ccu[0x1920/4]&0x10001)==0x10001 ? 1u<<16 : 0;
    } else if(a==CCU+0xb04 && r.flip_b04 && r.writes>=r.flip_b04) {
        *out^=1u<<18;r.flip_b04=0; /* A completed write that reads back wrong. */
    }
    return 1;
}
static int reg_write(void *v,uintptr_t a,uint32_t value)
{
    assert(v==&r && bootstrap_running && r.held);
    assert(a==CCU+0xb00 || a==CCU+0xb04 || a==CCU+0x1920 || a==CCU+0x2a0);
    assert(a!=CCU+0x2a0 || !(value&(1u<<28)));
    ++r.writes;*word(a)=value;return 1;
}
static int now_us(void *v,uint64_t *out){assert(v==&r);return pmic_model_now(&m,out);}
/* Raw counter for the LOCK spacing: one modeled microsecond per read. */
static uint64_t spin_us(void *v){assert(v==&r);return ++m.now;}
static int enter(void *v,uint64_t deadline)
{assert(v==&r && !r.held && m.now<deadline);++r.enters;r.held=1;return 1;}
static void leave(void *v){assert(v==&r && r.held);r.held=0;}
static void retain(void *v){assert(v==&r && r.held);r.retained=1;}

/* The image's GPU/PCK resource manager reads the oscillator, the PERI0 words
 * and the PCK bus clock gate; none needs a write here. */
static int gpu_now(void *v,uint64_t *out){assert(v==&r);return pmic_model_now(&m,out);}
static int gpu_read(void *v,uintptr_t a,uint32_t *out)
{
    assert(v==&r);
    if(a==0x07090160)*out=r.osc;
    else if(a==RCCU_BASE+0x1ac)*out=d.rccu_1ac;
    else if(a==CCU+0x5c0)*out=d.ccu_5c0;
    else if(a==CCU+0x5e0)*out=d.ccu_5e0;
    else *out=*word(a);
    return 1;
}
static int gpu_update(void *v,uintptr_t a,uint32_t q,uint32_t x,uint64_t t){(void)v;(void)a;(void)q;(void)x;(void)t;assert(0);return 0;}
static int gpu_rail(void *v,struct a733_gpu_resource_rail *o,uint64_t t){(void)v;(void)o;(void)t;assert(0);return 0;}
static void npu_registers(void)
{
    memset(&r,0,sizeof(r));r.lock_us=200;r.osc=0x10000002;
    /* Reference PLL at 24 MHz; AHB, NSI and MBUS on PERI0 600, 800 and 400 MHz
     * outputs; PERI0 enabled, locked and its outputs gated on. */
    r.ccu[0]=0xf8000000|(99<<16)|(99<<8);
    r.ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    r.ccu[0x1908/4]=0x0fff0fff;r.ccu[0x1988/4]=0x0fff0000;
    r.ccu[0x500/4]=0x03000001;r.ccu[0x580/4]=0x82000001;r.ccu[0x588/4]=0x84000000;
    r.ccu[0xb04/4]=1u<<19;r.ccu[0xc00/4]=1u<<31;r.ccu[0xc0c/4]=1u<<16;
    r.ccu[0x2ac/4]=0x30000000;r.ccu[0x2b0/4]=0x000b0000;
}
static struct a733_npu_startup_config config(void)
{
    return (struct a733_npu_startup_config){
        .fabric={{0x03000001,0x82000001,0x84000000},{300000000,400000000,400000000}},
        .owner=31,.deadline_us=3000000,.pck_client=4};
}
static void setup(void)
{
    domains_model_setup(0x28,23);m.now_ns=40;m.access_ns=200;m.edge_us=5;
    npu_registers();memset(&s,0,sizeof(s));scheduler_running=0;
    static const struct a733_gpu_resource_ops gpu={gpu_now,gpu_read,gpu_update,gpu_rail};
    m.gpu=(struct a733_gpu_resource_manager){0};assert(a733_gpu_resources_init(&m.gpu,&gpu,&r));
    counter=(struct a733_counter_clock){0};counter.initialized=1;
    locks=(struct a733_platform_locks){0};locks.initialized=1;
    const struct a733_npu_startup_config c=config();
    const struct a733_npu_startup_providers p={&counter,&m.clocks,&locks,&m.gpu,&m.rails,&d.owner};
    const struct a733_npu_startup_ops o={{reg_read,reg_write},now_us,spin_us,enter,leave,retain};
    assert(a733_npu_startup_init(&s,&c,&p,&o,&r) && a733_npu_startup_bind_task(&s,(TaskHandle_t)&s));
    scheduler_running=1;
}
static enum a733_npu_platform_result step(void)
{
    const unsigned pmic=m.reads+m.writes;
    bootstrap_running=1;enum a733_npu_platform_result x=a733_npu_startup_step(&s);bootstrap_running=0;
    r.pmic_reads_before+=m.reads+m.writes-pmic; /* PMIC traffic caused by startup. */
    return x;
}
struct run { enum a733_npu_platform_result result; unsigned calls,advancing,ticks,pending_clock_reads; uint64_t elapsed_us,stage_us[A733_NPU_STARTUP_STOPPED+1],supply_done_us; };
/* Boot: both tasks start at one tick. Each tick the bootstrap calls the step
 * once; the service runs after it whenever notified or due. */
static struct run boot(unsigned limit,unsigned service)
{
    struct run x={.result=A733_NPU_PLATFORM_PENDING};
    const uint64_t t0=m.now;uint64_t next=t0;d.wake_at=0;
    while(x.ticks<limit) {
        if(m.now>=next) {
            const unsigned stage=s.stage,clock_reads=s.clock_reads;
            x.result=step();++x.calls;
            if(s.stage!=stage || x.result==A733_NPU_PLATFORM_READY)++x.advancing;
            if(s.stage!=stage && s.stage<=A733_NPU_STARTUP_STOPPED)x.stage_us[s.stage]=m.now-t0;
            else x.pending_clock_reads+=s.clock_reads-clock_reads;
            if(x.result!=A733_NPU_PLATFORM_PENDING){x.elapsed_us=m.now-t0;break;}
            x.ticks=(unsigned)((m.now-t0)/TICK_US)+1;next=t0+(uint64_t)x.ticks*TICK_US;
            continue;
        }
        if(service && (d.notified || m.now>=d.wake_at)){
            service_step();
            if(!x.supply_done_us && (a733_power_authority_load(&m.rails.authority)&A733_POWER_SUPPLY_DONE))x.supply_done_us=m.now-t0;
            continue;
        }
        uint64_t until=next;
        if(service && d.wake_at<until)until=d.wake_at;
        if(until>m.now)m.now=until;
    }
    x.ticks=(unsigned)((m.now-t0+TICK_US-1)/TICK_US);
    return x;
}
static int tokens_released(void)
{
    return a733_clock_token_status(&m.clocks,&s.clock)!=A733_CLOCK_OK &&
        a733_rail_status(&m.rails,&s.rail)!=A733_RAIL_READY &&
        a733_gpu_resources_status(&m.gpu,s.config.pck_client,&s.pck)!=A733_GPU_RESOURCE_OK;
}
static int tokens_held(void)
{
    return a733_clock_token_status(&m.clocks,&s.clock)==A733_CLOCK_OK &&
        a733_rail_status(&m.rails,&s.rail)==A733_RAIL_READY &&
        a733_gpu_resources_status(&m.gpu,s.config.pck_client,&s.pck)==A733_GPU_RESOURCE_OK;
}
int main(void)
{
    (void)unused_boot;
    /* Boot to READY. */
    setup();
    struct run b=boot(200,1);
    assert(b.result==A733_NPU_PLATFORM_READY && s.stage==A733_NPU_STARTUP_READY);
    assert(r.ccu[0xb00/4]==0x80000000 && (r.ccu[0xb04/4]&0xf0001)==0xf0001 && r.ccu[0x2a0/4]==0xe8305100);
    assert((d.ppu[4][0]&15)==8 && (d.ppu[4][2]&15)==8 && d.owner.domain[1].state==A733_POWER_DOMAIN_ON);
    assert(!r.held && !r.pmic_reads_before && tokens_held());
    printf("{\"boot\":{\"ticks\":%u,\"elapsed_us\":%llu,\"calls\":%u,\"advancing_calls\":%u,"
        "\"register_reads\":%u,\"register_writes\":%u,\"lock_acquisitions\":%u,\"counter_reads\":%u,"
        "\"counter_reads_on_pending_calls\":%u,\"lock_samples\":%u,\"pmic_transactions_by_startup\":%u,"
        "\"supply_done_us\":%llu,\"row_us\":%llu,\"settle_us\":%llu},",
        b.ticks,(unsigned long long)b.elapsed_us,b.calls,b.advancing,s.reads,s.writes,s.lock_enters,
        s.clock_reads,b.pending_clock_reads,s.lock_samples,r.pmic_reads_before,
        (unsigned long long)b.supply_done_us,
        (unsigned long long)b.stage_us[A733_NPU_STARTUP_ROW],(unsigned long long)b.stage_us[A733_NPU_STARTUP_SETTLE]);
    /* Local stop: one locked pass of five writes with readback. */
    {
        const unsigned reads=s.reads,writes=s.writes,enters=s.lock_enters,clocks=s.clock_reads;
        bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
        assert((r.ccu[0xb00/4]&0x80000000)==0 && (r.ccu[0xb04/4]&0xf0001)==(1u<<19) && r.ccu[0x2a0/4]==0xe8305100);
        printf("\"stop\":{\"register_reads\":%u,\"register_writes\":%u,\"lock_acquisitions\":%u,\"counter_reads\":%u},",
            s.reads-reads,s.writes-writes,s.lock_enters-enters,s.clock_reads-clocks);
    }
    /* Supply not verified: DCDC2 at selector 31. Refused before any write. */
    setup();m.pmic[0x13]=31;
    b=boot(200,1);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && s.failed && !r.writes && !r.enters && tokens_released());
    /* The supply never completes (the service never runs): refused at the
     * wait's deadline, which the provider's startup deadline anchors. */
    setup();
    b=boot(1200,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !r.enters && tokens_released());
    assert(m.now>=m.provider.config.startup_deadline+A733_POWER_SUPPLY_SETTLE_US+A733_POWER_SUPPLY_BUDGET_US);
    printf("\"supply_wait_refused_after_us\":%llu,",(unsigned long long)b.elapsed_us);
    /* Register pass mismatch: PLL_NPU found enabled. */
    setup();r.ccu[0x2a0/4]=0x80000000;
    b=boot(200,1);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && r.enters==1 && !r.held && tokens_released());
    /* LOCK never seen: retained after 100 observations with the output gate
     * closed and the PLL left enabled. */
    setup();r.never_lock=1;
    b=boot(200,1);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && s.lock_samples==A733_NPU_PLL_SAMPLES);
    assert(r.ccu[0x2a0/4]==0xe0305100 && !(r.ccu[0xb04/4]&0x70001) && tokens_held() && (d.ppu[4][0]&15)==0);
    /* A stalled PPU: the owner fails the domain at its 10 ms bound and the
     * row reads FAILED, or the startup's row deadline expires; both retain. */
    setup();d.stall[4]=1;
    b=boot(200,1);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && tokens_held() && (r.ccu[0xb04/4]&0x70001)==0x70001);
    printf("\"stalled_domain_retained_after_us\":%llu,",(unsigned long long)b.elapsed_us);
    /* A readback mismatch in the local open table keeps the register lock. */
    setup();r.flip_b04=7;
    b=boot(200,1);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && r.held && r.retained && tokens_held());
    /* The reference clock: 24 MHz and normalized 26 MHz inputs are admitted;
     * a wrong ratio, a reference PLL that is not locked, enabled or stable,
     * and an oscillator that is not stable are refused before any write. */
    static const struct { uint32_t osc,ref; unsigned admitted; } refs[]={
        {0x10004002,0xf84f6300,1},{0x983fb0f7,0xf8675f00,1},
        {0x983fb0f7,0xf8675e00,0},{0x983fb0f7,0xe8675f00,0},{0x983fb0f7,0x78675f00,0},
        {0x983fb0f5,0xf8675f00,0},{0x883fb0f7,0xf8675f00,0}};
    for(unsigned i=0;i<sizeof(refs)/sizeof(*refs);++i) {
        setup();r.osc=refs[i].osc;r.ccu[0]=refs[i].ref;
        b=boot(200,1);
        assert(refs[i].admitted ? b.result==A733_NPU_PLATFORM_READY :
            b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && tokens_released());
    }
    puts("\"cases\":14}");
    puts("PASS NPU startup sequencer over the power-domain owner: boot, stop, three refusals, three retentions and seven reference-clock cases");
    return 0;
}
