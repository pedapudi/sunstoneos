/* SPDX-License-Identifier: Apache-2.0 */
/* CPU cluster clock and voltage transitions through the actual PMIC provider,
 * rail ledger, clock registry and thermal policy. CPU PLL, mux and SID words
 * and the policy's samples are modeled; the rail is modeled as ramping at the
 * board's 250 uV/us. */
#define PMIC_MODEL_NO_MAIN 1
#include "a733-pmic.c"
#include "a733_cpu_clock.h"
#define PLL(d) (UINT32_C(0x08871000)+UINT32_C(0x1000)*(d))
#define SEL(d) (PLL(d)+0x1c)
#define LOCKED UINT32_C(0xf8000000)
#define L0(n) (UINT32_C(0xf8800000)|((uint32_t)(n)<<8)) /* Recorded CPU-L word with N. */
#define REG_DCDC3 0x14
#define REG_DCDC5 0x16
struct event { uint64_t at; unsigned pmic; uint32_t address, value; };
struct cpu_model {
    unsigned bin, stuck_update, stuck_lock, ignore_pmic, reads, writes, count, now_cost, now_reads;
    uint64_t update_at[3], lock_at[3], lock_us; /* lock_us 0: lock 20 us after the update. */
    uint64_t park_at[2]; /* Time of each cluster's latest park write. */
    struct { uint32_t from_uv, to_uv; uint64_t at; } ramp[2]; /* DCDC5, DCDC3. */
    struct event events[64];
};
static struct pmic_model pm;
static struct cpu_model cm;
static struct a733_cpu_clock cpu;
static struct a733_thermal_policy policy;
static uint32_t sequence;
static int notify(void *v,enum a733_thermal_level level){(void)v;(void)level;return 1;}
/* One valid sample of all five channels: CPUL at the given temperature, the
 * others at 50 C. 95 C throttles, 115 C is critical. */
static enum a733_thermal_level heat(int32_t cpul)
{
    struct a733_thermal_sample s={.observed_us=pm.now,.sequence=++sequence,.valid=31};
    for(unsigned i=0;i<A733_THERMAL_SENSORS;++i)s.millicelsius[i]=i==3 ? cpul : 50000;
    return a733_thermal_policy_update(&policy,pm.now,A733_THERMAL_SAMPLE,&s);
}
static uint32_t *word(uintptr_t a){return pmic_model_word(&pm,a);}
static uint32_t selector_uv(uint8_t s){s&=127;return s<=70 ? 500000u+10000u*s : 1200000u+20000u*(s-70u);}
static uint32_t effective_uv(unsigned r)
{
    uint64_t dt=pm.now-cm.ramp[r].at;uint32_t from=cm.ramp[r].from_uv,to=cm.ramp[r].to_uv;
    if(to<=from)return to; /* A falling rail is assumed at its target at once. */
    return dt*250u>=to-from ? to : from+(uint32_t)(dt*250u);
}
static uint64_t rate(unsigned d)
{
    uint32_t pll=*word(PLL(d)),sel=*word(SEL(d));unsigned source=(sel>>24)&7;
    if(source<=2)return 26000000u;
    assert(source==3 && (pll&LOCKED)==LOCKED && !(pll&(1u<<26)));
    return (26000000ull*((pll>>8)&255))>>((sel>>16)&3);
}
/* Lowest table voltage for the bin at or above a rate, as the rail must supply. */
static uint32_t need_uv(unsigned d,uint64_t hz)
{
    unsigned count;const struct a733_cpu_opp *t=a733_cpu_clock_opps(d,&count);
    for(unsigned i=0;i<count;++i){uint32_t mv=a733_cpu_clock_opp_mv(&t[i],cm.bin);
        if(mv && t[i].mhz*1000000ull>=hz)return mv*1000u;}
    assert(0);return 0;
}
/* Every applied rate must be covered by the rail voltage at that instant. */
static void covered(void)
{
    assert(effective_uv(0)>=need_uv(A733_CPU_LITTLE,rate(A733_CPU_LITTLE)));
    assert(effective_uv(0)>=need_uv(A733_CPU_DSU,rate(A733_CPU_DSU)));
    assert(effective_uv(1)>=need_uv(A733_CPU_BIG,rate(A733_CPU_BIG)));
}
static void record(unsigned pmic,uint32_t a,uint32_t v)
{assert(cm.count<64);cm.events[cm.count++]=(struct event){pm.now,pmic,a,v};}
static void pmic_written(void *v,uint8_t reg,uint8_t value)
{
    (void)v;assert(reg==REG_DCDC5 || reg==REG_DCDC3);
    unsigned r=reg==REG_DCDC3;uint32_t old=effective_uv(r);
    if(cm.ignore_pmic){pm.pmic[reg]=(uint8_t)cm.ignore_pmic;return;}
    cm.ramp[r].from_uv=old;cm.ramp[r].to_uv=selector_uv(value);cm.ramp[r].at=pm.now;
    record(1,reg,value);covered();
}
/* Every fourth CPU clock time read advances time by now_cost microseconds,
 * so the driver's settle spin sees time advance and a settle shorter than
 * 2 us cannot pass on the cost of its own time reads. */
