/* High and low performance modes in the integrated OS image
 * (docs/designs/performance-modes.md). The thermal task, which owns the CPU
 * clock driver, runs the switch engine (drivers/power/a733_perf_mode.c): the
 * boot switch, then the commands that USB0 operation 8 posts. This file is
 * the engine's owner: CPU domain requests through the driver, the NPU's
 * retune through its bootstrap task, and the readback of every block. The
 * GPU is read back and never owned. */
#include "os.h"
#include "perf.h"
#include "../board/application.h"
#include "a733_cpu_clock.h"
#include "a733_mmio.h"
#include "a733_perf_mode.h"
#include <stdatomic.h>
#include <string.h>
#ifdef SUNSTONE_A733_OS_NPU
#include "npu.h"
#endif
#ifndef OS_PERF_BOOT_MODE
#error OS_PERF_BOOT_MODE comes from cmake/a733-os.cmake
#endif
__attribute__((used,retain)) const uint32_t os_perf_boot_mode=OS_PERF_BOOT_MODE;
__attribute__((used,retain)) const uint32_t os_perf_usb_operation=OS_PERF_OP;

#define CCU UINT32_C(0x02002000)
/* The boot switch waits at most this long for NORMAL with every sensor read,
 * inside the recognizer's 1 s wait for os_cpu_clock_ready. */
#define BOOT_WAIT_US 500000u
enum { BOOT_WAIT, BOOT_READ, BOOT_SWITCH, SERVE };
/* PMIC rails of the readback, in snapshot order. */
static const enum a733_rail rails[4]={A733_RAIL_DCDC5,A733_RAIL_DCDC3,A733_RAIL_DCDC4,A733_RAIL_DCDC2};
static struct {
    struct a733_perf engine;
    struct a733_cpu_clock *cpu;
    const struct a733_thermal_policy *policy;
    unsigned phase, initialized, failed, thermal_level, cancelled, cpu_busy;
    uint64_t boot_until;
    /* The CPU request in flight: started by the engine, finished by a burst. */
    unsigned cpu_started, cpu_pending, cpu_done, cpu_result;
    /* One readback pass: the register words, then one snapshot per rail
     * under a separate shared lease per snapshot. */
    struct a733_cpu_clock_report report;
    uint32_t ref, peri0, pll_gpu, gpu_clock, pll_npu, npu_clock;
    unsigned words_ok, stopped, requested, snapshot_ok, read_done;
    enum a733_perf_status read_status;
    uint32_t rail_mv[4];
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    unsigned npu_sequence;
    uint32_t number;     /* The command running; 0 for the boot switch. */
    uint32_t accepted, completed;
} perf;
/* USB0's command: number<<32 | command<<12 | selection; 0 when empty. */
static atomic_uint_fast64_t mailbox;
/* The report, written by the thermal task under report_sequence (odd while
 * writing) and copied by USB0. Words are atomics, so a racing copy is
 * defined; the sequence detects it. */
static _Atomic uint32_t report[OS_PERF_REPORT_BYTES/4];
static atomic_uint report_sequence;

static int now(void *v,uint64_t *t){return v==&perf && a733_counter_clock_now(&providers.shared.counter,t);}
static int read_cpu(void *v,uintptr_t a,uint32_t *out)
{(void)v;if(!a733_cpu_clock_address(a,0))return 0;*out=a733_mmio_read32(a);return 1;}
/* CCU words of the readback; the thermal start checked the CCU page's
 * mapping. Reads have no side effects. */
static uint32_t ccu(uint32_t offset){return a733_mmio_read32(CCU+offset);}
static void read_words(void)
{
    perf.words_ok=a733_cpu_clock_report(read_cpu,0,&perf.report);
    perf.ref=ccu(0x000);perf.peri0=ccu(0x0a0);perf.pll_gpu=ccu(0x0e0);perf.gpu_clock=ccu(0xb20);
    perf.pll_npu=ccu(0x2a0);perf.npu_clock=ccu(0xb00);
}
static enum a733_perf_status map(int r)
{
    switch(r) {
    case A733_CPU_CLOCK_COMPLETE:return A733_PERF_DONE;
    case A733_CPU_CLOCK_REFUSED:return A733_PERF_REFUSED;
    case A733_CPU_CLOCK_EXPIRED:return A733_PERF_EXPIRED;
    default:return A733_PERF_FAILED;
    }
}
/* One PMIC snapshot of rails[i] under the pass's shared lease. A provider
 * fault leaves the request outstanding and ends the service. */
