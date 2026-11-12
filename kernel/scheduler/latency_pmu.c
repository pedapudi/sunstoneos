/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "latency_pmu.h"
#ifdef SUNSTONE_SCHED_PMU
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <stdbool.h>

_Static_assert(configNUMBER_OF_CORES == SS_SCHED_PMU_CORES, "one PMU owner per admitted CPU");
_Static_assert(portTASK_REGISTER_EL1_BIT == 56, "PMU entry guard follows the port task tag");
#define READ(reg) ({ uint64_t value; __asm__ volatile("mrs %0, " #reg : "=r"(value) :: "memory"); value; })
#define WRITE(reg, value) __asm__ volatile("msr " #reg ", %0" :: "r"((uint64_t)(value)) : "memory")
#define ZERO(reg) __asm__ volatile("msr " #reg ", xzr" ::: "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")
#define ENABLE UINT64_C(0x8000003f)
#define RECORD_FLAGS (SS_SCHED_PMU_RAW_COUNTS | SS_SCHED_PMU_NESTED_COUNT_UNAVAILABLE)

/* The assembly entries read cookie before a C prologue can spill. Ownership
 * remains nonzero until frozen capture and publication have both completed. */
struct pmu_state {
    _Atomic uint64_t cookie;
    uint64_t sequence, prepared_kind, bank, enable, slot, flags, begin_tick;
    _Atomic uint64_t frozen;
    uint64_t configured, next_tick_bank, status;
    uintptr_t scope;
} __attribute__((aligned(128)));
static struct pmu_state ss_sched_pmu_state[SS_SCHED_PMU_CORES];
_Static_assert(sizeof(struct pmu_state) == 128, "assembly state stride");
_Static_assert(offsetof(struct pmu_state, frozen) == 64, "assembly frozen offset");
_Static_assert(offsetof(struct pmu_state, scope) == 96, "assembly scope offset");
static struct ss_sched_pmu_report report;
static _Atomic unsigned armed, initialized;
static const uint16_t events[SS_SCHED_PMU_BANKS][SS_SCHED_PMU_EVENTS] = {
    {0x03,0x01,0x17,0x2a,0x34,0x35},
    {0x03,0x01,0x17,0x24,0x34,0x35}
};

void ss_sched_pmu_arm(void)
{
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&initialized,&expected,1,memory_order_acq_rel,memory_order_acquire)) return;
    report=(struct ss_sched_pmu_report){.magic=SS_SCHED_PMU_MAGIC,.version=SS_SCHED_PMU_VERSION,
        .bytes=sizeof(report),.cores=SS_SCHED_PMU_CORES,.kinds=SS_SCHED_PMU_KINDS,
        .banks=SS_SCHED_PMU_BANKS,.events=SS_SCHED_PMU_EVENTS,.flags=RECORD_FLAGS};
    for (unsigned c=0;c<SS_SCHED_PMU_CORES;++c) report.core[c].status=SS_SCHED_PMU_UNARMED;
    atomic_store_explicit(&armed,1,memory_order_release);
}

static int configure(unsigned core,unsigned bank)
{
    struct pmu_state *s=&ss_sched_pmu_state[core];
    struct ss_sched_pmu_core *r=&report.core[core];
    uint64_t daif=READ(daif);
    __asm__ volatile("msr daifset, #2" ::: "memory");
    uint64_t pmcr=READ(pmcr_el0);
    unsigned n=(unsigned)((pmcr>>11)&31);
    int status=SS_SCHED_PMU_OK;
    if (!s->configured) {
        r->midr=READ(midr_el1); r->counter_count=n;
        r->pmceid0=READ(pmceid0_el0); r->pmceid1=READ(pmceid1_el0);
        if (n<SS_SCHED_PMU_EVENTS) status=SS_SCHED_PMU_NO_COUNTERS;
        else {
            uint64_t all=((UINT64_C(1)<<n)-1)|(UINT64_C(1)<<31);
            WRITE(pmcntenclr_el0,all); WRITE(pmintenclr_el1,all);
            ZERO(pmuserenr_el0); ZERO(pmccfiltr_el0);
            WRITE(pmcr_el0,(pmcr|UINT64_C(0x41))&~UINT64_C(0x0e));
            ISB();
            if ((READ(pmcr_el0)&UINT64_C(0x4f))!=UINT64_C(0x41) ||
                READ(pmccfiltr_el0)!=0 || (READ(pmuserenr_el0)&15) ||
                (READ(pmintenset_el1)&all) || (READ(pmcntenset_el0)&all))
                status=SS_SCHED_PMU_CONFIG_MISMATCH;
            for (unsigned b=0;b<SS_SCHED_PMU_BANKS;++b)
                for (unsigned i=0;i<SS_SCHED_PMU_EVENTS;++i) {
                    unsigned event=events[b][i];
                    uint64_t advertised=event<32?r->pmceid0:r->pmceid1;
                    if ((advertised>>(event&31))&1) r->supported[b]|=UINT64_C(1)<<i;
                }
        }
    }
    if (status==SS_SCHED_PMU_OK && (!(s->configured&(UINT64_C(1)<<bank)) || s->bank!=bank)) {
        const uint16_t *e=events[bank];
        WRITE(pmevtyper0_el0,e[0]); WRITE(pmevtyper1_el0,e[1]);
        WRITE(pmevtyper2_el0,e[2]); WRITE(pmevtyper3_el0,e[3]);
        WRITE(pmevtyper4_el0,e[4]); WRITE(pmevtyper5_el0,e[5]);
        ISB();
        if (READ(pmevtyper0_el0)!=e[0] || READ(pmevtyper1_el0)!=e[1] ||
            READ(pmevtyper2_el0)!=e[2] || READ(pmevtyper3_el0)!=e[3] ||
            READ(pmevtyper4_el0)!=e[4] || READ(pmevtyper5_el0)!=e[5])
            status=SS_SCHED_PMU_CONFIG_MISMATCH;
        else {
            s->bank=bank; s->enable=ENABLE;
            s->configured|=UINT64_C(1)<<bank;
            r->configured_banks=s->configured;
        }
    }
    s->status=(uint64_t)status; r->status=(uint64_t)status;
    WRITE(daif,daif);
    return status;
}

