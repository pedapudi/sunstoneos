/* SPDX-License-Identifier: Apache-2.0 */
/* The NPU startup sequencer over the power-domain owner, on the host.
 *
 * The model runs the actual sequencer (drivers/npu/a733_npu_startup.c) and its
 * tables with the actual PMIC provider, rail ledger, clock registry, GPU/PCK
 * resource manager and power-domain owner of checks/power-domains-model.h, at
 * the 400 kHz S_TWI0 timing of checks/power-domains-service.c. The NPU's CCU
 * words and the shared SRAM routing word (SYSCTRL 0x03000008) are a register
 * model; PLL_NPU reports LOCK lock_us after its enable table; domain 4 is the
 * owner model's PPU, which acknowledges a policy write 10 us later. Time is
 * the PMIC model clock.
 *
 * Scheduling follows the images: the bootstrap task (priority 4) calls the
 * step once per tick until READY; the power service (priority 3) runs after
 * it within a tick when notified or when its own ticks elapse. Both start at
 * the same tick, the service's first step, except in one case where the
 * service starts 3.5 ticks later.
 *
 * Cases: boot to READY with the counts of the design's budget table, which
 * include the owner's reads for the domain check, domain 4 ON before the first
 * CCU write, and the SRAM routed after the PLL output gate opens and before
 * the first reset release; the late power service; each refusal before the
 * first NPU write (supply unverified, supply never done, register pass
 * mismatch) releases the tokens and writes nothing; LOCK never seen, a
 * stalled PPU and a readback mismatch retain the tokens; local stop, which
 * routes the SRAM back to the CPU. --second-start runs a second startup over
 * the state that local stop leaves, which adopts PLL_NPU and domain 4, and
 * the adoption and routing refusals. --retune changes the rate of a stopped
 * NPU (a733_npu_startup_retune) in both directions, with its refusals and a
 * lock failure. Output: JSON lines, then PASS. */
#include "power-domains-model.h"
/* The model boots the service itself; it does not use domains_boot(). */
static void unused_boot(void){(void)domains_boot;}
#include "a733_npu_startup.h"