static enum a733_perf_status snapshot(unsigned i,uint64_t deadline)
{
    uint64_t t;
    if(perf.failed || !now(&perf,&t)){perf.failed=1;return A733_PERF_FAILED;}
    const uint64_t owner=perf.cpu->config.owner;
    if(!perf.rail.id) {
        if(t>=deadline)return A733_PERF_REFUSED;
        const a733_rail_mask_t shared=A733_RAIL_BIT(rails[i]);
        enum a733_rail_result r=a733_rail_acquire(&providers.shared.rails,owner,shared,0,deadline,&perf.rail);
        if(r==A733_RAIL_BUSY)return A733_PERF_PENDING;
        if(r!=A733_RAIL_PENDING){perf.rail=(struct a733_rail_token){0};return A733_PERF_REFUSED;}
    }
    if(!perf.requested) {
        if(t>=deadline)return A733_PERF_REFUSED;
        enum a733_pmic_status s=a733_pmic_submit(&providers.shared.pmic,owner,&perf.rail,rails[i],deadline,&perf.request);
        if(s==A733_PMIC_BUSY)return A733_PERF_PENDING;
        if(s!=A733_PMIC_PENDING || !perf.request.id)return A733_PERF_REFUSED;
        perf.requested=1;
    }
    if(perf.requested==1) {
        struct a733_pmic_snapshot shot={0};
        enum a733_pmic_status s=a733_pmic_result(&providers.shared.pmic,&perf.request,&shot);
        if(s==A733_PMIC_PENDING || s==A733_PMIC_BUSY) {
            if(t<deadline)return A733_PERF_PENDING;
            perf.failed=1;return A733_PERF_FAILED;
        }
        if(s!=A733_PMIC_COMPLETE && s!=A733_PMIC_EXPIRED){perf.failed=1;return A733_PERF_FAILED;}
        perf.snapshot_ok=s==A733_PMIC_COMPLETE && shot.rail==rails[i] && shot.enabled && shot.decoded;
        if(perf.snapshot_ok)perf.rail_mv[i]=shot.programmed_uv/1000u;
        perf.requested=2;
    }
    /* Reaping may contend after completion. Keep the result and original
     * deadline; do not submit the same snapshot again. */
    enum a733_pmic_status s=a733_pmic_reap(&providers.shared.pmic,&perf.request);
    if(s==A733_PMIC_BUSY && t<deadline)return A733_PERF_PENDING;
    if(s!=A733_PMIC_COMPLETE){perf.failed=1;return A733_PERF_FAILED;}
    perf.requested=0;perf.request=(struct a733_pmic_request){0};
    return perf.snapshot_ok ? A733_PERF_DONE : A733_PERF_REFUSED;
}
static enum a733_perf_status release_rail(uint64_t deadline)
{
    if(!perf.rail.id)return A733_PERF_DONE;
    if(perf.requested)return A733_PERF_FAILED;
    enum a733_rail_result r=a733_rail_release(&providers.shared.rails,&perf.rail,deadline);
    if(r==A733_RAIL_READY){perf.rail=(struct a733_rail_token){0};return A733_PERF_DONE;}
    uint64_t t;
    if((r==A733_RAIL_BUSY || r==A733_RAIL_PENDING) && now(&perf,&t) && t<deadline)return A733_PERF_PENDING;
    perf.failed=1;return A733_PERF_FAILED;
}
static enum a733_perf_status read_block(enum a733_perf_block b,uint64_t deadline,int start,struct a733_perf_point *out)
{
    /* A thermal transition invalidates the entire pass. Finish only an
     * outstanding provider request; its token cannot be abandoned. */
    if((perf.cancelled || perf.cpu_busy || a733_thermal_policy_level(perf.policy)!=A733_THERMAL_NORMAL) && !perf.requested) {
        enum a733_perf_status r=release_rail(deadline);
        return r==A733_PERF_DONE ? A733_PERF_REFUSED : r;
    }
    /* CPU-L opens the pass: the words, then DCDC5, which the DSU shares. */
    if(b==A733_PERF_CPU_L && start){read_words();perf.rail_mv[0]=perf.rail_mv[1]=perf.rail_mv[2]=perf.rail_mv[3]=0;perf.stopped=0;}
    static const int rail_of[A733_PERF_BLOCKS]={0,1,-1,2,3};
    if(start)perf.read_done=0;
    if(!perf.read_done) {
        perf.read_status=rail_of[b]>=0 ? snapshot((unsigned)rail_of[b],deadline) : A733_PERF_DONE;
        if(perf.read_status==A733_PERF_PENDING)return A733_PERF_PENDING;
        perf.read_done=1;
    }
    enum a733_perf_status st=perf.read_status;
    if(perf.requested)return A733_PERF_FAILED;
    enum a733_perf_status released=release_rail(deadline);
    if(released!=A733_PERF_DONE)return released;
    if(perf.failed)return A733_PERF_FAILED;
    if(perf.cancelled || perf.cpu_busy)return A733_PERF_REFUSED;
    const uint32_t mv=perf.rail_mv[b==A733_PERF_DSU ? 0 : (unsigned)rail_of[b]];
    if(st!=A733_PERF_DONE || !perf.words_ok || !mv)return st==A733_PERF_DONE ? A733_PERF_REFUSED : st;
    uint32_t mhz=0;unsigned stopped=0;
    if(b<=A733_PERF_DSU) {
        const struct a733_cpu_clock_domain *d=&perf.report.domain[b==A733_PERF_CPU_L ? A733_CPU_LITTLE :
            b==A733_PERF_CPU_B ? A733_CPU_BIG : A733_CPU_DSU];
        if(!d->decoded)return A733_PERF_REFUSED;
        mhz=(uint32_t)(d->hz/1000000u);
    } else if(b==A733_PERF_GPU ? !a733_perf_gpu_mhz(perf.report.oscillator,perf.ref,perf.peri0,perf.pll_gpu,perf.gpu_clock,&mhz,&stopped) :
              !a733_perf_npu_mhz(perf.report.oscillator,perf.ref,perf.pll_npu,perf.npu_clock,&mhz,&stopped))
        return A733_PERF_REFUSED;
    if(stopped)perf.stopped|=A733_PERF_BIT(b);
    *out=(struct a733_perf_point){mhz,mv};
    return A733_PERF_DONE;
}
static enum a733_perf_status act(void *v,enum a733_perf_action a,enum a733_perf_block b,uint32_t value,
    uint64_t deadline,int start,struct a733_perf_point *out)
{
    if(v!=&perf)return A733_PERF_FAILED;
    uint64_t t;
    /* A running CPU or NPU request resolves under its own bound. All new
     * writes stop after a thermal interruption, even if NORMAL has returned. */
    if((perf.cancelled || a733_thermal_policy_level(perf.policy)!=A733_THERMAL_NORMAL) &&
       (a==A733_PERF_HOLD || (a==A733_PERF_RATE && start) ||
        (a==A733_PERF_CHANGE_CPU && (start || !perf.cpu_started))))return A733_PERF_REFUSED;
    switch(a) {
    case A733_PERF_READ:return read_block(b,deadline,start,out);
    case A733_PERF_CHANGE_CPU: {
        if(start)perf.cpu_started=perf.cpu_pending=perf.cpu_done=0;
        if(!perf.cpu_started) {
            /* A thermal transition in progress answers BUSY: retry. */
            if(!now(&perf,&t))return A733_PERF_FAILED;
            if(t>=deadline)return A733_PERF_EXPIRED;
            const enum a733_cpu_domain d=b==A733_PERF_CPU_L ? A733_CPU_LITTLE : b==A733_PERF_CPU_B ? A733_CPU_BIG : A733_CPU_DSU;
            enum a733_cpu_clock_status r=a733_cpu_clock_request(perf.cpu,d,value,deadline);
            if(r==A733_CPU_CLOCK_BUSY)return A733_PERF_PENDING;
            if(r!=A733_CPU_CLOCK_PENDING)return map(r);
            perf.cpu_started=perf.cpu_pending=1;
            return A733_PERF_PENDING;
        }
        return perf.cpu_done ? map((int)perf.cpu_result) : A733_PERF_PENDING;
    }
#ifdef SUNSTONE_A733_OS_NPU
    case A733_PERF_HOLD:
        /* The NPU changes only once its session has stopped. */
        if(b!=A733_PERF_NPU || !os_npu_perf_ready())return A733_PERF_REFUSED;
        return os_npu_perf_stopped() ? A733_PERF_DONE : A733_PERF_PENDING;
    case A733_PERF_RATE:
        if(b!=A733_PERF_NPU)return A733_PERF_REFUSED;
        if(start) {
            perf.npu_sequence=perf.npu_sequence%0xffffu+1u;
            if(!os_npu_perf_retune(value,perf.npu_sequence))return A733_PERF_REFUSED;
        }
        switch(os_npu_perf_answer(perf.npu_sequence)) {
        case 1:return A733_PERF_DONE;
        case 2:return A733_PERF_REFUSED;
        case 3:perf.failed=1;return A733_PERF_FAILED;
        default:
            /* The bootstrap answers within its bounded retune; past the
             * deadline the NPU's state is not known. */
            if(now(&perf,&t) && t<deadline)return A733_PERF_PENDING;
            perf.failed=1;return A733_PERF_FAILED;
        }
#endif
    case A733_PERF_RELEASE:return A733_PERF_DONE; /* The stopped NPU stays stopped. */
    default:return A733_PERF_REFUSED;
    }
}
static enum a733_perf_reason admit(void *v,unsigned blocks,unsigned cpu)
{
    if(v!=&perf || perf.failed || perf.cpu_busy)return A733_PERF_NOT_READY;
    if(a733_thermal_policy_level(perf.policy)!=A733_THERMAL_NORMAL || a733_thermal_policy_seen(perf.policy)!=31)
        return A733_PERF_THERMAL;
    /* Two changes stay for the driver's restores after a THROTTLE. */
    if(cpu && perf.cpu->changes<cpu+2u)return A733_PERF_BUDGET;
#ifdef SUNSTONE_A733_OS_NPU
    if((blocks&A733_PERF_BIT(A733_PERF_NPU)) && !os_npu_perf_ready())return A733_PERF_NOT_READY;
#else
    (void)blocks;
#endif
    return A733_PERF_OK;
}
static const struct a733_perf_ops ops={now,admit,act};

