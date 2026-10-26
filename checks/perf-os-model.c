/* SPDX-License-Identifier: Apache-2.0 */
/* Host model of the OS performance-mode owner (apps/os/perf.c), compiled as
 * one unit with it so the report's sequence word can be driven. The shims in
 * checks/perf-os-host stand in for the OS headers; the engine and the CPU
 * clock driver's tables are the actual code. The CPU clock driver's request
 * and burst, the PMIC provider's snapshot calls, the rail ledger, the thermal
 * policy and the NPU hooks are modeled: a CPU change ends after a few owner
 * steps and writes the rail before the clock when raising and after it when
 * lowering; a thermal transition can hold the driver; the NPU retune answers
 * after two steps once its session has stopped.
 *
 * checks/perf-mode.py copies apps/os/perf.c and these shims into its output
 * directory and builds this file there, with and without
 * SUNSTONE_A733_OS_NPU. Output: one JSON line, then PASS. */
#define a733_cpu_clock_request fake_cpu_clock_request
#define a733_cpu_clock_report fake_cpu_clock_report
#define a733_rail_acquire fake_rail_acquire
#define a733_rail_release fake_rail_release
#define a733_pmic_submit fake_pmic_submit
#define a733_pmic_result fake_pmic_result
#define a733_pmic_reap fake_pmic_reap
#define a733_thermal_policy_level fake_policy_level
#define a733_thermal_policy_seen fake_policy_seen
#define a733_counter_clock_now fake_counter_now
#define OS_PERF_BOOT_MODE 1
#include "perf.c"
#include <assert.h>
#include <stdio.h>

struct board_shim board={{UINT64_C(0x1122334455667788),7}};
struct providers_shim providers;
enum { L, B, D, G, N };
static struct {
    uint64_t now;
    uint32_t mhz[5], mv[5];
    enum a733_thermal_level level; unsigned seen;
    /* The driver: a transition's owner (1 the service, 2 thermal), its
     * domain, target and the steps it still needs; the result that the next
     * burst returns. */
    unsigned active, block, steps, result_pending; uint32_t target;
    unsigned requests, busy_answers, thermal_completions, mmio_reads;
    /* Rail ledger and PMIC provider. */
    unsigned leases, outstanding, polls, snapshots, rail_busy, submit_busy, result_stuck, reap_busy, release_busy;
    /* NPU. */
    unsigned npu_ready, npu_stopped, retunes, retune_steps, retune_stuck; uint32_t retune_mhz; unsigned retune_seq;
} m;
static struct a733_cpu_clock cpu;
static struct a733_thermal_policy policy;

