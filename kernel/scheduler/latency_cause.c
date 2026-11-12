/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "latency_cause.h"
#ifdef SUNSTONE_LATENCY_CAUSE
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
extern void *memset(void *,int,size_t);
extern void *memcpy(void *,const void *,size_t);

#define READ(reg) ({ uint64_t v; __asm__ volatile("mrs %0, " #reg : "=r"(v) :: "memory"); v; })
#define WRITE(reg,v) __asm__ volatile("msr " #reg ", %0" :: "r"((uint64_t)(v)) : "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")
#define CC_ENABLE (UINT64_C(1) << 31)
#define DEPTHS (configUNIQUE_INTERRUPT_PRIORITIES + 1u)
/* The A733 configuration defines the SGI value in FreeRTOSConfig.h. The QEMU
 * model relies on the port's default in port.c, which this file cannot see, so
 * the same default (target-list bit, Aff1 zero) is repeated here. */
#ifndef portSGI1R_VALUE
#define portSGI1R_VALUE(interrupt, core) \
    ((uint64_t)(((1UL << (core)) & 0xFFFFu) | (((uint64_t)(interrupt) & 0xFu) << 24u)))
#endif
_Static_assert(configNUMBER_OF_CORES == SS_LATENCY_CAUSE_CORES, "cause topology");
_Static_assert(portTASK_REGISTER_EL1_BIT == 56, "cause task tag");
struct sgi_scope {
    uint64_t begin, irq, sends;
    unsigned nesting, target;
};
struct cause_state {
    _Atomic unsigned ready, frozen;
    unsigned depth, tick_depth, release_depth, release_target, phase;
    unsigned state_depth, state_target, mask_depth;
    struct ss_latency_cause_tick tick;
    struct sgi_scope sgi[DEPTHS];
};
static struct cause_state state[SS_LATENCY_CAUSE_CORES];
static struct ss_latency_cause_report report;
static _Atomic unsigned armed, initialized, bench_requested, bench_completed;
static _Alignas(64) uint64_t bench_store;
static uint64_t bench_irq_before;
static uint64_t observer_sample[SS_LATENCY_CAUSE_SAMPLES];

static unsigned own(void)
{ return (unsigned)((READ(tpidrro_el0) >> 48) & 255); }
static bool own_el1(unsigned core)
{
    uint64_t tag=READ(tpidrro_el0);
    return core<SS_LATENCY_CAUSE_CORES && ((tag>>56)&1) &&
        ((tag>>48)&255)==core && (READ(tpidr_el1)&255)==core;
}
static bool active(unsigned core)
{
    return core<SS_LATENCY_CAUSE_CORES && atomic_load_explicit(&armed,memory_order_acquire) &&
        atomic_load_explicit(&state[core].ready,memory_order_acquire) &&
        !atomic_load_explicit(&state[core].frozen,memory_order_acquire);
}
static uint64_t cycles(void)
{ ISB(); return READ(pmccntr_el0); }
static void add(uint64_t *p,uint64_t v)
{ __atomic_fetch_add(p,v,__ATOMIC_RELAXED); }
static void maximum(uint64_t *p,uint64_t v)
{ __asm__ volatile("ldumax %x0, xzr, [%1]" :: "r"(v),"r"(p) : "memory"); }