/* An entry refusal executes only register operations and loads. In particular,
 * nested calls neither spill a C frame nor update a rejection counter. */
#define GUARD \
    "mrs x9, tpidrro_el0\n" \
    "tbz x9, #56, 91f\n" \
    "cmp w0, #7\n" \
    "b.hi 92f\n" \
    "mrs x9, tpidr_el1\n" \
    "and x9, x9, #0xff\n" \
    "cmp w0, w9\n" \
    "b.ne 92f\n" \
    "adrp x10, ss_sched_pmu_state\n" \
    "add x10, x10, :lo12:ss_sched_pmu_state\n" \
    "add x10, x10, x9, lsl #7\n"

static int restore_status(uint64_t daif,int status)
{ WRITE(daif,daif); return status; }
static __attribute__((used,noinline)) int prepare_c(unsigned core,unsigned kind,uint64_t daif)
{
    struct pmu_state *s=&ss_sched_pmu_state[core];
    if (!atomic_load_explicit(&armed,memory_order_acquire)) return restore_status(daif,SS_SCHED_PMU_UNARMED);
    if (atomic_load_explicit(&s->frozen,memory_order_acquire)) return restore_status(daif,SS_SCHED_PMU_FROZEN);
    if (kind>=SS_SCHED_PMU_KINDS) return restore_status(daif,SS_SCHED_PMU_NOT_PREPARED);
    if (s->status && s->status!=SS_SCHED_PMU_UNARMED) return restore_status(daif,(int)s->status);
    unsigned bank=kind==SS_SCHED_PMU_TICK?(unsigned)s->next_tick_bank:core&1u;
    int status=configure(core,bank);
    if (status) return restore_status(daif,status);
    s->prepared_kind=kind;
    /* Capture one raw empty-window reference for each configured bank. */
    if (!report.core[core].row[SS_SCHED_PMU_BLANK][bank].count) {
        s->prepared_kind=SS_SCHED_PMU_BLANK;
        int64_t cookie=ss_sched_pmu_begin(core,SS_SCHED_PMU_BLANK,UINT32_MAX,0);
        if (cookie>0) (void)ss_sched_pmu_end(core,cookie);
        s->prepared_kind=kind;
    }
    return restore_status(daif,(int)s->status);
}
__attribute__((naked)) int ss_sched_pmu_prepare(unsigned core __attribute__((unused)),unsigned kind __attribute__((unused)))
{
    __asm__(GUARD
        "ldar x11, [x10]\ncbnz x11, 93f\nmrs x2, daif\nmsr daifset, #2\n"
        "ldar x11, [x10]\ncbnz x11, 94f\nb prepare_c\n94: msr daif, x2\nb 93f\n"
        "91: mov w0, #2\nret\n92: mov w0, #3\nret\n93: mov w0, #4\nret\n");
}