int fake_counter_now(void *c,uint64_t *t){assert(c==&providers.shared.counter);*t=m.now;return 1;}
enum a733_thermal_level fake_policy_level(const struct a733_thermal_policy *p){assert(p==&policy);return m.level;}
unsigned fake_policy_seen(const struct a733_thermal_policy *p){assert(p==&policy);return m.seen;}
static unsigned domain_block(enum a733_cpu_domain d){return d==A733_CPU_LITTLE ? L : d==A733_CPU_BIG ? B : D;}
static uint32_t row_mv(enum a733_cpu_domain d,uint32_t mhz)
{
    unsigned n;const struct a733_cpu_opp *t=a733_cpu_clock_opps(d,&n);
    for(unsigned i=0;i<n;++i)if(t[i].mhz==mhz)return a733_cpu_clock_opp_mv(&t[i],4);
    return 0;
}
enum a733_cpu_clock_status fake_cpu_clock_request(struct a733_cpu_clock *c,enum a733_cpu_domain d,uint32_t mhz,uint64_t deadline)
{
    assert(c==&cpu && deadline>m.now);
    if(m.active)return ++m.busy_answers,A733_CPU_CLOCK_BUSY;
    assert(!m.leases); /* The readback's shared lease is released before a change. */
    ++m.requests;--cpu.changes;
    m.active=1;m.block=domain_block(d);m.target=mhz;m.steps=4;
    return A733_CPU_CLOCK_PENDING;
}
int fake_cpu_clock_report(int (*read)(void *,uintptr_t,uint32_t *),void *v,struct a733_cpu_clock_report *r)
{
    (void)read;(void)v;
    memset(r,0,sizeof(*r));r->oscillator=0x10000002;r->dcxo_hz=26000000;r->sid_word=0x496;r->bin=4;r->binned=1;
    for(unsigned d=0;d<3;++d){r->domain[d].decoded=1;r->domain[d].source=3;r->domain[d].hz=(uint64_t)m.mhz[domain_block(d)]*1000000u;}
    return 1;
}
/* The driver's burst, once per owner step. */
static int burst(void)
{
    if(!m.active)return A733_CPU_CLOCK_READY;
    if(--m.steps)return A733_CPU_CLOCK_PENDING;
    const unsigned b=m.block,owner=m.active;
    const int up=m.target>m.mhz[b];
    if(b!=D) {
        const enum a733_cpu_domain d=b==L ? A733_CPU_LITTLE : A733_CPU_BIG;
        uint32_t mv=row_mv(d,m.target);
        if(b==L && row_mv(A733_CPU_DSU,m.mhz[D])>mv)mv=row_mv(A733_CPU_DSU,m.mhz[D]);
        if(up && mv>m.mv[b])m.mv[b]=mv;
        assert(m.mv[b]>=row_mv(d,m.target)); /* Voltage before a raise. */
        m.mhz[b]=m.target;
        if(!up && owner==1 && mv<m.mv[b])m.mv[b]=mv;
        if(b==L)m.mv[D]=m.mv[L];
    } else {
        assert(m.mv[L]>=row_mv(A733_CPU_DSU,m.target)); /* DCDC5 already covers the DSU. */
        m.mhz[D]=m.target;
    }
    m.active=0;
    if(owner==2)++m.thermal_completions;
    return A733_CPU_CLOCK_COMPLETE;
}
enum a733_rail_result fake_rail_acquire(struct a733_rail_leases *l,uint64_t owner,a733_rail_mask_t shared,
    a733_rail_mask_t exclusive,uint64_t deadline,struct a733_rail_token *t)
{
    assert(l==&providers.shared.rails && owner==cpu.config.owner && shared && !exclusive && deadline>m.now && !m.leases);
    if(m.active || m.rail_busy)return A733_RAIL_BUSY; /* The driver holds DCDC3 or DCDC5 exclusively. */
    ++m.leases;*t=(struct a733_rail_token){1,owner,shared,0};return A733_RAIL_PENDING;
}
enum a733_rail_result fake_rail_release(struct a733_rail_leases *l,const struct a733_rail_token *t,uint64_t deadline)
{
    assert(l==&providers.shared.rails && t->id==1 && m.leases==1 && !m.outstanding);
    if(m.release_busy){--m.release_busy;return A733_RAIL_BUSY;}
    if(m.now>=deadline)return A733_RAIL_EXPIRED;
    m.leases=0;return A733_RAIL_READY;
}
static enum a733_rail requested;
enum a733_pmic_status fake_pmic_submit(struct a733_pmic *p,uint64_t owner,const struct a733_rail_token *t,
    enum a733_rail rail,uint64_t deadline,struct a733_pmic_request *r)
{
    assert(p==&providers.shared.pmic && owner==cpu.config.owner && t->id==1 && deadline>m.now && !m.outstanding);
    if(m.submit_busy)return A733_PMIC_BUSY;
    m.outstanding=1;m.polls=2;requested=rail;r->id=9;return A733_PMIC_PENDING;
}
enum a733_pmic_status fake_pmic_result(struct a733_pmic *p,const struct a733_pmic_request *r,struct a733_pmic_snapshot *s)
{
    assert(p==&providers.shared.pmic && r->id==9 && m.outstanding);
    if(m.result_stuck)return A733_PMIC_BUSY;
    if(m.polls){--m.polls;return A733_PMIC_PENDING;}
    const unsigned b=requested==A733_RAIL_DCDC5 ? L : requested==A733_RAIL_DCDC3 ? B : requested==A733_RAIL_DCDC4 ? G : N;
    *s=(struct a733_pmic_snapshot){.rail=requested,.enabled=1,.decoded=1,.programmed_uv=m.mv[b]*1000u};
    ++m.snapshots;return A733_PMIC_COMPLETE;
}
enum a733_pmic_status fake_pmic_reap(struct a733_pmic *p,const struct a733_pmic_request *r)
{assert(p==&providers.shared.pmic && r->id==9 && m.outstanding);if(m.reap_busy){--m.reap_busy;return A733_PMIC_BUSY;}m.outstanding=0;return A733_PMIC_COMPLETE;}
/* CCU words: the 24 MHz reference, PERI0, PLL_GPU0 disabled with the GPU
 * gated, and PLL_NPU at the NPU's rate with its module clock gated once
 * stopped. */
