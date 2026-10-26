/* SPDX-License-Identifier: Apache-2.0 */
/* Host model of the performance-mode engine (drivers/power/a733_perf_mode.c)
 * with the CPU clock driver's operating-point tables.
 *
 * The owner is modeled. Its CPU domain changes follow the CPU clock driver's
 * rules (docs/cpu-clock.md): a raise writes the rail before the PLL, a
 * lowering the PLL before the rail; DCDC5 covers CPU-L and the DSU's need;
 * a DSU change writes no voltage and is refused when DCDC5 does not already
 * cover it. Each change takes the driver's time: the PMIC write, a 1 ms
 * settle per raise and the relock. The NPU's session runs until job_end; a
 * hold completes once the session has stopped and keeps it stopped until
 * RELEASE. The engine is stepped once per 1 ms tick, as the OS thermal task
 * runs it. Every hardware event is logged with its time and checked: no rate
 * above what its voltage covers, no event that breaks a DSU rule that held
 * before it, and no NPU rate change while its session runs.
 *
 * Build: clang -std=c11 -Wall -Wextra -Werror -Iinclude checks/perf-mode-model.c
 *   drivers/power/a733_perf_mode.c drivers/clock/a733_cpu_clock.c (plus the
 *   driver's link dependencies, which checks/perf-mode.py supplies).
 * Output: one JSON line of measurements, then PASS. */
#include "a733_perf_mode.h"
#include "a733_cpu_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define L A733_PERF_CPU_L
#define B A733_PERF_CPU_B
#define D A733_PERF_DSU
#define G A733_PERF_GPU
#define N A733_PERF_NPU
#define BIT A733_PERF_BIT
#define STEP_US 1000u
#define COLUMN 4u /* vf0400 */
#define SID UINT32_C(0x496)