#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07064000)
#define SYSCTRL UINT32_C(0x03000008)
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
    unsigned flip_pll,open_pll_reads;
    /* The routing word, and whether it ignores writes. Write positions (1 =
     * first write) of the routing clear, the PLL output gate opening and the
     * first bus and reset word write; PCK600 writes before the first write. */
    uint32_t sysctrl,sid; /* The routing word; the SID speed-bin word. */
    unsigned sysctrl_stuck,route_at,pll_open_at,first_b04_at,ppu_before_first_write,pll_writes,gate_writes;
    unsigned gate_status_off; /* +0x19a0 reads 0 whatever the gate control holds. */
    unsigned woken,wakes,waits; /* The power service's notifications of the bootstrap; its blocks. */
    uint32_t pll_log[16];unsigned pll_log_n; /* PLL_NPU control writes in order. */
    /* Counted in the callbacks: sequencer register reads, LOCK samples among
     * them (PLL_NPU reads without the register lock), the owner's register
     * reads during a step, and counter reads. */
    unsigned reads,lock_samples,owner_reads,clock_reads,pck_reads;
    uintptr_t last_write;
} r;
static uint32_t *word(uintptr_t a)
{assert(a>=CCU && a<CCU+0x2000 && !(a&3));return &r.ccu[(a-CCU)/4];}
static int reg_read(void *v,uintptr_t a,uint32_t *out)
{
    assert(v==&r && bootstrap_running);
    ++r.reads;if(a==CCU+0x2a0 && !r.held)++r.lock_samples;
    if(a==0x07090160){*out=r.osc;return 1;}
    if(a==SYSCTRL){*out=r.sysctrl;return 1;}
    if(a==UINT32_C(0x0300624c)){*out=r.sid;return 1;}
    if(a==PCK || a==PCK+8){++r.pck_reads;return owner_read(&d,a,out);} /* The owner model's PPU words. */
    if(a==CCU+0x2a0) {
        if(s.stage==A733_NPU_STARTUP_OPEN)++r.open_pll_reads;
        const unsigned flip=r.flip_pll;
        if((flip==1 && s.stage==A733_NPU_STARTUP_ENABLE && r.writes==2) ||
           (flip==2 && !r.held && r.writes) ||
           (flip==3 && s.stage==A733_NPU_STARTUP_OPEN && r.open_pll_reads==1) ||
           (flip==4 && s.stage==A733_NPU_STARTUP_OPEN && r.open_pll_reads==2) ||
           (flip==5 && s.stage==A733_NPU_STARTUP_FINAL) ||
           (flip==6 && s.stage==A733_NPU_STARTUP_READY)) {
            *word(a)^=1u<<24;r.flip_pll=0;
        }
    }
    *out=*word(a);
    if(a==CCU+0x2a0) {
        if((*out&0xa0000000)==0xa0000000 && r.never_lock!=1) {
            if(!r.locked_at)r.locked_at=m.now+r.lock_us;
            if(m.now>=r.locked_at)*out|=1u<<28;
        }
    } else if(a==CCU+0x19a0) {
        *out=(r.ccu[0x1920/4]&0x10001)==0x10001 && !r.gate_status_off ? 1u<<16 : 0;
    } else if(a==CCU+0xb04 && r.flip_b04 && r.writes>=r.flip_b04 && r.last_write==a) {
        *out^=1u<<18;r.flip_b04=0; /* A completed write that reads back wrong. */
    }
    return 1;
}
static int reg_write(void *v,uintptr_t a,uint32_t value)
{
    assert(v==&r && bootstrap_running && r.held);
    assert(a==CCU+0xb00 || a==CCU+0xb04 || a==CCU+0x1920 || a==CCU+0x2a0 || a==SYSCTRL);
    if(!r.writes)r.ppu_before_first_write=d.ppu_writes;
    ++r.writes;r.last_write=a;
    if(a==SYSCTRL) {
        assert(!((r.sysctrl^value)&~2u));
        if(!(value&2) && !r.route_at)r.route_at=r.writes;
        if(!r.sysctrl_stuck)r.sysctrl=value;
        return 1;
    }
    assert(a!=CCU+0x2a0 || !(value&(1u<<28)));
    if(a==CCU+0x2a0) {
        assert(!((*word(a)^value)&(1u<<24)));++r.pll_writes;
        /* New factors or a cleared lock enable: LOCK returns lock_us later. */
        if(((*word(a)^value)&0x0070ff02) || !(value&(1u<<29)))r.locked_at=0;
        /* 2: LOCK stops once new factors are written. */
        if(r.never_lock==2 && ((*word(a)^value)&0x0070ff02))r.never_lock=1;
        if(r.pll_log_n<16)r.pll_log[r.pll_log_n++]=value;
        if((value&(1u<<27)) && !r.pll_open_at)r.pll_open_at=r.writes;
    }
    if(a==CCU+0x1920)++r.gate_writes;
    if(a==CCU+0xb04 && !r.first_b04_at)r.first_b04_at=r.writes;
    *word(a)=value;return 1;
}
static int now_us(void *v,uint64_t *out){assert(v==&r);++r.clock_reads;return pmic_model_now(&m,out);}
/* Raw counter for the LOCK spacing: one modeled microsecond per read. */
static uint64_t spin_us(void *v){assert(v==&r);return ++m.now;}
static int enter(void *v,uint64_t deadline)
{assert(v==&r && !r.held && m.now<deadline);++r.enters;r.held=1;return 1;}
static void leave(void *v){assert(v==&r && r.held);r.held=0;}
static void retain(void *v){assert(v==&r && r.held);r.retained=1;}
/* The power service's notification: the bootstrap runs after the service's step. */
static void wake(void *v){assert(v==&r && !bootstrap_running);r.woken=1;++r.wakes;}

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
    memset(&r,0,sizeof(r));r.lock_us=200;r.osc=0x10000002;r.sysctrl=2; /* The boot ROM's CPU routing. */
    r.sid=0x496; /* Port 3-3's fuse word: code 4, column vf0400. */
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
static unsigned setup_mhz=492; /* The profile that setup() selects. */
static unsigned setup_bus_100khz; /* 1: setup() runs the PMIC bus at 100 kHz. */
static void setup(void)
{
    /* S_TWI0 at 400 kHz, or at 100 kHz as checks/power-domains-service.c models it. */
    if(setup_bus_100khz){domains_model_setup(0x2a,90);m.now_ns=40;m.access_ns=200;m.edge_us=20;}
    else{domains_model_setup(0x28,23);m.now_ns=40;m.access_ns=200;m.edge_us=5;}
    npu_registers();memset(&s,0,sizeof(s));scheduler_running=0;
    static const struct a733_gpu_resource_ops gpu={gpu_now,gpu_read,gpu_update,gpu_rail};
    m.gpu=(struct a733_gpu_resource_manager){0};assert(a733_gpu_resources_init(&m.gpu,&gpu,&r));
    counter=(struct a733_counter_clock){0};counter.initialized=1;
    locks=(struct a733_platform_locks){0};locks.initialized=1;
    const struct a733_npu_startup_config c=config();
    const struct a733_npu_startup_providers p={&counter,&m.clocks,&locks,&m.gpu,&m.rails,&d.owner};
    const struct a733_npu_startup_ops o={{reg_read,reg_write},now_us,spin_us,enter,leave,retain,wake};
    assert(a733_npu_startup_init(&s,&c,&p,&o,&r) && a733_npu_startup_rate(&s,setup_mhz) &&
        a733_npu_startup_bind_task(&s,(TaskHandle_t)&s) && !a733_npu_startup_rate(&s,492));
    scheduler_running=1;
}
static enum a733_npu_platform_result step(void)
{
    const unsigned pmic=m.reads+m.writes,owner=d.level_reads+d.ppu_reads-r.pck_reads;
    bootstrap_running=1;enum a733_npu_platform_result x=a733_npu_startup_step(&s);bootstrap_running=0;
    r.pmic_reads_before+=m.reads+m.writes-pmic; /* PMIC traffic caused by startup. */
    r.owner_reads+=d.level_reads+d.ppu_reads-r.pck_reads-owner; /* The owner's domain check. */
    return x;
}
struct run { enum a733_npu_platform_result result; unsigned calls,advancing,ticks,pending_clock_reads; uint64_t elapsed_us,stage_us[A733_NPU_STARTUP_STOPPED+1],supply_done_us,row_on_us; };
/* Boot: the bootstrap calls the step, and after PENDING blocks for the ticks
 * that a733_npu_startup_wait_ticks gives (0: at once), or until the power
 * service notifies it; the service runs after it whenever notified or due,
 * from its first step service_from_us after the bootstrap's first call. */
