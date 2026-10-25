/* SPDX-License-Identifier: GPL-2.0-only */
/* The power-domain owner's PCK600 sequences over modeled registers, with the
 * actual PMIC provider, rail ledger and supply verification
 * (checks/power-domains-model.h).
 *
 * An OFF domain receives the pinned sun60iw2 delay row, each word read back,
 * and one ON command that preserves the other policy bits; an ON domain is
 * adopted without writes; an incoherent domain is refused without writes.
 * Admission requires the verified supply, the rail token and the domain-level
 * reads, and issues no PMIC transaction: supply verification makes 12 per
 * boot. A slow acknowledgement spins at least 50 us, then costs at most one
 * tick per retry and fails by 10 ms plus one tick, retaining the row, its rail
 * pin and the domain. Domain 5 powers off only with the GPU gate and reset
 * closed. The read-only check for the NPU writes nothing. Output: one JSON
 * line of measurements, then PASS.
 *
 * With one argument, the check also runs the GPU controller through one
 * power-on and power-off with the owner and writes every register write to
 * that path, one "kind bank offset mask value" line each (kind 0 a write, 1 a
 * masked update; bank 0 CCU, 2 PCK at 0x07060000), the format that
 * checks/a733-gpu-power-reference.py compares with the pinned PCK functions.
 * The owner's policy write is recorded as an update of bits 3:0 after the
 * check confirms that it preserved the other bits. */
#include "power-domains-model.h"

static FILE *gpu_trace;
/* CORE6 PPU words 0..0x20 and IDR0; a policy write, or the power-up state,
 * leaves a transition pending that completes at the first status read
 * after GPU clock/reset progress. */
static uint32_t gpu_core[0xc14/4]; /* policy..PWCR and the delay row, by word */
static unsigned gpu_core_pending;
#define GPU_CORE_IDR0 UINT32_C(0x00031001)
static uint32_t policy_before;
static void owner_written(uintptr_t a,uint32_t v)
{
    const unsigned offset=(unsigned)(a-UINT32_C(0x07060000));
    if((offset&0xfff)==0) {
        assert((v&~15u)==(policy_before&~15u));policy_before=v;
        fprintf(gpu_trace,"1 2 %x f %x\n",offset,v&15);
    } else fprintf(gpu_trace,"0 2 %x 0 %x\n",offset,v);
}
static int gpu_now(void *v,uint64_t *out){(void)v;return pmic_model_now(&m,out);}
static int gpu_acquire(void *v,struct a733_gpu_power_resources *out)
{(void)v;*out=(struct a733_gpu_power_resources){A733_GPU_INITIAL_HZ,A733_GPU_INITIAL_UV};return 1;}
static int gpu_release(void *v){(void)v;return 1;}
static int gpu_read(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t *out)
{
    (void)v;
    if(bank==A733_GPU_CORE) {
        assert((d.ppu[5][2]&15)==8 && (off==0 || off==8 || off==0x14 || off==0x20 || off==0xfb0 ||
               off==0x170 || off==0x174 || off==0xc00 || off==0xc04 || off==0xc10));
        if(off==8 && gpu_core_pending && (d.ccu_b20&0x80000000) && (d.ccu_b24&0x10001)==0x10001) {
            gpu_core[2]=gpu_core[0];gpu_core_pending=0;
        }
        *out=off==0xfb0 ? GPU_CORE_IDR0 : gpu_core[off/4];return 1;
    }
    uint32_t *w=level_word((bank==A733_GPU_CCU ? CCU_BASE : RCCU_BASE)+off);assert(w);*out=*w;return 1;
}
static int gpu_update(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t clear,uint32_t set)
{
    (void)v;
    if(bank==A733_GPU_CORE) {
        assert(clear==UINT32_MAX && ((off==0 && (set==0 || set==8)) || (off==0x20 && !set) ||
               (off==0x170 && set==0x1f1f1f) || (off==0x174 && set==0x1f1f)
               || (off==0xc00 && set==0x08080808) || (off==0xc04 && set==0x0808) || (off==0xc10 && set==8)));
        gpu_core[off/4]=set;if(!off)gpu_core_pending=1;
        if(gpu_trace)fprintf(gpu_trace,"1 3 %x %x %x\n",off,clear,set);
        return 1;
    }
    assert(bank==A733_GPU_CCU && (off==0xb20 || off==0xb24));
    uint32_t *w=level_word(CCU_BASE+off);*w=(*w&~clear)|set;
    if(off==0xb20)*w&=~(1u<<27); /* the update key reads as zero */
    if(gpu_trace)fprintf(gpu_trace,"1 0 %x %x %x\n",off,clear,set);
    return 1;
}
static void gpu_run(struct a733_gpu_power *g,unsigned on)
{
    assert((on ? a733_gpu_power_on(g,m.now+1000000) : a733_gpu_power_off(g,m.now+1000000))==A733_GPU_POWER_AGAIN);
    enum a733_gpu_power_result r;unsigned n=0;
    while((r=a733_gpu_power_poll(g))==A733_GPU_POWER_AGAIN){assert(++n<10);service_step();}
    assert(r==A733_GPU_POWER_OK && g->phase==(on ? A733_GPU_POWER_ON : A733_GPU_POWER_OFF));
}
static void gpu_trace_run(const char *path)
{
    static const struct a733_gpu_power_ops ops={gpu_now,gpu_acquire,gpu_release,gpu_read,gpu_update};
    static struct a733_gpu_power g;struct a733_rail_token t={0};
    domains_model_setup(0x28,23);d.ack_us=0;memset(&g,0,sizeof(g));
    /* The board's power-up CORE6 state: policy 8, status 0, reset PWCR. */
    memset(gpu_core,0,sizeof(gpu_core));gpu_core[0]=8;gpu_core[8]=A733_GPU_CORE_RESET_PWCR;gpu_core_pending=1;
    assert(a733_gpu_power_init(&g,&ops,0) && a733_power_domain_row_init(&g.row,&d.owner,5,9,&t));
    domains_boot();t=domains_rail(9,A733_RAIL_DCDC4);
    gpu_trace=fopen(path,"w");assert(gpu_trace);policy_before=d.ppu[5][0];d.written=owner_written;
    gpu_run(&g,1);
    assert((d.ppu[5][2]&15)==8 && d.ccu_b20==0x93200100 && d.ccu_b24==0x24690001);
    assert(g.core_policy==8 && !g.core_status && g.core_config==A733_GPU_CORE_RESET_PWCR && g.core_seen==0x1ff && gpu_core[0x170/4]==0x1f1f1f && gpu_core[0xc10/4]==8 &&
           g.core_note[A733_GPU_NOTE_IDR0]==GPU_CORE_IDR0 && gpu_core[2]==8 && !gpu_core[8]);
    gpu_run(&g,0);
    assert((d.ppu[5][2]&15)==0 && d.ccu_b20==0x13200100 && d.ccu_b24==0x24680000 && d.ppu[5][0]==0xa5a50000);
    d.written=0;assert(!fclose(gpu_trace));gpu_trace=0;
}