uint32_t a733_mmio_read32(uintptr_t a)
{
    ++m.mmio_reads;
    switch(a-CCU) {
    case 0x000:return 0xf8000000u|(99u<<16)|(99u<<8);
    case 0x0a0:return 0xfe000000u|(1u<<20)|(2u<<16)|(99u<<8)|(4u<<2);
    case 0x0e0:return 0x01105300u;
    case 0xb20:return 0;
    case 0x2a0:return m.mhz[N]==852 ? 0xf8104600u : 0xf8305100u;
    case 0xb00:return m.npu_stopped ? 0 : 0x80000000u;
    default:assert(0);return 0;
    }
}
#ifdef SUNSTONE_A733_OS_NPU
int os_npu_perf_ready(void){return (int)m.npu_ready;}
int os_npu_perf_stopped(void){return m.npu_ready && m.npu_stopped;}
int os_npu_perf_retune(unsigned mhz,unsigned sequence)
{
    assert(m.npu_stopped && (mhz==492 || mhz==852) && sequence);
    ++m.retunes;m.retune_mhz=mhz;m.retune_seq=sequence;m.retune_steps=2;return 1;
}
unsigned os_npu_perf_answer(unsigned sequence)
{
    if(sequence!=m.retune_seq || m.retune_stuck)return 0;
    if(m.retune_steps){if(!--m.retune_steps)m.mhz[N]=m.retune_mhz;else return 0;}
    return 1;
}
#endif
/* One owner step: the thermal task's burst, then the service. */
static int tick(void)
{
    m.now+=1000;
    return os_perf_step(&cpu,&policy,burst());
}
static struct os_perf_report usb(unsigned command,uint32_t selection,uint64_t epoch)
{
    uint8_t raw[OS_PERF_REPORT_BYTES];struct os_perf_report r;
    os_perf_usb(command,selection,epoch,raw);memcpy(&r,raw,sizeof(r));
    assert(r.magic==OS_PERF_REPORT_MAGIC && r.version==1 && r.bytes==OS_PERF_REPORT_BYTES &&
        r.epoch==board.session.epoch && r.generation==7);
    return r;
}
/* Run until the command numbered `number` has completed. */
static struct os_perf_report wait(uint32_t number,unsigned limit)
{
    for(unsigned i=0;i<limit;++i) {
        (void)tick();
        struct os_perf_report r=usb(OS_PERF_REPORT,0,0);
        if(r.completed==number && !r.state)return r;
    }
    fprintf(stderr,"wait number=%u phase=%u engine=%u accepted=%u completed=%u busy=%u cancelled=%u failed=%u\n",number,perf.phase,perf.engine.phase,perf.accepted,perf.completed,perf.cpu_busy,perf.cancelled,perf.failed);
    assert(0);return usb(OS_PERF_REPORT,0,0);
}
#ifdef SUNSTONE_A733_OS_NPU
#define OWNED 0x13u
#else
#define OWNED 0x03u
#endif
int main(void)
{
    unsigned cases=0;
    /* boot0's state: CPU-L and the DSU at 494 MHz, CPU-B at 1014 MHz, every
     * CPU rail at 800 mV; DCDC4 at 960 mV; the NPU at 852 MHz, running. */
    m.mhz[L]=494;m.mhz[B]=1014;m.mhz[D]=494;m.mhz[N]=852;
    m.mv[L]=m.mv[B]=m.mv[D]=800;m.mv[G]=960;m.mv[N]=800;
    cpu.config.owner=77;cpu.config.grant.clusters=3;cpu.changes=64;cpu.report.sid_word=0x496;
    m.level=A733_THERMAL_THROTTLE;m.seen=0x1f;m.npu_ready=1;
    /* Boot: nothing until NORMAL with every sensor read. */
    for(unsigned i=0;i<10;++i)assert(tick()==0);
    /* Before the first publication a report request is answered busy. */
    assert(!m.requests && !m.snapshots && usb(OS_PERF_REPORT,0,0).answer==OS_PERF_ANSWER_BUSY);
    m.level=A733_THERMAL_NORMAL;
    unsigned boot_ticks=0;
    while(!tick())++boot_ticks;
    struct os_perf_report r=usb(OS_PERF_REPORT,0,0);
    assert(m.mhz[L]==1794 && m.mhz[B]==2002 && m.mhz[D]==1274 && m.mv[L]==1000 && m.mv[B]==1050);
    assert(r.mode==A733_PERF_HIGH && !r.state && r.boot_mode==1 && r.bin==0x400 && r.owned==(OWNED|4u) &&
        r.read_ok==0x1f && r.changes_left==61 && r.block[L].mhz==1794 && r.block[B].mv==1050 &&
        r.block[G].mhz==0 && (r.stopped&8u) && r.accepted==0 && r.completed==0 && !m.leases);
    ++cases;
    /* Idle service overhead includes no register reads, snapshots or
     * requests. The mailbox and policy publication are the only inputs. */
    const unsigned idle_reads=m.mmio_reads,idle_snapshots=m.snapshots,idle_requests=m.requests;
    for(unsigned i=0;i<1000;++i)assert(tick()==1);
    assert(m.mmio_reads==idle_reads && m.snapshots==idle_snapshots && m.requests==idle_requests);
    ++cases;
    /* A switch with a stale epoch is refused by USB0 and posts nothing. */
    r=usb(OS_PERF_SWITCH,0x200|OWNED,board.session.epoch+1);
    assert(r.answer==OS_PERF_ANSWER_STALE && !r.number && !atomic_load(&mailbox));
    ++cases;
#ifdef SUNSTONE_A733_OS_NPU
    /* High to low while the NPU session runs: the switch holds until it
     * stops, then retunes the stopped NPU; a second command meanwhile is
     * answered busy. */
    r=usb(OS_PERF_SWITCH,0x200|OWNED,board.session.epoch);
    assert(r.answer==OS_PERF_ANSWER_ACCEPTED && r.number==1);
    assert(usb(OS_PERF_READ,0,0).answer==OS_PERF_ANSWER_BUSY);
    for(unsigned i=0;i<100;++i){(void)tick();assert(usb(OS_PERF_REPORT,0,0).state && !m.retunes && m.mhz[L]==1794);}
    m.npu_stopped=1;
    r=wait(1,100);
    assert(m.retunes==1 && m.mhz[N]==492 && (r.stopped&16u) && r.block[N].mhz==492);
#else
    r=usb(OS_PERF_SWITCH,0x200|OWNED,board.session.epoch);
    assert(r.answer==OS_PERF_ANSWER_ACCEPTED && r.number==1);
    assert(usb(OS_PERF_READ,0,0).answer==OS_PERF_ANSWER_BUSY);
    r=wait(1,100);
    /* The NPU is not owned in this build: a switch that names it is refused. */
#endif
    assert(r.result==A733_PERF_RESULT_DONE && !r.reason && r.mode==A733_PERF_LOW && r.command==A733_PERF_COMMAND_SWITCH);
    assert(m.mhz[L]==1196 && m.mhz[B]==1404 && m.mhz[D]==910 && m.mv[L]==800 && m.mv[B]==800 && !m.leases);
    assert(r.changes_left==58 && r.block[D].target_mhz==910);
    ++cases;
    /* A read back: numbered, completed, nothing changed. */
    const unsigned requests=m.requests;
    r=usb(OS_PERF_READ,0,0);assert(r.answer==OS_PERF_ANSWER_ACCEPTED && r.number==2);
    r=wait(2,100);assert(r.result==A733_PERF_RESULT_DONE && r.command==A733_PERF_COMMAND_READ && m.requests==requests);
    ++cases;
    /* A thermal change invalidates the cached plan before any write. */
    m.active=2;m.block=B;m.target=1014;m.steps=3;
    r=usb(OS_PERF_SWITCH,0x100|0x3u,board.session.epoch);assert(r.answer==OS_PERF_ANSWER_ACCEPTED);
    r=wait(r.number,20);
    assert(r.result==A733_PERF_RESULT_REFUSED && !r.read_ok && m.requests==requests);
    while(m.active)(void)tick();
    r=usb(OS_PERF_READ,0,0);r=wait(r.number,100);
    assert(r.read_ok==0x1f && m.thermal_completions==1);
    r=usb(OS_PERF_SWITCH,0x100|0x3u,board.session.epoch);r=wait(r.number,100);
    assert(r.result==A733_PERF_RESULT_DONE && m.mhz[L]==1794 && m.mhz[B]==2002 && m.mhz[D]==1274);
    ++cases;
    /* NORMAL returns before an in-flight change finishes. Its completion
     * cannot authorize the rest of the old plan. */
    unsigned before=m.requests;
    r=usb(OS_PERF_SWITCH,0x200|3u,board.session.epoch);
    (void)tick();assert(m.active==1);
    m.level=A733_THERMAL_THROTTLE;(void)tick();
    m.level=A733_THERMAL_NORMAL;(void)tick();assert(m.active==1);
    r=wait(r.number,100);
    assert(r.result==A733_PERF_RESULT_STOPPED && r.reason==A733_PERF_THERMAL && !r.read_ok &&
        m.requests==before+1 && !m.leases);
    r=usb(OS_PERF_SWITCH,0x100|3u,board.session.epoch);r=wait(r.number,10);
    assert(r.result==A733_PERF_RESULT_REFUSED && m.requests==before+1);
    r=usb(OS_PERF_READ,0,0);r=wait(r.number,100);assert(r.read_ok==0x1f);
    ++cases;
    /* A thermal transition during a snapshot drains that request and
     * releases its rail; it does not complete the stale readback pass. */
    r=usb(OS_PERF_READ,0,0);(void)tick();assert(m.outstanding && m.leases);
    m.level=A733_THERMAL_THROTTLE;r=wait(r.number,100);
    assert(r.reason==A733_PERF_THERMAL && !r.read_ok && !m.leases && !m.outstanding);
    m.level=A733_THERMAL_NORMAL;(void)tick();
    r=usb(OS_PERF_READ,0,0);r=wait(r.number,100);assert(r.read_ok==0x1f);
    ++cases;
    /* Refusals: the grant's changes, the thermal level, a block not owned
     * (the GPU), the DSU named. Each completes its number with nothing changed. */
    struct { const char *name; uint32_t selection; unsigned changes, level; enum a733_perf_reason reason; } refusals[]={
        {"changes",0x200|0x3u,3,A733_THERMAL_NORMAL,A733_PERF_BUDGET},
        {"thermal",0x200|0x3u,40,A733_THERMAL_THROTTLE,A733_PERF_NOT_READY},
        {"gpu",0x200|0x8u,40,A733_THERMAL_NORMAL,A733_PERF_NOT_OWNED},
        {"dsu",0x200|0x4u,40,A733_THERMAL_NORMAL,A733_PERF_REQUEST},
        {"no mode",0x003u,40,A733_THERMAL_NORMAL,A733_PERF_REQUEST}};
    for(unsigned i=0;i<sizeof(refusals)/sizeof(*refusals);++i) {
        cpu.changes=refusals[i].changes;m.level=refusals[i].level;
        const unsigned before=m.requests;
        r=usb(OS_PERF_SWITCH,refusals[i].selection,board.session.epoch);assert(r.answer==OS_PERF_ANSWER_ACCEPTED);
        r=wait(r.number,10);
        assert(r.result==A733_PERF_RESULT_REFUSED && r.reason==refusals[i].reason && m.requests==before && !m.leases);
        ++cases;
    }
    m.level=A733_THERMAL_NORMAL;cpu.changes=40;
    /* A report copied while the thermal task writes it is answered busy,
     * and a command is not posted on an unstable copy. */
    atomic_fetch_add(&report_sequence,1);
    r=usb(OS_PERF_SWITCH,0x200|0x3u,board.session.epoch);
    assert(r.answer==OS_PERF_ANSWER_BUSY && !atomic_load(&mailbox) && !r.number);
    atomic_fetch_add(&report_sequence,1);
    ++cases;
    /* Ordinary cleanup contention retries the completed snapshot, without
     * resubmitting it or renewing its deadline. */
    m.reap_busy=2;m.release_busy=2;
    unsigned snapshots=m.snapshots;
    r=usb(OS_PERF_READ,0,0);r=wait(r.number,100);
    assert(!perf.failed && !m.outstanding && !m.leases && r.read_ok==0x1f && m.snapshots==snapshots+4);
    ++cases;
    /* Provider lifecycle failures use the owner's terminal return. Each
     * independent fixture restores host state; no native recovery is tried. */
    for(unsigned failure=0;failure<5;++failure) {
        const __typeof__(perf) saved_perf=perf;const __typeof__(m) saved_m=m;
        if(failure==0)m.rail_busy=1;
        if(failure==1)m.submit_busy=1;
        if(failure==2)m.result_stuck=1;
        if(failure==3)m.reap_busy=1000;
        if(failure==4)m.release_busy=1000;
        r=usb(OS_PERF_READ,0,0);assert(r.answer==OS_PERF_ANSWER_ACCEPTED);
        int status=0;unsigned ticks=0;
        for(;ticks<800;++ticks){status=tick();r=usb(OS_PERF_REPORT,0,0);if(status<0)break;
            if(r.completed==perf.number && !r.state)break;}
        assert(ticks<800);
        if(failure==0)assert(status>=0 && !m.leases && !m.outstanding);
        else assert(status==-1 && perf.failed && m.leases &&
            (failure==1 || failure==4 ? !m.outstanding : m.outstanding) && r.reason==A733_PERF_STEP_FAILED);
        perf=saved_perf;m=saved_m;atomic_store(&mailbox,0);publish();++cases;
    }
#ifdef SUNSTONE_A733_OS_NPU
    /* An unanswered retune is unknown, even if the bootstrap did not
     * publish its own failure. The CPU owner must terminate the session. */
    r=usb(OS_PERF_READ,0,0);r=wait(r.number,100);assert(r.read_ok==0x1f);
    m.retune_stuck=1;
    r=usb(OS_PERF_SWITCH,0x100|16u,board.session.epoch);assert(r.answer==OS_PERF_ANSWER_ACCEPTED);
    int terminal=0;
    for(unsigned i=0;i<100 && !terminal;++i)terminal=tick()<0;
    r=usb(OS_PERF_REPORT,0,0);
    assert(terminal && perf.failed && r.reason==A733_PERF_STEP_FAILED && m.npu_stopped);
    ++cases;
#endif
    /* A retained driver: the report records the stop before the session ends. */
    const unsigned completed=usb(OS_PERF_REPORT,0,0).completed;
    (void)os_perf_step(&cpu,&policy,A733_CPU_CLOCK_RETAINED);
    r=usb(OS_PERF_REPORT,0,0);
    assert(r.result==A733_PERF_RESULT_STOPPED && r.reason==A733_PERF_STEP_FAILED && r.completed==completed);
    ++cases;
    printf("{\"cases\":%u,\"boot_ticks\":%u,\"requests\":%u,\"busy_answers\":%u,\"snapshots\":%u}\n",
        cases,boot_ticks,m.requests,m.busy_answers,m.snapshots);
    puts("PASS OS performance-mode owner: boot switch, USB0 answers and numbers, NPU hold, driver attribution, refusals and the report");
    return 0;
}