static void put(unsigned word,uint32_t value){atomic_store_explicit(&report[word],value,memory_order_relaxed);}
static void publish(void)
{
    const struct a733_perf_record *r=&perf.engine.record;
    const unsigned s=atomic_load_explicit(&report_sequence,memory_order_relaxed)+1u;
    atomic_store_explicit(&report_sequence,s,memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    const struct os_perf_report head={OS_PERF_REPORT_MAGIC,1,OS_PERF_REPORT_BYTES,0,0,0,0,0,
        perf.accepted,perf.completed,perf.engine.phase!=0 || perf.phase!=SERVE,r->command,r->result,r->reason,
        r->failed_block,OS_PERF_BOOT_MODE,r->mode,perf.engine.entry ? perf.engine.entry->name : 0u,
        perf.cpu->changes,r->owned,r->read_ok,perf.stopped,{{0}},r->blocks,r->mode_requested,
        (uint32_t)(r->finished_us>r->started_us ? r->finished_us-r->started_us : 0),0};
    uint32_t words[OS_PERF_REPORT_BYTES/4];
    memcpy(words,&head,sizeof(words));
    struct os_perf_block *block=(struct os_perf_block *)&words[24];
    for(unsigned b=0;b<A733_PERF_BLOCKS;++b)
        block[b]=(struct os_perf_block){r->target[b].mhz,r->target[b].mv,r->read[b].mhz,r->read[b].mv};
    /* The USB0 task fills the sequence, session and answer words itself. */
    for(unsigned i=10;i<OS_PERF_REPORT_BYTES/4;++i)put(i,words[i]);
    put(0,words[0]);put(1,words[1]);put(2,words[2]);
    atomic_store_explicit(&report_sequence,s+1u,memory_order_release);
}
static unsigned cpu_owned(void)
{
    const uint32_t clusters=perf.cpu->config.grant.clusters;
    return (clusters&1u ? A733_PERF_BIT(A733_PERF_CPU_L) : 0u)|(clusters&2u ? A733_PERF_BIT(A733_PERF_CPU_B) : 0u);
}
int os_perf_step(struct a733_cpu_clock *cpu,const struct a733_thermal_policy *policy,int burst)
{
    uint64_t t;
    if(!perf.initialized) {
        if(!cpu || !policy || !now(&perf,&t))return 1;
        perf.cpu=cpu;perf.policy=policy;perf.initialized=1;perf.boot_until=t+BOOT_WAIT_US;
        perf.thermal_level=a733_thermal_policy_level(policy);
        unsigned owned=cpu_owned();
#ifdef SUNSTONE_A733_OS_NPU
        owned|=A733_PERF_BIT(A733_PERF_NPU);
#endif
        /* A bin without an entry still reads back; every switch is refused. */
        (void)a733_perf_init(&perf.engine,cpu->report.sid_word,owned,&ops,&perf);
    }
    const unsigned level=a733_thermal_policy_level(policy);
    perf.cpu_busy=burst==A733_CPU_CLOCK_PENDING || burst==A733_CPU_CLOCK_BUSY;
    if((level!=perf.thermal_level || (!perf.cpu_pending && burst!=A733_CPU_CLOCK_READY)) &&
       (perf.engine.record.read_ok || (perf.engine.phase && !perf.cancelled))) {
        perf.engine.record.read_ok=0;perf.engine.record.mode=A733_PERF_UNKNOWN;
        if(perf.engine.phase)perf.cancelled=1;
        publish();
    }
    perf.thermal_level=level;
    /* A terminal driver result belongs to the engine only when its own
     * request started the transition. */
    if(perf.cpu_pending && burst!=A733_CPU_CLOCK_PENDING && burst!=A733_CPU_CLOCK_READY && burst!=A733_CPU_CLOCK_BUSY) {
        perf.cpu_pending=0;perf.cpu_done=1;perf.cpu_result=(unsigned)burst;
    }
    const int retained=burst==A733_CPU_CLOCK_RETAINED || burst==A733_CPU_CLOCK_INVALID;
    if(retained) {
        /* The thermal task fails the board next; the report stays in RAM. */
        perf.engine.record.result=A733_PERF_RESULT_STOPPED;perf.engine.record.reason=A733_PERF_STEP_FAILED;
        publish();return -1;
    }
    unsigned changed=0,taken=0;
    if(perf.engine.phase==0) {
        if(perf.phase==BOOT_WAIT) {
            if(!now(&perf,&t))return 1;
            if(admit(&perf,A733_PERF_CPU,0)==A733_PERF_OK){perf.phase=BOOT_READ;(void)a733_perf_request(&perf.engine,A733_PERF_COMMAND_READ,0,0);}
            else if(t>=perf.boot_until){perf.phase=SERVE;publish();return 1;}
            else return 0;
        } else if(perf.phase==BOOT_READ) {
            perf.phase=BOOT_SWITCH;
            (void)a733_perf_request(&perf.engine,A733_PERF_COMMAND_SWITCH,(enum a733_perf_mode)OS_PERF_BOOT_MODE,
                cpu_owned());
            changed=1;
        } else if(perf.phase==BOOT_SWITCH) {
            perf.phase=SERVE;publish();return 1;
        } else {
            const uint64_t word=atomic_load_explicit(&mailbox,memory_order_acquire);
            if(!word)return 1;
            perf.number=(uint32_t)(word>>32);perf.accepted=perf.number;
            const unsigned command=(unsigned)(word>>12)&15u,selection=(unsigned)word&0xfffu;
            if(command==OS_PERF_READ)perf.cancelled=0;
            (void)a733_perf_request(&perf.engine,command==OS_PERF_READ ? A733_PERF_COMMAND_READ : A733_PERF_COMMAND_SWITCH,
                command==OS_PERF_READ ? A733_PERF_UNKNOWN : (enum a733_perf_mode)(selection>>8&3u),
                command==OS_PERF_READ ? 0u : selection&A733_PERF_ALL);
            taken=1;changed=1;
        }
    }
    if(perf.engine.phase!=0) {
        (void)a733_perf_step(&perf.engine);
        changed=1;
    }
    if(perf.engine.phase==0 && perf.cancelled) {
        perf.engine.record.read_ok=0;perf.engine.record.mode=A733_PERF_UNKNOWN;
        perf.engine.record.result=A733_PERF_RESULT_STOPPED;perf.engine.record.reason=A733_PERF_THERMAL;
        perf.cancelled=0;changed=1;
    }
    if(perf.failed) {
        perf.engine.record.result=A733_PERF_RESULT_STOPPED;perf.engine.record.reason=A733_PERF_STEP_FAILED;
        publish();return -1;
    }
    if(perf.engine.phase==0 && perf.phase==SERVE && perf.number && perf.completed!=perf.number){perf.completed=perf.number;changed=1;}
    if(changed)publish();
    /* After the publication that shows the command, so USB0 answers busy
     * rather than queueing a second one. */
    if(taken)atomic_store_explicit(&mailbox,0,memory_order_release);
    return perf.phase==SERVE;
}
/* USB0 task. A stable copy of the publication, then this request's answer. */
void os_perf_usb(unsigned command,uint32_t selection,uint64_t epoch,uint8_t out[OS_PERF_REPORT_BYTES])
{
    static uint32_t next=1; /* USB0's command numbers. */
    uint32_t words[OS_PERF_REPORT_BYTES/4]={0};
    unsigned stable=0;
    for(unsigned attempt=0;attempt<4 && !stable;++attempt) {
        const unsigned s=atomic_load_explicit(&report_sequence,memory_order_acquire);
        if(s&1u)continue;
        for(unsigned i=0;i<OS_PERF_REPORT_BYTES/4;++i)words[i]=atomic_load_explicit(&report[i],memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        stable=s && atomic_load_explicit(&report_sequence,memory_order_relaxed)==s;
        words[3]=s;
    }
    struct os_perf_report *r=(struct os_perf_report *)words;
    if(!stable)memset(words,0,sizeof(words));
    r->magic=OS_PERF_REPORT_MAGIC;r->version=1;r->bytes=OS_PERF_REPORT_BYTES;
    r->epoch=board.session.epoch;r->generation=board.session.generation;
    r->answer=OS_PERF_ANSWER_REPORT;r->number=0;
    if(command!=OS_PERF_REPORT) {
        uint64_t empty=0;
        if(command==OS_PERF_SWITCH && epoch!=board.session.epoch)r->answer=OS_PERF_ANSWER_STALE;
        else if(!stable || r->state ||
                !atomic_compare_exchange_strong_explicit(&mailbox,&empty,(uint64_t)next<<32|command<<12|selection,
                    memory_order_acq_rel,memory_order_acquire))r->answer=OS_PERF_ANSWER_BUSY;
        else {r->answer=OS_PERF_ANSWER_ACCEPTED;r->number=next++;if(!next)next=1;}
    } else if(!stable)r->answer=OS_PERF_ANSWER_BUSY;
    memcpy(out,words,OS_PERF_REPORT_BYTES);
}