static unsigned writes_since(unsigned from)
{unsigned n=0;for(unsigned i=from;i<d.trace_n;++i)n+=d.trace[i].write;return n;}
static unsigned transactions(void){return m.writes;} /* TWI register writes */
static enum a733_power_row_state settle(struct a733_power_domain_row *r,unsigned *steps)
{
    unsigned n=0;
    while(a733_power_domain_state(r)==A733_POWER_ROW_PENDING) {
        assert(++n<20000);if(d.wake_at!=UINT64_MAX && d.wake_at>m.now)m.now=d.wake_at;service_step();
    }
    if(steps)*steps=n;
    return a733_power_domain_state(r);
}
static void setup(void){domains_model_setup(0x28,23);}
/* Holds the rail ledger busy from the first time read after the verification
 * request is queued until the provider expires it, so the request cannot be
 * activated: 1 armed, 2 holding, 3 released after the expiry. */
static unsigned hold_ledger;
static void ledger_hook(struct pmic_model *p)
{
    (void)p;
    if(hold_ledger==1 && d.owner.supply_request.id){atomic_store(&m.rails.busy,1);hold_ledger=2;}
    if(hold_ledger!=2)return;
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)if(m.provider.slots[i].state==4) {
        assert(a733_power_authority_load(&m.rails.authority)&A733_POWER_SUPPLY_DONE);
        atomic_store(&m.rails.busy,0);hold_ledger=3;
    }
}
/* Inject elapsed time or one clock failure after a real target-status read.
 * These callbacks retain the ordinary register/clock model everywhere else. */