enum { RAIL5, RAIL3, RAIL4, RAIL2, RAILS };
static struct {
    uint64_t now;
    uint32_t mhz[A733_PERF_BLOCKS], rail[RAILS];
    /* The CPU action in progress: domain, target, phase, time it completes. */
    int cpu_active; unsigned cpu_block, cpu_phase; uint32_t cpu_target; uint64_t cpu_until;
    /* The NPU session runs until job_end; a hold keeps it stopped. */
    uint64_t job_end; unsigned held, holds, releases, rate_during_job, rule_ok;
    uint64_t hold_at, release_at, unavailable_max;
    /* Injected faults. */
    unsigned fail_block, fail_status, read_fail, admit_reason, changes_left;
    unsigned events, cpu_changes, pmic_writes;
    unsigned release_mode, release_polls, hold_failed;
    uint64_t release_deadline;
    char log[64][24];
} m;
static uint32_t row_mv(enum a733_cpu_domain d,uint32_t mhz)
{
    unsigned n;const struct a733_cpu_opp *t=a733_cpu_clock_opps(d,&n);
    for(unsigned i=0;i<n;++i)if(t[i].mhz==mhz)return a733_cpu_clock_opp_mv(&t[i],COLUMN);
    return 0;
}
/* Voltage of the lowest DSU row at or above the rate, as the driver's dsu_mv. */
static uint32_t dsu_need(uint32_t mhz)
{
    unsigned n;const struct a733_cpu_opp *t=a733_cpu_clock_opps(A733_CPU_DSU,&n);
    for(unsigned i=0;i<n;++i){uint32_t mv=a733_cpu_clock_opp_mv(&t[i],COLUMN);if(mv && t[i].mhz>=mhz)return mv;}
    return 0;
}
/* A rate that is not a row (an inherited boot rate) needs the next row's voltage. */
static uint32_t need(unsigned block,uint32_t mhz)
{
    if(block==D)return dsu_need(mhz);
    unsigned n;const struct a733_cpu_opp *t=a733_cpu_clock_opps(block==L ? A733_CPU_LITTLE : A733_CPU_BIG,&n);
    for(unsigned i=0;i<n;++i){uint32_t mv=a733_cpu_clock_opp_mv(&t[i],COLUMN);if(mv && t[i].mhz>=mhz)return mv;}
    return 0;
}
static uint32_t cap(void){uint32_t f=m.mhz[L]>m.mhz[B]?m.mhz[L]:m.mhz[B];f=f*3/4;return f-f%26;}
/* Checked after every hardware event. */
static void invariants(void)
{
    assert(m.rail[RAIL5]>=need(L,m.mhz[L]) && m.rail[RAIL5]>=need(D,m.mhz[D]));
    assert(m.rail[RAIL3]>=need(B,m.mhz[B]));
    /* The DSU rule, once it holds, keeps holding. */
    if(m.rule_ok)assert(m.mhz[D]<=cap());
    m.rule_ok=m.mhz[D]<=cap();
    if(m.mhz[G])assert(m.rail[RAIL4]>=(m.mhz[G]>600 ? 960u : 800u));
}
static void event(const char *name,unsigned block,uint32_t value)
{
    if(m.events<64)snprintf(m.log[m.events],sizeof(m.log[0]),"%s%u=%u",name,block,(unsigned)value);
    ++m.events;invariants();
}
static int now(void *v,uint64_t *t){assert(v==&m);*t=m.now;return 1;}
static enum a733_perf_reason admit(void *v,unsigned blocks,unsigned cpu)
{
    assert(v==&m);(void)blocks;
    if(m.admit_reason)return (enum a733_perf_reason)m.admit_reason;
    return cpu>m.changes_left ? A733_PERF_BUDGET : A733_PERF_OK;
}
static unsigned rail_of(unsigned b){return b==B ? RAIL3 : b==G ? RAIL4 : b==N ? RAIL2 : RAIL5;}
static enum a733_perf_status act(void *v,enum a733_perf_action a,enum a733_perf_block b,uint32_t value,
    uint64_t deadline,int start,struct a733_perf_point *read)
{
    assert(v==&m);
    if(a==A733_PERF_READ) {
        assert(read);
        if(m.read_fail&BIT(b))return A733_PERF_REFUSED;
        read->mhz=m.mhz[b];read->mv=m.rail[rail_of(b)];return A733_PERF_DONE;
    }
    if(a==A733_PERF_HOLD) {
        if(m.hold_failed)return m.hold_failed==2 ? A733_PERF_REFUSED : A733_PERF_FAILED;
        assert(b==N || b==G);
        if(start){if(!m.held){m.hold_at=m.now;++m.holds;}m.held|=BIT(b);}
        return b==N && m.now<m.job_end ? (m.now>=deadline ? A733_PERF_EXPIRED : A733_PERF_PENDING) : A733_PERF_DONE;
    }
    if(a==A733_PERF_RELEASE) {
        assert(m.held&BIT(b));
        if(start)m.release_deadline=deadline;
        assert(deadline==m.release_deadline);
        ++m.release_polls;
        if(m.release_mode==2)return A733_PERF_FAILED;
        if(m.release_mode==3 || (m.release_mode==1 && m.release_polls<3))return A733_PERF_PENDING;
        m.held&=~BIT(b);++m.releases;
        if(!m.held && m.now-m.hold_at>m.unavailable_max)m.unavailable_max=m.now-m.hold_at;
        return A733_PERF_DONE;
    }
    if(m.fail_block==BIT(b) && start) {
        /* An EXPIRED or REFUSED change writes nothing here; FAILED leaves the
         * rail written and the clock unknown. */
        if(m.fail_status==A733_PERF_FAILED && a==A733_PERF_CHANGE_CPU){m.rail[rail_of(b)]=1050;event("V",b,1050);}
        return (enum a733_perf_status)m.fail_status;
    }
    if(a==A733_PERF_VOLTAGE || a==A733_PERF_RATE) {
        assert(b==N || b==G);
        assert(m.held&BIT(b)); /* Only between jobs, under a hold. */
        if(b==N && m.now<m.job_end)++m.rate_during_job; /* The session must have stopped. */
        if(a==A733_PERF_VOLTAGE){m.rail[rail_of(b)]=value;++m.pmic_writes;event("V",b,value);}
        else{m.mhz[b]=value;event("F",b,value);}
        m.now+=b==N ? 400 : 1200; /* The PLL relock; the GPU's 1.2 ms supply settle. */
        return A733_PERF_DONE;
    }
    assert(a==A733_PERF_CHANGE_CPU);
    if(start) {
        assert(!m.cpu_active);
        if(!m.changes_left)return A733_PERF_REFUSED;
        --m.changes_left;++m.cpu_changes;
        /* The driver's DSU checks: the rule's cap, and DCDC5 already covering it. */
        if(b==D && (value>cap() || m.rail[RAIL5]<dsu_need(value)))return A733_PERF_REFUSED;
        m.cpu_active=1;m.cpu_block=b;m.cpu_target=value;m.cpu_phase=0;m.cpu_until=m.now;
    }
    assert(m.cpu_active && m.cpu_block==b && m.cpu_target==value);
    if(m.now>deadline)return A733_PERF_FAILED;
    const unsigned rail=rail_of(b);
    const uint32_t mv=b==D ? m.rail[RAIL5] : b==L ? (need(L,value)>dsu_need(m.mhz[D]) ? need(L,value) : dsu_need(m.mhz[D])) : need(B,value);
    const int up=value>m.mhz[b];
    /* Phase 0: the first write; phase 1: the second; each after its wait. */
    while(m.now>=m.cpu_until && m.cpu_phase<2) {
        /* The driver raises the voltage only before a raise and lowers it
         * only after a lowering. */
        const int voltage=b!=D && (up ? m.cpu_phase==0 && mv>m.rail[rail] : m.cpu_phase==1 && mv<m.rail[rail]);
        if(voltage){m.rail[rail]=mv;++m.pmic_writes;event("V",b,mv);m.cpu_until=m.now+(up?1000:800);}
        else if(up ? m.cpu_phase==1 : m.cpu_phase==0){m.mhz[b]=value;event("F",b,value);m.cpu_until=m.now+100;}
        ++m.cpu_phase;
    }
    if(m.cpu_phase<2 || m.now<m.cpu_until)return A733_PERF_PENDING;
    m.cpu_active=0;return A733_PERF_DONE;
}
static const struct a733_perf_ops ops={now,admit,act};
static struct a733_perf p;
static void boot_state(void)
{
    memset(&m,0,sizeof(m));
    /* boot0's state on port 3-3: CPU-L and the DSU at 494 MHz, CPU-B at
     * 1014 MHz, every CPU rail at 800 mV; the GPU clock off with DCDC4 at
     * 800 mV; the NPU at its startup point. */
    m.mhz[L]=494;m.mhz[B]=1014;m.mhz[D]=494;m.mhz[G]=0;m.mhz[N]=852;
    m.rail[RAIL5]=m.rail[RAIL3]=m.rail[RAIL4]=m.rail[RAIL2]=800;
    m.changes_left=64;m.fail_block=0;m.rule_ok=1;
}
/* Step the engine once per owner period until it finishes. */
static enum a733_perf_result run(uint64_t *elapsed)
{
    const uint64_t begin=m.now;
    enum a733_perf_result r;
    for(unsigned i=0;i<4000;++i) {
        r=a733_perf_step(&p);
        if(r!=A733_PERF_RESULT_RUNNING)break;
        m.now+=STEP_US;
    }
    if(elapsed)*elapsed=m.now-begin;
    return r;
}
static void read_back(void)
{
    assert(a733_perf_request(&p,A733_PERF_COMMAND_READ,0,0)==A733_PERF_OK);
    assert(run(0)==A733_PERF_RESULT_DONE);
}
static enum a733_perf_result request(enum a733_perf_mode mode,unsigned blocks,uint64_t *elapsed)
{
    m.events=0;
    enum a733_perf_reason r=a733_perf_request(&p,A733_PERF_COMMAND_SWITCH,mode,blocks);
    if(r!=A733_PERF_OK)return p.record.result;
    return run(elapsed);
}
/* The log, compared as one string. */
static void expect(const char *want)
{
    char got[512]="";
    for(unsigned i=0;i<m.events && i<64;++i){if(i)strcat(got," ");strcat(got,m.log[i]);}
    if(strcmp(got,want)){fprintf(stderr,"events: %s\nwanted: %s\n",got,want);assert(0);}
}
static const unsigned OWNED=A733_PERF_CPU|BIT(N);