static struct run boot(unsigned limit,unsigned service,uint64_t service_from_us)
{
    struct run x={.result=A733_NPU_PLATFORM_PENDING};
    const uint64_t t0=m.now;uint64_t next=t0;d.wake_at=t0+service_from_us;
    while(x.ticks<limit) {
        if(m.now>=next || r.woken) {
            r.woken=0;
            const unsigned stage=s.stage,clock_reads=r.clock_reads;
            x.result=step();++x.calls;
            /* The register window opens only at READY. */
            assert((atomic_load(&s.window_open)==1)==(x.result==A733_NPU_PLATFORM_READY));
            if(s.stage!=stage || x.result==A733_NPU_PLATFORM_READY)++x.advancing;
            if(s.stage!=stage && s.stage<=A733_NPU_STARTUP_STOPPED)x.stage_us[s.stage]=m.now-t0;
            else x.pending_clock_reads+=r.clock_reads-clock_reads;
            if(x.result!=A733_NPU_PLATFORM_PENDING){x.elapsed_us=m.now-t0;break;}
            bootstrap_running=1;const uint32_t w=a733_npu_startup_wait_ticks(&s);bootstrap_running=0;
            if(w)++r.waits;
            x.ticks=(unsigned)((m.now-t0)/TICK_US)+1;
            next=w ? t0+((m.now-t0)/TICK_US+w)*TICK_US : m.now;
            continue;
        }
        if(service && m.now>=t0+service_from_us && (d.notified || m.now>=d.wake_at)){
            service_step();
            if(!x.supply_done_us && (a733_power_authority_load(&m.rails.authority)&A733_POWER_SUPPLY_DONE))x.supply_done_us=m.now-t0;
            /* The end of the service step that answered the domain-4 row ON. */
            if(!x.row_on_us && a733_power_domain_state(&s.row)==A733_POWER_ROW_ON)x.row_on_us=m.now-t0;
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
/* The inherited ON state still requires exclusive external ownership, closed
 * local gates, asserted engine resets and a disabled PLL. The register model
 * supplies those facts; physical PCK reads alone cannot establish them. */
static int inherited_on(void)
{
    setup();d.ppu[4][0]|=8;d.ppu[4][2]|=8;
    struct run b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_READY && tokens_held());
    assert(!d.ppu_writes && s.row.counted==1 && d.owner.domain[1].count==1);
    assert(m.rails.pins[s.rail.id&(A733_RAIL_CLIENTS-1)]==1);
    assert(b.ticks<=13 && r.writes<=13 && !r.pmic_reads_before && r.sysctrl==0);
    printf("{\"inherited_on\":{\"ticks\":%u,\"elapsed_us\":%llu,\"settle_us\":%llu,\"local_writes\":%u,\"pck_writes\":%u},",
        b.ticks,(unsigned long long)b.elapsed_us,
        (unsigned long long)b.stage_us[A733_NPU_STARTUP_SETTLE],r.writes,d.ppu_writes);
    /* Local CLOSE keeps the power-domain row and rail pin. The actual owner
     * refuses a later OFF request for domain 4, without releasing its pin. */
    bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
    assert(tokens_held() && s.row.counted==1 && d.owner.domain[1].count==1 && !d.ppu_writes);
    const unsigned request=atomic_load(&s.row.request),state=atomic_load(&s.row.state);
    assert(!a733_power_domain_request(&s.row,0));service_step();
    assert(atomic_load(&s.row.request)==request && atomic_load(&s.row.state)==state &&
        a733_power_domain_state(&s.row)==A733_POWER_ROW_ON);
    assert(s.row.counted==1 && d.owner.domain[1].count==1 && !d.ppu_writes && tokens_held());
    assert(m.rails.pins[s.rail.id&(A733_RAIL_CLIENTS-1)]==1);
    /* Each field below invalidates the inherited-state precheck separately. */
    static const struct { unsigned offset,bit; } unsafe[]={
        {0xb00,31},{0xb04,0},{0xb04,16},{0xb04,17},{0xb04,18},{0x2a0,31}};
    for(unsigned i=0;i<sizeof(unsafe)/sizeof(*unsafe);++i) {
        setup();d.ppu[4][0]|=8;d.ppu[4][2]|=8;
        r.ccu[unsafe[i].offset/4]|=1u<<unsafe[i].bit;
        b=boot(200,1,0);
        assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !d.ppu_writes &&
            !s.row.counted && !r.held && tokens_released());
    }
    /* Policy/status mismatch and a transitioning status are not adoption. */
    for(unsigned status=0;status<2;++status) {
        setup();d.ppu[4][0]|=8;d.ppu[4][2]|=status ? 1 : 0;
        b=boot(200,1,0);
        assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !d.ppu_writes && tokens_released());
    }
    /* A later failed write has changed hardware and retains resources. */
    setup();d.ppu[4][0]|=8;d.ppu[4][2]|=8;r.flip_b04=7;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && r.writes && r.held && r.retained &&
        tokens_held() && !d.ppu_writes);
    puts("\"unsafe_local_refusals\":6,\"incoherent_refusals\":2,\"postwrite_retention\":true,\"domain_off_refused\":true}");
    puts("PASS inherited NPU domain ownership, local-state refusal, retention and release policy");
    return 0;
}
static int inherited_pll(void)
{
    struct {unsigned reads,writes,clocks,locks;uint64_t elapsed;} baseline={0};
    unsigned drift_refusals=0;
    for(unsigned inherited=0;inherited<2;++inherited) {
        const uint32_t keep=inherited<<24;
        setup();d.ppu[4][0]|=8;d.ppu[4][2]|=8;r.ccu[0x2a0/4]=0x40104500|keep;
        struct run b=boot(200,1,0);
        assert(b.result==A733_NPU_PLATFORM_READY && s.pll_inherited==keep &&
            r.ccu[0x2a0/4]==(0xe8305100|keep) && !d.ppu_writes && tokens_held());
        if(!inherited){baseline.reads=r.reads;baseline.writes=r.writes;baseline.clocks=r.clock_reads;baseline.locks=r.enters;baseline.elapsed=b.elapsed_us;}
        else assert(r.reads==baseline.reads && r.writes==baseline.writes &&
            r.clock_reads==baseline.clocks && r.enters==baseline.locks && b.elapsed_us==baseline.elapsed);
        bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
        assert(r.ccu[0x2a0/4]==(0xe8305100|keep) && tokens_held());
        for(unsigned boundary=1;boundary<=6;++boundary) {
            setup();d.ppu[4][0]|=8;d.ppu[4][2]|=8;r.ccu[0x2a0/4]=0x40104500|keep;
            r.flip_pll=boundary==6 ? 0 : boundary;b=boot(200,1,0);
            if(boundary==6) {
                assert(b.result==A733_NPU_PLATFORM_READY);r.flip_pll=6;
                bootstrap_running=1;b.result=a733_npu_startup_stop(&s);bootstrap_running=0;
            }
            assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.flip_pll && s.pll_inherited==keep &&
                r.writes && tokens_held() && !d.ppu_writes);
            if(boundary==1)assert(r.writes==2); /* No PLL programming after drift. */
            ++drift_refusals;
        }
    }
    const unsigned unknown[]={23,25,26};
    for(unsigned i=0;i<sizeof(unknown)/sizeof(*unknown);++i) {
        setup();d.ppu[4][0]|=8;d.ppu[4][2]|=8;r.ccu[0x2a0/4]=0x41104500|(1u<<unknown[i]);
        struct run b=boot(200,1,0);
        assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !d.ppu_writes && tokens_released());
    }
    printf("{\"inherited_pll\":{\"control\":\"0x41104500\",\"elapsed_us\":%llu,\"reads\":%u,\"writes\":%u,"
        "\"counter_reads\":%u,\"locks\":%u,\"same_as_bit24_clear\":true,\"drift_refusals\":%u,\"other_reserved_refusals\":3}}\n",
        (unsigned long long)baseline.elapsed,baseline.reads,baseline.writes,baseline.clocks,baseline.locks,drift_refusals);
    puts("PASS inherited PLL reserved bit preservation and drift retention");return 0;
}
/* A second startup in one power lifetime: a new image finds PLL_NPU locked
 * with its output gate open, domain 4 ON, the local resets asserted and the
 * SRAM routed to the CPU, as local stop and the EL3 return leave them. Each
 * case runs with the reserved PLL bit 24 clear and set. */