void ss_latency_cause_arm(void)
{
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&initialized,&expected,1,memory_order_acq_rel,memory_order_acquire)) return;
    report=(struct ss_latency_cause_report){.magic=SS_LATENCY_CAUSE_MAGIC,
        .version=SS_LATENCY_CAUSE_VERSION,.bytes=sizeof(report),.cores=SS_LATENCY_CAUSE_CORES,
        .phases=SS_LATENCY_CAUSE_PHASES,.cases=SS_CAUSE_CASES,.samples=SS_LATENCY_CAUSE_SAMPLES,
        .flags=SS_CAUSE_RAW|SS_CAUSE_COMPONENT_OBSERVER_INCLUDED|SS_CAUSE_BENCH_IRQ_MASKED};
    for (unsigned c=0;c<SS_LATENCY_CAUSE_CORES;++c) report.core[c].status=SS_CAUSE_UNARMED;
    atomic_store_explicit(&armed,1,memory_order_release);
}
bool ss_latency_cause_core_ready(unsigned core)
{
    return core<SS_LATENCY_CAUSE_CORES && atomic_load_explicit(&state[core].ready,memory_order_acquire) &&
        !atomic_load_explicit(&state[core].frozen,memory_order_acquire);
}
uint64_t ss_latency_cause_irq_count(unsigned core)
{ return core<SS_LATENCY_CAUSE_CORES?__atomic_load_n(&report.core[core].irq_count,__ATOMIC_RELAXED):0; }
void ss_latency_cause_bench_request(void)
{ atomic_store_explicit(&bench_requested,1,memory_order_release); }
bool ss_latency_cause_bench_done(void)
{ return atomic_load_explicit(&bench_completed,memory_order_acquire)!=0; }
void ss_latency_cause_freeze(unsigned core)
{
    if (core>=SS_LATENCY_CAUSE_CORES || own()!=core) return;
    report.core[core].frozen=1;
    atomic_store_explicit(&state[core].frozen,1,memory_order_release);
}
bool ss_latency_cause_snapshot(void *out,size_t bytes)
{
    if (bytes<sizeof(report)) return false;
    for (unsigned c=0;c<SS_LATENCY_CAUSE_CORES;++c) {
        if (!atomic_load_explicit(&state[c].frozen,memory_order_acquire)) {
            struct ss_latency_cause_report *r=out;
            memset(r,0,sizeof(*r));
            r->magic=SS_LATENCY_CAUSE_MAGIC;r->version=SS_LATENCY_CAUSE_VERSION;r->bytes=sizeof(*r);
            r->cores=SS_LATENCY_CAUSE_CORES;r->phases=SS_LATENCY_CAUSE_PHASES;
            r->cases=SS_CAUSE_CASES;r->samples=SS_LATENCY_CAUSE_SAMPLES;
            r->flags=SS_CAUSE_SNAPSHOT_INCOMPLETE;
            return false;
        }
    }
    atomic_store_explicit(&armed,0,memory_order_release);
    memcpy(out,&report,sizeof(report));
    return true;
}
static void configure(unsigned core)
{
    struct ss_latency_cause_core *r=&report.core[core];
    uint64_t saved=READ(daif);__asm__ volatile("msr daifset, #2" ::: "memory");
    uint64_t pmcr=READ(pmcr_el0),n=(pmcr>>11)&31;
    uint64_t all=((UINT64_C(1)<<n)-1)|CC_ENABLE;
    WRITE(pmcntenclr_el0,all);WRITE(pmintenclr_el1,all);WRITE(pmovsclr_el0,all);
    WRITE(pmccfiltr_el0,0);WRITE(pmuserenr_el0,4);
    WRITE(pmcr_el0,(pmcr|UINT64_C(0x41))&~UINT64_C(0x0e));
    WRITE(pmccntr_el0,0);WRITE(pmcntenset_el0,CC_ENABLE);ISB();
    r->midr=READ(midr_el1);r->mpidr=READ(mpidr_el1);r->pmcr=READ(pmcr_el0);
    r->userenr=READ(pmuserenr_el0);r->filter=READ(pmccfiltr_el0);
    r->enabled=READ(pmcntenset_el0);r->overflow=READ(pmovsclr_el0);
    r->icc_ctlr=READ(icc_ctlr_el1);r->pmr=READ(icc_pmr_el1);
    r->status=((r->pmcr&0x4f)==0x41 && r->userenr==4 && !r->filter &&
        (r->enabled&all)==CC_ENABLE && !(READ(pmintenset_el1)&all) && !(r->overflow&all))?
        SS_CAUSE_OK:SS_CAUSE_CONFIG;
    if (r->status==SS_CAUSE_OK) {
        r->cycle_begin=cycles();r->tick_begin=READ(cntpct_el0);r->ready=1;
        atomic_store_explicit(&state[core].ready,1,memory_order_release);
    }
    WRITE(daif,saved);
}
void ss_latency_cause_irq_begin(unsigned core,unsigned interrupt)
{
    if (!atomic_load_explicit(&armed,memory_order_acquire) || !own_el1(core) ||
        atomic_load_explicit(&state[core].frozen,memory_order_acquire)) return;
    if (!atomic_load_explicit(&state[core].ready,memory_order_acquire)) {
        if (report.core[core].status==SS_CAUSE_CONFIG) return;
        configure(core);
    }
    if (!active(core)) return;
    ++state[core].depth;
    add(&report.core[core].irq_count,1);
    if (!interrupt) add(&report.core[core].sgi_irq_count,1);
}
void ss_latency_cause_irq_end(unsigned core)
{
    if (!active(core) || !own_el1(core) || !state[core].depth) return;
    uint64_t saved=READ(daif);__asm__ volatile("msr daifset, #2" ::: "memory");
    report.core[core].cycle_end=cycles();report.core[core].tick_end=READ(cntpct_el0);
    report.core[core].overflow=READ(pmovsclr_el0);
    if (report.core[core].overflow&CC_ENABLE) report.core[core].status=SS_CAUSE_CONFIG;
    --state[core].depth;
    WRITE(daif,saved);
}
void ss_latency_cause_tick_begin(void)
{
    if (!active(0) || !own_el1(0) || state[0].depth!=1 || state[0].tick_depth) return;
    struct cause_state *s=&state[0];
    s->tick=(struct ss_latency_cause_tick){0};s->phase=SS_LATENCY_CAUSE_PHASES;
    s->tick_depth=s->depth;
    uint64_t saved=READ(daif);__asm__ volatile("msr daifset, #2" ::: "memory");
    s->tick.interrupted=ss_latency_cause_irq_count(0);
    s->tick.begin_tick=READ(cntpct_el0);s->tick.begin_cycles=cycles();
    WRITE(daif,saved);
}
void ss_latency_cause_release_begin(unsigned target,unsigned phase)
{
    struct cause_state *s=&state[0];
    if (target>=SS_LATENCY_CAUSE_CORES || !active(0) || own()!=0 ||
        !s->tick_depth || s->depth!=s->tick_depth || s->release_depth) return;
    s->phase=phase;s->release_target=target;s->release_depth=s->depth;
    s->tick.target[target].release_begin=cycles();
}
void ss_latency_cause_release_end(unsigned target)
{
    uint64_t end;
    struct cause_state *s=&state[0];
    if (target>=SS_LATENCY_CAUSE_CORES || !active(0) || own()!=0 ||
        s->release_depth!=s->depth || !s->release_depth || s->release_target!=target) return;
    end=cycles();
    struct ss_latency_cause_tick_target *t=&s->tick.target[target];
    t->release_end=end;s->tick.release_mask|=UINT64_C(1)<<target;
    if (s->phase<SS_LATENCY_CAUSE_PHASES) {
        struct ss_latency_cause_target *r=&report.target[s->phase][target];
        uint64_t span=end-t->release_begin;
        add(&r->release_count,1);add(&r->release_total,span);maximum(&r->release_max,span);
        add(&r->release_sgi_total,t->release_sgi_cycles);maximum(&r->release_sgi_max,t->release_sgi_cycles);
        add(&r->release_sends,t->release_sends);
    }
    s->release_depth=0;
}
void ss_latency_cause_state_begin(unsigned target)
{
    struct cause_state *s=&state[0];
    if (target>=SS_LATENCY_CAUSE_CORES || !active(0) || own()!=0 ||
        !s->tick_depth || s->depth!=s->tick_depth || s->state_depth) return;
    s->state_target=target;s->state_depth=s->depth;
    s->tick.target[target].state_begin=cycles();
}
void ss_latency_cause_state_end(unsigned target,unsigned ds,uint64_t deadline,uint64_t slice,bool due)
{
    uint64_t end;
    struct cause_state *s=&state[0];
    if (target>=SS_LATENCY_CAUSE_CORES || !active(0) || own()!=0 ||
        !s->state_depth || s->state_depth!=s->depth || target!=s->state_target) return;
    end=cycles();
    struct ss_latency_cause_tick_target *t=&s->tick.target[target];
    t->state_end=end;t->deadline_state=ds;t->deadline=deadline;t->slice_deadline=slice;t->due=due;
    s->tick.state_mask|=UINT64_C(1)<<target;
    if (s->phase<SS_LATENCY_CAUSE_PHASES) {
        struct ss_latency_cause_target *r=&report.target[s->phase][target];uint64_t span=end-t->state_begin;
        add(&r->state_count,1);add(&r->state_total,span);maximum(&r->state_max,span);
        add(&r->state_sgi_total,t->state_sgi_cycles);maximum(&r->state_sgi_max,t->state_sgi_cycles);
        add(&r->state_sends,t->state_sends);
    }
    s->state_depth=0;
}
void ss_latency_cause_mask_tail_begin(void)
{
    struct cause_state *s=&state[0];
    if (!active(0) || own()!=0 || !s->release_depth || s->depth!=s->release_depth || s->mask_depth) return;
    s->mask_depth=s->depth;s->tick.target[s->release_target].mask_tail_begin=cycles();
}
void ss_latency_cause_mask_tail_end(void)
{
    struct cause_state *s=&state[0];
    if (!active(0) || own()!=0 || !s->mask_depth || s->depth!=s->mask_depth) return;
    uint64_t end=cycles();
    struct ss_latency_cause_tick_target *t=&s->tick.target[s->release_target];t->mask_tail_end=end;
    if (s->phase<SS_LATENCY_CAUSE_PHASES) {
        struct ss_latency_cause_target *r=&report.target[s->phase][s->release_target];
        add(&r->mask_tail_total,end-t->mask_tail_begin);maximum(&r->mask_tail_max,end-t->mask_tail_begin);
    }
    s->mask_depth=0;
}
void ss_latency_cause_sgi_begin(unsigned target)
{
    unsigned core=own();
    if (!active(core) || target>=SS_LATENCY_CAUSE_CORES || state[core].depth>=DEPTHS) return;
    struct sgi_scope *p=&state[core].sgi[state[core].depth];
    if (p->nesting++) return;
    p->target=target;p->sends=0;p->irq=ss_latency_cause_irq_count(core);p->begin=cycles();
}
void ss_latency_cause_sgi_sent(unsigned target)
{
    unsigned core=own();
    if (!active(core) || target>=SS_LATENCY_CAUSE_CORES || state[core].depth>=DEPTHS) return;
    struct sgi_scope *p=&state[core].sgi[state[core].depth];
    if (p->nesting && p->target==target) ++p->sends;
}
void ss_latency_cause_sgi_end(unsigned target)
{
    unsigned core=own();
    if (!active(core) || target>=SS_LATENCY_CAUSE_CORES || state[core].depth>=DEPTHS) return;
    struct cause_state *s=&state[core];struct sgi_scope *p=&s->sgi[s->depth];
    if (!p->nesting) return;
    if (--p->nesting || p->target!=target) return;
    uint64_t end=cycles(),span=end-p->begin;
    if (!p->sends) return;
    struct ss_latency_cause_sgi *r=&report.sgi[core][target];
    add(&r->count,p->sends);add(&r->total_cycles,span);maximum(&r->max_cycles,span);
    if (p->irq!=ss_latency_cause_irq_count(core)) add(&r->interrupted,1);
    if (core || !s->tick_depth || s->depth!=s->tick_depth) return;
    if (s->release_depth && s->release_target==target) {
        s->tick.target[target].release_sgi_cycles+=span;s->tick.target[target].release_sends+=p->sends;
    } else if (s->state_depth && s->state_target==target) {
        s->tick.target[target].state_sgi_cycles+=span;s->tick.target[target].state_sends+=p->sends;
    }
}