static int cpu_now(void *v,uint64_t *out)
{(void)v;int r=pmic_model_now(&pm,out);if(!(++cm.now_reads&3))pm.now+=cm.now_cost;return r;}
static void advance(unsigned d)
{
    uint32_t *pll=word(PLL(d));
    if((*pll&(1u<<26)) && !cm.stuck_update && pm.now>=cm.update_at[d])*pll&=~(1u<<26);
    if(!(*pll&(1u<<28)) && !(*pll&(1u<<26)) && !cm.stuck_lock && pm.now>=cm.lock_at[d])*pll|=1u<<28;
}
static int cpu_read(void *v,uintptr_t a,uint32_t *out)
{
    (void)v;assert(a733_cpu_clock_address(a,0));++cm.reads;
    for(unsigned d=0;d<3;++d)if(a==PLL(d))advance(d);
    *out=*word(a);return 1;
}
static int cpu_write(void *v,uintptr_t a,uint32_t value)
{
    (void)v;assert(a733_cpu_clock_address(a,1));++cm.writes;record(0,(uint32_t)a,value);
    for(unsigned d=0;d<2;++d) {
        if(a==PLL(d)) {
            /* The PLL changes only while its cluster runs from the DCXO, and
             * at least the driver's 2 us settle after the park (twice the
             * mainline mux notifier delay). */
            assert(((*word(SEL(d))>>24)&7)==0);
            assert(pm.now>=cm.park_at[d]+2);
            *word(a)=value&~(1u<<28);
            if(value&(1u<<26)){cm.update_at[d]=pm.now+10;cm.lock_at[d]=pm.now+(cm.lock_us ? cm.lock_us : 20);}
        }
        if(a==SEL(d)){if(!((value>>24)&7))cm.park_at[d]=pm.now;*word(a)=value;covered();}
    }
    return 1;
}
static const struct a733_cpu_clock_ops ops={cpu_now,cpu_read,cpu_write};
/* Fresh platform: 26 MHz DCXO with APBS1 on the 24 MHz PLL_REF, CPU-L at the
 * recorded 494 MHz, CPU-B at 1014 MHz and the DSU at a chosen rate. */
static void setup(uint32_t sid,unsigned bin,uint8_t dcdc5,uint8_t dcdc3,unsigned dsu_n,
    const struct a733_cpu_clock_grant *grant)
{
    pmic_model_setup(&pm);
    *word(0x07090160)=0x10008002;*word(0x07010010)=0x04000000;
    *word(0x02002000)=LOCKED|(95u<<8)|(103u<<16);
    pm.pmic[REG_DCDC5]=dcdc5;pm.pmic[REG_DCDC3]=dcdc3;pm.pmic_written=pmic_written;
    memset(&cm,0,sizeof(cm));cm.bin=bin;cm.now_cost=1;
    cm.ramp[0].from_uv=cm.ramp[0].to_uv=selector_uv(dcdc5);
    cm.ramp[1].from_uv=cm.ramp[1].to_uv=selector_uv(dcdc3);
    *word(0x0300624c)=sid;
    *word(PLL(0))=0xf8802600;*word(SEL(0))=0x03010000;
    *word(PLL(1))=LOCKED|(39u<<8);*word(SEL(1))=0x03000000;
    *word(PLL(2))=LOCKED|(dsu_n<<8);*word(SEL(2))=0x03000000;
    for(unsigned i=0;i<16 && a733_pmic_live(&pm.provider,&pm.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&pm.provider,&pm.lifetime)==A733_PMIC_READY);
    memset(&cpu,0,sizeof(cpu));memset(&policy,0,sizeof(policy));sequence=0;
    const struct a733_cpu_clock_config config={4242,grant ? *grant : (struct a733_cpu_clock_grant){0}};
    assert(!a733_cpu_clock_init(&cpu,&config,&ops,&cm,&pm.clocks,&pm.rails,&pm.provider,&policy) && !cpu.self);
    assert(a733_thermal_policy_init(&policy,pm.now,notify,0));
    assert(a733_cpu_clock_init(&cpu,&config,&ops,&cm,&pm.clocks,&pm.rails,&pm.provider,&policy));
    cm.reads=cm.writes=0;
}
/* Owner task and PMIC task each poll once per 50 us tick. */
static enum a733_cpu_clock_status run(unsigned limit)
{
    for(unsigned i=0;i<limit;++i) {
        assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED || cpu.failed || pm.provider.failed);
        enum a733_cpu_clock_status r=a733_cpu_clock_poll(&cpu);
        pm.now+=50;
        if(r!=A733_CPU_CLOCK_PENDING)return r;
    }
    return A733_CPU_CLOCK_PENDING;
}
/* The build wraps a733_rail_floor: with floor_busy set, the next call returns
 * BUSY as if another core held the ledger, and leaves the request it follows
 * outstanding. */