static int second_start(void)
{
    unsigned refusals=0;
    for(unsigned inherited=0;inherited<2;++inherited) {
        const uint32_t keep=inherited<<24;
        setup();r.ccu[0x2a0/4]=keep;
        struct run b=boot(200,1,0);
        assert(b.result==A733_NPU_PLATFORM_READY && (d.ppu[4][2]&15)==8 && s.pll_inherited==keep);
        bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
        static uint32_t left[0x2000/4];memcpy(left,r.ccu,sizeof(left));
        const uint32_t policy=d.ppu[4][0],status=d.ppu[4][2];
        const unsigned first_writes=r.writes;
        assert(r.sysctrl==2 && left[0x2a0/4]==(0xe8305100|keep) && (left[0xb04/4]&0xf0001)==(1u<<19));
        /* adopted: 0 the stopped state; 1 other factors; 2 output gate closed;
         * 3 the gate control's No-Auto bit clear; 4 no LOCK; 5 the effective
         * gate status clear. */
        for(unsigned variant=0;variant<6;++variant) {
            setup();memcpy(r.ccu,left,sizeof(left));d.ppu[4][0]=policy;d.ppu[4][2]=status;r.locked_at=1;
            if(variant==1)r.ccu[0x2a0/4]=0xe8306100|keep;
            if(variant==2)r.ccu[0x2a0/4]=0xe0305100|keep;
            if(variant==3)r.ccu[0x1920/4]&=~1u;
            if(variant==4)r.never_lock=1;
            if(variant==5)r.gate_status_off=1;
            b=boot(200,1,0);
            if(variant) {
                assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !d.ppu_writes && !s.row.counted && tokens_released());
                ++refusals;continue;
            }
            assert(b.result==A733_NPU_PLATFORM_READY && s.pll_adopted && s.pll_inherited==keep && tokens_held());
            assert(!r.pll_writes && !r.gate_writes && !d.ppu_writes && r.ccu[0x2a0/4]==(0xe8305100|keep));
            assert(r.sysctrl==0 && r.route_at==1 && r.first_b04_at==2 && r.lock_samples==3);
            if(!inherited)printf("{\"second_start\":{\"ticks\":%u,\"elapsed_us\":%llu,\"local_writes\":%u,\"first_start_writes\":%u,"
                "\"pll_writes\":%u,\"pck_writes\":%u,\"lock_samples\":%u,",
                b.ticks,(unsigned long long)b.elapsed_us,r.writes,first_writes,r.pll_writes,d.ppu_writes,r.lock_samples);
            bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
            assert(r.sysctrl==2 && !memcmp(r.ccu,left,sizeof(left)));
        }
    }
    printf("\"adoption_refusals\":%u}}\n",refusals);
    puts("PASS second NPU startup adopts the locked PLL_NPU and the ON domain with bit 24 clear and set; ten adoption refusals write nothing");
    return 0;
}
/* The 852 MHz profile: a cold enable at the 852 factors, the speed-bin
 * admission, and the vendor's rate change of a PLL that an earlier start
 * left running at the other profile, in both directions. */