int main(void)
{
    unsigned cases=0;
    /* The table: one entry, vf0400, whose DSU points follow the rule. */
    unsigned count;const struct a733_perf_entry *e=a733_perf_entries(&count);
    assert(count==1 && e[0].column==COLUMN && a733_perf_entry(SID)==&e[0] && a733_perf_entry(0x495)==&e[0]);
    for(unsigned mode=0;mode<2;++mode) {
        struct a733_perf_point dsu;
        assert(a733_perf_dsu(COLUMN,e->point[mode][L].mhz,e->point[mode][B].mhz,&dsu));
        assert(dsu.mhz==e->point[mode][D].mhz && dsu.mv==e->point[mode][D].mv);
        for(unsigned b=0;b<3;++b)assert(row_mv(b==D?A733_CPU_DSU:b==L?A733_CPU_LITTLE:A733_CPU_BIG,e->point[mode][b].mhz)==e->point[mode][b].mv);
    }
    /* The rule's other combinations, and its floor for a fast CPU-B. */
    struct a733_perf_point dsu;
    assert(a733_perf_dsu(COLUMN,1794,1404,&dsu) && dsu.mhz==1274);
    assert(a733_perf_dsu(COLUMN,1196,2002,&dsu) && dsu.mhz==910 && dsu.mv==800);
    assert(!a733_perf_dsu(COLUMN,494,1014,&dsu) && !a733_perf_dsu(9,1196,1404,&dsu));
    ++cases;
    /* Bins without an entry refuse every switch before any access and still read back. */
    static const uint32_t refused_bins[]={0,0x100,0x1100,0x1200,0x200,0x300,0x500,0x600,0x1400,0x1500,0x2200};
    for(unsigned i=0;i<sizeof(refused_bins)/sizeof(*refused_bins);++i) {
        boot_state();
        assert(!a733_perf_init(&p,refused_bins[i],OWNED,&ops,&m));
        read_back();
        assert(a733_perf_request(&p,A733_PERF_COMMAND_SWITCH,A733_PERF_HIGH,OWNED)==A733_PERF_NO_BIN);
        assert(p.record.result==A733_PERF_RESULT_REFUSED && !m.events && p.record.mode==A733_PERF_UNKNOWN);
    }
    ++cases;

    /* Boot: read back, then the boot switch to high. CPU-B, then CPU-L, each
     * voltage before frequency, then the DSU; the NPU already runs at 852. */
    boot_state();
    assert(a733_perf_init(&p,SID,OWNED,&ops,&m) && p.owned==(OWNED|BIT(D)));
    read_back();
    assert(p.record.mode==A733_PERF_MIXED && p.record.read[L].mhz==494);
    uint64_t boot_us;
    assert(request(A733_PERF_HIGH,OWNED,&boot_us)==A733_PERF_RESULT_DONE);
    expect("V1=1050 F1=2002 V0=1000 F0=1794 F2=1274");
    assert(p.record.mode==A733_PERF_HIGH && m.cpu_changes==3 && !m.holds);
    ++cases;
    /* High to low: the NPU under a hold first, then the DSU, CPU-L and
     * CPU-B, each frequency before voltage. */
    uint64_t low_us;unsigned writes=m.pmic_writes;
    m.cpu_changes=0;
    assert(request(A733_PERF_LOW,OWNED,&low_us)==A733_PERF_RESULT_DONE);
    expect("F4=492 F2=910 F0=1196 V0=800 F1=1404 V1=800");
    assert(p.record.mode==A733_PERF_LOW && m.cpu_changes==3 && m.pmic_writes-writes==2 && !m.held && m.releases==1);
    ++cases;
    /* Low to high: voltage before frequency, the DSU last. */
    uint64_t high_us;writes=m.pmic_writes;m.cpu_changes=0;
    assert(request(A733_PERF_HIGH,OWNED,&high_us)==A733_PERF_RESULT_DONE);
    expect("F4=852 V1=1050 F1=2002 V0=1000 F0=1794 F2=1274");
    assert(p.record.mode==A733_PERF_HIGH && m.cpu_changes==3 && m.pmic_writes-writes==2);
    const unsigned full_switch_cpu_changes=m.cpu_changes,full_switch_pmic_writes=m.pmic_writes-writes;
    ++cases;
    /* A switch to the present mode makes no change and reads back. */
    assert(request(A733_PERF_HIGH,OWNED,0)==A733_PERF_RESULT_DONE && !m.events && p.record.mode==A733_PERF_HIGH);
    ++cases;
    /* One block at a time. CPU-B low leaves the DSU at 1274 MHz; CPU-L low
     * then lowers the DSU first. */
    assert(request(A733_PERF_LOW,BIT(B),0)==A733_PERF_RESULT_DONE);
    expect("F1=1404 V1=800");
    assert(p.record.mode==A733_PERF_MIXED);
    assert(request(A733_PERF_LOW,BIT(L),0)==A733_PERF_RESULT_DONE);
    expect("F2=910 F0=1196 V0=800");
    assert(request(A733_PERF_LOW,BIT(N),0)==A733_PERF_RESULT_DONE);
    expect("F4=492");
    assert(p.record.mode==A733_PERF_LOW);
    assert(request(A733_PERF_HIGH,BIT(L),0)==A733_PERF_RESULT_DONE);
    expect("V0=1000 F0=1794 F2=1274");
    assert(request(A733_PERF_HIGH,OWNED,0)==A733_PERF_RESULT_DONE);
    expect("F4=852 V1=1050 F1=2002");
    ++cases;

    /* Refusals before any access. */
    struct { const char *name; enum a733_perf_command c; enum a733_perf_mode mode; unsigned blocks;
             unsigned admit; enum a733_perf_reason want; } refusals[]={
        {"no mode",A733_PERF_COMMAND_SWITCH,A733_PERF_UNKNOWN,OWNED,0,A733_PERF_REQUEST},
        {"mixed is not a mode",A733_PERF_COMMAND_SWITCH,A733_PERF_MIXED,OWNED,0,A733_PERF_REQUEST},
        {"no block",A733_PERF_COMMAND_SWITCH,A733_PERF_LOW,0,0,A733_PERF_REQUEST},
        {"the DSU named",A733_PERF_COMMAND_SWITCH,A733_PERF_LOW,BIT(D),0,A733_PERF_REQUEST},
        {"the GPU, not owned",A733_PERF_COMMAND_SWITCH,A733_PERF_LOW,BIT(G),0,A733_PERF_NOT_OWNED},
        {"a read with a mode",A733_PERF_COMMAND_READ,A733_PERF_LOW,0,0,A733_PERF_REQUEST},
        {"thermal",A733_PERF_COMMAND_SWITCH,A733_PERF_LOW,OWNED,A733_PERF_THERMAL,A733_PERF_THERMAL},
        {"NPU not ready",A733_PERF_COMMAND_SWITCH,A733_PERF_LOW,OWNED,A733_PERF_NOT_READY,A733_PERF_NOT_READY},
    };
    for(unsigned i=0;i<sizeof(refusals)/sizeof(*refusals);++i) {
        m.events=0;m.admit_reason=refusals[i].admit;
        assert(a733_perf_request(&p,refusals[i].c,refusals[i].mode,refusals[i].blocks)==refusals[i].want);
        assert(p.record.result==A733_PERF_RESULT_REFUSED && p.record.reason==refusals[i].want && !m.events && !m.held);
        ++cases;
    }
    m.admit_reason=0;
    /* The grant's remaining changes: a switch of three domains needs three. */
    m.changes_left=2;
    assert(a733_perf_request(&p,A733_PERF_COMMAND_SWITCH,A733_PERF_LOW,OWNED)==A733_PERF_BUDGET && !m.events);
    m.changes_left=64;++cases;
    /* A second command while one runs. */
    m.job_end=m.now+100000;
    assert(a733_perf_request(&p,A733_PERF_COMMAND_SWITCH,A733_PERF_LOW,OWNED)==A733_PERF_OK);
    assert(a733_perf_step(&p)==A733_PERF_RESULT_RUNNING);
    assert(a733_perf_request(&p,A733_PERF_COMMAND_READ,0,0)==A733_PERF_ACTIVE);
    /* The NPU job in flight finishes inside the bound: the hold waits for
     * it, and the NPU changes only afterwards. */
    assert(run(0)==A733_PERF_RESULT_DONE && p.record.mode==A733_PERF_LOW && !m.rate_during_job);
    assert(m.unavailable_max<=m.job_end-m.hold_at+STEP_US+2000);
    const uint64_t npu_hold_after_stop=m.unavailable_max-(m.job_end-m.hold_at);
    ++cases;
    /* A job that outlasts the bound: the switch is refused as busy, the hold
     * released, and nothing written. */
    m.job_end=m.now+5000000;m.events=0;
    assert(a733_perf_request(&p,A733_PERF_COMMAND_SWITCH,A733_PERF_HIGH,OWNED)==A733_PERF_OK);
    uint64_t busy_us;
    assert(run(&busy_us)==A733_PERF_RESULT_REFUSED && p.record.reason==A733_PERF_BUSY && p.record.failed_block==N);
    assert(!m.events && !m.held && p.record.mode==A733_PERF_LOW && busy_us>=A733_PERF_HOLD_US && busy_us<A733_PERF_HOLD_US+2*STEP_US);
    m.job_end=0;++cases;

    /* A failed step stops the switch: blocks changed before it keep their
     * new point, the failed block reports its state, and the rest keep
     * their old point. No rollback; every hold is released. */
    static const struct { unsigned block, status; enum a733_perf_reason reason; } failures[]={
        {L,A733_PERF_EXPIRED,A733_PERF_STEP_EXPIRED},{L,A733_PERF_REFUSED,A733_PERF_STEP_REFUSED},
        {B,A733_PERF_FAILED,A733_PERF_STEP_FAILED},{N,A733_PERF_FAILED,A733_PERF_STEP_FAILED},
        {D,A733_PERF_EXPIRED,A733_PERF_STEP_EXPIRED}};
    for(unsigned i=0;i<sizeof(failures)/sizeof(*failures);++i) {
        boot_state();assert(a733_perf_init(&p,SID,OWNED,&ops,&m));read_back();
        assert(request(A733_PERF_HIGH,OWNED,0)==A733_PERF_RESULT_DONE);
        /* High to low: the NPU first, then the DSU, CPU-L and CPU-B. */
        m.fail_block=BIT(failures[i].block);m.fail_status=failures[i].status;
        const enum a733_perf_result result=request(A733_PERF_LOW,OWNED,0);
        m.fail_block=0;
        /* Only the NPU's hold precedes its step, so its failure wrote nothing. */
        assert(result==A733_PERF_RESULT_STOPPED);
        assert(p.record.reason==failures[i].reason && p.record.failed_block==failures[i].block);
        if(failures[i].status==A733_PERF_FAILED) {
            assert(p.retained==BIT(failures[i].block) && p.record.mode==A733_PERF_UNKNOWN && !p.record.read_ok);
            assert(a733_perf_request(&p,A733_PERF_COMMAND_READ,0,0)==A733_PERF_ACTIVE);
            ++cases;continue;
        }
        assert(p.record.mode==A733_PERF_MIXED && !m.held && p.record.read_ok==A733_PERF_ALL);
        /* The read-back points are the hardware's; blocks after the failed
         * one keep their old point. */
        for(unsigned b=0;b<A733_PERF_BLOCKS;++b)assert(p.record.read[b].mhz==m.mhz[b] && p.record.read[b].mv==m.rail[rail_of(b)]);
        if(failures[i].block==L)assert(m.mhz[N]==492 && m.mhz[D]==910 && m.mhz[L]==1794 && m.mhz[B]==2002);
        if(failures[i].block==B)assert(m.mhz[L]==1196 && m.mhz[B]==2002 && m.rail[RAIL3]==1050);
        ++cases;
    }
    /* The NPU failing as it rises: the CPU domains keep their points. */
    boot_state();assert(a733_perf_init(&p,SID,OWNED,&ops,&m));read_back();
    assert(request(A733_PERF_LOW,OWNED,0)==A733_PERF_RESULT_DONE);
    m.fail_block=BIT(N);m.fail_status=A733_PERF_FAILED;
    assert(request(A733_PERF_HIGH,OWNED,0)==A733_PERF_RESULT_STOPPED);
    /* Failure may have written hardware; retain the NPU hold and refuse reuse. */
    assert(p.record.reason==A733_PERF_STEP_FAILED && p.record.failed_block==N && !m.events && m.held==BIT(N) && p.retained==BIT(N));
    m.fail_block=0;++cases;
    boot_state();assert(a733_perf_init(&p,SID,OWNED,&ops,&m));read_back();
    assert(request(A733_PERF_LOW,OWNED,0)==A733_PERF_RESULT_DONE);
    /* A block that cannot be read back is reported, and a switch then
     * refuses until a read succeeds. */
    m.read_fail=BIT(B);read_back();
    assert(p.record.reason==A733_PERF_READ_FAILED && !(p.record.read_ok&BIT(B)) && p.record.mode==A733_PERF_UNKNOWN);
    assert(a733_perf_request(&p,A733_PERF_COMMAND_SWITCH,A733_PERF_HIGH,OWNED)==A733_PERF_NOT_READY);
    m.read_fail=BIT(G);read_back();
    assert(p.record.reason==A733_PERF_OK && p.record.mode==A733_PERF_LOW); /* The GPU is not owned. */
    m.read_fail=0;++cases;
    /* A THROTTLE lowering without a restore can leave the DSU above the cap
     * of its clusters: the DSU lowering then waits for the cluster raises,
     * and DCDC5 keeps the voltage of the old DSU point. */
    boot_state();assert(a733_perf_init(&p,SID,OWNED,&ops,&m));read_back();
    assert(request(A733_PERF_HIGH,OWNED,0)==A733_PERF_RESULT_DONE);
    m.mhz[L]=m.mhz[B]=1014;m.rule_ok=0;read_back();
    assert(request(A733_PERF_LOW,A733_PERF_CPU,0)==A733_PERF_RESULT_DONE);
    expect("F1=1404 F0=1196 F2=910");
    assert(p.record.mode==A733_PERF_MIXED && p.record.read[L].mv==1000 && p.record.read[B].mv==1050);
    /* A later round trip reaches the table points. */
    assert(request(A733_PERF_HIGH,A733_PERF_CPU,0)==A733_PERF_RESULT_DONE);
    assert(request(A733_PERF_LOW,A733_PERF_CPU,0)==A733_PERF_RESULT_DONE && p.record.read[L].mv==800);
    ++cases;
    /* The planner refuses a DSU target above the cap of the clusters it
     * would serve. */
    {
        struct a733_perf_point from[A733_PERF_BLOCKS]={{1196,800},{1404,800},{910,800},{0,800},{852,800}},to[A733_PERF_BLOCKS];
        memcpy(to,from,sizeof(to));to[D]=(struct a733_perf_point){1274,1000};
        struct a733_perf_step plan[A733_PERF_STEPS];
        assert(a733_perf_plan(e,BIT(D),from,to,plan)==-1);
        to[L]=(struct a733_perf_point){1794,1000};
        assert(a733_perf_plan(e,BIT(L)|BIT(D),from,to,plan)==2 && plan[0].block==L && plan[1].block==D);
        ++cases;
    }
    /* Readback decoders: PLL_GPU0 at 1008 MHz, PLL_NPU at 852 and 492 MHz,
     * the GPU on PERI0's 600 MHz output, gated clocks, and refusals. The
     * reference and PERI0 words are those of checks/npu-startup-host.c. */
    {
        const uint32_t osc=0x10000002,ref=0xf8000000|(99u<<16)|(99u<<8),peri0=0xfe000000|(1u<<20)|(2u<<16)|(99u<<8)|(4u<<2);
        uint32_t mhz;unsigned stopped;
        assert(a733_perf_pll_mhz(osc,ref,0xf9105300)==1008 && a733_perf_pll_mhz(osc,ref,0xf8104600)==852 &&
            a733_perf_pll_mhz(osc,ref,0xf8305100)==492 && !a733_perf_pll_mhz(osc,ref,0xe8104600) &&
            !a733_perf_pll_mhz(osc,0xf8000000,0xf8104600));
        assert(a733_perf_gpu_mhz(osc,ref,peri0,0xf9105300,0x80000000,&mhz,&stopped) && mhz==1008 && !stopped);
        assert(a733_perf_gpu_mhz(osc,ref,peri0,0xf9105300,0x82000000,&mhz,&stopped) && mhz==600 && !stopped);
        assert(a733_perf_gpu_mhz(osc,ref,peri0,0xf9105300,0x82000008,&mhz,&stopped) && mhz==300);
        assert(a733_perf_gpu_mhz(osc,ref,peri0,0x01105300,0x00000000,&mhz,&stopped) && mhz==0 && stopped);
        assert(!a733_perf_gpu_mhz(osc,ref,peri0,0xf9105300,0x86000000,&mhz,&stopped) &&
            !a733_perf_gpu_mhz(osc,ref,peri0,0xf9105300,0x80000003,&mhz,&stopped) &&
            !a733_perf_gpu_mhz(osc,ref,0,0xf9105300,0x82000000,&mhz,&stopped));
        assert(a733_perf_npu_mhz(osc,ref,0xf8104600,0x80000000,&mhz,&stopped) && mhz==852 && !stopped);
        assert(a733_perf_npu_mhz(osc,ref,0xf8305100,0x00000000,&mhz,&stopped) && mhz==492 && stopped);
        assert(a733_perf_npu_mhz(osc,ref,0xf8104600,0x80000001,&mhz,&stopped) && mhz==426);
        assert(!a733_perf_npu_mhz(osc,ref,0xf8104600,0x81000000,&mhz,&stopped));
        ++cases;
    }
    /* The firmware RELEASE is asynchronous. Its fixed deadline and hold
     * survive pending, failed and missing replies; failed HOLD may already
     * have published a device-writing request before DONE. */
    for(unsigned mode=1;mode<=5;++mode) {
        boot_state();m.mhz[G]=1008;m.rail[RAIL4]=960;
        assert(a733_perf_init(&p,SID,BIT(G),&ops,&m));read_back();
        m.release_mode=mode;m.hold_failed=mode==5 ? 2 : mode==4;
        enum a733_perf_result result=request(A733_PERF_LOW,BIT(G),0);
        if(mode==1)assert(result==A733_PERF_RESULT_DONE && m.release_polls==3 && !p.held && !p.retained);
        else if(mode==5)assert(result==A733_PERF_RESULT_REFUSED && !p.held && !p.retained && !m.releases && !m.release_polls);
        else {
            assert(result==A733_PERF_RESULT_STOPPED && p.retained==BIT(G) && (p.held&BIT(G)) && !m.releases);
            assert(a733_perf_request(&p,A733_PERF_COMMAND_READ,0,0)==A733_PERF_ACTIVE);
        }
        ++cases;
    }
    printf("{\"cases\":%u,\"boot_switch_us\":%llu,\"high_to_low_us\":%llu,\"low_to_high_us\":%llu,"
        "\"cpu_changes_per_switch\":%u,\"pmic_writes_per_switch\":%u,\"npu_hold_after_stop_us\":%llu,"
        "\"busy_refusal_us\":%llu,\"step_period_us\":%u}\n",cases,(unsigned long long)boot_us,
        (unsigned long long)low_us,(unsigned long long)high_us,full_switch_cpu_changes,full_switch_pmic_writes,
        (unsigned long long)npu_hold_after_stop,(unsigned long long)busy_us,STEP_US);
    puts("PASS performance-mode engine: per-bin table, DSU rule, switch order, holds, refusals and failures");
    return 0;
}