static __attribute__((used,noinline)) int64_t begin_c(unsigned core,unsigned kind,unsigned slot,unsigned flags,
                                                    uintptr_t scope,uint64_t daif)
{
    struct pmu_state *s=&ss_sched_pmu_state[core];
    if (!atomic_load_explicit(&armed,memory_order_acquire)) return restore_status(daif,-SS_SCHED_PMU_UNARMED);
    if (atomic_load_explicit(&s->frozen,memory_order_acquire)) return restore_status(daif,-SS_SCHED_PMU_FROZEN);
    if (!s->configured || s->prepared_kind!=kind || kind>=SS_SCHED_PMU_KINDS) return restore_status(daif,-SS_SCHED_PMU_NOT_PREPARED);
    if (s->status) return restore_status(daif,-(int)s->status);
    uint64_t sequence=++s->sequence;
    if (!sequence || sequence>INT64_MAX) {
        s->status=report.core[core].status=SS_SCHED_PMU_CONFIG_MISMATCH;
        return restore_status(daif,-SS_SCHED_PMU_CONFIG_MISMATCH);
    }
    s->slot=slot; s->flags=RECORD_FLAGS | (kind==SS_SCHED_PMU_SELECT_SELF?SS_SCHED_PMU_SELF:0);
    s->scope=scope;
    (void)flags;
    ++report.core[core].attempts;
    atomic_store_explicit(&s->cookie,sequence,memory_order_release);
    ZERO(pmevcntr0_el0); ZERO(pmevcntr1_el0); ZERO(pmevcntr2_el0);
    ZERO(pmevcntr3_el0); ZERO(pmevcntr4_el0); ZERO(pmevcntr5_el0);
    ZERO(pmccntr_el0); WRITE(pmovsclr_el0,ENABLE); ISB();
    s->begin_tick=READ(cntpct_el0);
    /* This is the final memory store before counting starts. The compiler may
     * restore its frame with loads, but must emit no store after this MSR. */
    uint64_t enable=s->enable;
    __asm__ volatile("msr pmcntenset_el0, %0\n isb\n msr daif, %1" :: "r"(enable),"r"(daif) : "memory");
    return (int64_t)sequence;
}
__attribute__((naked)) int64_t ss_sched_pmu_begin(unsigned core __attribute__((unused)),unsigned kind __attribute__((unused)),
                                               unsigned slot __attribute__((unused)),unsigned flags __attribute__((unused)))
{
    __asm__(GUARD
        "ldar x11, [x10]\ncbnz x11, 93f\nmov x4, xzr\nmrs x5, daif\nmsr daifset, #2\n"
        "ldar x11, [x10]\ncbnz x11, 94f\nb begin_c\n94: msr daif, x5\nb 93f\n"
        "91: mov x0, #-2\nret\n92: mov x0, #-3\nret\n93: mov x0, #-4\nret\n");
}
__attribute__((naked)) int64_t ss_sched_pmu_begin_scope(unsigned core __attribute__((unused)),unsigned kind __attribute__((unused)),
    unsigned slot __attribute__((unused)),unsigned flags __attribute__((unused)),void *scope __attribute__((unused)))
{
    __asm__(GUARD
        "ldar x11, [x10]\ncbnz x11, 93f\ncbz x4, 95f\nmrs x5, daif\nmsr daifset, #2\n"
        "ldar x11, [x10]\ncbnz x11, 94f\nb begin_c\n94: msr daif, x5\nb 93f\n"
        "91: mov x0, #-2\nret\n92: mov x0, #-3\nret\n93: mov x0, #-4\nret\n95: mov x0, #-6\nret\n");
}

static __attribute__((used,noinline)) int finish_c(unsigned core,int64_t cookie,uint64_t end,uint64_t cycles,uint64_t overflow)
{
    struct pmu_state *s=&ss_sched_pmu_state[core];
    struct ss_sched_pmu_core *c=&report.core[core];
    uint64_t value[SS_SCHED_PMU_EVENTS];
    __asm__ volatile("mrs %0,pmevcntr0_el0\n mrs %1,pmevcntr1_el0\n mrs %2,pmevcntr2_el0\n"
                     "mrs %3,pmevcntr3_el0\n mrs %4,pmevcntr4_el0\n mrs %5,pmevcntr5_el0"
                     : "=r"(value[0]),"=r"(value[1]),"=r"(value[2]),"=r"(value[3]),"=r"(value[4]),"=r"(value[5]) :: "memory");
    int status=(overflow&ENABLE)?SS_SCHED_PMU_OVERFLOW:SS_SCHED_PMU_OK;
    if (!cycles || (READ(pmcntenset_el0)&ENABLE) || !(READ(pmcr_el0)&1))
        status=SS_SCHED_PMU_CONFIG_MISMATCH;
    struct ss_sched_pmu_row *row=&c->row[s->prepared_kind][s->bank];
    uint64_t count=row->count+1;
    if (!row->count || cycles>row->cycles || status) {
        *row=(struct ss_sched_pmu_row){.count=count,.status=(uint64_t)status,.begin_tick=s->begin_tick,
            .end_tick=end,.cycles=cycles,.supported=c->supported[s->bank],.overflow=overflow&ENABLE,
            .slot=s->slot,.flags=s->flags,.cookie=(uint64_t)cookie};
        for (unsigned i=0;i<SS_SCHED_PMU_EVENTS;++i)
            if (row->supported&(UINT64_C(1)<<i)) row->event[i]=(uint32_t)value[i];
    } else row->count=count;
    ++c->completed;
    if (s->prepared_kind==SS_SCHED_PMU_TICK) s->next_tick_bank^=1;
    if (status) { s->status=(uint64_t)status; c->status=(uint64_t)status; }
    atomic_store_explicit(&s->cookie,0,memory_order_release);
    return status;
}
/* Exclude an IRQ between freezing cycles and reading wall time. Both leaves
 * restore the caller's DAIF before disabled-counter result publication. */