/* x0 selects the case, x1 the resident Normal-memory line, x2 the production
 * SGI value, x3 the unchanged PMR. Every body shares the same ISB endpoints.
 * No stack or report store occurs between the two PMCCNTR reads. Raw SGI+DSB
 * does not have the preceding context synchronization of the SYNC cases. */
__attribute__((naked,noinline)) uint64_t ss_latency_cause_bench_leaf(unsigned which,uint64_t *line,uint64_t sgi,uint64_t pmr)
{
    __asm__ volatile(
        "mrs x4, daif\n msr daifset, #2\n ldr x5, [x1]\n add x5, x5, #1\n dsb sy\n isb\n dsb ishst\n"
        "adr x7, 80f\n ldrsw x6, [x7, w0, uxtw #2]\n add x7, x7, x6\n br x7\n"
        ".macro CASE label, body:vararg\n \\label:\n isb\n mrs x6, pmccntr_el0\n \\body\n b 90f\n .endm\n"
        "CASE 1, nop\n CASE 2, isb\n CASE 3, dsb ish\n CASE 4, dsb sy\n"
        "CASE 5, str x5, [x1]\n"
        "6: isb\n mrs x6, pmccntr_el0\n str x5, [x1]\n dsb ish\n b 90f\n"
        "7: isb\n mrs x6, pmccntr_el0\n str x5, [x1]\n dsb sy\n b 90f\n"
        "CASE 8, msr icc_sgi1r_el1, x2\n"
        "9: isb\n mrs x6, pmccntr_el0\n msr icc_sgi1r_el1, x2\n dsb ish\n b 90f\n"
        "10: isb\n mrs x6, pmccntr_el0\n msr icc_sgi1r_el1, x2\n dsb sy\n b 90f\n"
        "CASE 11, msr icc_pmr_el1, x3\n"
        "12: isb\n mrs x6, pmccntr_el0\n msr icc_sgi1r_el1, x2\n isb\n b 90f\n"
        "13: isb\n mrs x6, pmccntr_el0\n msr icc_sgi1r_el1, x2\n isb\n dsb ish\n b 90f\n"
        "14: isb\n mrs x6, pmccntr_el0\n msr icc_sgi1r_el1, x2\n isb\n dsb sy\n b 90f\n"
        "15: isb\n mrs x6, pmccntr_el0\n msr icc_pmr_el1, x3\n dsb sy\n b 90f\n"
        "90: isb\n mrs x7, pmccntr_el0\n sub x0, x7, x6\n dsb sy\n isb\n msr daif, x4\n ret\n"
        ".purgem CASE\n .balign 4\n 80:\n"
        ".word 1b-80b,2b-80b,3b-80b,4b-80b,5b-80b,6b-80b,7b-80b\n"
        ".word 8b-80b,9b-80b,10b-80b,11b-80b,12b-80b,13b-80b,14b-80b,15b-80b\n");
}
static void bench_step(void)
{
    if (!atomic_load_explicit(&bench_requested,memory_order_acquire) ||
        atomic_load_explicit(&bench_completed,memory_order_acquire)) return;
    report.bench.requested=1;
    if (report.bench.pending) {
        uint64_t observed=__atomic_load_n(&report.core[1].sgi_irq_count,__ATOMIC_ACQUIRE);
        if (observed==bench_irq_before) { ++report.bench.delivery_wait_ticks;return; }
        report.bench.sgi_observed+=observed-bench_irq_before;report.bench.pending=0;
    }
    for (unsigned c=0;c<SS_CAUSE_CASES;++c) {
        unsigned sample=(unsigned)report.bench.count[c];
        if (sample>=SS_LATENCY_CAUSE_SAMPLES) continue;
        if (c==SS_CAUSE_BLANK) {
            /* The real empty release hooks retain their begin store and call
             * overhead. No notification or SGI runs between them. The measured
             * workload already stopped, and no phase aggregate is changed. */
            state[0].tick_depth=state[0].depth;
            ss_latency_cause_release_begin(0,SS_LATENCY_CAUSE_PHASES);
            ss_latency_cause_release_end(0);
            state[0].tick_depth=0;
            observer_sample[sample]=state[0].tick.target[0].release_end-state[0].tick.target[0].release_begin;
            ++report.bench.observer_count;
        }
        bool sends=c==SS_CAUSE_SGI || c==SS_CAUSE_SGI_ISH || c==SS_CAUSE_SGI_SY ||
            c==SS_CAUSE_SGI_SYNC || c==SS_CAUSE_SGI_SYNC_ISH || c==SS_CAUSE_SGI_SYNC_SY;
        if (sends) bench_irq_before=__atomic_load_n(&report.core[1].sgi_irq_count,__ATOMIC_ACQUIRE);
        uint64_t value=ss_latency_cause_bench_leaf(c,&bench_store,portSGI1R_VALUE(0,1),READ(icc_pmr_el1));
        if (sends) { ++report.bench.sgi_sends;report.bench.pending=1; }
        report.bench.cycles[c][sample]=value;report.bench.count[c]=sample+1;
        if (c+1==SS_CAUSE_CASES && sample+1==SS_LATENCY_CAUSE_SAMPLES) {
            for (unsigned i=1;i<SS_LATENCY_CAUSE_SAMPLES;++i) {
                uint64_t v=observer_sample[i];unsigned j=i;
                while (j && observer_sample[j-1]>v) { observer_sample[j]=observer_sample[j-1];--j; }
                observer_sample[j]=v;
            }
            report.bench.observer_min=observer_sample[0];
            report.bench.observer_median=observer_sample[SS_LATENCY_CAUSE_SAMPLES/2-1];
            report.bench.observer_max=observer_sample[SS_LATENCY_CAUSE_SAMPLES-1];
            report.bench.done=1;atomic_store_explicit(&bench_completed,1,memory_order_release);
        }
        return;
    }
}
void ss_latency_cause_tick_end(void)
{
    struct cause_state *s=&state[0];
    if (!active(0) || !own_el1(0) || !s->tick_depth || s->depth!=s->tick_depth) return;
    uint64_t saved=READ(daif);__asm__ volatile("msr daifset, #2" ::: "memory");
    uint64_t end=cycles(),tick=READ(cntpct_el0);
    s->tick.end_cycles=end;s->tick.end_tick=tick;
    s->tick.interrupted=ss_latency_cause_irq_count(0)-s->tick.interrupted;
    WRITE(daif,saved);
    uint64_t count=report.tick.count+1;
    if (s->tick.release_mask==255 && s->tick.state_mask==255) {
        if (end-s->tick.begin_cycles>report.tick.end_cycles-report.tick.begin_cycles) report.tick=s->tick;
        report.tick.count=count;
    }
    s->tick_depth=0;
    bench_step();
}
#endif