static int rate_852(void)
{
    setup_mhz=852;setup();
    struct run b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_READY && r.ccu[0x2a0/4]==0xe8104600 && r.sysctrl==0);
    printf("{\"cold_852\":{\"elapsed_us\":%llu,\"calls\":%u,\"writes\":%u,\"lock_samples\":%u},",
        (unsigned long long)b.elapsed_us,b.calls,r.writes,r.lock_samples);
    /* Codes with an 800 mV column at 852 MHz are admitted; the rest refuse
     * before any write. A nonzero dvfs_bak (bits 23:16) takes precedence. */
    static const struct { uint32_t sid; unsigned admitted; } bins[]={
        {0x0200,1},{0x1200,1},{0x0400,1},{0x0500,1},{0x1500,1},{0x00030400,0},{0x00040300,1},
        {0,0},{0x0100,0},{0x1100,0},{0x0300,0},{0x0600,0},{0x1400,0},{0x2200,0}};
    unsigned refused=0;
    for(unsigned i=0;i<sizeof(bins)/sizeof(*bins);++i) {
        setup();r.sid=bins[i].sid;b=boot(200,1,0);
        if(bins[i].admitted)assert(b.result==A733_NPU_PLATFORM_READY);
        else{assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !d.ppu_writes && tokens_released());++refused;}
    }
    /* Rate changes over the state that local stop left at the other profile. */
    for(unsigned up=0;up<2;++up) {
        setup_mhz=up ? 492 : 852;setup();
        b=boot(200,1,0);assert(b.result==A733_NPU_PLATFORM_READY);
        bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
        static uint32_t left[0x2000/4];memcpy(left,r.ccu,sizeof(left));
        const uint32_t policy=d.ppu[4][0],status=d.ppu[4][2],from=left[0x2a0/4];
        setup_mhz=up ? 852 : 492;setup();memcpy(r.ccu,left,sizeof(left));d.ppu[4][0]=policy;d.ppu[4][2]=status;
        r.locked_at=1;
        b=boot(200,1,0);
        const uint32_t to=up ? 0xe8104600 : 0xe8305100,f=to&0x0070ff02;
        assert(b.result==A733_NPU_PLATFORM_READY && !s.pll_adopted && r.ccu[0x2a0/4]==to && !r.gate_writes && !d.ppu_writes);
        /* Output gate off, factors, lock enable off then on, then the output
         * gate on in the open stage; LOCK was sampled after the new factors. */
        assert(r.pll_log_n==5 && r.pll_log[0]==(from&~(1u<<27)) && r.pll_log[1]==((0xe0000000)|f) &&
            r.pll_log[2]==(0xc0000000|f) && r.pll_log[3]==(0xe0000000|f) && r.pll_log[4]==to);
        assert(r.lock_samples>3);
        if(up)printf("\"retune_492_to_852\":{\"elapsed_us\":%llu,\"calls\":%u,\"writes\":%u,\"pll_writes\":%u,\"lock_samples\":%u},",
            (unsigned long long)b.elapsed_us,b.calls,r.writes,r.pll_writes,r.lock_samples);
        setup_mhz=492;
    }
    printf("\"bin_refusals\":%u}\n",refused);
    setup_mhz=492;setup();assert(!a733_npu_startup_rate(&s,600));
    puts("PASS NPU 852 MHz profile: cold enable, speed-bin admission, and rate changes in the vendor order");
    return 0;
}
/* The retune of a stopped NPU (a733_npu_startup_retune): after local stop,
 * PLL_NPU moves between the two profiles in the vendor's rate change while
 * the NPU's gates stay closed and its resets asserted. The refusals write
 * nothing; a lock that never comes retains with the output gate closed. */