__attribute__((naked)) int ss_sched_pmu_end(unsigned core __attribute__((unused)),int64_t cookie __attribute__((unused)))
{
    __asm__(GUARD
        "cmp x1, #0\nb.le 94f\nldar x11, [x10]\ncbz x11, 95f\ncmp x11, x1\nb.ne 94f\n"
        "mov x11, #0x3f\nmovk x11, #0x8000, lsl #16\nmrs x5,daif\nmsr daifset,#2\nmsr pmcntenclr_el0, x11\nisb\n"
        "mrs x2, cntpct_el0\nmrs x3, pmccntr_el0\nmrs x4, pmovsclr_el0\nmsr daif,x5\nb finish_c\n"
        "91: mov w0, #2\nret\n92: mov w0, #3\nret\n94: mov w0, #6\nret\n95: mov w0, #5\nret\n");
}
__attribute__((naked)) int ss_sched_pmu_end_scope(unsigned core __attribute__((unused)),void *scope __attribute__((unused)))
{
    __asm__(GUARD
        "cbz x1, 94f\nldar x11, [x10]\ncbz x11, 95f\nldr x12, [x10,#96]\ncmp x12,x1\nb.ne 94f\n"
        "mov x1,x11\nmov x11,#0x3f\nmovk x11,#0x8000,lsl #16\nmrs x5,daif\nmsr daifset,#2\nmsr pmcntenclr_el0,x11\nisb\n"
        "mrs x2,cntpct_el0\nmrs x3,pmccntr_el0\nmrs x4,pmovsclr_el0\nmsr daif,x5\nb finish_c\n"
        "91: mov w0,#2\nret\n92: mov w0,#3\nret\n94: mov w0,#6\nret\n95: mov w0,#5\nret\n");
}

int ss_sched_pmu_freeze(unsigned core)
{
    uint64_t tag=READ(tpidrro_el0);
    if (!(tag&UINT64_C(0x0000ffffffffffff)) || core>=SS_SCHED_PMU_CORES || ((tag>>48)&255)!=core)
        return SS_SCHED_PMU_WRONG_CORE;
    struct pmu_state *s=&ss_sched_pmu_state[core];
    if (atomic_load_explicit(&s->cookie,memory_order_acquire)) return SS_SCHED_PMU_NESTED;
    atomic_store_explicit(&s->frozen,1,memory_order_release);
    return SS_SCHED_PMU_OK;
}

void ss_sched_pmu_snapshot(struct ss_sched_pmu_report *out)
{
    atomic_store_explicit(&armed,0,memory_order_release);
    bool complete=true;
    for (unsigned core=0;core<SS_SCHED_PMU_CORES;++core)
        if (!atomic_load_explicit(&ss_sched_pmu_state[core].frozen,memory_order_acquire) ||
            atomic_load_explicit(&ss_sched_pmu_state[core].cookie,memory_order_acquire)) complete=false;
    if (complete) *out=report;
    else {
        *out=(struct ss_sched_pmu_report){.magic=SS_SCHED_PMU_MAGIC,.version=SS_SCHED_PMU_VERSION,
            .bytes=sizeof(report),.cores=SS_SCHED_PMU_CORES,.kinds=SS_SCHED_PMU_KINDS,
            .banks=SS_SCHED_PMU_BANKS,.events=SS_SCHED_PMU_EVENTS,
            .flags=RECORD_FLAGS|SS_SCHED_PMU_SNAPSHOT_INCOMPLETE};
        for (unsigned core=0;core<SS_SCHED_PMU_CORES;++core) out->core[core].status=SS_SCHED_PMU_NOT_ACTIVE;
    }
}
#endif