static uint64_t target_read_finish;
static unsigned target_clock_fault,target_injected,clock_fault_pending;
static int deadline_read(void *v,uintptr_t address,uint32_t *out)
{
    const int result=owner_read(v,address,out);
    if(address==A733_POWER_PCK_DOMAIN(5)+8 && !target_injected) {
        ++target_injected;
        if(target_read_finish)m.now=target_read_finish;
        clock_fault_pending=target_clock_fault;
    }
    return result;
}
static int deadline_clock(void *v,uint64_t *out)
{
    const unsigned fault=clock_fault_pending;clock_fault_pending=0;
    if(fault==1)return 0;
    if(fault==2){*out=0;return 1;}
    return pmic_model_now(v,out);
}
static void transition_deadlines(void)
{
    /* Complete a genuine ON or OFF request just before, at or after its
     * deadline. Later acknowledgements keep the actual row/rail pin. */
    for(unsigned on=0;on<2;++on)for(unsigned mode=0;mode<7;++mode) {
        setup();
        struct a733_rail_token token={0};struct a733_power_domain_row row={0};
        assert(a733_power_domain_row_init(&row,&d.owner,5,76,&token));
        domains_boot();token=domains_rail(76,A733_RAIL_DCDC4);
        if(!on) {
            assert(a733_power_domain_request(&row,1));service_step();
            assert(a733_power_domain_state(&row)==A733_POWER_ROW_ON);
        }
        d.stall[5]=1;
        assert(a733_power_domain_request(&row,on));service_step();
        assert(a733_power_domain_state(&row)==A733_POWER_ROW_PENDING && row.counted);
        const uint64_t deadline=d.owner.domain[2].deadline;
        m.now=mode==1 ? deadline : mode==2 ? deadline+1 : deadline-1;
        m.now_step=0;d.stall[5]=0;d.ack_at[5]=0;
        target_read_finish=mode==3 ? deadline : mode==4 ? deadline+1 : 0;
        target_clock_fault=mode==5 ? 1 : mode==6 ? 2 : 0;
        target_injected=clock_fault_pending=0;
        d.owner.ops.read=deadline_read;m.provider.pmic_bus_ops.now=deadline_clock;
        const unsigned from=d.trace_n,reads=d.ppu_reads;
        service_step();
        assert(!writes_since(from));
        assert(target_injected==(mode==1 || mode==2 ? 0u : 1u));
        assert(d.ppu_reads-reads==target_injected);
        if(!mode) {
            assert(a733_power_domain_state(&row)==(on ? A733_POWER_ROW_ON : A733_POWER_ROW_OFF));
            assert(row.counted==on && d.owner.domain[2].count==on && !d.owner.failed);
        } else {
            assert(a733_power_domain_state(&row)==A733_POWER_ROW_FAILED && row.counted && d.owner.domain[2].count==1);
            assert(a733_power_authority_load(&m.rails.authority)&A733_POWER_DOMAIN5_FAULT);
            assert(a733_rail_release(&m.rails,&token,m.now+1000)==A733_RAIL_BUSY);
            assert(d.owner.failed==(mode>=5));
        }
    }
    /* An earlier session deadline bounds both directions. A session that
     * already expired cannot write a delay row or a power policy. */
    for(unsigned on=0;on<2;++on)for(unsigned expired=0;expired<2;++expired) {
        setup();
        struct a733_rail_token token={0};struct a733_power_domain_row row={0};
        assert(a733_power_domain_row_init(&row,&d.owner,5,77,&token));
        domains_boot();token=domains_rail(77,A733_RAIL_DCDC4);
        if(!on) {
            assert(a733_power_domain_request(&row,1));service_step();
            assert(a733_power_domain_state(&row)==A733_POWER_ROW_ON);
        }
        d.owner.config.deadline=m.now+(expired ? 0 : 1000);
        const unsigned from=d.trace_n;
        d.stall[5]=1;assert(a733_power_domain_request(&row,on));service_step();
        if(!expired) {
            assert(a733_power_domain_state(&row)==A733_POWER_ROW_PENDING);
            assert(d.owner.domain[2].deadline==d.owner.config.deadline);
            m.now=d.owner.config.deadline;m.now_step=0;d.stall[5]=0;d.ack_at[5]=0;
            const unsigned reads=d.ppu_reads;service_step();assert(d.ppu_reads==reads);
        } else assert(!writes_since(from));
        assert(a733_power_domain_state(&row)==A733_POWER_ROW_FAILED && row.counted);
        assert(a733_rail_release(&m.rails,&token,m.now+1000)==A733_RAIL_BUSY);
    }
}
static const unsigned delay_offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
static const uint32_t delay_values[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
static unsigned delay_fault,delay_index,delay_hits;
static int delay_write(void *v,uintptr_t a,uint32_t value)
{
    const int result=owner_write(v,a,value);
    if(delay_fault==1 && a==A733_POWER_PCK_DOMAIN(5)+delay_offsets[delay_index]) {
        ++delay_hits;return 0; /* the write may already have reached the device */
    }
    return result;
}
static int delay_read(void *v,uintptr_t a,uint32_t *out)
{
    const int result=owner_read(v,a,out);
    if(delay_fault>=2 && a==A733_POWER_PCK_DOMAIN(5)+delay_offsets[delay_index]) {
        ++delay_hits;
        if(delay_fault==2)return 0;
        if(delay_fault==3)*out^=1;
        if(delay_fault==4)m.now=d.owner.config.deadline;
        if(delay_fault>=5)clock_fault_pending=delay_fault-4;
    }
    return result;
}
static void delay_trace(unsigned from,unsigned initialized,unsigned on)
{
    unsigned n=0;
    for(unsigned i=from;i<d.trace_n;++i) {
        const struct access *a=&d.trace[i];
        assert(a->address<A733_POWER_PCK_DOMAIN(4) || a->address>=A733_POWER_PCK_DOMAIN(5));
        if(!a->write)continue;
        if(initialized && n<5) {
            assert(a->address==A733_POWER_PCK_DOMAIN(5)+delay_offsets[n] && a->value==delay_values[n]);
            assert(i+1<d.trace_n && !d.trace[i+1].write && d.trace[i+1].address==a->address &&
                   d.trace[i+1].value==a->value);
        } else assert(n==(initialized ? 5u : 0u) && a->address==A733_POWER_PCK_DOMAIN(5) &&
                      a->value==(0xa5a50000u|(on ? 8u : 0u)));
        ++n;
    }
    assert(n==(initialized ? 6u : 1u));
}
static void transition_delays(void)
{
    /* The new flag uses existing padding, preserving every old field offset. */
    _Static_assert(sizeof(struct a733_power_domain)==32 && offsetof(struct a733_power_domain,id)==0 &&
        offsetof(struct a733_power_domain,state)==4 && offsetof(struct a733_power_domain,count)==8 &&
        offsetof(struct a733_power_domain,delays_ready)==12 &&
        offsetof(struct a733_power_domain,command_at)==16 && offsetof(struct a733_power_domain,deadline)==24,
        "power-domain layout is unchanged");
    for(unsigned inherited=0;inherited<2;++inherited) {
        setup();d.ack_us=0;d.ppu[5][0]|=inherited*8;d.ppu[5][2]|=inherited*8;
        d.ppu[4][0]|=8;d.ppu[4][2]|=8;d.ppu[4][0x170/4]=0x12345678;
        uint32_t npu[0x1000/4];memcpy(npu,d.ppu[4],sizeof(npu));
        struct a733_rail_token token={0};struct a733_power_domain_row row={0};
        assert(a733_power_domain_row_init(&row,&d.owner,5,77,&token));
        domains_boot();token=domains_rail(77,A733_RAIL_DCDC4);
        for(unsigned cycle=0;cycle<2;++cycle) {
            unsigned from=d.trace_n;
            assert(a733_power_domain_request(&row,1));service_step();
            assert(a733_power_domain_state(&row)==A733_POWER_ROW_ON && row.counted);
            if(inherited && !cycle)assert(!writes_since(from) && !d.owner.domain[2].delays_ready);
            else delay_trace(from,1,1);
            from=d.trace_n;
            assert(a733_power_domain_request(&row,0));service_step();
            assert(a733_power_domain_state(&row)==A733_POWER_ROW_OFF && !row.counted &&
                   d.owner.domain[2].delays_ready);
            delay_trace(from,inherited && !cycle,0);
        }
        assert(!memcmp(npu,d.ppu[4],sizeof(npu)) && d.owner.domain[1].state==A733_POWER_DOMAIN_UNKNOWN);
        assert(a733_rail_release(&m.rails,&token,m.now+1000)==A733_RAIL_READY);
        printf("delay initialization: inherited %s, two ON/OFF cycles, retained NPU untouched\n",inherited ? "ON" : "OFF");
    }
    /* Fault each first-OFF delay write/read/readback. Also cross the deadline
     * or fail/regress time after the last readback, before the policy write. */
    unsigned faults=0;
    for(unsigned fault=1;fault<=6;++fault)for(unsigned index=0;index<(fault<=3 ? 5u : 1u);++index) {
        setup();d.ack_us=0;d.ppu[5][0]|=8;d.ppu[5][2]|=8;
        struct a733_rail_token token={0};struct a733_power_domain_row row={0};
        assert(a733_power_domain_row_init(&row,&d.owner,5,78,&token));
        domains_boot();token=domains_rail(78,A733_RAIL_DCDC4);
        assert(a733_power_domain_request(&row,1));service_step();
        assert(a733_power_domain_state(&row)==A733_POWER_ROW_ON && !d.owner.domain[2].delays_ready);
        delay_fault=fault;delay_index=fault<=3 ? index : 4;delay_hits=clock_fault_pending=0;
        d.owner.ops.read=delay_read;d.owner.ops.write=delay_write;m.provider.pmic_bus_ops.now=deadline_clock;
        const unsigned from=d.trace_n;
        assert(a733_power_domain_request(&row,0));service_step();
        assert(delay_hits==1 && a733_power_domain_state(&row)==A733_POWER_ROW_FAILED && row.counted);
        assert(d.owner.domain[2].delays_ready==(fault>=4));
        assert(a733_power_authority_load(&m.rails.authority)&A733_POWER_DOMAIN5_FAULT);
        assert(d.ppu[5][0]==0xa5a50008 && d.ppu[5][2]==0x42000008);
        for(unsigned i=from;i<d.trace_n;++i)assert(!d.trace[i].write ||
            (d.trace[i].address>A733_POWER_PCK_DOMAIN(5) && d.trace[i].address<A733_POWER_PCK_DOMAIN(5)+0x1000));
        assert(a733_rail_release(&m.rails,&token,m.now+1000)==A733_RAIL_BUSY);
        delay_fault=0;const unsigned retained=d.trace_n;
        assert(a733_power_domain_request(&row,1));service_step();
        assert(a733_power_domain_state(&row)==A733_POWER_ROW_FAILED && !writes_since(retained));
        ++faults;
    }
    printf("delay initialization: %u injected failures retain row/pin without policy write or revival\n",faults);
}

int main(int argc,char **argv)
{
    assert(argc<=3);
    if(argc==3 && !strcmp(argv[1],"--gpu-trace")){gpu_trace_run(argv[2]);puts("PASS focused GPU TOP/CORE write trace");return 0;}
    if(argc==2 && !strcmp(argv[1],"--delays")) {
        transition_delays();puts("PASS first-transition delay initialization and retained failures");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--deadlines")) {
        transition_deadlines();puts("PASS power-domain transition deadline boundaries and retained pins");return 0;
    }
    if(argc==2)gpu_trace_run(argv[1]);
    /* An image with no row verifies nothing and reads no PMIC register. */
    setup();domains_boot();
    assert(!(a733_power_authority_load(&m.rails.authority)&(A733_POWER_DCDC2_VERIFIED|A733_POWER_DCDC4_VERIFIED)));
    assert(!m.trace_count && d.steps==1);
    /* Boot verification: 12 single-register transactions, both rails verified,
     * and the verification token released afterwards. */
    setup();
    struct a733_rail_token boot_token={0};struct a733_power_domain_row boot_row={0};
    assert(a733_power_domain_row_init(&boot_row,&d.owner,1,59,&boot_token));
    domains_boot();service_step();
    assert(d.owner.supply_stage==6 && a733_rail_status(&m.rails,&d.owner.supply_token)==A733_RAIL_INVALID);
    const unsigned w=a733_power_authority_load(&m.rails.authority);
    assert((w&(A733_POWER_SUPPLY_DONE|A733_POWER_DCDC2_VERIFIED|A733_POWER_DCDC4_VERIFIED))==
        (A733_POWER_SUPPLY_DONE|A733_POWER_DCDC2_VERIFIED|A733_POWER_DCDC4_VERIFIED));
    assert(d.owner.dcdc4.programmed_uv==800000 && (d.owner.dcdc4.selector&127)==30);
    unsigned starts=0;
    for(unsigned i=0;i<m.trace_count;++i)starts+=m.trace[i].address==0x0708300c && m.trace[i].value==0x68;
    const unsigned boot_transactions=starts/2; /* START and repeated START per read */
    assert(boot_transactions==12);
    assert(d.ppu_writes==0 && d.ppu_reads==0);

    /* Domain 1 and 5 power-on from OFF: delay row, ON command, acknowledged
     * within the spin; the row reads ON after the step, with no PMIC traffic. */
    unsigned ppu_on=0,level_on=0;
    const unsigned ids[]={1,5};
    for(unsigned k=0;k<2;++k) {
        setup();d.ack_us=0; /* acknowledged by the first status read */
        struct a733_rail_token t={0};struct a733_power_domain_row row={0};
        assert(a733_power_domain_row_init(&row,&d.owner,ids[k],60,&t));
        domains_boot();t=domains_rail(60,ids[k]==5 ? A733_RAIL_DCDC4 : A733_RAIL_DCDC2);
        const unsigned twi=transactions(),from=d.trace_n,reads=d.ppu_reads,level=d.level_reads;
        assert(a733_power_domain_request(&row,1) && d.notifies==1);
        assert(!a733_power_domain_request(&row,1)); /* unanswered */
        assert(a733_power_domain_state(&row)==A733_POWER_ROW_PENDING);
        service_step();
        assert(a733_power_domain_state(&row)==A733_POWER_ROW_ON && transactions()==twi);
        const uintptr_t base=A733_POWER_PCK_DOMAIN(ids[k]);
        static const unsigned offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
        static const uint32_t values[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
        unsigned n=0;
        for(unsigned i=from;i<d.trace_n;++i)if(d.trace[i].write) {
            if(n<5)assert(d.trace[i].address==base+offsets[n] && d.trace[i].value==values[n]);
            else assert(n==5 && d.trace[i].address==base && d.trace[i].value==0xa5a50008);
            ++n;
        }
        assert(n==6);
        ppu_on=d.ppu_reads-reads+n;level_on=d.level_reads-level;
        /* Pinned: the token cannot be released while the row is counted. */
        assert(a733_rail_release(&m.rails,&t,m.now+1000)==A733_RAIL_BUSY);
        if(ids[k]==1)assert(!a733_power_domain_request(&row,0));
    }
    assert(ppu_on==14);

    /* Adoption of an inherited ON domain writes nothing. */
    setup();d.ppu[1][0]|=8;d.ppu[1][2]|=8;
    struct a733_rail_token t2={0};struct a733_power_domain_row a={0};
    assert(a733_power_domain_row_init(&a,&d.owner,1,61,&t2));
    domains_boot();t2=domains_rail(61,A733_RAIL_DCDC2);
    unsigned from=d.trace_n;assert(a733_power_domain_request(&a,1));service_step();
    assert(a733_power_domain_state(&a)==A733_POWER_ROW_ON && !writes_since(from));

    /* Incoherent adoption: refused with no write, fault bit set, later rows
     * on the domain refused, other domains unaffected. */
    setup();d.ppu[1][0]|=8;
    struct a733_power_domain_row b={0},b2={0};struct a733_rail_token t3={0};
    assert(a733_power_domain_row_init(&b,&d.owner,1,62,&t3) && a733_power_domain_row_init(&b2,&d.owner,1,62,&t3));
    domains_boot();t3=domains_rail(62,A733_RAIL_DCDC2);
    from=d.trace_n;assert(a733_power_domain_request(&b,1));service_step();
    assert(a733_power_domain_state(&b)==A733_POWER_ROW_REFUSED && !writes_since(from));
    assert(a733_power_authority_load(&m.rails.authority)&A733_POWER_DOMAIN1_FAULT);
    assert(a733_power_domain_request(&b2,1));service_step();assert(a733_power_domain_state(&b2)==A733_POWER_ROW_REFUSED);
    assert(a733_rail_release(&m.rails,&t3,m.now+1000)==A733_RAIL_READY);

    /* Domain-level refusals: no PPU write, nothing retained. */
    for(unsigned c=0;c<5;++c) {
        setup();
        uint32_t *words[]={&d.ccu_5c0,&d.ccu_5e0,&d.rccu_1ac,&d.ccu_b20,&d.ccu_b24};
        const uint32_t flip[]={1u<<7,1u<<16,1,1u<<31,1u<<16};
        *words[c]^=flip[c];
        struct a733_rail_token t={0};struct a733_power_domain_row r={0};
        assert(a733_power_domain_row_init(&r,&d.owner,5,63,&t));
        domains_boot();t=domains_rail(63,A733_RAIL_DCDC4);
        from=d.trace_n;assert(a733_power_domain_request(&r,1));service_step();
        assert(a733_power_domain_state(&r)==A733_POWER_ROW_REFUSED && !writes_since(from));
        assert(a733_rail_release(&m.rails,&t,m.now+1000)==A733_RAIL_READY);
    }
    /* A token of another owner, or without the domain's supply, is refused. */
    setup();
    struct a733_rail_token wrong={0};struct a733_power_domain_row r5={0};
    assert(a733_power_domain_row_init(&r5,&d.owner,5,64,&wrong));
    domains_boot();wrong=domains_rail(64,A733_RAIL_DCDC2);
    assert(a733_power_domain_request(&r5,1));service_step();assert(a733_power_domain_state(&r5)==A733_POWER_ROW_REFUSED);

    /* A DCDC4 selector other than 800 mV leaves its verified bit clear: the
     * GPU row is refused, domain 1 still powers on. */
    setup();m.pmic[0x15]=31;
    struct a733_rail_token g4={0},g2={0};struct a733_power_domain_row gpu={0},de={0};
    assert(a733_power_domain_row_init(&gpu,&d.owner,5,65,&g4) && a733_power_domain_row_init(&de,&d.owner,1,66,&g2));
    domains_boot();
    assert(!(a733_power_authority_load(&m.rails.authority)&A733_POWER_DCDC4_VERIFIED));
    g4=domains_rail(65,A733_RAIL_DCDC4);g2=domains_rail(66,A733_RAIL_DCDC2);
    assert(a733_power_domain_request(&gpu,1) && a733_power_domain_request(&de,1));service_step();
    assert(a733_power_domain_state(&gpu)==A733_POWER_ROW_REFUSED && a733_power_domain_state(&de)==A733_POWER_ROW_ON);

    /* A row requested before verification waits for it, then is admitted. */
    setup();
    struct a733_rail_token e4=domains_rail(67,A733_RAIL_DCDC4);struct a733_power_domain_row early={0};
    assert(a733_power_domain_row_init(&early,&d.owner,5,67,&e4) && a733_power_domain_request(&early,1));
    unsigned steps=0;assert(settle(&early,&steps)==A733_POWER_ROW_ON);
    assert(d.owner.verify_at && m.now>=d.owner.verify_at);

    /* A provider fault ends verification with no verified bit; rows are
     * refused within the bound instead of waiting forever. */
    setup();
    struct a733_rail_token f4=domains_rail(68,A733_RAIL_DCDC4);struct a733_power_domain_row fr={0};
    assert(a733_power_domain_row_init(&fr,&d.owner,5,68,&f4) && a733_power_domain_request(&fr,1));
    a733_power_authority_set(&m.rails.authority,A733_POWER_LEDGER_FAULT);
    assert(settle(&fr,0)==A733_POWER_ROW_REFUSED);

    /* The verification budget starts when the provider is READY: startup
     * held off for 200 ms by another holder of the clock registry still
     * verifies both rails. */
    setup();
    struct a733_rail_token l4=domains_rail(73,A733_RAIL_DCDC4);struct a733_power_domain_row late_start={0};
    assert(a733_power_domain_row_init(&late_start,&d.owner,5,73,&l4));
    atomic_store(&m.clocks.busy,1);const uint64_t held_from=m.now;
    while(m.now-held_from<200000){service_step();m.now+=TICK_US;}
    assert(!(a733_power_authority_load(&m.rails.authority)&(A733_POWER_PROVIDER_READY|A733_POWER_SUPPLY_DONE)));
    atomic_store(&m.clocks.busy,0);domains_boot();
    assert((a733_power_authority_load(&m.rails.authority)&(A733_POWER_DCDC2_VERIFIED|A733_POWER_DCDC4_VERIFIED))==
        (A733_POWER_DCDC2_VERIFIED|A733_POWER_DCDC4_VERIFIED));

    /* A verification request that expires in the queue, its activation held
     * off by a busy rail ledger, ends verification with no verified bit at
     * once; the expired result is reaped and the token released, so no slot
     * or ledger holder stays behind. */
    setup();
    struct a733_rail_token x4=domains_rail(74,A733_RAIL_DCDC4);struct a733_power_domain_row expired={0};
    assert(a733_power_domain_row_init(&expired,&d.owner,5,74,&x4));
    m.concurrent=ledger_hook;hold_ledger=1;
    for(unsigned i=0;i<1000 && d.owner.supply_stage!=6;++i){service_step();m.now+=TICK_US;}
    m.concurrent=0;assert(hold_ledger==3);
    const unsigned xw=a733_power_authority_load(&m.rails.authority);
    assert((xw&A733_POWER_SUPPLY_DONE) && !(xw&(A733_POWER_DCDC2_VERIFIED|A733_POWER_DCDC4_VERIFIED|A733_POWER_PMIC_TRANSFER)));
    assert(d.owner.supply_stage==6 && !d.owner.supply_request.id);
    assert(a733_rail_status(&m.rails,&d.owner.supply_token)==A733_RAIL_INVALID);
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)assert(m.provider.slots[i].state==0);
    assert(a733_power_domain_request(&expired,1));service_step();
    assert(a733_power_domain_state(&expired)==A733_POWER_ROW_REFUSED);

    /* Slow acknowledgement: the command step spins at least 50 us, each later
     * step reads status once and returns one tick, and the domain fails by
     * 10 ms plus one tick, retaining the row, its pin and the fault bit. */
    setup();d.stall[5]=1;
    struct a733_rail_token s4={0};struct a733_power_domain_row slow={0};
    assert(a733_power_domain_row_init(&slow,&d.owner,5,69,&s4));
    domains_boot();s4=domains_rail(69,A733_RAIL_DCDC4);assert(a733_power_domain_request(&slow,1));
    uint64_t before=m.now;assert(service_step()==1);
    const uint64_t spin=m.now-before;assert(spin>=A733_POWER_PCK_SPIN_US);
    const uint64_t commanded=d.owner.domain[2].command_at;
    unsigned retries=0,reads=d.ppu_reads;
    while(a733_power_domain_state(&slow)==A733_POWER_ROW_PENDING) {
        m.now=d.wake_at;const uint32_t ticks=service_step();++retries;assert(retries<20);
        assert(ticks==1 || a733_power_domain_state(&slow)!=A733_POWER_ROW_PENDING);
    }
    const uint64_t failed_after=m.now-commanded;
    assert(a733_power_domain_state(&slow)==A733_POWER_ROW_FAILED);
    assert(failed_after<=A733_POWER_PCK_BOUND_US+TICK_US+100 && d.ppu_reads-reads==retries-1);
    assert(a733_power_authority_load(&m.rails.authority)&A733_POWER_DOMAIN5_FAULT);
    assert(a733_rail_release(&m.rails,&s4,m.now+1000)==A733_RAIL_BUSY);
    /* A later acknowledgement does not revive the failed domain. */
    d.stall[5]=0;assert(a733_power_domain_request(&slow,1));service_step();
    assert(a733_power_domain_state(&slow)==A733_POWER_ROW_FAILED);

    /* An unexpected status during the transition fails the domain at once;
     * at adoption it refuses without a write. */
    setup();d.stall[5]=1;
    struct a733_rail_token u4={0};struct a733_power_domain_row un={0};
    assert(a733_power_domain_row_init(&un,&d.owner,5,70,&u4));
    domains_boot();u4=domains_rail(70,A733_RAIL_DCDC4);assert(a733_power_domain_request(&un,1));
    service_step();assert(a733_power_domain_state(&un)==A733_POWER_ROW_PENDING);
    d.status_override[5]=4;m.now=d.wake_at;service_step();
    assert(a733_power_domain_state(&un)==A733_POWER_ROW_FAILED);
    setup();d.status_override[5]=4;
    struct a733_rail_token v4={0};struct a733_power_domain_row ua={0};
    assert(a733_power_domain_row_init(&ua,&d.owner,5,70,&v4));
    domains_boot();v4=domains_rail(70,A733_RAIL_DCDC4);assert(a733_power_domain_request(&ua,1));
    from=d.trace_n;service_step();assert(a733_power_domain_state(&ua)==A733_POWER_ROW_REFUSED && !writes_since(from));

    /* Domain 5 power-off: refused while the GPU reset is deasserted (retained),
     * otherwise the domain reaches OFF and the pin is dropped. */
    for(unsigned open=0;open<2;++open) {
        setup();
        struct a733_rail_token o4={0};struct a733_power_domain_row off={0};
        assert(a733_power_domain_row_init(&off,&d.owner,5,71,&o4));
        domains_boot();o4=domains_rail(71,A733_RAIL_DCDC4);assert(a733_power_domain_request(&off,1));
        service_step();assert(a733_power_domain_state(&off)==A733_POWER_ROW_ON);
        if(open)d.ccu_b24|=1u<<16;
        from=d.trace_n;assert(a733_power_domain_request(&off,0));service_step();
        if(open) {
            assert(a733_power_domain_state(&off)==A733_POWER_ROW_FAILED && !writes_since(from));
            assert(a733_rail_release(&m.rails,&o4,m.now+1000)==A733_RAIL_BUSY);
        } else {
            assert(a733_power_domain_state(&off)==A733_POWER_ROW_OFF && writes_since(from)==1);
            assert((d.ppu[5][0]&15)==0 && (d.ppu[5][2]&15)==0 && d.ppu[5][0]==0xa5a50000);
            assert(a733_rail_release(&m.rails,&o4,m.now+1000)==A733_RAIL_READY);
            /* Reopen: the same row powers on again. */
            o4=domains_rail(71,A733_RAIL_DCDC4);
            assert(a733_power_domain_request(&off,1));service_step();
            assert(a733_power_domain_state(&off)==A733_POWER_ROW_ON);
        }
    }
    /* A failed unpin after power-off leaves the row counted with the domain
     * OFF; its next ON request is answered FAILED instead of left pending. */
    setup();
    struct a733_rail_token k4={0};struct a733_power_domain_row kept={0};
    assert(a733_power_domain_row_init(&kept,&d.owner,5,75,&k4));
    domains_boot();k4=domains_rail(75,A733_RAIL_DCDC4);assert(a733_power_domain_request(&kept,1));
    service_step();assert(a733_power_domain_state(&kept)==A733_POWER_ROW_ON);
    a733_power_authority_set(&m.rails.authority,A733_POWER_LEDGER_FAULT);
    assert(a733_power_domain_request(&kept,0));service_step();
    assert(a733_power_domain_state(&kept)==A733_POWER_ROW_FAILED && (d.ppu[5][2]&15)==0 && kept.counted);
    assert(a733_power_domain_request(&kept,1));service_step();
    assert(a733_power_domain_state(&kept)==A733_POWER_ROW_FAILED);

    /* The NPU interface: a read-only check, then a domain-4 row. */
    setup();
    struct a733_rail_token n2={0};struct a733_power_domain_row npu={0};
    assert(a733_power_domain_row_init(&npu,&d.owner,4,72,&n2));
    domains_boot();
    unsigned seen=99;from=d.trace_n;assert(a733_power_domain_check(&d.owner,4,&seen)==1 && seen==0 && !writes_since(from));
    d.ccu_5e0&=~(1u<<18);assert(a733_power_domain_check(&d.owner,4,NULL)==0);d.ccu_5e0|=1u<<18;
    d.ppu[4][2]|=8;assert(a733_power_domain_check(&d.owner,4,NULL)==0);d.ppu[4][2]&=~8u;
    assert(!writes_since(from) && a733_power_domain_check(&d.owner,2,NULL)==0);
    n2=domains_rail(72,A733_RAIL_DCDC2);assert(a733_power_domain_request(&npu,1));
    service_step();assert(a733_power_domain_state(&npu)==A733_POWER_ROW_ON && (d.ppu[4][0]&15)==8);
    assert(!a733_power_domain_request(&npu,0));
    /* Registration closes once the service runs. */
    struct a733_power_domain_row late={0};assert(!a733_power_domain_row_init(&late,&d.owner,4,72,&n2));

    transition_deadlines();

    printf("{\"boot_transactions\":%u,\"row_transactions\":0,\"ppu_accesses_per_on\":%u,"
        "\"level_reads_per_admission\":%u,\"slow_ack\":{\"spin_us\":%llu,\"retries\":%u,\"failed_after_us\":%llu}}\n",
        boot_transactions,ppu_on,level_on,(unsigned long long)spin,retries,(unsigned long long)failed_after);
    puts("PASS power-domain owner: delay row and ON command, adoption, incoherence and domain-level refusals, "
         "verified supplies, bounded slow acknowledgement with retention, domain-5 power-off, NPU check");
    return 0;
}