static int stopped_npu(unsigned mhz)
{
    setup_mhz=mhz;setup();
    struct run b=boot(200,1,0);assert(b.result==A733_NPU_PLATFORM_READY);
    /* Before the stop the NPU runs: refused, nothing written. */
    const unsigned writes=r.writes;
    bootstrap_running=1;assert(a733_npu_startup_retune(&s,mhz==852 ? 492 : 852)==0);
    assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
    assert(r.writes>writes && s.stage==A733_NPU_STARTUP_STOPPED);
    return 1;
}
static int retune(void)
{
    static const struct { unsigned from,to; uint32_t want; } runs[]={{852,492,0xe8305100},{492,852,0xe8104600}};
    unsigned refusals=0;
    printf("{");
    for(unsigned i=0;i<2;++i) {
        stopped_npu(runs[i].from);
        const uint32_t b00=r.ccu[0xb00/4],b04=r.ccu[0xb04/4],from=r.ccu[0x2a0/4],f=runs[i].want&0x0070ff02;
        const unsigned writes=r.writes,enters=r.enters,samples=r.lock_samples,reads=r.reads;
        const uint64_t t0=m.now;r.pll_log_n=0;
        bootstrap_running=1;const int x=a733_npu_startup_retune(&s,runs[i].to);bootstrap_running=0;
        /* Output gate off, factors, lock enable off and on, LOCK, output
         * gate on; the module and bus gates, the resets and the routing stay. */
        assert(x==1 && r.ccu[0x2a0/4]==runs[i].want && r.ccu[0xb00/4]==b00 && r.ccu[0xb04/4]==b04 && r.sysctrl==2);
        assert(!(b00&(1u<<31)) && !(b04&0x70001));
        assert(r.pll_log_n==5 && r.pll_log[0]==(from&~(1u<<27)) && r.pll_log[1]==(0xe0000000|f) &&
            r.pll_log[2]==(0xc0000000|f) && r.pll_log[3]==(0xe0000000|f) && r.pll_log[4]==runs[i].want);
        assert(r.writes-writes==5 && r.enters-enters==2 && !r.held && r.lock_samples-samples>=3 &&
            s.stage==A733_NPU_STARTUP_STOPPED && !s.failed && tokens_held());
        printf("\"retune_%u_to_%u\":{\"pll_writes\":%u,\"register_reads\":%u,\"lock_samples\":%u,\"lock_acquisitions\":%u,\"elapsed_us\":%llu},",
            runs[i].from,runs[i].to,r.writes-writes,r.reads-reads-(r.lock_samples-samples),r.lock_samples-samples,
            r.enters-enters,(unsigned long long)(m.now-t0));
        /* The same rate again, and a rate outside the two profiles, write nothing. */
        bootstrap_running=1;
        assert(a733_npu_startup_retune(&s,runs[i].to)==1 && r.writes-writes==5);
        assert(a733_npu_startup_retune(&s,600)==0 && r.writes-writes==5);
        bootstrap_running=0;refusals+=1;
    }
    /* Another task: refused. */
    assert(a733_npu_startup_retune(&s,492)==0);++refusals;
    /* 852 MHz on a bin whose column lists another voltage (vf0401, code
     * 0x14): refused before any write. */
    stopped_npu(492);r.sid=0x1400;
    unsigned writes=r.writes;
    bootstrap_running=1;assert(a733_npu_startup_retune(&s,852)==0 && r.writes==writes && !r.held);bootstrap_running=0;
    ++refusals;
    /* A state other than the one local stop leaves (the module gate open):
     * refused, nothing written, the register lock released. */
    stopped_npu(852);r.ccu[0xb00/4]|=1u<<31;writes=r.writes;
    bootstrap_running=1;assert(a733_npu_startup_retune(&s,492)==0 && r.writes==writes && !r.held && !s.failed);bootstrap_running=0;
    ++refusals;
    /* LOCK never seen: retained after 100 observations, the output gate
     * closed at the new factors and every token held. */
    stopped_npu(852);r.never_lock=2;
    bootstrap_running=1;assert(a733_npu_startup_retune(&s,492)==-1);bootstrap_running=0;
    assert(s.failed && r.ccu[0x2a0/4]==0xe0305100 && tokens_held() && !r.held);
    printf("\"refusals\":%u,\"lock_failure_retained\":1}\n",refusals);
    puts("PASS stopped NPU retune: both directions in the vendor order with the gates closed, five refusals before any write, and a retained lock failure");
    return 0;
}
int main(int argc,char **argv)