static unsigned floor_busy;
enum a733_rail_result __real_a733_rail_floor(struct a733_rail_leases *,const struct a733_rail_token *,
    enum a733_rail,uint32_t,uint32_t *);
enum a733_rail_result __wrap_a733_rail_floor(struct a733_rail_leases *m,const struct a733_rail_token *t,
    enum a733_rail rail,uint32_t observed_uv,uint32_t *floor_uv)
{
    if(floor_busy){floor_busy=0;assert(cpu.request.id);return A733_RAIL_BUSY;}
    return __real_a733_rail_floor(m,t,rail,observed_uv,floor_uv);
}
static int ledger_failed(void)
{return (atomic_load(&pm.rails.authority.word)&A733_POWER_LEDGER_FAULT)!=0;}
static int released(void)
{
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)if(pm.clocks.slots[i].owner==4242)return 0;
    for(unsigned i=0;i<A733_RAIL_CLIENTS;++i)if(pm.rails.slots[i].id)return 0;
    return !cpu.clock.id && !cpu.rail.id && !atomic_load(&pm.clocks.failed) && !ledger_failed();
}
static unsigned first(unsigned pmic)
{for(unsigned i=0;i<cm.count;++i)if(cm.events[i].pmic==pmic)return i;return cm.count;}
static void retained(void)
{
    assert(cpu.failed && atomic_load(&pm.clocks.failed) && ledger_failed() && cpu.clock.id && cpu.rail.id);
    unsigned writes=cm.writes,count=cm.count;
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_RETAINED && cm.writes==writes && cm.count==count);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,416,pm.now+1000000)==A733_CPU_CLOCK_RETAINED);
}
static const struct a733_cpu_clock_grant both={77,3,{1404,1508},8};
int main(void)
{
    /* Read-only report of the recorded configuration: 26 MHz x 38 / 2. */
    setup(0x00000100,1,30,30,30,0);
    struct a733_cpu_clock_report report;
    assert(a733_cpu_clock_report(cpu_read,&cm,&report) && report.dcxo_hz==26000000 &&
        report.binned && report.bin==1 && report.domain[0].hz==494000000 &&
        report.domain[1].hz==1014000000 && report.domain[2].hz==780000000 && !cm.writes);
    struct a733_cpu_clock_domain d;
    a733_cpu_clock_decode(26000000,0xf8802600,0x03030000,&d);assert(d.decoded && d.hz==123500000);
    a733_cpu_clock_decode(26000000,0xf8802600,0x04000000,&d);assert(!d.decoded);
    a733_cpu_clock_decode(26000000,0x78802600,0x03000000,&d);assert(!d.decoded);
    /* Bin selection: dvfs_bak overrides dvfs_ori; codes without a column fail. */
    unsigned bin;
    assert(a733_cpu_clock_bin(0x00150400,&bin) && bin==8 && a733_cpu_clock_bin(0x00000600,&bin) && bin==6);
    assert(a733_cpu_clock_bin(0,&bin) && bin==A733_CPU_VF_ENVELOPE);
    assert(!a733_cpu_clock_bin(0x1100,&bin) && !a733_cpu_clock_bin(0x120000,&bin) && !a733_cpu_clock_bin(0x0700,&bin));
    unsigned count;const struct a733_cpu_opp *t=a733_cpu_clock_opps(A733_CPU_LITTLE,&count);
    assert(a733_cpu_clock_opp_mv(&t[5],A733_CPU_VF_ENVELOPE)==850 && !a733_cpu_clock_opp_mv(&t[12],A733_CPU_VF_ENVELOPE));
    /* Access policy: the DSU and every other word are never written. */
    assert(!a733_cpu_clock_address(SEL(2),1) && !a733_cpu_clock_address(PLL(2),1) &&
        a733_cpu_clock_address(SEL(2),0) && !a733_cpu_clock_address(0x08870000,0) &&
        !a733_cpu_clock_address(0x0300624c,1) && !a733_cpu_clock_address(0x07090160,1));

    /* No grant, no CPUL reading, ungranted cluster or point: refused, no access.
     * The policy is NORMAL before its first reading. A CPUB reading alone does
     * not admit the little cluster. */
    assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    const struct a733_cpu_clock_grant little={77,1,{1196,0},2};
    setup(0x00000100,1,30,30,30,&little);
    assert(a733_thermal_policy_level(&policy)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    struct a733_thermal_sample cpub={.observed_us=pm.now,.sequence=++sequence,.valid=1,.millicelsius={50000}};
    assert(a733_thermal_policy_update(&policy,pm.now,A733_THERMAL_SAMPLE,&cpub)==A733_THERMAL_NORMAL);
    assert(a733_thermal_policy_seen(&policy)==1);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    assert(heat(50000)==A733_THERMAL_NORMAL && a733_thermal_policy_seen(&policy)==31);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_BIG,1014,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1404,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1100,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_DSU,780,pm.now+1000000)==A733_CPU_CLOCK_INVALID);
    assert(!cm.reads && !cm.writes && !cm.count);

    /* Raise, vf0100: DCDC5 800 -> 850 mV, a 200 us ramp, then 494 -> 1196 MHz. */
    pm.pmic[REG_DCDC5]=0x80|30;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1014,pm.now+1000000)==A733_CPU_CLOCK_BUSY);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released());
    assert(pm.pmic[REG_DCDC5]==(0x80|35) && *word(PLL(0))==L0(46) && *word(SEL(0))==0x03000000);
    unsigned v=first(1),f=first(0);
    assert(v<f && cm.events[f].at-cm.events[v].at>=200 && cm.events[v].address==REG_DCDC5);
    assert(cm.events[f].address==SEL(0) && cm.events[f].value==0x00010000);
    assert(cm.events[cm.count-1].address==SEL(0) && cm.events[cm.count-1].value==0x03000000);
    assert(cpu.applied_mhz[0]==1196 && cpu.changes==1);
    /* Lower to 1014 MHz: frequency first, then DCDC5 back to 800 mV. */
    cm.count=0;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1014,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && !cpu.changes);
    assert(first(0)<first(1) && pm.pmic[REG_DCDC5]==(0x80|30) && *word(PLL(0))==L0(39));
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,416,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);

    /* Enough inherited voltage: no PMIC write. 416 MHz uses N=32, P=/2. */
    setup(0x00000200,2,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && first(1)==cm.count);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,416,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && *word(PLL(0))==L0(32) && *word(SEL(0))==0x03010000);
    /* The big cluster uses DCDC3 only: vf0200 1508 MHz needs 800 mV. vf0100 needs 900. */
    setup(0x00000100,1,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_BIG,1508,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released());
    assert(pm.pmic[REG_DCDC3]==40 && pm.pmic[REG_DCDC5]==30 && *word(PLL(1))==(LOCKED|(58u<<8)));
    assert(cm.events[first(1)].address==REG_DCDC3 && first(1)<first(0));
    assert(cm.events[first(0)].at-cm.events[first(1)].at>=400); /* 100 mV at 250 uV/us. */

    /* DCDC5 also covers the DSU, whose rate the driver never changes, so a
     * valid boot voltage already covers it and the boot baseline below keeps
     * covering it. */
    /* The boot baseline is a floor: with DCDC5 inherited at 950 mV, lowering
     * to a point that needs 900 mV keeps 950 mV. The ledger keeps the floor
     * after the driver releases its rail token. */
    setup(0x00000100,1,45,30,38,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1014,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && first(1)==cm.count && pm.rails.floor_uv[1]==950000);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,416,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && pm.pmic[REG_DCDC5]==45 && first(1)==cm.count && released());
    assert(pm.rails.floor_uv[1]==950000 && !pm.rails.floor_uv[0]);
    /* Another core holding the ledger during the floor call leaves the read
     * outstanding; the next poll repeats both steps and the transition
     * completes with no fault. */
    setup(0x00000100,1,45,30,38,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    floor_busy=1;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1014,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && !floor_busy && !cpu.failed && !ledger_failed());
    assert(pm.rails.floor_uv[1]==950000 && released());
    /* Code 0 uses the envelope: 1196 MHz needs 850 mV even if the chip is vf0000. */
    setup(0,A733_CPU_VF_ENVELOPE,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && pm.pmic[REG_DCDC5]==35);

    /* THROTTLE: refuse points above 1014 MHz and lower a raised cluster to it
     * without a change or a voltage write. CRITICAL is terminal. */
    setup(0x00000200,2,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1404,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && pm.pmic[REG_DCDC5]==35);
    assert(heat(95000)==A733_THERMAL_THROTTLE);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    unsigned changes=cpu.changes;cm.count=0;
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && cpu.applied_mhz[0]==1014);
    assert(cpu.changes==changes && first(1)==cm.count && pm.pmic[REG_DCDC5]==35);
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY);
    /* NORMAL restores the earlier point once, consuming a change. The kept
     * voltage covers it, so no PMIC write occurs. */
    assert(heat(87000)==A733_THERMAL_NORMAL);cm.count=0;
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && cpu.applied_mhz[0]==1404);
    assert(cpu.changes==changes-1 && first(1)==cm.count && *word(PLL(0))==L0(54));
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY);
    /* A stale channel throttles without any CPU reading: the lowering needs no
     * reading. Without a change left, NORMAL keeps the ceiling. */
    pm.now+=1100000;
    assert(a733_thermal_policy_update(&policy,pm.now,A733_THERMAL_AGAIN,NULL)==A733_THERMAL_THROTTLE);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && cpu.applied_mhz[0]==1014 && cpu.throttled==1);
    cpu.changes=0;assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY && !cpu.throttled && cpu.applied_mhz[0]==1014);
    assert(heat(115000)==A733_THERMAL_CRITICAL && heat(50000)==A733_THERMAL_CRITICAL);
    cpu.changes=1;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,416,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY && cpu.critical);
    /* A restore is refused, before any write, when THROTTLE returns first. */
    setup(0x00000200,2,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_BIG,1508,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && heat(95000)==A733_THERMAL_THROTTLE);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && cpu.applied_mhz[1]==1014 && heat(50000)==A733_THERMAL_NORMAL);
    changes=cpu.changes;cm.count=0;cm.writes=0;
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_PENDING && cpu.thermal==2 && heat(95000)==A733_THERMAL_THROTTLE);
    assert(run(400)==A733_CPU_CLOCK_REFUSED && released() && !cm.writes && cpu.changes==changes-1);
    assert(cpu.applied_mhz[1]==1014 && a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY);

    /* A throttle lowering that is refused is not retried while idle. */
    setup(0x00000200,2,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1404,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE);
    *word(SEL(2))=0x05000000;assert(heat(95000)==A733_THERMAL_THROTTLE);
    assert(run(400)==A733_CPU_CLOCK_REFUSED && released() && cpu.applied_mhz[0]==1404 && !cpu.failed);
    unsigned reads=cm.reads;
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY && a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY);
    assert(cm.reads==reads && !cpu.clock.id && !cpu.rail.id);
    /* The next THROTTLE episode tries again. */
    *word(SEL(2))=0x03000000;assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_READY && cm.reads==reads && !cpu.throttle_failed);
    assert(heat(95000)==A733_THERMAL_THROTTLE);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && cpu.applied_mhz[0]==1014);

    /* Inspection refusals end before any write and release both leases. */
    const uint32_t sids[]={0x1100,0x00000100,0x00000100,0x00000100,0x00000100,0x00000100,0x00000100,
        0x00000100,0x00000100};
    for(unsigned i=0;i<9;++i) {
        setup(sids[i],1,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
        if(i==1)*word(0x07090160)=0x10000002;          /* 24 MHz DCXO. */
        if(i==2)*word(SEL(0))=0x04000000;              /* PERI0 2X parent. */
        if(i==3){*word(PLL(0))=0xe8802600;cm.stuck_lock=1;} /* Not locked. */
        if(i==4)*word(SEL(2))=0x05000000;              /* DSU rate unknown. */
        if(i==5)pm.pmic[0x10]=0xef;                    /* DCDC5 snapshot disabled. */
        if(i==6)*word(PLL(0)+0x4)=UINT32_C(0x60000000);  /* PAT0 triangular pattern. */
        if(i==7)*word(PLL(0)+0x8)=UINT32_C(0x80000000);  /* PAT1 lfm_en. */
        if(i==8)*word(PLL(0)+0x18)=UINT32_C(0x80000100); /* LFM ramp enabled. */
        assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
        enum a733_cpu_clock_status r=run(400);
        assert(r==A733_CPU_CLOCK_REFUSED && !cm.writes && first(1)==cm.count && released() && !cpu.failed);
    }
    /* A held CPU_L lease delays admission until the deadline: clean expiry. */
    setup(0x00000100,1,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    struct a733_clock_token other;
    assert(a733_clock_acquire(&pm.clocks,99,A733_CLOCK_CPU_L,0,pm.now+1000000,&other)==A733_CLOCK_OK);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+2000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_EXPIRED && !cm.writes && !cm.reads && !cpu.clock.id);
    assert(a733_clock_release(&pm.clocks,&other,pm.now+1000)==A733_CLOCK_OK && released());
    /* A snapshot still queued at the deadline expires without bus effects. */
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+600)==A733_CPU_CLOCK_PENDING);
    for(unsigned i=0;i<4;++i){assert(a733_cpu_clock_poll(&cpu)==A733_CPU_CLOCK_PENDING);pm.now+=50;}
    assert(cpu.request.id);pm.now+=1000;assert(pmic_model_step(&pm)==A733_PMIC_EXPIRED);
    assert(run(10)==A733_CPU_CLOCK_EXPIRED && released() && !cm.writes && first(1)==cm.count);

    /* A deadline during the raise ramp wait ends cleanly: the confirmed higher
     * voltage covers the unchanged rate and the PLL was never touched. */
    setup(0x00000100,1,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_BIG,1508,pm.now+100000)==A733_CPU_CLOCK_PENDING);
    for(unsigned i=0;i<400 && cpu.state!=7;++i)assert(run(1)==A733_CPU_CLOCK_PENDING); /* Ramp wait. */
    assert(cpu.state==7 && pm.pmic[REG_DCDC3]==40);pm.now+=200000;
    assert(run(10)==A733_CPU_CLOCK_EXPIRED && released() && !cm.writes && *word(PLL(1))==(LOCKED|(39u<<8)));
    /* A deadline that cannot cover both PLL waits ends before the PLL is
     * touched, even with a slow but valid lock and after a raise's voltage
     * write. A deadline that covers the waits lets the same lock complete. */
    setup(0x00000100,1,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    cm.lock_us=5000;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+3000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_EXPIRED && released() && !cm.writes && pm.pmic[REG_DCDC5]==35);
    assert(*word(PLL(0))==0xf8802600 && *word(SEL(0))==0x03010000 && !cpu.applied_mhz[0]);
    cm.lock_us=60000;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+95000)==A733_CPU_CLOCK_PENDING);
    assert(run(2000)==A733_CPU_CLOCK_COMPLETE && released() && *word(PLL(0))==L0(46) && cpu.applied_mhz[0]==1196);
    assert(cm.events[cm.count-1].at-cm.events[first(0)].at>=60000);
    /* Faults after a write retain both leases and leave the hardware as it is. */
    for(unsigned i=0;i<2;++i) {
        setup(0x00000200,2,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
        if(i)cm.stuck_lock=1;else cm.stuck_update=1;
        assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
        assert(run(2000)==A733_CPU_CLOCK_RETAINED && ((*word(SEL(0))>>24)&7)==0);
        assert(pm.now-cm.events[cm.count-1].at>=(i ? 70000u : 10000u));
        retained();
    }
    /* A rejected PMIC write (readback differs) retains before any PLL access. */
    setup(0x00000100,1,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    cm.ignore_pmic=30;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_RETAINED && pm.provider.failed && !cm.writes && cpu.request.id);
    retained();
    /* A selector changed after the snapshot stops the write before its data byte. */
    setup(0x00000100,1,30,30,30,&both);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    for(unsigned i=0;i<400 && !cpu.from_uv;++i)run(1);
    assert(cpu.from_uv==800000);pm.pmic[REG_DCDC5]=31;
    assert(run(400)==A733_CPU_CLOCK_RETAINED && pm.provider.failed && !cm.writes && first(1)==cm.count);
    retained();

    /* PMIC write admission: CPU rails, exclusive lease, 800-1050 mV only. */
    setup(0x00000100,1,30,30,30,&both);
    struct a733_rail_token shared={0},exclusive={0};struct a733_pmic_request q={0};
    assert(a733_rail_acquire(&pm.rails,5,A733_RAIL_BIT(A733_RAIL_DCDC3),A733_RAIL_BIT(A733_RAIL_DCDC5),pm.now+100000,&exclusive)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&pm.rails,&exclusive)==A733_RAIL_READY);shared=exclusive;
    assert(a733_pmic_submit_write(&pm.provider,5,&shared,A733_RAIL_DCDC3,30,35,pm.now+100000,&q)==A733_PMIC_INVALID);
    assert(a733_pmic_submit_write(&pm.provider,5,&exclusive,A733_RAIL_DCDC5,30,29,pm.now+100000,&q)==A733_PMIC_INVALID);
    assert(a733_pmic_submit_write(&pm.provider,5,&exclusive,A733_RAIL_DCDC5,30,56,pm.now+100000,&q)==A733_PMIC_INVALID);
    assert(a733_pmic_submit_write(&pm.provider,5,&exclusive,A733_RAIL_DCDC5,128,35,pm.now+100000,&q)==A733_PMIC_INVALID);
    assert(!q.id);
    struct a733_rail_token npu={0};
    assert(a733_rail_acquire(&pm.rails,6,0,A733_RAIL_BIT(A733_RAIL_DCDC2),pm.now+100000,&npu)==A733_RAIL_PENDING);
    for(unsigned i=0;i<4;++i)a733_rail_poll(&pm.rails,&npu);
    assert(a733_rail_poll(&pm.rails,&npu)==A733_RAIL_READY);
    assert(a733_pmic_submit_write(&pm.provider,6,&npu,A733_RAIL_DCDC2,30,35,pm.now+100000,&q)==A733_PMIC_INVALID);
    /* Snapshots of the two new outputs decode like DCDC2/4. */
    struct a733_pmic_snapshot s;
    assert(a733_pmic_decode(A733_RAIL_DCDC3,0x04,40,&s) && s.enabled && s.programmed_uv==900000);
    assert(a733_pmic_decode(A733_RAIL_DCDC5,0x10,87,&s) && s.enabled && s.programmed_uv==1540000);
    assert(a733_pmic_decode(A733_RAIL_DCDC5,0xef,88,&s) && !s.enabled && !s.decoded);

    /* Grant records: nothing granted, a grant bound to its session, and one
     * refusal per field, each leaving the output unchanged. */
    struct a733_cpu_clock_grant_record g={A733_CPU_CLOCK_GRANT_MAGIC,1,96,4242,0,0,0,{0,0},0,{0}};
    struct a733_cpu_clock_config loaded;memset(&loaded,0x5a,sizeof(loaded));
    assert(a733_cpu_clock_grant_load(&g,11,12,&loaded) && loaded.owner==4242 && !loaded.grant.session &&
        !loaded.grant.clusters && !loaded.grant.max_mhz[0] && !loaded.grant.max_mhz[1] && !loaded.grant.changes);
    g.epoch=11;g.generation=12;g.clusters=3;g.max_mhz[0]=1196;g.max_mhz[1]=2002;g.changes=64;g.approval[31]=1;
    assert(a733_cpu_clock_grant_load(&g,11,12,&loaded) && loaded.grant.session==11 && loaded.grant.clusters==3 &&
        loaded.grant.max_mhz[0]==1196 && loaded.grant.max_mhz[1]==2002 && loaded.grant.changes==64);
    g.clusters=1;g.max_mhz[1]=0;g.changes=2;
    assert(a733_cpu_clock_grant_load(&g,11,12,&loaded) && loaded.grant.clusters==1 && !loaded.grant.max_mhz[1]);
    const struct a733_cpu_clock_config kept=loaded;
    for(unsigned i=0;i<16;++i) {
        struct a733_cpu_clock_grant_record r=g;uint64_t epoch=11,generation=12;
        switch(i) {
        case 0:r.magic^=1;break;            case 1:r.version=2;break;
        case 2:r.bytes=95;break;            case 3:r.owner=0;break;
        case 4:r.epoch=13;break;            case 5:r.generation=13;break;
        case 6:r.epoch=epoch=0;break;       case 7:r.generation=generation=0;break;
        case 8:r.clusters=5;break;          case 9:r.max_mhz[0]=1100;break;
        case 10:r.max_mhz[1]=1014;break;    case 11:r.clusters=2;break; /* 1196 kept for LITTLE. */
        case 12:r.changes=0;break;          case 13:r.changes=65;break;
        case 14:r.approval[31]=0;break;
        case 15:r=(struct a733_cpu_clock_grant_record){A733_CPU_CLOCK_GRANT_MAGIC,1,96,4242,11,0,0,{0,0},0,{0}};break;
        }
        assert(!a733_cpu_clock_grant_load(&r,epoch,generation,&loaded) && !memcmp(&loaded,&kept,sizeof(kept)));
    }
    assert(!a733_cpu_clock_grant_load(0,11,12,&loaded) && !a733_cpu_clock_grant_load(&g,11,12,0));
    /* The loaded grant admits its point through the actual driver. */
    setup(0x00000100,1,30,30,30,&loaded.grant);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1300,pm.now+1000000)==A733_CPU_CLOCK_REFUSED);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,1196,pm.now+1000000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && cpu.applied_mhz[0]==1196 && cpu.changes==1);

    /* Cycle timing names the core, its cluster and both rates. */
    struct a733_cpu_cycle_sample b={0x410fd055,0x81000000,1000,24000000},e=b;
    struct a733_cpu_timing timing;
    e.cycles+=494000000;e.ticks+=24000000;
    assert(a733_cpu_clock_timing(&b,&e,24000000,&report,&timing) && timing.core==0 &&
        timing.cluster==A733_CPU_LITTLE && timing.cycles==494000000 && timing.ticks==24000000 &&
        timing.measured_hz==494000000 && timing.configured_hz==494000000);
    struct a733_cpu_cycle_sample bb={0x414fd0b1,0x81000700,5,100},be=bb;
    be.cycles+=1014000;be.ticks+=24000;
    assert(a733_cpu_clock_timing(&bb,&be,24000000,&report,&timing) && timing.core==7 &&
        timing.cluster==A733_CPU_BIG && timing.measured_hz==1014000000 && timing.configured_hz==1014000000);
    be.cycles=bb.cycles+10;be.ticks=bb.ticks+7; /* 10 x 24 MHz / 7, rounded down. */
    assert(a733_cpu_clock_timing(&bb,&be,24000000,0,&timing) && timing.measured_hz==34285714 && !timing.configured_hz);
    const struct a733_cpu_timing held=timing;
    for(unsigned i=0;i<10;++i) {
        struct a733_cpu_cycle_sample x=b,y=e;uint32_t hz=24000000;
        switch(i) {
        case 0:y.mpidr=0x81000100;break;               /* Another core. */
        case 1:x.midr=y.midr=0x410fd0b0;break;         /* A76 identity on core 0. */
        case 2:x.mpidr=y.mpidr=0x81000001;break;       /* Aff0 set. */
        case 3:y.midr=0x410fd051;break;                /* Identity changed. */
        case 4:y.ticks=x.ticks;break;
        case 5:y.ticks=x.ticks-1;break;
        case 6:y.cycles=x.cycles-1;break;
        case 7:hz=0;break;
        case 8:y.ticks=x.ticks+UINT64_MAX/hz+1;break;  /* Too long to scale. */
        case 9:y.ticks=x.ticks+1;y.cycles=x.cycles+UINT64_MAX/2;break; /* Rate overflows. */
        }
        assert(!a733_cpu_clock_timing(&x,&y,hz,&report,&timing) && !memcmp(&timing,&held,sizeof(held)));
    }
    /* Read-only inspection, as the OS owner takes for B1: no grant, no
     * write, no change consumed; the voltage, the pattern and LFM words and the
     * boot floor are recorded. Board 3-3's fuse word 0x00000496 selects vf0400. */
    setup(0x00000496,4,35,40,30,0);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_BIG,1196,pm.now+120000)==A733_CPU_CLOCK_REFUSED);
    assert(a733_cpu_clock_inspect(&cpu,A733_CPU_BIG,pm.now+120000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && !cm.writes && first(1)==cm.count);
    assert(cpu.rail_uv[1]==900000 && !cpu.words[1][0] && !cpu.words[1][2] && pm.rails.floor_uv[0]==900000);
    assert(a733_cpu_clock_inspect(&cpu,A733_CPU_LITTLE,pm.now+120000)==A733_CPU_CLOCK_PENDING);
    assert(run(400)==A733_CPU_CLOCK_COMPLETE && released() && !cm.writes && cpu.rail_uv[0]==850000);
    /* A2 on board 3-3: CPU-B 1014 -> 2002 MHz at 1,050 mV with the OS owner's
     * 120 ms deadline, one burst per 1 ms tick with a 200 us budget. A lock of
     * 20 us leaves no tick boundary inside the parked window. */
    const struct a733_cpu_clock_grant a2={77,2,{0,2002},1};
    setup(0x00000496,4,35,40,30,&a2);assert(heat(50000)==A733_THERMAL_NORMAL);
    assert(a733_cpu_clock_request(&cpu,A733_CPU_BIG,2002,pm.now+120000)==A733_CPU_CLOCK_PENDING);
    unsigned ticks=0,parked_ticks=0;enum a733_cpu_clock_status r2=A733_CPU_CLOCK_PENDING;
    for(;r2==A733_CPU_CLOCK_PENDING && ticks<200;++ticks) {
        for(unsigned i=0;i<4;++i)assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
        r2=a733_cpu_clock_burst(&cpu,200);
        if(a733_cpu_clock_parked(&cpu))++parked_ticks;
        pm.now+=1000;
    }
    assert(r2==A733_CPU_CLOCK_COMPLETE && released() && *word(PLL(1))==(LOCKED|(77u<<8)) && pm.pmic[REG_DCDC3]==55);
    assert(cpu.rail_uv[1]==1050000 && cpu.applied_mhz[1]==2002 && !parked_ticks && cpu.parked_max_us<200);
    unsigned writes=0;for(unsigned i=0;i<cm.count;++i)writes+=!cm.events[i].pmic;
    printf("BUDGET raise_with_voltage clock_writes=%u pmic_writes=%u owner_ticks=%u parked_ticks=%u parked_us=%llu\n",
        writes,cm.count-writes,ticks,parked_ticks,(unsigned long long)cpu.parked_max_us);
    assert(writes==5 && cm.count-writes==1);
    /* A lock of 300 us exceeds the burst budget: the owner sleeps parked ticks
     * and the parked time is recorded. */
    setup(0x00000496,4,35,55,30,&a2);assert(heat(50000)==A733_THERMAL_NORMAL);cm.lock_us=300;
    assert(a733_cpu_clock_request(&cpu,A733_CPU_BIG,2002,pm.now+120000)==A733_CPU_CLOCK_PENDING);
    ticks=parked_ticks=0;r2=A733_CPU_CLOCK_PENDING;
    for(;r2==A733_CPU_CLOCK_PENDING && ticks<200;++ticks) {
        for(unsigned i=0;i<4;++i)assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
        r2=a733_cpu_clock_burst(&cpu,200);
        if(a733_cpu_clock_parked(&cpu))++parked_ticks;
        pm.now+=1000;
    }
    assert(r2==A733_CPU_CLOCK_COMPLETE && parked_ticks==1 && cpu.parked_max_us>=300 && first(1)==cm.count);
    printf("BUDGET slow_lock parked_ticks=%u parked_us=%llu\n",parked_ticks,(unsigned long long)cpu.parked_max_us);
    puts("PASS CPU clock: report, bins, grant records, grant and thermal-policy admission, "
         "voltage-before-frequency raise, frequency-before-voltage lowering, DSU rail share, throttle and "
         "restore, refusals, expiry before the PLL waits, retained faults, cycle timing, settle after "
         "each mux switch, read-only inspection and parked bursts");
    return 0;
}