{
    (void)unused_boot;
    if(argc==2 && !strcmp(argv[1],"--inherited-on"))return inherited_on();
    if(argc==2 && !strcmp(argv[1],"--inherited-pll"))return inherited_pll();
    if(argc==2 && !strcmp(argv[1],"--second-start"))return second_start();
    if(argc==2 && !strcmp(argv[1],"--rate-852"))return rate_852();
    if(argc==2 && !strcmp(argv[1],"--retune"))return retune();
    assert(argc==1);
    /* Boot to READY. */
    setup();
    struct run b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_READY && s.stage==A733_NPU_STARTUP_READY);
    assert(r.ccu[0xb00/4]==0x80000000 && (r.ccu[0xb04/4]&0xf0001)==0xf0001 && r.ccu[0x2a0/4]==0xe8305100);
    assert((d.ppu[4][0]&15)==8 && (d.ppu[4][2]&15)==8 && d.owner.domain[1].state==A733_POWER_DOMAIN_ON);
    assert(!r.held && !r.pmic_reads_before && tokens_held());
    /* Power before clocks: domain 4's PCK600 command precedes the first CCU
     * write. The SRAM is routed to the NPU after the PLL output gate opens and
     * before the first write of the bus and reset word. */
    assert(r.ppu_before_first_write && !s.pll_adopted && r.sysctrl==0 && s.sram_routed);
    /* Startup init marks the supply settled: verification ends before the
     * 5,160 us settle would, and the mark is refused once the service ran. */
    assert(d.owner.settled_supply && b.supply_done_us<A733_POWER_SUPPLY_SETTLE_US &&
        !a733_power_domains_settled_supply(&d.owner));
    assert(r.pll_open_at && r.pll_open_at<r.route_at && r.route_at+1==r.first_b04_at);
    /* register_reads: the sequencer's reads without the LOCK samples, plus the
     * owner's domain-check reads, as the base counted its own domain reads.
     * device_reads adds the native write adapter's read of each written word. */
    const unsigned reads=r.reads-r.lock_samples+r.owner_reads;
    printf("{\"boot\":{\"ticks\":%u,\"elapsed_us\":%llu,\"calls\":%u,\"advancing_calls\":%u,"
        "\"register_reads\":%u,\"sequencer_reads\":%u,\"owner_reads\":%u,\"device_reads\":%u,"
        "\"register_writes\":%u,\"lock_acquisitions\":%u,\"counter_reads\":%u,"
        "\"counter_reads_on_pending_calls\":%u,\"lock_samples\":%u,\"pmic_transactions_by_startup\":%u,"
        "\"supply_done_us\":%llu,\"row_us\":%llu,\"settle_us\":%llu,\"pck_writes_before_first_write\":%u,"
        "\"routing_write\":%u,\"pll_output_gate_write\":%u,\"first_reset_write\":%u},",
        b.ticks,(unsigned long long)b.elapsed_us,b.calls,b.advancing,reads,r.reads-r.lock_samples,r.owner_reads,
        reads+r.writes,r.writes,r.enters,
        r.clock_reads,b.pending_clock_reads,r.lock_samples,r.pmic_reads_before,
        (unsigned long long)b.supply_done_us,
        (unsigned long long)b.stage_us[A733_NPU_STARTUP_ROW],(unsigned long long)b.stage_us[A733_NPU_STARTUP_SETTLE],
        r.ppu_before_first_write,r.route_at,r.pll_open_at,r.first_b04_at);
    /* Local stop: one locked pass of five writes with readback. */
    {
        const unsigned reads=r.reads,writes=r.writes,enters=r.enters,clocks=r.clock_reads;
        bootstrap_running=1;assert(a733_npu_startup_stop(&s)==A733_NPU_PLATFORM_READY);bootstrap_running=0;
        assert((r.ccu[0xb00/4]&0x80000000)==0 && (r.ccu[0xb04/4]&0xf0001)==(1u<<19) && r.ccu[0x2a0/4]==0xe8305100);
        assert(r.sysctrl==2 && !s.sram_routed && !atomic_load(&s.window_open));
        printf("\"stop\":{\"register_reads\":%u,\"register_writes\":%u,\"lock_acquisitions\":%u,\"counter_reads\":%u},",
            r.reads-reads,r.writes-writes,r.enters-enters,r.clock_reads-clocks);
    }
    /* Supply not verified: DCDC2 at selector 31. Refused before any write. */
    setup();m.pmic[0x13]=31;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && s.failed && !r.writes && !r.enters && tokens_released());
    /* The supply never completes (the service never runs): refused at the
     * wait's deadline, which the provider's startup deadline anchors. */
    setup();
    b=boot(1200,0,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !r.enters && tokens_released());
    assert(m.now>=m.provider.config.startup_deadline+A733_POWER_SUPPLY_SETTLE_US+A733_POWER_SUPPLY_BUDGET_US);
    printf("\"supply_wait_refused_after_us\":%llu,",(unsigned long long)b.elapsed_us);
    /* The power service's first step comes 3.5 ticks after the startup's
     * token step; the supply wait spans it and the startup reaches READY. */
    setup();
    b=boot(200,1,3500);
    assert(b.result==A733_NPU_PLATFORM_READY && b.supply_done_us>3500 && tokens_held());
    printf("\"late_service_ready_after_us\":%llu,",(unsigned long long)b.elapsed_us);
    /* Register pass mismatch: PLL_NPU found enabled. */
    setup();r.ccu[0x2a0/4]=0x80000000;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && r.enters==1 && !r.held && tokens_released());
    /* LOCK never seen: retained after 100 observations with the output gate
     * closed and the PLL left enabled. */
    setup();r.never_lock=1;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && r.lock_samples==100);
    assert(r.ccu[0x2a0/4]==0xe0305100 && !(r.ccu[0xb04/4]&0x70001) && tokens_held() && (d.ppu[4][0]&15)==8);
    assert(r.sysctrl==2 && !r.route_at);
    /* A stalled PPU: the owner fails the domain at its 10 ms bound and the
     * row reads FAILED, or the startup's row deadline expires; both retain.
     * No NPU clock, reset or routing write precedes power. */
    setup();d.stall[4]=1;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && tokens_held() && !r.writes && r.sysctrl==2);
    printf("\"stalled_domain_retained_after_us\":%llu,",(unsigned long long)b.elapsed_us);
    /* A readback mismatch in the local open table keeps the register lock. */
    setup();r.flip_b04=7;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && r.held && r.retained && tokens_held());
    /* Routing refusals. A word that already routes the SRAM to the NPU
     * refuses in the read-only pass, before the domain-4 request. A word that
     * ignores the write retains after PLL_NPU locks, before any reset
     * release. */
    setup();r.sysctrl=0;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && !d.ppu_writes && !s.row.counted && tokens_released());
    setup();r.sysctrl_stuck=1;
    b=boot(200,1,0);
    assert(b.result==A733_NPU_PLATFORM_RETAINED && r.held && r.retained && tokens_held());
    assert(!r.first_b04_at && !s.sram_routed && r.route_at && r.sysctrl==2);
    /* A restore that does not read back retains at stop. */
    setup();b=boot(200,1,0);assert(b.result==A733_NPU_PLATFORM_READY);r.sysctrl_stuck=1;
    bootstrap_running=1;b.result=a733_npu_startup_stop(&s);bootstrap_running=0;
    assert(b.result==A733_NPU_PLATFORM_RETAINED && r.held && r.retained && !atomic_load(&s.window_open));
    /* The reference clock: 24 MHz and normalized 26 MHz inputs are admitted;
     * a wrong ratio, a reference PLL that is not locked, enabled or stable,
     * and an oscillator that is not stable are refused before any write. */
    static const struct { uint32_t osc,ref; unsigned admitted; } refs[]={
        {0x10004002,0xf84f6300,1},{0x983fb0f7,0xf8675f00,1},
        {0x983fb0f7,0xf8675e00,0},{0x983fb0f7,0xe8675f00,0},{0x983fb0f7,0x78675f00,0},
        {0x983fb0f5,0xf8675f00,0},{0x883fb0f7,0xf8675f00,0}};
    for(unsigned i=0;i<sizeof(refs)/sizeof(*refs);++i) {
        setup();r.osc=refs[i].osc;r.ccu[0]=refs[i].ref;
        b=boot(200,1,0);
        assert(refs[i].admitted ? b.result==A733_NPU_PLATFORM_READY :
            b.result==A733_NPU_PLATFORM_RETAINED && !r.writes && tokens_released());
    }
    puts("\"cases\":18}");
    puts("PASS NPU startup sequencer over the power-domain owner: boot, a late power service, stop, four refusals, five retentions and seven reference-clock cases");
    return 0;
}
