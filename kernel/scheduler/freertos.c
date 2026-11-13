/* SPDX-License-Identifier: GPL-2.0-or-later */
/* This adaptation is included by the generated tasks.c after its private TCB,
 * variables and prototypes. Stack construction, task ABI and port frame remain
 * upstream implementations. It is never compiled as a separate translation unit. */
#ifndef SS_FREERTOS_TASKS_TRANSLATION
#error "Include this adapter only from the patched FreeRTOS tasks.c"
#endif
#include <stddef.h>
#include "freertos.h"
#include "freertos_registry.h"
#include "freertos_control.h"
#include "notification.h"
#include "deadline.h"
#include "latency_trace.h"
#include "latency_pmu.h"
#include "latency_cause.h"

/* The private fork rejects optional configurations whose API branches have
 * not been converted. No enabled public API may fall through to legacy lists.
 * Tickless scheduling belongs to series D. Restricted creation requires MPU
 * conversion; ordinary static and dynamic tasks use the native port ABI. */
#if ( portUSING_MPU_WRAPPERS != 0 ) || \
    ( configUSE_TICKLESS_IDLE != 0 ) || ( configUSE_STATS_FORMATTING_FUNCTIONS > 0 )
#error "partitioned scheduler: optional API configuration is not converted"
#endif

_Static_assert(configNUMBER_OF_CORES > 1 && configNUMBER_OF_CORES <= 8,
               "the admitted owner fork supports two through eight CPUs");
_Static_assert(configMAX_PRIORITIES == SS_OWNER_PRIORITIES,
               "owner priorities must match FreeRTOS priorities");

#ifndef SS_OBJECT_SLOTS
#define SS_OBJECT_SLOTS 64u
#endif

#ifndef SS_PULL_MOVES_PER_PASS
#define SS_PULL_MOVES_PER_PASS 0u /* transfers one idle-pull pass may start; 0 is unlimited */
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
static struct { _Alignas(SS_CACHE_LINE) struct ss_sched_trace_lock value; } ssSchedTraceLocks[SS_OBJECT_SLOTS + 1];
#ifndef SUNSTONE_LOCK_TAS_CONTROL
#define SUNSTONE_LOCK_TAS_CONTROL 0
#endif
extern uint32_t ulSunstoneTraceTaskTickets(void);
#define SS_LTRACE_BEGIN(id, site, serving, next, tickets) \
    const uint64_t ss_trace_generation = atomic_load_explicit(&ssSchedTraceLocks[id].value.generation, memory_order_acquire); \
    struct ss_sched_trace_wait ss_trace_wait = ss_sched_trace_lock_begin(&ssSchedTraceLocks[id].value, id, site, serving, next, tickets); \
    if (ss_trace_wait.holder_generation != ss_trace_generation) ss_trace_wait.flags &= ~SS_SCHED_WAIT_TICKETS
#define SS_LTRACE_GLOBAL_BEGIN(site) \
    const uint64_t ss_trace_generation = atomic_load_explicit(&ssSchedTraceLocks[0].value.generation, memory_order_acquire); \
    const uint32_t ss_trace_word = ulSunstoneTraceTaskTickets(); \
    struct ss_sched_trace_wait ss_trace_wait = ss_sched_trace_lock_begin(&ssSchedTraceLocks[0].value, 0, site, \
        ss_trace_word & UINT32_C(0xffff), ss_trace_word >> 16, !SUNSTONE_LOCK_TAS_CONTROL); \
    if (ss_trace_wait.holder_generation != ss_trace_generation) ss_trace_wait.flags &= ~SS_SCHED_WAIT_TICKETS
#define SS_LTRACE_ACQUIRED(core, id) ss_sched_trace_lock_acquired(&ssSchedTraceLocks[id].value, core, &ss_trace_wait)
#define SS_LTRACE_RELEASE(id) ss_sched_trace_lock_release(&ssSchedTraceLocks[id].value)
#define SS_SERVICE_TRACE_BEGIN const uint64_t ss_service_begin = ss_sched_trace_now()
#define SS_SERVICE_TRACE_END(core, part) ss_sched_trace_service(core, part, ss_service_begin, ss_sched_trace_now())
#else
#define SS_LTRACE_BEGIN(id, site, serving, next, tickets) ((void)0)
#define SS_LTRACE_GLOBAL_BEGIN(site) ((void)0)
#define SS_LTRACE_ACQUIRED(core, id) ((void)0)
#define SS_LTRACE_RELEASE(id) ((void)0)
#define SS_SERVICE_TRACE_BEGIN ((void)0)
#define SS_SERVICE_TRACE_END(core, part) ((void)0)
#endif
enum ssKernelExecution { SS_K_READY, SS_K_RUNNING, SS_K_BLOCKED, SS_K_SUSPENDED, SS_K_DEAD, SS_K_TRANSFERRING };
enum ssKernelWait { SS_K_NONE, SS_K_DELAY, SS_K_EVENT, SS_K_NOTIFICATION };
enum ssKernelOutcome { SS_K_WAITING, SS_K_EVENT_READY, SS_K_TIMED_OUT, SS_K_ABORTED };
/* Fields are grouped by the wake path that reads them, so that a wake touches
 * as few lines as possible:
 * - first line: what any wake reads or writes (owner, execution, outcome,
 *   effective priority and notification index zero);
 * - the ready link's line: the owner's queue link, the notification wait
 *   record and the wait generation, read when a wake makes the task ready;
 * - the next line: the wait kind and state flags, then the deadline entry
 *   whose owner field a ready transition reads.
 * Cold fields fill the space between. */
struct ssKernelTask {
    _Alignas(SS_CACHE_LINE) _Atomic unsigned owner;
    _Atomic unsigned execution, outcome, effective;
    struct ss_notification notification[configTASK_NOTIFICATION_ARRAY_ENTRIES];
    _Atomic unsigned base;
    _Atomic unsigned transfer_execution;
    /* Compatibility object exclusion serializes donation writers. Owners read
     * committed donation without acquiring an object lock before their FIFO. */
    _Atomic unsigned donation, mutexes;
    _Atomic uint64_t published_wait_generation;
    struct ss_kernel_pin event_reference;
    _Atomic unsigned cleanup_shield;
    uint64_t resume_generation, abort_generation, offcpu_ack;
    uint64_t object_sequence, object_requested;
    uint64_t object_wait_generation;
    unsigned object_priority, object_requested_priority;
    /* The lock slot of the object whose event list this task waits on; set when it blocks, under that object's lock. */
    unsigned event_slot;
    bool object_detach;
    _Alignas(SS_CACHE_LINE) struct ss_owner_task ready;
    struct ss_notification_wait notify_wait;
    uint64_t generation;
    unsigned wait_kind, notify_index;
    bool indefinite, unordered, off_cpu;
    bool suspended, dead, cleanup_queued, resources_destroyed, cleanup_control_deferred;
    bool offcpu_wait, detach_pending;
    struct ss_deadline_entry deadline;
};
#define SS_K_SAME_LINE(a, b) (offsetof(struct ssKernelTask, a) / SS_CACHE_LINE == \
                              (offsetof(struct ssKernelTask, b) + sizeof(((struct ssKernelTask *)0)->b) - 1) / SS_CACHE_LINE)
_Static_assert(SS_K_SAME_LINE(owner, notification[0]), "a wake's first line holds owner state and notification zero");
_Static_assert(SS_K_SAME_LINE(ready, generation), "the ready link shares its line with the wait record");
_Static_assert(SS_K_SAME_LINE(wait_kind, deadline.owner), "the wait flags share a line with the deadline owner");
#undef SS_K_SAME_LINE
struct ssKernelCPU {
    struct ss_owner run;
    struct ss_deadline_heap deadlines;
    /* The owner's flags and depths, which a wake and the tick read, share one
     * line (asserted below). */
    _Alignas(SS_CACHE_LINE) bool yield_tail;
    bool reschedule, irq_drained, command_finishing, control_draining, selecting;
    bool direct_suspend, explicit_yield, idle_pull_deferred;
    unsigned preempt, object_depth, critical_depth, suspend_depth, isr_depth;
    /* The per-object lock domain (SS_OBJECT_SLOTS). o_depth counts the holds of the task-side lock (critical sections
     * and suspensions), o_critical the critical sections, o_suspend the suspensions and o_isr the interrupt-side regions.
     * The interrupt-side lock is held while o_critical + o_isr is nonzero. t_slot and i_slot name the slots held. */
    unsigned o_depth, o_critical, o_suspend, o_isr, t_slot, i_slot;
    UBaseType_t object_mask;
    UBaseType_t o_mask;
    UBaseType_t control_open_mask;
    unsigned service_depth;
    uint64_t control_seen[2];
    bool command_no_wait[SS_OWNER_SLOTS], command_delete[SS_OWNER_SLOTS];
    /* The tick's flush reads both words; keep them on one line. */
    _Alignas(32) uint64_t publication[2];
    uint64_t command_pending[2];
    uint64_t repair[2], object_repair[2];
    uint64_t command_sequence[SS_OWNER_SLOTS];
    struct ss_kernel_pin command_reference[SS_OWNER_SLOTS];
    struct ss_kernel_pin reference[SS_OWNER_SLOTS];
    /* CPU0's tick samples this deadline of every owner; idle peers read the pull set. */
    _Alignas(SS_CACHE_LINE) _Atomic uint64_t slice_deadline;
    /* Idle pull. pull_targets: CPUs that a ready migratable task here may move to.
     * pull_requests: idle CPUs that asked this owner for work. */
    _Atomic uint64_t pull_targets, pull_requests;
};
_Static_assert(offsetof(struct ssKernelCPU, yield_tail) / SS_CACHE_LINE ==
               (offsetof(struct ssKernelCPU, object_mask) + sizeof(UBaseType_t) - 1) / SS_CACHE_LINE,
               "the owner's flags and depths share one line");
/* Bit c is set while CPU c runs only its idle task. */
static _Atomic uint64_t ssKernelIdleMask;
_Static_assert(sizeof(struct ssKernelCPU) % SS_CACHE_LINE == 0 && _Alignof(struct ssKernelCPU) == SS_CACHE_LINE,
               "owners do not share cache lines");
_Static_assert(offsetof(struct ss_owner, incoming) % SS_CACHE_LINE == 0 &&
               offsetof(struct ss_owner, detached) % SS_CACHE_LINE == 0 &&
               offsetof(struct ss_owner, incoming) / SS_CACHE_LINE != offsetof(struct ss_owner, current) / SS_CACHE_LINE,
               "remote wake words have their own lines");
/* Critical-section commands belong to their caller across migration. Keep
 * these bounded slow-path cookies separate from hot owner/task metadata. No
 * lifetime pin may remain on a stack that another CPU can delete. */
struct ssKernelTaskCommands {
    uint64_t pending[2], deletion[2];
    uint64_t sequence[SS_OWNER_SLOTS], incarnation[SS_OWNER_SLOTS];
};
struct ssKernelWaitSet {
    _Atomic uint64_t cpus;
    _Atomic uint64_t slots[configNUMBER_OF_CORES][2];
};
static struct ssKernelTaskCommands ssKernelTaskCommands[SS_OWNER_SLOTS];
static struct ssKernelTask ssKernelTasks[SS_OWNER_SLOTS];
static struct ssKernelCPU ssKernelCPUs[configNUMBER_OF_CORES];
/* True while this CPU is inside a compatibility section or an object section of either domain. */
static inline bool ssKernelHolds(const struct ssKernelCPU *cpu)
{
    return cpu->object_depth || cpu->isr_depth || cpu->o_depth || cpu->o_isr;
}
static struct ss_kernel_registry ssKernelRegistry;
static struct ss_control ssKernelControl;
static _Atomic uint64_t ssKernelTime;
static _Atomic uint64_t ssKernelAdminNext, ssKernelAdminServing;
static struct ssKernelWaitSet ssKernelAdminWait;
static _Atomic uint64_t ssKernelCleanupPending[2];
static _Atomic uint64_t ssKernelCleanupCompleted[SS_OWNER_SLOTS];
static bool ssKernelInitialized;
/* Registered live task count. Registration is administration-serialized;
 * migration updates estimates atomically without reversing the control order.
 * The series-B placement input includes running and blocked home tasks. */
static _Atomic unsigned ssKernelLoad[configNUMBER_OF_CORES];
static unsigned ssKernelIdleNext;

#ifdef SUNSTONE_PMU_ACCOUNT
/* Measurement: cycles, L1D refills, L2D refills and backend stall cycles spent in the
 * publish-and-wake paths, per CPU (counters: runtime/irq_count.c). Region 0 is
 * ssKernelPublishReference, region 1 ssKernelService (called around the exit of every
 * interrupt on the owner). Written by the CPU itself; 32-bit counter differences. */
static struct { uint64_t v[4]; unsigned long entries; } ssKernelPmu[configNUMBER_OF_CORES][2];
static inline void ssPmuRead(uint32_t o[4])
{
    uint64_t c, a, b, s;
    __asm__ volatile("mrs %0, pmccntr_el0\n mrs %1, pmevcntr0_el0\n mrs %2, pmevcntr1_el0\n mrs %3, pmevcntr2_el0"
                     : "=r"(c), "=r"(a), "=r"(b), "=r"(s) :: "memory");
    o[0] = (uint32_t)c; o[1] = (uint32_t)a; o[2] = (uint32_t)b; o[3] = (uint32_t)s;
}
#define SS_PMU_ENTER uint32_t ss_pmu0[4]; ssPmuRead(ss_pmu0)
#define SS_PMU_LEAVE(region) do { uint32_t ss_pmu1[4]; ssPmuRead(ss_pmu1); unsigned ss_core = (unsigned)portGET_CORE_ID(); \
    for (unsigned k = 0; k < 4; ++k) ssKernelPmu[ss_core][region].v[k] += (uint32_t)(ss_pmu1[k] - ss_pmu0[k]); \
    ++ssKernelPmu[ss_core][region].entries; } while (0)
unsigned long ulSunstoneKernelPmu(unsigned core, unsigned region, unsigned counter)
{
    if (core >= configNUMBER_OF_CORES || region >= 2 || counter > 4) return 0;
    return counter == 4 ? ssKernelPmu[core][region].entries : (unsigned long)ssKernelPmu[core][region].v[counter];
}
#else
#define SS_PMU_ENTER ((void)0)
#define SS_PMU_LEAVE(region) ((void)0)
#endif
#ifdef SUNSTONE_OWNER_COUNTERS
/* Measurement: how long each phase of the wake path takes on each CPU, in counter ticks of 41.7 ns: the count, the sum,
 * the longest and the count over 5 us. Written by the CPU itself. Phases: 0 notification, 1 interrupt-exit work, 2 service
 * pass, 3 control drain, 4 incoming hints, 5 idle pull, 6 deadline expiry, 7 slice refresh, 8 yield tail, 9 task switch. */
#define SS_PHASES 10u
struct ssPhase { uint64_t n, sum, max, over5, pad[4]; };
static struct ssPhase ssPhases[configNUMBER_OF_CORES][SS_PHASES] __attribute__((aligned(64)));
static inline uint64_t ssPhaseNow(void)
{
    uint64_t t;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(t) :: "memory");
    return t;
}
#define SS_PH_BEGIN const uint64_t ssph_t0 = ssPhaseNow()
#define SS_PH_END(phase) do { const uint64_t d = ssPhaseNow() - ssph_t0; struct ssPhase *x = &ssPhases[ssKernelCPUId()][phase]; \
    ++x->n; x->sum += d; if (d > x->max) x->max = d; if (d > 120) ++x->over5; } while (0)
uint64_t ulSunstonePhase(unsigned cpu, unsigned phase, unsigned field)
{
    if (cpu >= configNUMBER_OF_CORES || phase >= SS_PHASES || field > 3) return 0;
    const struct ssPhase *x = &ssPhases[cpu][phase];
    return field == 0 ? x->n : field == 1 ? x->sum : field == 2 ? x->max : x->over5;
}
#else
#define SS_PH_BEGIN ((void)0)
#define SS_PH_END(phase) ((void)0)
#endif
#ifdef SUNSTONE_IRQ_COUNT
/* Measurement: which call sites send SGIs, to which CPU. One table per sending
 * CPU, written by that CPU only; read after the measured interval. */
#define SS_SGI_SITES 24u
static struct { uintptr_t site; unsigned target; unsigned long count; } ssKernelSgiSites[configNUMBER_OF_CORES][SS_SGI_SITES];
static __attribute__((noinline)) void ssKernelSgiCount(uintptr_t site, unsigned cpu)
{
    unsigned self = (unsigned)portGET_CORE_ID();
    for (unsigned i = 0; i < SS_SGI_SITES; ++i) {
        if (ssKernelSgiSites[self][i].site == site && ssKernelSgiSites[self][i].target == cpu) {
            __atomic_store_n(&ssKernelSgiSites[self][i].count, ssKernelSgiSites[self][i].count + 1, __ATOMIC_RELAXED);
            return;
        }
        if (!ssKernelSgiSites[self][i].site) {
            ssKernelSgiSites[self][i].target = cpu;
            __atomic_store_n(&ssKernelSgiSites[self][i].count, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&ssKernelSgiSites[self][i].site, site, __ATOMIC_RELEASE);
            return;
        }
    }
}
unsigned long ulSunstoneSgiSite(unsigned sender, unsigned index, unsigned long *site, unsigned *target)
{
    if (sender >= configNUMBER_OF_CORES || index >= SS_SGI_SITES) return 0;
    *site = __atomic_load_n(&ssKernelSgiSites[sender][index].site, __ATOMIC_ACQUIRE);
    *target = ssKernelSgiSites[sender][index].target;
    return *site ? __atomic_load_n(&ssKernelSgiSites[sender][index].count, __ATOMIC_RELAXED) : 0;
}
#endif
static __attribute__((noinline)) void ssKernelSGI(void *context, unsigned cpu)
{
    (void)context;
    /* The active owner observes all remaining local publications before it
     * returns. One continuation replaces per-record local SGIs and SVCs. */
    if (cpu == (unsigned)portGET_CORE_ID() && ssKernelCPUs[cpu].control_draining) return;
#ifdef SUNSTONE_IRQ_COUNT
    if (xSchedulerRunning) ssKernelSgiCount((uintptr_t)__builtin_return_address(0), cpu);
#endif
    if (xSchedulerRunning) {
#ifdef SUNSTONE_LATENCY_CAUSE
        ss_latency_cause_sgi_begin(cpu);
        ss_latency_cause_sgi_sent(cpu);
#endif
        portYIELD_CORE((BaseType_t)cpu);
#ifdef SUNSTONE_LATENCY_CAUSE
        ss_latency_cause_sgi_end(cpu);
#endif
    }
}
/* True when PSTATE.I is set on this CPU (an exception handler, or code that masked all interrupts). */
static inline bool ssKernelIrqsDisabled(void)
{
#if defined(__aarch64__)
    /* DAIF is not readable at EL0; the interrupt and supervisor entries mark the task register as EL1. */
    if (!((ullPortGetTaskRegister() >> portTASK_REGISTER_EL1_BIT) & 1U)) return false;
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return (daif & 0x80) != 0;
#else
    return false;
#endif
}
static const struct ss_owner_backend ssKernelBackend = { .sgi = ssKernelSGI };
/* The kernel backend's barrier is the architectural default, so a pin passes
 * no backend and reads no backend or registry line besides the descriptor. */
static bool ssKernelHandlePin(void *task, struct ss_kernel_pin *pin)
{
    return ss_kernel_handle_pin_with(&ssKernelRegistry,
                                     ssKernelBackend.barrier ? &ssKernelBackend : NULL, task, pin);
}
static unsigned ssKernelCPUId(void) { return (unsigned)portGET_CORE_ID(); }
static TaskHandle_t ssKernelCurrentSnapshot(unsigned cpu)
{
    return __atomic_load_n(&pxCurrentTCBs[cpu], __ATOMIC_ACQUIRE);
}
static uint64_t ssKernelNow(void)
{ return atomic_load_explicit(&ssKernelTime, memory_order_acquire); }
static TCB_t *ssKernelSelf(void)
{
    /* Boot has no migration and has not installed the task register yet. */
    if (!xSchedulerRunning) return pxCurrentTCBs[ssKernelCPUId()];
    /* One architectural snapshot binds task identity across migration. */
    return (TCB_t *)(uintptr_t)(ullPortGetTaskRegister() & portTASK_REGISTER_TCB_MASK);
}
static UBaseType_t ssKernelTaskMask(void);
static BaseType_t ssKernelSchedulerState(void)
{
    if (!xSchedulerRunning) return taskSCHEDULER_NOT_STARTED;
    UBaseType_t irq = ssKernelTaskMask();
    BaseType_t state = ssKernelCPUs[ssKernelCPUId()].preempt ?
        taskSCHEDULER_SUSPENDED : taskSCHEDULER_RUNNING;
    portCLEAR_INTERRUPT_MASK(irq);
    return state;
}
static void ssKernelMissedYield(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    ssKernelCPUs[ssKernelCPUId()].reschedule = true;
    portCLEAR_INTERRUPT_MASK(irq);
}
static struct ssKernelTask *ssKernelCurrent(void)
{
    TCB_t *t = ssKernelSelf();
    configASSERT(t && t->uxOwnerSlot < SS_OWNER_SLOTS);
    return &ssKernelTasks[t->uxOwnerSlot];
}
static bool ssKernelNeedsFrame(const struct ssKernelCPU *cpu)
{
    if (!cpu->run.current) return true;
    struct ssKernelTask *s = &ssKernelTasks[cpu->run.current->slot];
    unsigned state = atomic_load_explicit(&s->execution, memory_order_acquire);
    return (state != SS_K_RUNNING && state != SS_K_READY) || s->detach_pending;
}
/* New task-side work waits before it owns notification or object resources.
 * Nested sections inherit the outer admission. The API mask excludes this
 * CPU's freeze ACK until its newly admitted block or lock hold is committed. */
static UBaseType_t ssKernelTaskMask(void)
{
    for (;;) {
        UBaseType_t irq = portSET_INTERRUPT_MASK();
        if (!xSchedulerRunning) return irq;
        unsigned c = ssKernelCPUId();
        struct ssKernelCPU *cpu = &ssKernelCPUs[c];
        if (cpu->preempt || ssKernelHolds(cpu) || irq <= portMAX_API_PRIORITY_MASK) return irq;
        unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
        if (gate != SS_GATE_ACQUIRING && gate != SS_GATE_HELD) return irq;
        configASSERT(!ssKernelNeedsFrame(cpu));
        (void)ss_control_checkpoint(&ssKernelControl, c);
        portCLEAR_INTERRUPT_MASK(irq);
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
        extern void ss_kernel_test_admission_wait(unsigned, unsigned) __attribute__((weak));
        if (ss_kernel_test_admission_wait) ss_kernel_test_admission_wait(c, gate);
#endif
        ss_owner_wait(&ssKernelBackend);
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
        extern void ss_kernel_test_admission_resume(void) __attribute__((weak));
        if (ss_kernel_test_admission_resume) ss_kernel_test_admission_resume();
#endif
    }
}
static bool ssKernelCanPreempt(void)
{
    /* Boot-time controls can mark ready work before a task owns SP_EL0.
     * The first-task restore establishes that stack before ordinary yields. */
    if (!xSchedulerRunning) return false;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    TCB_t *task = ssKernelSelf();
    if (task && task->xPreemptionDisable) {
        unsigned state = atomic_load_explicit(&ssKernelTasks[task->uxOwnerSlot].execution,
                                               memory_order_acquire);
        /* Blocking, suspension and deletion still have to save their frame.
         * A voluntary yield deferred by an object hold also survives a task's
         * automatic-preemption disable setting. */
        if (state == SS_K_RUNNING || state == SS_K_READY)
            return ssKernelCPUs[ssKernelCPUId()].explicit_yield;
    }
#endif
    return true;
}
static inline __attribute__((always_inline)) void ssKernelForce(unsigned cpu)
{
    /* Controls and retirement are urgent regardless of running priority. */
#ifdef SUNSTONE_LATENCY_CAUSE
    ss_latency_cause_sgi_begin(cpu);
#endif
    ss_owner_barrier(&ssKernelBackend);
    ssKernelSGI(NULL, cpu);
#ifdef SUNSTONE_LATENCY_CAUSE
    ss_latency_cause_sgi_end(cpu);
#endif
}
static void ssKernelWaitSetWakeCPU(struct ssKernelWaitSet *wait, unsigned cpu)
{
    uint64_t slots[2];
    for (unsigned w = 0; w < 2; ++w)
        slots[w] = atomic_exchange_explicit(&wait->slots[cpu][w], 0, memory_order_acquire);
    ss_control_wake_mask(&ssKernelControl, cpu, slots, wait);
}
static void ssKernelWaitSetRelease(struct ssKernelWaitSet *wait)
{
    uint64_t cpus = atomic_exchange_explicit(&wait->cpus, 0, memory_order_acquire);
    while (cpus) {
        unsigned cpu = (unsigned)__builtin_ctzll(cpus);
        cpus &= cpus - 1;
        ssKernelWaitSetWakeCPU(wait, cpu);
    }
}
static void ssKernelWaitSetPark(struct ssKernelWaitSet *wait, unsigned cpu, unsigned slot)
{
    ss_control_park_on(&ssKernelControl, cpu, slot, wait);
    atomic_fetch_or_explicit(&wait->slots[cpu][slot / 64],
                            UINT64_C(1) << (slot % 64), memory_order_release);
    atomic_fetch_or_explicit(&wait->cpus, UINT64_C(1) << cpu, memory_order_release);
    ss_owner_barrier(&ssKernelBackend);
}
static uint64_t ssKernelAdminEnter(void)
{
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    extern void ss_kernel_test_admin_enter(unsigned);
    ss_kernel_test_admin_enter(ssKernelCPUId());
#endif
    uint64_t ticket = atomic_fetch_add_explicit(&ssKernelAdminNext, 1, memory_order_relaxed);
    while (atomic_load_explicit(&ssKernelAdminServing, memory_order_acquire) != ticket)
        ss_owner_wait(&ssKernelBackend);
    return ticket;
}
static void ssKernelAdminExit(uint64_t ticket)
{
    atomic_store_explicit(&ssKernelAdminServing, ticket + 1, memory_order_release);
    ss_owner_event(&ssKernelBackend);
    ssKernelWaitSetRelease(&ssKernelAdminWait);
}
static bool ssKernelAdminTryEnter(uint64_t *ticket)
{
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    extern void ss_kernel_test_admin_enter(unsigned);
    ss_kernel_test_admin_enter(ssKernelCPUId());
#endif
    uint64_t serving = atomic_load_explicit(&ssKernelAdminServing, memory_order_acquire);
    if (!atomic_compare_exchange_strong_explicit(&ssKernelAdminNext, &serving,
        serving + 1, memory_order_acq_rel, memory_order_acquire)) return false;
    *ticket = serving;
    return true;
}
static bool ssKernelApply(void *, unsigned, void *, const struct ss_control_state *);
static bool ssKernelControlPrepare(void *, unsigned, unsigned);
static bool ssKernelControlPause(void *, unsigned);
static bool ssKernelDetach(void *, unsigned, void *, struct ss_control_transfer *);
static bool ssKernelControlDeferred(void *, unsigned);
static void ssKernelAdopt(void *, unsigned, void *, const struct ss_control_transfer *);
static bool ssKernelDrained(void *, unsigned, unsigned);
static void ssKernelRetired(void *, unsigned, uint64_t);
static bool ssKernelDispatch(void *, unsigned);
static void ssKernelInspect(void *, unsigned, unsigned);
static void ssKernelExpired(void *, const struct ss_deadline_result *);
static void ssKernelPublishReference(struct ss_kernel_pin);
static void ssKernelSliceRefresh(unsigned, bool);
static void ssKernelRetirement(void *context, unsigned slot, unsigned coordinator)
{
    (void)context; (void)coordinator;
    ss_control_retirement(&ssKernelControl, slot);
}

static void ssKernelInitialize(void)
{
    if (ssKernelInitialized) return; /* Boot CPU only, before scheduler start. */
    ss_kernel_registry_init_retirement(&ssKernelRegistry, &ssKernelBackend,
                                       ssKernelRetirement, NULL, 0);
    struct ss_control_ops ops = {
        .prepare = ssKernelControlPrepare, .pause = ssKernelControlPause,
        .apply = ssKernelApply, .deferred = ssKernelControlDeferred,
        .detach = ssKernelDetach, .adopt = ssKernelAdopt,
        .drained = ssKernelDrained, .retired = ssKernelRetired,
        .dispatch = ssKernelDispatch
    };
    ss_control_init(&ssKernelControl, (UINT64_C(1) << configNUMBER_OF_CORES) - 1,
                    &ssKernelBackend, &ops);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        ss_owner_init(&ssKernelCPUs[c].run, c, &ssKernelBackend);
        ss_deadline_init(&ssKernelCPUs[c].deadlines);
        atomic_init(&ssKernelCPUs[c].slice_deadline, UINT64_MAX);
    }
    atomic_init(&ssKernelTime, configINITIAL_TICK_COUNT);
    atomic_init(&ssKernelAdminNext, 0);
    atomic_init(&ssKernelAdminServing, 0);
    ssKernelInitialized = true;
}

/* Reject duplicate/live/cleanup-pending static buffers before upstream
 * construction can overwrite their TLS and allocation ownership fields. */
static bool ssKernelStaticAvailable(StaticTask_t *buffer)
{
    ssKernelInitialize();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    uint64_t admin = ssKernelAdminEnter();
    bool available = true;
    for (unsigned slot = 0; slot < SS_OWNER_SLOTS; ++slot)
        if (ssKernelRegistry.registered[slot] &&
            ssKernelRegistry.descriptor[slot].task == (void *)buffer) { available = false; break; }
    ssKernelAdminExit(admin);
    portCLEAR_INTERRUPT_MASK(irq);
    return available;
}

struct ssKernelCreation { TCB_t *task; unsigned owner; };
static void ssKernelNotificationWake(void *context, struct ss_notification *notification, uint64_t waiters)
{
    struct ssKernelTask *s = context;
    while (waiters) {
        unsigned cpu = (unsigned)__builtin_ctzll(waiters);
        waiters &= waiters - 1;
        ss_control_wake_on(&ssKernelControl, cpu, s->ready.slot, notification);
    }
}
static void ssKernelSlotInitialize(void *opaque, unsigned slot, uint64_t incarnation)
{
    struct ssKernelCreation *creation = opaque;
    TCB_t *t = creation->task;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    *s = (struct ssKernelTask){0};
    ssKernelTaskCommands[slot] = (struct ssKernelTaskCommands){0};
    s->ready = (struct ss_owner_task){ .context = t, .slot = slot,
                                      .priority = (unsigned)t->uxPriority };
    ss_deadline_entry_init(&s->deadline);
    for (unsigned n = 0; n < configTASK_NOTIFICATION_ARRAY_ENTRIES; ++n) {
        ss_notification_init(&s->notification[n], 0);
        s->notification[n].owner_wake = ssKernelNotificationWake;
        s->notification[n].owner_context = s;
    }
    atomic_init(&s->owner, creation->owner);
    atomic_init(&s->execution, SS_K_READY);
    atomic_init(&s->transfer_execution, SS_K_READY);
    atomic_init(&s->outcome, SS_K_WAITING);
    atomic_init(&s->effective, (unsigned)t->uxPriority);
    atomic_init(&s->base, (unsigned)t->uxPriority);
    atomic_init(&s->donation, 0);
    atomic_init(&s->mutexes, 0);
    s->off_cpu = true;
    t->uxOwnerSlot = slot;
    ss_control_register(&ssKernelControl, slot, &ssKernelRegistry.descriptor[slot],
                        incarnation, creation->owner, (unsigned)t->uxPriority,
                        (uint64_t)t->uxCoreAffinityMask);
}
static bool ssKernelAddTask(TCB_t *t)
{
    ssKernelInitialize();
    t->uxOwnerSlot = SS_KERNEL_NO_SLOT;
    uint64_t mask = (uint64_t)t->uxCoreAffinityMask &
                    ((UINT64_C(1) << configNUMBER_OF_CORES) - 1);
    if (!mask) return false;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    uint64_t admin = ssKernelAdminEnter();
    unsigned owner = (unsigned)__builtin_ctzll(mask);
    for (unsigned c = owner + 1; c < configNUMBER_OF_CORES; ++c)
        if ((mask & (UINT64_C(1) << c)) && atomic_load_explicit(&ssKernelLoad[c], memory_order_relaxed) <
            atomic_load_explicit(&ssKernelLoad[owner], memory_order_relaxed))
            owner = c;
    if ((t->uxTaskAttributes & taskATTRIBUTE_IS_IDLE) != 0) {
        configASSERT(!xSchedulerRunning && ssKernelIdleNext < configNUMBER_OF_CORES);
        owner = ssKernelIdleNext++;
        t->uxCoreAffinityMask = (UBaseType_t)1 << owner;
    }
    struct ssKernelCreation creation = { t, owner };
    unsigned slot = ss_kernel_handle_register(&ssKernelRegistry, t,
                                              ssKernelSlotInitialize, &creation);
    if (slot < SS_OWNER_SLOTS) {
        atomic_fetch_add_explicit(&ssKernelLoad[owner], 1, memory_order_relaxed);
        ++uxCurrentNumberOfTasks;
        ++uxTaskNumber;
#if configUSE_TRACE_FACILITY == 1
        t->uxTCBNumber = uxTaskNumber;
#endif
        portSETUP_TCB(t);
        traceTASK_CREATE(t);
        tracePOST_MOVED_TASK_TO_READY_STATE(t);
    }
    ssKernelAdminExit(admin);
    if (slot < SS_OWNER_SLOTS) {
        if (!xSchedulerRunning || owner == ssKernelCPUId()) {
            configASSERT(ss_owner_enqueue(&ssKernelCPUs[owner].run, &ssKernelTasks[slot].ready, false));
        } else {
            struct ss_kernel_pin pin;
            configASSERT(ssKernelHandlePin(t, &pin));
            ssKernelPublishReference(pin);
        }
    }
    bool yield = false;
    if (slot < SS_OWNER_SLOTS && xSchedulerRunning) {
        struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
        if (owner == ssKernelCPUId() && t->uxPriority > pxCurrentTCBs[owner]->uxPriority) {
            cpu->reschedule = true;
            yield = !cpu->preempt;
        }
        if (owner == ssKernelCPUId()) ssKernelSliceRefresh(owner, false);
    }
    portCLEAR_INTERRUPT_MASK(irq);
    if (yield && ssKernelCanPreempt()) portYIELD();
    return slot < SS_OWNER_SLOTS;
}
static void ssKernelBootstrap(void)
{
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        TCB_t *t = xIdleTaskHandles[c];
        struct ssKernelTask *s = &ssKernelTasks[t->uxOwnerSlot];
        configASSERT(atomic_load_explicit(&s->owner, memory_order_relaxed) == c);
        configASSERT(ss_owner_remove(&ssKernelCPUs[c].run, &s->ready));
        ssKernelCPUs[c].run.current = &s->ready;
        atomic_store_explicit(&s->execution, SS_K_RUNNING, memory_order_relaxed);
        s->off_cpu = false;
    }
}

/* Upstream construction initializes TLS, allocation ownership and the port
 * frame. Its destructor also handles admission failure before slot ownership. */
static bool ssKernelAdmitTask(TCB_t *task)
{
    if (ssKernelAddTask(task)) return true;
    prvDeleteTCB(task);
    return false;
}
static void ssKernelControlDrain(void);
static void ssKernelPullServe(unsigned);
static uint64_t ssKernelPullTargets(struct ssKernelCPU *, unsigned);
#ifndef SS_SERVICE_PASSES
#define SS_SERVICE_PASSES 1u
#endif
static void ssKernelPullIdle(unsigned, bool);
static void ssKernelCommandsFinish(bool);
static void ssKernelWait(unsigned, uint64_t, uint64_t);
static void ssKernelCommandsDetach(void);
/* Object publications retain waiter references until the outer metadata lock
 * is released. No producer writes a foreign owner node or timeout entry. */
static void ssKernelPublishReference(struct ss_kernel_pin pin)
{
    SS_PMU_ENTER;
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    unsigned priority = atomic_load_explicit(&s->effective, memory_order_acquire);
    struct ss_control_route route;
    if (ss_control_route_pin(&ssKernelControl, pin.slot, &route)) {
        unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
        if (route.owner == ssKernelCPUId() && !ssKernelCPUs[route.owner].control_draining &&
            gate != SS_GATE_ACQUIRING && gate != SS_GATE_HELD)
            ssKernelInspect(NULL, pin.slot, priority);
        else ss_owner_publish(&ssKernelCPUs[route.owner].run, pin.slot, priority);
        ss_control_route_put(&route);
    } else {
        /* One failed admission never waits for a moving route. Ordinary hints
         * let each owner inspect authoritative completion after adoption. */
        for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu)
            ss_owner_publish(&ssKernelCPUs[cpu].run, pin.slot, priority);
    }
    ss_kernel_handle_put(&pin);
    SS_PMU_LEAVE(0);
}
static void ssKernelFlush(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (ssKernelHolds(cpu)) return;
    ssKernelCommandsFinish(false);
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t bits = cpu->publication[w];
        cpu->publication[w] = 0;
        while (bits) {
            unsigned b = (unsigned)__builtin_ctzll(bits), slot = w * 64 + b;
            bits &= bits - 1;
            struct ss_kernel_pin pin = cpu->reference[slot];
            cpu->reference[slot] = (struct ss_kernel_pin){0};
            uint64_t bit = UINT64_C(1) << b;
            if (cpu->repair[w] & bit) {
                cpu->repair[w] &= ~bit;
                ss_control_repair(&ssKernelControl, slot);
            }
            ssKernelPublishReference(pin);
        }
    }
}
static void ssKernelDefer(struct ss_kernel_pin pin)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    unsigned slot = pin.slot;
    configASSERT(pin.descriptor);
    if (cpu->reference[slot].descriptor) {
        /* Several event operations can coalesce before the outer lock exits.
         * The first retained reference protects every coalesced inspection. */
        ss_kernel_handle_put(&pin);
        return;
    }
    cpu->reference[slot] = pin;
    cpu->publication[slot / 64] |= UINT64_C(1) << (slot % 64);
}

#ifdef SUNSTONE_OBJECT_TRACE
/* Measurement: who waits for, and who holds, the object locks. Each CPU keeps a direct-mapped table keyed by
 * (object, caller) of the lock acquisitions it made: count, wait and hold of the task-side lock (lock 0) and the
 * interrupt-side lock (lock 1), and waits over 5 us. A second table keyed by (waiter, holder) counts the waits over
 * 5 us against the acquisition that last took that lock before the waiter arrived. A CPU writes only its own tables;
 * the holder words are the one shared line per lock, written once per acquisition. Read after the measured interval. */
#define SS_TRACE_ENTRIES 128u
#define SS_TRACE_PAIRS 64u
#define SS_TRACE_LONG 120u /* 5 us in counter ticks */
struct ssTraceEntry {
    uintptr_t object, site, valid;
    uint64_t count[2], wait_sum[2], wait_max[2], hold_sum[2], hold_max[2], long_wait[2];
};
struct ssTracePair {
    uintptr_t object, site, holder_object, holder_site;
    uint64_t count, wait_sum, wait_max, valid, lock, holder_core;
};
struct ssTraceHolder { uintptr_t object, site; uint64_t core; };
/* Holder words: the two global locks, then a pair per object lock slot. */
static struct { _Alignas(SS_CACHE_LINE) struct ssTraceHolder h; } ssTraceHolders[2 + 2 * (SS_OBJECT_SLOTS + 1)];
static struct {
    _Alignas(SS_CACHE_LINE) struct ssTraceEntry entry[SS_TRACE_ENTRIES];
    struct ssTracePair pair[SS_TRACE_PAIRS];
    uint64_t overflow, pair_overflow;
    struct ssTraceEntry *current[2];
    uint64_t acquired[2];
} ssTrace[configNUMBER_OF_CORES];
static inline uint64_t ssTraceNow(void)
{
    uint64_t t;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(t) :: "memory");
    return t;
}
static struct ssTraceEntry *ssTraceFind(unsigned c, uintptr_t object, uintptr_t site)
{
    unsigned index = (unsigned)((((object >> 4) * 0x9e3779b1u) ^ ((site >> 2) * 0x85ebca6bu)) >> 7) % SS_TRACE_ENTRIES;
    struct ssTraceEntry *e = &ssTrace[c].entry[index];
    if (e->valid && (e->object != object || e->site != site)) { ++ssTrace[c].overflow; return NULL; }
    if (!e->valid) { e->valid = 1; e->object = object; e->site = site; }
    return e;
}
static inline struct ssTraceHolder ssTraceArrive(unsigned holder)
{
    return ssTraceHolders[holder].h;
}
/* The lock is held; start is the counter before the lock call. */
static inline void ssTraceLocked(unsigned c, int lock, unsigned holder, uint64_t start, struct ssTraceHolder arrived,
                                 uintptr_t object, uintptr_t site)
{
    const uint64_t now = ssTraceNow();
    const uint64_t wait = now - start;
    struct ssTraceEntry *e = ssTraceFind(c, object, site);
    ssTrace[c].current[lock] = e;
    ssTrace[c].acquired[lock] = now;
    if (e) {
        ++e->count[lock];
        e->wait_sum[lock] += wait;
        if (wait > e->wait_max[lock]) e->wait_max[lock] = wait;
        if (wait > SS_TRACE_LONG) ++e->long_wait[lock];
    }
    if (wait > SS_TRACE_LONG) {
        unsigned index = (unsigned)(((((object >> 4) * 0x9e3779b1u) ^ ((site >> 2) * 0x85ebca6bu) ^
                                      ((arrived.object >> 4) * 0xc2b2ae35u) ^ ((arrived.site >> 2) * 0x27d4eb2fu)) >> 5) + (unsigned)lock) % SS_TRACE_PAIRS;
        struct ssTracePair *pr = &ssTrace[c].pair[index];
        if (pr->valid && (pr->object != object || pr->site != site || pr->holder_object != arrived.object ||
                          pr->holder_site != arrived.site || pr->lock != (uint64_t)lock)) ++ssTrace[c].pair_overflow;
        else {
            if (!pr->valid) { pr->valid = 1; pr->object = object; pr->site = site; pr->holder_object = arrived.object;
                              pr->holder_site = arrived.site; pr->lock = (uint64_t)lock; pr->holder_core = arrived.core; }
            ++pr->count; pr->wait_sum += wait;
            if (wait > pr->wait_max) pr->wait_max = wait;
        }
    }
    ssTraceHolders[holder].h = (struct ssTraceHolder){ object, site, c };
}
static inline void ssTraceReleased(unsigned c, int lock)
{
    struct ssTraceEntry *e = ssTrace[c].current[lock];
    if (!e) return;
    const uint64_t hold = ssTraceNow() - ssTrace[c].acquired[lock];
    e->hold_sum[lock] += hold;
    if (hold > e->hold_max[lock]) e->hold_max[lock] = hold;
}
/* Rows of one CPU's entries: index 0 to SS_TRACE_ENTRIES - 1. Returns 0 past the table or for an empty row. */
unsigned ulSunstoneObjectTraceEntry(unsigned core, unsigned index, uint64_t out[14])
{
    if (core >= configNUMBER_OF_CORES || index >= SS_TRACE_ENTRIES || !ssTrace[core].entry[index].valid) return 0;
    const struct ssTraceEntry *e = &ssTrace[core].entry[index];
    out[0] = e->object; out[1] = e->site;
    for (unsigned l = 0; l < 2; ++l) {
        out[2 + 6 * l] = e->count[l]; out[3 + 6 * l] = e->wait_sum[l]; out[4 + 6 * l] = e->wait_max[l];
        out[5 + 6 * l] = e->hold_sum[l]; out[6 + 6 * l] = e->hold_max[l]; out[7 + 6 * l] = e->long_wait[l];
    }
    return 1;
}
unsigned ulSunstoneObjectTracePair(unsigned core, unsigned index, uint64_t out[9])
{
    if (core >= configNUMBER_OF_CORES || index >= SS_TRACE_PAIRS || !ssTrace[core].pair[index].valid) return 0;
    const struct ssTracePair *p = &ssTrace[core].pair[index];
    out[0] = p->object; out[1] = p->site; out[2] = p->holder_object; out[3] = p->holder_site; out[4] = p->holder_core;
    out[5] = p->lock; out[6] = p->count; out[7] = p->wait_sum; out[8] = p->wait_max;
    return 1;
}
uint64_t ulSunstoneObjectTraceOverflow(unsigned core, unsigned which)
{
    return core >= configNUMBER_OF_CORES ? 0 : which ? ssTrace[core].pair_overflow : ssTrace[core].overflow;
}
#define SS_TR_START(c, lock, holder) const uint64_t tr_start_##lock = ssTraceNow(); \
    const struct ssTraceHolder tr_arrived_##lock = ssTraceArrive(holder); (void)(c)
#define SS_TR_LOCKED(c, lock, holder, object, site) ssTraceLocked(c, lock, holder, tr_start_##lock, tr_arrived_##lock, object, site)
#define SS_TR_RELEASED(c, lock) ssTraceReleased(c, lock)
#else
#define SS_TR_START(c, lock, holder) ((void)0)
#define SS_TR_LOCKED(c, lock, holder, object, site) ((void)0)
#define SS_TR_RELEASED(c, lock) ((void)0)
#endif

#ifdef SUNSTONE_LATENCY
#include "sunstone_latency.h"
/* The recorder pairs an acquisition with a release per lock; lock 0 is the interrupt lock, lock 1 the task lock. */
#define SS_LAT_START(lock) const uint64_t lat_start_##lock = ss_latency_now()
#define SS_LAT_ACQUIRED(c, lock, site) ss_latency_lock_acquired(c, lock, lat_start_##lock, site)
#define SS_LAT_RELEASING(c, lock, site) ss_latency_lock_releasing(c, lock, site)
#else
#define SS_LAT_START(lock) ((void)0)
#define SS_LAT_ACQUIRED(c, lock, site) ((void)0)
#define SS_LAT_RELEASING(c, lock, site) ((void)0)
#endif

/* ---- Global compatibility sections --------------------------------------------------------------------------------
 * The public critical sections (vTaskEnterCritical and its FromISR form) keep the two global locks of the port: lock 0
 * is the task lock, lock 1 the interrupt lock in the trace. Only the public critical sections and the timer service's
 * suspension use them. */
static __attribute__((noinline)) void ssKernelObjectEnterAt(uintptr_t object, uintptr_t site)
{
    UBaseType_t irq = ssKernelTaskMask();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    if (!cpu->object_depth) {
        ++cpu->preempt;
        SS_TR_START(c, 0, 0);
        SS_LTRACE_GLOBAL_BEGIN(site);
        portGET_TASK_LOCK((BaseType_t)c);
        SS_LTRACE_ACQUIRED(c, 0);
        SS_TR_LOCKED(c, 0, 0, object, site);
    }
    if (!cpu->critical_depth) {
        cpu->object_mask = irq;
        SS_TR_START(c, 1, 1);
        portGET_ISR_LOCK((BaseType_t)c);
        SS_TR_LOCKED(c, 1, 1, object, site);
    }
    ++cpu->object_depth;
    ++cpu->critical_depth;
}
__attribute__((noinline)) void ssKernelObjectEnterFor(const void *object, uintptr_t site) { ssKernelObjectEnterAt((uintptr_t)object, site); }
void ssKernelObjectExit(void)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->object_depth && cpu->critical_depth && cpu->preempt);
    --cpu->object_depth;
    if (--cpu->critical_depth) return;
    SS_TR_RELEASED(c, 1);
    portRELEASE_ISR_LOCK((BaseType_t)c);
    if (!cpu->object_depth) {
        SS_TR_RELEASED(c, 0);
        SS_LTRACE_RELEASE(0);
        portRELEASE_TASK_LOCK((BaseType_t)c);
        ssKernelFlush();
        --cpu->preempt;
    }
    UBaseType_t irq = cpu->object_mask;
    bool yield = !cpu->preempt && cpu->reschedule;
    ssKernelCommandsDetach();
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelCommandsFinish(true);
    if (yield && ssKernelCanPreempt()) portYIELD();
}
static __attribute__((noinline)) UBaseType_t ssKernelObjectEnterFromISRAt(uintptr_t object, uintptr_t site)
{
    /* Also used for metadata repair from task APIs. The port's execution tag
     * selects direct privileged masking in IRQ/SVC and SVC masking at EL0. */
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    if (!cpu->isr_depth++) {
        SS_TR_START(c, 1, 1);
        portGET_ISR_LOCK((BaseType_t)c);
        SS_TR_LOCKED(c, 1, 1, object, site);
    }
    return irq;
}
__attribute__((noinline)) UBaseType_t ssKernelObjectEnterFromISR(void) { return ssKernelObjectEnterFromISRAt(0, (uintptr_t)__builtin_return_address(0)); }
void ssKernelObjectExitFromISR(UBaseType_t irq)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->isr_depth);
    if (!--cpu->isr_depth) {
        SS_TR_RELEASED(c, 1);
        portRELEASE_ISR_LOCK((BaseType_t)c);
        ssKernelFlush();
    }
    portCLEAR_INTERRUPT_MASK(irq);
}
static __attribute__((noinline)) void ssKernelObjectSuspendAt(uintptr_t object, uintptr_t site)
{
    /* Queue retry bookkeeping retains T with local preemption disabled. API
     * IRQs remain enabled outside its separate inner I-protected sections. */
    UBaseType_t irq = ssKernelTaskMask();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    if (!cpu->object_depth) {
        ++cpu->preempt;
        SS_TR_START(c, 0, 0);
        SS_LTRACE_GLOBAL_BEGIN(site);
        portGET_TASK_LOCK((BaseType_t)c);
        SS_LTRACE_ACQUIRED(c, 0);
        SS_TR_LOCKED(c, 0, 0, object, site);
    }
    ++cpu->object_depth;
    ++cpu->suspend_depth;
    portCLEAR_INTERRUPT_MASK(irq);
}
__attribute__((noinline)) void ssKernelObjectSuspend(void) { ssKernelObjectSuspendAt(0, (uintptr_t)__builtin_return_address(0)); }
BaseType_t ssKernelObjectResume(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->suspend_depth && cpu->object_depth && !cpu->critical_depth);
    --cpu->suspend_depth;
    if (!--cpu->object_depth) {
        SS_TR_RELEASED(c, 0);
        SS_LTRACE_RELEASE(0);
        portRELEASE_TASK_LOCK((BaseType_t)c);
        ssKernelFlush();
        configASSERT(cpu->preempt);
        --cpu->preempt;
    }
    bool yield = !cpu->preempt && cpu->reschedule;
    ssKernelCommandsDetach();
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelCommandsFinish(true);
    if (yield && ssKernelCanPreempt()) portYIELD();
    return yield ? pdTRUE : pdFALSE;
}

/* ---- Object sections ------------------------------------------------------------------------------------------------
 * The queue, event group, stream buffer and timer sources enter and leave through these. An object hashes to one of
 * SS_OBJECT_SLOTS slots; each slot has a task-side ticket lock (held across a suspension, with the mask open) and an
 * interrupt-side ticket lock (held with the mask raised), the roles of the two global locks. Operations on objects of
 * different slots do not wait for each other. The slots need no storage in the object, so the public static
 * layouts are unchanged.
 *
 * Rules, checked by assertions. A CPU holds at most one task-side slot and at most one interrupt-side slot at a time:
 * nesting inside a section is on the same object (or one that hashes to the same slot). No object section takes a
 * global compatibility lock, so the order is global locks, then one slot, then the donation lock. The donation lock is a
 * leaf. */
struct ssObjectTicket { _Atomic uint32_t next, serving; };
/* A shared object can block several records on one CPU. The bitmaps use the
 * admitted object, CPU and registered-task declarations; they allocate no pool. */
static struct ssKernelWaitSet ssObjectRepairWait[SS_OBJECT_SLOTS];
static struct {
    _Alignas(SS_CACHE_LINE) struct ssObjectTicket task;
    _Alignas(SS_CACHE_LINE) struct ssObjectTicket isr;
} ssObjectLocks[SS_OBJECT_SLOTS];
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
_Static_assert(sizeof(ssSchedTraceLocks) / sizeof(ssSchedTraceLocks[0]) ==
    1 + sizeof(ssObjectLocks) / sizeof(ssObjectLocks[0]),
    "scheduler tracing covers the global task lock and every object task lock");
#endif
/* The leaf lock of priority donation: one writer at a time to a holder's donated priority and mutex count. */
static struct { _Alignas(SS_CACHE_LINE) struct ssObjectTicket lock; } ssDonation;
static inline unsigned ssObjectSlot(const void *object)
{
#if SS_OBJECT_SLOTS == 1
    (void)object;
    return 0;
#else
    return (unsigned)((((uint64_t)(uintptr_t)object >> 4) * UINT64_C(0x9e3779b97f4a7c15)) >> 40) % SS_OBJECT_SLOTS;
#endif
}
static inline void ssTicketAcquire(struct ssObjectTicket *lock)
{
    const uint32_t ticket = atomic_fetch_add_explicit(&lock->next, 1, memory_order_relaxed);
    while (atomic_load_explicit(&lock->serving, memory_order_acquire) != ticket) ss_owner_wait(&ssKernelBackend);
}
static inline void ssTicketRelease(struct ssObjectTicket *lock)
{
    atomic_store_explicit(&lock->serving, atomic_load_explicit(&lock->serving, memory_order_relaxed) + 1, memory_order_release);
    ss_owner_event(&ssKernelBackend);
}
static bool ssTicketTryAcquire(struct ssObjectTicket *lock)
{
    uint32_t serving = atomic_load_explicit(&lock->serving, memory_order_acquire);
    return atomic_compare_exchange_strong_explicit(&lock->next, &serving,
        serving + 1, memory_order_acq_rel, memory_order_acquire);
}
static void ssObjectRepairPark(unsigned object_slot, unsigned cpu, unsigned task_slot)
{
    struct ssKernelWaitSet *wait = &ssObjectRepairWait[object_slot];
    struct ssObjectTicket *lock = &ssObjectLocks[object_slot].isr;
    uint64_t bit = UINT64_C(1) << cpu;
    ssKernelWaitSetPark(wait, cpu, task_slot);
    if (atomic_load_explicit(&lock->next, memory_order_acquire) ==
        atomic_load_explicit(&lock->serving, memory_order_acquire) &&
        (atomic_fetch_and_explicit(&wait->cpus, ~bit, memory_order_acq_rel) & bit))
        ssKernelWaitSetWakeCPU(wait, cpu);
}
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
/* The slot of an object, so that a check can pick objects that share no lock. */
unsigned ss_kernel_test_object_slot(const void *object);
unsigned ss_kernel_test_object_slot(const void *object) { return ssObjectSlot(object); }
#endif
/* Take the interrupt-side lock of a slot, if this CPU does not hold it already; the mask is raised. */
static inline void ssObjectIAcquire(struct ssKernelCPU *cpu, unsigned c, unsigned slot, const void *object, uintptr_t site)
{
    if (!(cpu->o_critical + cpu->o_isr)) {
        cpu->i_slot = slot;
        SS_TR_START(c, 1, 2 + 2 * slot + 1);
        SS_LAT_START(0);
        ssTicketAcquire(&ssObjectLocks[slot].isr);
        SS_TR_LOCKED(c, 1, 2 + 2 * slot + 1, (uintptr_t)object, site);
        SS_LAT_ACQUIRED(c, 0, site);
    } else configASSERT(cpu->i_slot == slot);
}
static bool ssObjectITryAcquire(struct ssKernelCPU *cpu, unsigned c, unsigned slot)
{
    (void)c;
    configASSERT(!(cpu->o_critical + cpu->o_isr));
    SS_TR_START(c, 1, 2 + 2 * slot + 1);
    SS_LAT_START(0);
    if (!ssTicketTryAcquire(&ssObjectLocks[slot].isr)) return false;
    cpu->i_slot = slot;
    SS_TR_LOCKED(c, 1, 2 + 2 * slot + 1, 0, 0);
    SS_LAT_ACQUIRED(c, 0, 0);
    return true;
}
static inline void ssObjectIRelease(struct ssKernelCPU *cpu, unsigned c, uintptr_t site)
{
    SS_TR_RELEASED(c, 1);
    SS_LAT_RELEASING(c, 0, site);
    ssTicketRelease(&ssObjectLocks[cpu->i_slot].isr);
    ssKernelWaitSetRelease(&ssObjectRepairWait[cpu->i_slot]);
}
static __attribute__((noinline)) void ssKernelObjectEnterOnAt(const void *object, uintptr_t site)
{
    UBaseType_t irq = ssKernelTaskMask();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    const unsigned slot = ssObjectSlot(object);
    if (!cpu->o_depth) {
        ++cpu->preempt;
        cpu->t_slot = slot;
        SS_TR_START(c, 0, 2 + 2 * slot);
        SS_LAT_START(1);
        SS_LTRACE_BEGIN(slot + 1, site,
            atomic_load_explicit(&ssObjectLocks[slot].task.serving, memory_order_relaxed),
            atomic_load_explicit(&ssObjectLocks[slot].task.next, memory_order_relaxed), 1);
        ssTicketAcquire(&ssObjectLocks[slot].task);
        SS_LTRACE_ACQUIRED(c, slot + 1);
        SS_TR_LOCKED(c, 0, 2 + 2 * slot, (uintptr_t)object, site);
        SS_LAT_ACQUIRED(c, 1, site);
    } else configASSERT(cpu->t_slot == slot);
    if (!cpu->o_critical) cpu->o_mask = irq;
    ssObjectIAcquire(cpu, c, slot, object, site);
    ++cpu->o_depth;
    ++cpu->o_critical;
}
__attribute__((noinline)) void ssKernelObjectEnterOn(const void *object) { ssKernelObjectEnterOnAt(object, (uintptr_t)__builtin_return_address(0)); }
void ssKernelObjectLeave(void)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->o_depth && cpu->o_critical && cpu->preempt);
    --cpu->o_depth;
    if (--cpu->o_critical) return;
    if (!cpu->o_isr) ssObjectIRelease(cpu, c, 0);
    if (!cpu->o_depth) {
        SS_TR_RELEASED(c, 0);
        SS_LAT_RELEASING(c, 1, 0);
        SS_LTRACE_RELEASE(cpu->t_slot + 1);
        ssTicketRelease(&ssObjectLocks[cpu->t_slot].task);
        ssKernelFlush();
        --cpu->preempt;
    }
    UBaseType_t irq = cpu->o_mask;
    bool yield = !cpu->preempt && cpu->reschedule;
    ssKernelCommandsDetach();
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelCommandsFinish(true);
    if (yield && ssKernelCanPreempt()) portYIELD();
}
/* The interrupt-side section of a slot. Also used by the owner to repair or time out a waiter on the slot's lists. */
static __attribute__((noinline)) UBaseType_t ssObjectIsrEnterSlot(unsigned slot, const void *object, uintptr_t site)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    ssObjectIAcquire(cpu, c, slot, object, site);
    ++cpu->o_isr;
    return irq;
}
static void ssObjectIsrLeave(UBaseType_t irq)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->o_isr);
    if (!--cpu->o_isr && !cpu->o_critical) {
        ssObjectIRelease(cpu, c, 0);
        ssKernelFlush();
    }
    portCLEAR_INTERRUPT_MASK(irq);
}
__attribute__((noinline)) UBaseType_t ssKernelObjectEnterFromISROn(const void *object)
{
    return ssObjectIsrEnterSlot(ssObjectSlot(object), object, (uintptr_t)__builtin_return_address(0));
}
void ssKernelObjectLeaveFromISR(UBaseType_t irq) { ssObjectIsrLeave(irq); }
/* The interrupt-side section of the slot of the object this task last waited on: the section that excludes the waker and
 * the owner while they edit the task's event-list item. */
__attribute__((noinline)) UBaseType_t ssKernelObjectEnterWaitSlot(void)
{
    return ssObjectIsrEnterSlot(ssKernelCurrent()->event_slot, NULL, (uintptr_t)__builtin_return_address(0));
}
static __attribute__((noinline)) void ssKernelObjectSuspendOnAt(const void *object, uintptr_t site)
{
    UBaseType_t irq = ssKernelTaskMask();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    const unsigned slot = ssObjectSlot(object);
    if (!cpu->o_depth) {
        ++cpu->preempt;
        cpu->t_slot = slot;
        SS_TR_START(c, 0, 2 + 2 * slot);
        SS_LAT_START(1);
        SS_LTRACE_BEGIN(slot + 1, site,
            atomic_load_explicit(&ssObjectLocks[slot].task.serving, memory_order_relaxed),
            atomic_load_explicit(&ssObjectLocks[slot].task.next, memory_order_relaxed), 1);
        ssTicketAcquire(&ssObjectLocks[slot].task);
        SS_LTRACE_ACQUIRED(c, slot + 1);
        SS_TR_LOCKED(c, 0, 2 + 2 * slot, (uintptr_t)object, site);
        SS_LAT_ACQUIRED(c, 1, site);
    } else configASSERT(cpu->t_slot == slot);
    ++cpu->o_depth;
    ++cpu->o_suspend;
    portCLEAR_INTERRUPT_MASK(irq);
}
__attribute__((noinline)) void ssKernelObjectSuspendOn(const void *object) { ssKernelObjectSuspendOnAt(object, (uintptr_t)__builtin_return_address(0)); }
BaseType_t ssKernelObjectResumeOn(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->o_suspend && cpu->o_depth && !cpu->o_critical);
    --cpu->o_suspend;
    if (!--cpu->o_depth) {
        SS_TR_RELEASED(c, 0);
        SS_LAT_RELEASING(c, 1, 0);
        SS_LTRACE_RELEASE(cpu->t_slot + 1);
        ssTicketRelease(&ssObjectLocks[cpu->t_slot].task);
        ssKernelFlush();
        configASSERT(cpu->preempt);
        --cpu->preempt;
    }
    bool yield = !cpu->preempt && cpu->reschedule;
    ssKernelCommandsDetach();
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelCommandsFinish(true);
    if (yield && ssKernelCanPreempt()) portYIELD();
    return yield ? pdTRUE : pdFALSE;
}
/* The leaf lock of priority donation, taken with the mask raised. */
static UBaseType_t ssDonationEnter(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    ssTicketAcquire(&ssDonation.lock);
    return irq;
}
static void ssDonationLeave(UBaseType_t irq)
{
    ssTicketRelease(&ssDonation.lock);
    ssKernelFlush(); /* publishes a donation repair now unless an enclosing section does it at its exit */
    portCLEAR_INTERRUPT_MASK(irq);
}

/* Owner-local state transitions. Event-list metadata alone is protected by the
 * compatibility object lock. Only the selected owner touches ready/deadlines. */
static void ssKernelCancel(struct ssKernelTask *s)
{
    unsigned c = ssKernelCPUId();
    configASSERT(atomic_load_explicit(&s->owner, memory_order_relaxed) == c);
    if (s->deadline.owner)
        configASSERT(ss_deadline_cancel(&ssKernelCPUs[c].deadlines,
                                        &s->deadline, s->generation));
}
/* Only the owner inspects its queues/current task. CPU0 samples this published
 * deadline without reading a remote task or queue. The first equal peer arms
 * a slice immediately, including local wakes that need no scheduler IRQ. */
static void ssKernelSliceRefresh(unsigned c, bool reset)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(c == ssKernelCPUId());
    bool eligible = false;
#if ( configUSE_TIME_SLICING == 1 )
    struct ss_owner_task *current = cpu->run.current;
    if (current) {
        TCB_t *t = current->context;
        eligible = !(t->uxTaskAttributes & taskATTRIBUTE_IS_IDLE) &&
            atomic_load_explicit(&ssKernelTasks[current->slot].execution, memory_order_acquire) == SS_K_RUNNING &&
            ss_owner_ready(&cpu->run, current->priority)->head != NULL;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
        eligible = eligible && !t->xPreemptionDisable;
#endif
    }
#endif
    uint64_t deadline = atomic_load_explicit(&cpu->slice_deadline, memory_order_relaxed);
    if (!eligible) deadline = UINT64_MAX;
    else if (reset || deadline == UINT64_MAX) {
        uint64_t now = ssKernelNow();
        configASSERT(now < UINT64_MAX - 1);
        deadline = now + 1;
    }
    atomic_store_explicit(&cpu->slice_deadline, deadline, memory_order_release);
}
static void ssKernelMakeReady(struct ssKernelTask *s)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(atomic_load_explicit(&s->owner, memory_order_acquire) == c);
    ssKernelCancel(s);
    if (s->dead || s->suspended) return;
    atomic_store_explicit(&s->execution, SS_K_READY, memory_order_release);
    if (&s->ready != cpu->run.current && !s->ready.queued && !s->detach_pending &&
        (atomic_load_explicit(&ssKernelControl.slot[s->ready.slot].affinity, memory_order_acquire) & (UINT64_C(1) << c)))
        configASSERT(ss_owner_enqueue(&cpu->run, &s->ready, false));
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    extern void ss_kernel_test_detach_incoming(unsigned, bool) __attribute__((weak));
    if (s->detach_pending && ss_kernel_test_detach_incoming)
        ss_kernel_test_detach_incoming(s->ready.slot, s->ready.queued);
#endif
    struct ss_owner_task *current = cpu->run.current;
    if (!current || s->ready.priority > current->priority)
        cpu->reschedule = true;
    /* The slice deadline depends on the current task's execution and on its
     * own priority's FIFO. A ready transition at another priority changes
     * neither, so only a transition at the current priority refreshes it;
     * a woken higher-priority task leaves the current task's state alone. */
    if (!current || s->ready.priority == current->priority)
        ssKernelSliceRefresh(c, false);
}
static void ssKernelBlock(TickType_t ticks, bool indefinite, unsigned kind)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelTask *s = ssKernelCurrent();
    unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
    configASSERT(gate != SS_GATE_HELD &&
        (gate != SS_GATE_ACQUIRING || !(atomic_load_explicit(&ssKernelControl.gate_ack, memory_order_acquire) & (UINT64_C(1) << c))));
#if INCLUDE_xTaskAbortDelay == 1
    ((TCB_t *)s->ready.context)->ucDelayAborted = pdFALSE;
#endif
    configASSERT(!s->deadline.owner && !s->ready.queued);
    configASSERT(atomic_load_explicit(&s->owner, memory_order_relaxed) == c);
    configASSERT(s->generation != UINT64_MAX);
    ++s->generation;
    atomic_store_explicit(&s->published_wait_generation, s->generation, memory_order_release);
    s->wait_kind = kind;
    s->indefinite = indefinite && ticks == portMAX_DELAY;
    atomic_store_explicit(&s->outcome, SS_K_WAITING, memory_order_release);
    atomic_store_explicit(&s->execution, SS_K_BLOCKED, memory_order_release);
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    extern void ss_kernel_test_wait_blocked(unsigned, uint64_t) __attribute__((weak));
    if (ss_kernel_test_wait_blocked) ss_kernel_test_wait_blocked(s->ready.slot, s->generation);
#endif
    if (!s->indefinite) {
        uint64_t deadline;
        configASSERT(ss_deadline_after(ssKernelNow(), (uint64_t)ticks, &deadline));
        configASSERT(ss_deadline_insert(&ssKernelCPUs[c].deadlines, &s->deadline,
                                        s, s->generation, deadline));
    }
    ssKernelCPUs[c].reschedule = true;
}
static void ssKernelInspect(void *context, unsigned slot, unsigned hint_priority)
{
    (void)context; (void)hint_priority;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    unsigned c = ssKernelCPUId();
    if (atomic_load_explicit(&s->owner, memory_order_acquire) != c) {
        ss_control_forward(&ssKernelControl, slot);
        return;
    }
    if (s->dead) return;
    unsigned state = atomic_load_explicit(&s->execution, memory_order_acquire);
    if (state == SS_K_TRANSFERRING) {
        ss_control_forward(&ssKernelControl, slot);
        return;
    }
    if (state == SS_K_READY) { ssKernelMakeReady(s); return; }
    if (state != SS_K_BLOCKED) return;
    bool complete = atomic_load_explicit(&s->outcome, memory_order_acquire) != SS_K_WAITING;
    if (!complete && s->wait_kind == SS_K_NOTIFICATION) {
        struct ss_notification_guard guard;
        struct ss_notification *notification = &s->notification[s->notify_index];
        if (!ss_notification_gate_try_begin(notification, &ssKernelBackend, &guard)) {
            ss_control_park_on(&ssKernelControl, c, slot, notification);
            ss_notification_owner_wait(notification, c, &ssKernelBackend);
            return;
        }
        complete = ss_notification_wait_observe(&guard, &s->notify_wait, s->generation);
        if (complete) {
            atomic_store_explicit(&s->outcome, SS_K_EVENT_READY, memory_order_release);
            ssKernelMakeReady(s);
        }
        ss_notification_gate_end(&guard);
    } else if (complete) ssKernelMakeReady(s);
}
static void ssKernelExpired(void *context, const struct ss_deadline_result *result)
{
    (void)context;
    struct ssKernelTask *s = result->context;
    if (s->generation != result->generation || s->dead) return;
    if (atomic_load_explicit(&s->execution, memory_order_acquire) != SS_K_BLOCKED) return;
    if (s->wait_kind == SS_K_NOTIFICATION) {
        struct ss_notification_guard guard;
        ss_notification_gate_begin(&s->notification[s->notify_index], &ssKernelBackend, &guard);
        bool won = ss_notification_wait_observe(&guard, &s->notify_wait, s->generation);
        if (!won) {
            /* Timeout closes WAITING while holding the notification gate. A
             * later producer may update the value, but cannot claim another
             * scheduler completion for this same expired wait. */
            guard.payload &= ~SS_NOTIFICATION_STATE;
            s->notify_wait.blocked = false;
        }
        atomic_store_explicit(&s->outcome, won ? SS_K_EVENT_READY : SS_K_TIMED_OUT,
                               memory_order_release);
        if (!won) tracePOST_MOVED_TASK_TO_READY_STATE(s->ready.context);
        ssKernelMakeReady(s);
        ss_notification_gate_end(&guard);
    } else if (s->wait_kind == SS_K_EVENT) {
        UBaseType_t irq = ssObjectIsrEnterSlot(s->event_slot, NULL, (uintptr_t)__builtin_return_address(0));
        unsigned expected = SS_K_WAITING;
        if (atomic_compare_exchange_strong_explicit(&s->outcome, &expected, SS_K_TIMED_OUT,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            tracePOST_MOVED_TASK_TO_READY_STATE(s->ready.context);
            TCB_t *t = s->ready.context;
            if (listLIST_ITEM_CONTAINER(&t->xEventListItem)) {
                listREMOVE_ITEM(&t->xEventListItem);
                ss_kernel_handle_put(&s->event_reference);
            }
        }
        /* This entry already runs with API IRQs excluded. Restoring that mask
         * cannot enable IRQs; the lock is released before ready-link writes. */
        ssObjectIsrLeave(irq);
        ssKernelMakeReady(s);
    } else {
        atomic_store_explicit(&s->outcome, SS_K_TIMED_OUT, memory_order_release);
        tracePOST_MOVED_TASK_TO_READY_STATE(s->ready.context);
        ssKernelMakeReady(s);
    }
}
/* One maintenance pass. True when work remains: a due deadline, a hint beyond the
 * batch or one that arrived after the drain, or object repair. */
static bool ssKernelServiceOnce(unsigned c, struct ssKernelCPU *cpu)
{
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    ss_sched_trace_service_pass(c);
#endif
    if (!cpu->selecting && cpu->idle_pull_deferred) {
        cpu->idle_pull_deferred = false;
        if (atomic_load_explicit(&ssKernelIdleMask, memory_order_acquire) & (UINT64_C(1) << c))
            ssKernelPullIdle(c, true);
    }
    if (!cpu->selecting) {
        SS_PH_BEGIN;
        SS_SERVICE_TRACE_BEGIN;
        ssKernelControlDrain();
        SS_SERVICE_TRACE_END(c, 0);
        SS_PH_END(3);
    }
    {
        SS_PH_BEGIN;
        SS_SERVICE_TRACE_BEGIN;
        ss_owner_drain(&cpu->run, ssKernelInspect, NULL);
        SS_SERVICE_TRACE_END(c, 1);
        SS_PH_END(4);
    }
    if (!cpu->selecting) {
        SS_PH_BEGIN;
        SS_SERVICE_TRACE_BEGIN;
        ssKernelPullServe(c);
        SS_SERVICE_TRACE_END(c, 2);
        SS_PH_END(5);
    }
    /* Event timeout completion edits object metadata. Exception entry does
     * that maintenance before selection; newly due work gets another entry. */
    if (!cpu->selecting) {
        SS_PH_BEGIN;
        SS_SERVICE_TRACE_BEGIN;
        ss_deadline_expire(&cpu->deadlines, ssKernelNow(), SS_DEADLINE_BATCH,
                           ssKernelExpired, NULL);
        SS_SERVICE_TRACE_END(c, 3);
        SS_PH_END(6);
    }
    {
        SS_PH_BEGIN;
        SS_SERVICE_TRACE_BEGIN;
        ssKernelSliceRefresh(c, false);
        SS_SERVICE_TRACE_END(c, 4);
        SS_PH_END(7);
    }
    if (atomic_load_explicit(&cpu->slice_deadline, memory_order_relaxed) <= ssKernelNow()) {
        cpu->yield_tail = true;
        cpu->reschedule = true;
        atomic_store_explicit(&cpu->slice_deadline, UINT64_MAX, memory_order_release);
    }
    uint64_t next;
    return (ss_deadline_next(&cpu->deadlines, &next) && next <= ssKernelNow()) ||
        ss_owner_has_pending(&cpu->run) || ss_control_has_pending(&ssKernelControl, c) ||
        (cpu->object_repair[0] & ~atomic_load_explicit(&ssKernelControl.parked[c].word[0], memory_order_acquire)) ||
        (cpu->object_repair[1] & ~atomic_load_explicit(&ssKernelControl.parked[c].word[1], memory_order_acquire));
}
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
static unsigned long ssKernelContinuations;
unsigned long ss_kernel_test_continuations(void) { return ssKernelContinuations; }
#endif
static void ssKernelService(void)
{
    const bool disabled = ssKernelIrqsDisabled();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    UBaseType_t previous_open = cpu->control_open_mask;
    cpu->control_open_mask = irq > portMAX_API_PRIORITY_MASK ? irq : previous_open;
    if (!cpu->service_depth++) cpu->control_seen[0] = cpu->control_seen[1] = 0;
#if defined(__aarch64__)
    if (disabled) __asm__ volatile("msr daifclr, #2" ::: "memory");
#endif
    SS_PH_BEGIN;
    SS_PMU_ENTER;
    /* Work that arrived during the pass is taken by the next pass here. A new
     * exception entry costs an SGI, the exception and a full service entry, a
     * long span when the memory system is busy, so it is the last resort. */
    bool more = ssKernelServiceOnce(c, cpu);
    for (unsigned pass = 1; more && pass < SS_SERVICE_PASSES; ++pass) more = ssKernelServiceOnce(c, cpu);
    if (more) {
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
        ++ssKernelContinuations;
#endif
        ssKernelForce(c);
    }
    SS_PMU_LEAVE(1);
    SS_PH_END(2);
    --cpu->service_depth;
    cpu->control_open_mask = previous_open;
#if defined(__aarch64__)
    if (disabled) __asm__ volatile("msr daifset, #2" ::: "memory");
#else
    (void)disabled;
#endif
    portCLEAR_INTERRUPT_MASK(irq);
}
static bool ssKernelDispatch(void *context, unsigned cpu)
{
    (void)context;
    configASSERT(cpu == ssKernelCPUId());
    ssKernelService();
    return ssKernelCPUs[cpu].reschedule;
}
/* Called only after the port has saved the outgoing task frame. No global
 * scheduler lock participates in local pick, preemption or voluntary yield. */
static void ssKernelSwitch(unsigned c)
{
    configASSERT(c == ssKernelCPUId());
    SS_PH_BEGIN;
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    const bool direct = cpu->direct_suspend;
    const bool mandatory = direct || ssKernelNeedsFrame(cpu);
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    extern void ss_kernel_test_selecting(unsigned, bool) __attribute__((weak));
    if (ss_kernel_test_selecting) ss_kernel_test_selecting(c, mandatory);
#endif
    if (cpu->preempt || (!mandatory && ss_control_checkpoint(&ssKernelControl, c))) {
        cpu->reschedule = true;
        return;
    }
#ifdef SUNSTONE_SCHED_PMU
    unsigned pmu_kind=direct?SS_SCHED_PMU_SELECT_SELF:SS_SCHED_PMU_SELECT_OTHER;
    (void)ss_sched_pmu_prepare(c,pmu_kind);
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    ss_sched_trace_switch_begin(c);
#endif
#ifdef SUNSTONE_SCHED_PMU
    unsigned char pmu_scope;
    (void)ss_sched_pmu_begin_scope(c,pmu_kind,
        cpu->run.current?cpu->run.current->slot:UINT32_MAX,direct?SS_SCHED_PMU_SELF:0,&pmu_scope);
#endif
    struct ss_owner_task *previous = cpu->run.current;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    struct ss_owner_descriptor *previous_reference = previous ?
        &ssKernelRegistry.descriptor[previous->slot] : NULL;
    /* Current-owner exclusion proves this task cannot retire yet, including
     * after close. Retain that existing execution lifetime through service. */
    if (previous_reference && !direct)
        atomic_fetch_add_explicit(&previous_reference->references, 1, memory_order_relaxed);
#endif
    if (previous) {
        traceTASK_SWITCHED_OUT();
#if ( configGENERATE_RUN_TIME_STATS == 1 )
#ifdef portALT_GET_RUN_TIME_COUNTER_VALUE
        portALT_GET_RUN_TIME_COUNTER_VALUE(ulTotalRunTime[c]);
#else
        ulTotalRunTime[c] = portGET_RUN_TIME_COUNTER_VALUE();
#endif
        if (ulTotalRunTime[c] > ulTaskSwitchedInTime[c])
            ((TCB_t *)previous->context)->ulRunTimeCounter += ulTotalRunTime[c] - ulTaskSwitchedInTime[c];
        ulTaskSwitchedInTime[c] = ulTotalRunTime[c];
#endif
        taskCHECK_FOR_STACK_OVERFLOW();
#if ( configUSE_POSIX_ERRNO == 1 )
        ((TCB_t *)previous->context)->iTaskErrno = FreeRTOS_errno;
#endif
        struct ssKernelTask *s = &ssKernelTasks[previous->slot];
        s->off_cpu = true;
        ((TCB_t *)previous->context)->xTaskRunState = taskTASK_NOT_RUNNING;
        cpu->run.current = NULL;
        unsigned state = atomic_load_explicit(&s->execution, memory_order_acquire);
        if (state == SS_K_RUNNING || state == SS_K_READY) {
            atomic_store_explicit(&s->execution, SS_K_READY, memory_order_release);
            if (!s->detach_pending)
                configASSERT(ss_owner_enqueue(&cpu->run, previous, !cpu->yield_tail));
        }
        if (s->offcpu_wait) {
            s->offcpu_wait = false;
            ss_control_wake_on(&ssKernelControl, c, previous->slot, &s->off_cpu);
        }
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
        extern void ss_kernel_test_saved_frame(unsigned, unsigned) __attribute__((weak));
        if (ss_kernel_test_saved_frame) ss_kernel_test_saved_frame(c, previous->slot);
#endif
    }
    bool voluntary = cpu->explicit_yield;
    cpu->yield_tail = false;
    cpu->explicit_yield = false;
    cpu->selecting = true;
    if (!direct) ssKernelService();
    cpu->selecting = false;
    struct ss_owner_task *selected = NULL;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    if (previous && !voluntary && !direct) {
        struct ssKernelTask *s = &ssKernelTasks[previous->slot];
        if (((TCB_t *)previous->context)->xPreemptionDisable && previous->queued &&
            atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_READY &&
            atomic_load_explicit(&s->owner, memory_order_acquire) == c) {
            configASSERT(ss_owner_remove(&cpu->run, previous));
            cpu->run.current = selected = previous;
        }
    }
#else
    (void)voluntary;
#endif
    if (!selected) selected = ss_owner_select(&cpu->run);
    configASSERT(selected); /* A pinned idle task always exists on each CPU. */
    struct ssKernelTask *s = &ssKernelTasks[selected->slot];
    configASSERT(!s->dead && !s->suspended && s->off_cpu);
    configASSERT(atomic_load_explicit(&s->owner, memory_order_acquire) == c);
    atomic_store_explicit(&s->execution, SS_K_RUNNING, memory_order_release);
    s->off_cpu = false;
    ssKernelPullIdle(c, (((TCB_t *)selected->context)->uxTaskAttributes & taskATTRIBUTE_IS_IDLE) != 0);
    __atomic_store_n(&pxCurrentTCBs[c], selected->context, __ATOMIC_RELEASE);
    pxCurrentTCBs[c]->xTaskRunState = (BaseType_t)c;
#ifndef SS_KERNEL_TEST_NO_IDLE_PULL
    /* The selecting pass skips the pull, and the pull takes compatibility locks the selector
     * never takes. A migratable task left waiting behind the task just selected, with an
     * allowed CPU idle, would otherwise wait for an entry that may never come: ask for one.
     * The entry serves only an idle target, so it cannot repeat without a move. */
    if (!direct && cpu->run.ready_bitmap && !(((TCB_t *)selected->context)->uxTaskAttributes & taskATTRIBUTE_IS_IDLE) &&
        (ssKernelPullTargets(cpu, c) & atomic_load_explicit(&ssKernelIdleMask, memory_order_acquire)))
        ssKernelForce(c);
#endif
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    if (previous_reference && !direct) ss_owner_put(previous_reference);
#endif
    ssKernelSliceRefresh(c, true);
    traceTASK_SWITCHED_IN();
    portTASK_SWITCH_HOOK(pxCurrentTCBs[c]);
#if ( configUSE_POSIX_ERRNO == 1 )
    FreeRTOS_errno = pxCurrentTCBs[c]->iTaskErrno;
#endif
#if ( configUSE_C_RUNTIME_TLS_SUPPORT == 1 )
    configSET_TLS_BLOCK(pxCurrentTCBs[c]->xTLSBlock);
#endif
    cpu->reschedule = false;
    bool idle = (pxCurrentTCBs[c]->uxTaskAttributes & taskATTRIBUTE_IS_IDLE) != 0;
    if (ss_owner_commit_dispatch(&cpu->run, selected->priority, idle)) {
        cpu->reschedule = true;
        ssKernelForce(c);
    }
    cpu->direct_suspend = false;
    if (mandatory) {
        /* A freeze that starts after local suspension observes this CPU only
         * after its replacement task and dispatch state have been selected. */
        (void)ss_control_checkpoint(&ssKernelControl, c);
    }
    if (direct) {
        if (ss_control_has_pending(&ssKernelControl, c) || cpu->idle_pull_deferred)
            ssKernelForce(c);
    }
#ifdef SUNSTONE_SCHED_PMU
    (void)ss_sched_pmu_end_scope(c,&pmu_scope);
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    ss_sched_trace_switch_end(c, selected->slot);
#endif
    SS_PH_END(9);
}
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
void ss_sched_trace_wake(unsigned core, uint64_t ready, uint64_t running, unsigned busy)
{
    if (!busy) return;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    configASSERT(core == ssKernelCPUId());
    unsigned slot = ssKernelSelf()->uxOwnerSlot;
    portCLEAR_INTERRUPT_MASK(irq);
    ss_sched_trace_wake_sample(core, ready, running, slot);
}
#endif
#if defined(SUNSTONE_LATENCY) || defined(SS_KERNEL_TEST_ADMIN_ENTER)
bool ss_latency_task_suspended(void *task)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    bool suspended = false;
    if (ssKernelHandlePin(task, &pin)) {
        unsigned flags = ss_control_suspension(&ssKernelControl, pin.slot);
        suspended = (flags & (SS_CONTROL_SUSPENDED | SS_CONTROL_OFFCPU)) == SS_CONTROL_SUSPENDED &&
            atomic_load_explicit(&ssKernelTasks[pin.slot].execution, memory_order_acquire) == SS_K_SUSPENDED;
        ss_kernel_handle_put(&pin);
    }
    portCLEAR_INTERRUPT_MASK(irq);
    return suspended;
}
#endif
#ifdef SUNSTONE_OWNER_COUNTERS
/* Owner-work drains by CPU (measurement): from the outermost interrupt exit, skipped by the mask guard there, and
 * from the tick. Each CPU writes its own line. */
static struct { uint64_t exit_work, exit_skipped, tick_work, service_run, exit_max, yield_max, exit_over5, yield_over5, exit_raised, skip_pending, fast_switch, pad[5]; } ssOwnerCounters[8] __attribute__((aligned(64)));
extern uint64_t ullPortYieldRequired[];
uint64_t ulSunstoneOwnerCounters(unsigned cpu, unsigned which)
{
    if (cpu >= 8 || which > 10) return 0;
    const uint64_t *row = &ssOwnerCounters[cpu].exit_work;
    return row[which];
}
#define SS_OWNER_COUNT(field) (++ssOwnerCounters[ssKernelCPUId()].field)
/* A skipped exit that leaves a switch request pending: the outer exit must find it still set. */
#define SS_OWNER_PENDING() do { if (ullPortYieldRequired[ssKernelCPUId()]) SS_OWNER_COUNT(skip_pending); } while (0)
static inline uint64_t ssOwnerClock(void)
{
    uint64_t t;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(t) :: "memory");
    return t;
}
/* Time with PSTATE.I set spent in C, in counter ticks of 41.7 ns: the longest and the count over 5 us. */
static inline void ssOwnerSpan(uint64_t begin, bool yield)
{
    const uint64_t length = ssOwnerClock() - begin;
    typeof(ssOwnerCounters[0]) *row = &ssOwnerCounters[ssKernelCPUId()];
    uint64_t *max = yield ? &row->yield_max : &row->exit_max, *over = yield ? &row->yield_over5 : &row->exit_over5;
    if (length > *max) *max = length;
    if (length > 120) ++*over;
}
#else
#define SS_OWNER_COUNT(field) ((void)0)
#define SS_OWNER_PENDING() ((void)0)
#define ssOwnerClock() 0
#define ssOwnerSpan(begin, yield) ((void)(begin))
#endif
void ssKernelOwnerYieldTail(void)
{
    SS_PH_BEGIN;
    const uint64_t span_begin = ssOwnerClock();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    UBaseType_t previous_open = cpu->control_open_mask;
    if (irq > portMAX_API_PRIORITY_MASK) cpu->control_open_mask = irq;
    /* The port calls this at SVC entry with the outgoing frame saved, before
     * vTaskSwitchContext. Match IRQ entry's object/timeout maintenance phase. */
    if (!cpu->direct_suspend && !ssKernelNeedsFrame(cpu) && !cpu->preempt &&
        !ss_control_checkpoint(&ssKernelControl, ssKernelCPUId()))
        ssKernelService();
    struct ssKernelTask *s = ssKernelCurrent();
    unsigned state = atomic_load_explicit(&s->execution, memory_order_acquire);
    if (state == SS_K_RUNNING || state == SS_K_READY) cpu->yield_tail = cpu->explicit_yield = true;
    cpu->reschedule = true;
    cpu->control_open_mask = previous_open;
    portCLEAR_INTERRUPT_MASK(irq);
    ssOwnerSpan(span_begin, true);
    SS_PH_END(8);
}
void ssKernelOwnerYield(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    bool defer = cpu->preempt || ssKernelHolds(cpu);
    if (defer) cpu->yield_tail = cpu->explicit_yield = cpu->reschedule = true;
    portCLEAR_INTERRUPT_MASK(irq);
    /* The port restores only its global critical nesting count. Private
     * object holds must finish before that restoration can admit API IRQs. */
    if (defer) return;
    /* SVC saves the frame and calls OwnerYieldTail exactly once. */
    portYIELD();
}
static BaseType_t ssKernelOwnerIRQExitWork(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (cpu->control_draining) return pdFALSE;
    if (ssKernelNeedsFrame(cpu)) {
        cpu->reschedule = true;
        return cpu->preempt ? pdFALSE : pdTRUE;
    }
    if (!cpu->preempt && !ss_control_checkpoint(&ssKernelControl, ssKernelCPUId())) {
        SS_OWNER_COUNT(service_run);
        ssKernelService();
    }
    return cpu->reschedule && !cpu->preempt ? pdTRUE : pdFALSE;
}
#if defined(__aarch64__)
/* A local wake has already inserted its ready task. With PSTATE.I set, a
 * required switch can proceed directly to saving the outgoing frame when no
 * object, control or timer maintenance must precede selection. The selecting
 * service pass still drains incoming work and closes the dispatch handshake. */
static bool ssKernelIRQSwitchReady(unsigned c, struct ssKernelCPU *cpu)
{
    struct ss_owner_task *current = cpu->run.current;
    if (cpu->preempt || !cpu->reschedule || !current) return false;
    unsigned state = atomic_load_explicit(&ssKernelTasks[current->slot].execution,
                                          memory_order_acquire);
    if (state == SS_K_RUNNING || state == SS_K_READY) {
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
        if (((TCB_t *)current->context)->xPreemptionDisable) return false;
#endif
        if (!cpu->run.ready_bitmap) return false;
        unsigned priority = 31U - (unsigned)__builtin_clz(cpu->run.ready_bitmap);
        configASSERT(ss_owner_ready(&cpu->run, priority)->head);
        if (priority <= current->priority) return false;
    }
    if (ss_control_has_pending(&ssKernelControl, c) ||
        (cpu->object_repair[0] & ~atomic_load_explicit(&ssKernelControl.parked[c].word[0], memory_order_acquire)) ||
        (cpu->object_repair[1] & ~atomic_load_explicit(&ssKernelControl.parked[c].word[1], memory_order_acquire))) return false;
    uint64_t now = ssKernelNow(), deadline;
    if ((ss_deadline_next(&cpu->deadlines, &deadline) && deadline <= now) ||
        atomic_load_explicit(&cpu->slice_deadline, memory_order_relaxed) <= now)
        return false;
    /* A global suspension still acknowledges this owner's checkpoint. Do it
     * last so the ordinary fallback retains its existing checkpoint order. */
    return ssKernelNeedsFrame(cpu) || !ss_control_checkpoint(&ssKernelControl, c);
}
#ifdef SS_KERNEL_TEST_IRQ_EXIT
#ifndef SUNSTONE_OWNER_COUNTERS
#error "IRQ-exit regression requires owner service counters"
#endif
/* The linked IRQ fixture supplies real local wakes and injects one pending
 * maintenance condition after MakeReady. Each static node outlives its heap
 * membership; a stale generation expires without changing a task's wait. */
static struct {
    struct ss_deadline_entry stale;
    uint64_t services;
    unsigned active, slot;
} ssKernelIRQTests[configNUMBER_OF_CORES];
extern void ss_kernel_test_irq_result(unsigned kind, bool fast, uint64_t services);
bool ss_kernel_test_irq_prepare(unsigned kind)
{
    configASSERT(kind < 5);
    UBaseType_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    typeof(ssKernelIRQTests[0]) *test = &ssKernelIRQTests[c];
    configASSERT(!test->active);
    if (!ssKernelIRQSwitchReady(c, cpu)) {
        portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
        return false;
    }
    struct ssKernelTask *s = &ssKernelTasks[cpu->run.current->slot];
    if (atomic_load_explicit(&s->execution, memory_order_acquire) != SS_K_RUNNING ||
        s->wait_kind != SS_K_NONE) {
        portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
        return false;
    }
    test->slot = s->ready.slot;
    if (kind == 1) ss_control_repair(&ssKernelControl, test->slot);
    else if (kind == 2) {
        configASSERT(!test->stale.owner && cpu->deadlines.count < SS_DEADLINE_CAPACITY);
        configASSERT(s->generation != UINT64_MAX);
        ss_deadline_entry_init(&test->stale);
        configASSERT(ss_deadline_insert(&cpu->deadlines, &test->stale, s,
                                       UINT64_MAX, ssKernelNow()));
    } else if (kind == 3) {
        atomic_store_explicit(&cpu->slice_deadline, ssKernelNow(), memory_order_release);
    } else if (kind == 4) {
        TCB_t *t = s->ready.context;
        configASSERT(!s->event_reference.descriptor && !listLIST_ITEM_CONTAINER(&t->xEventListItem));
        s->object_requested = s->object_sequence;
        s->object_requested_priority = s->object_priority;
        cpu->object_repair[test->slot / 64] |= UINT64_C(1) << (test->slot % 64);
        ss_control_repair(&ssKernelControl, test->slot);
    }
    test->active = kind + 1;
    test->services = ssOwnerCounters[c].service_run;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return true;
}
static void ssKernelIRQTestComplete(bool fast)
{
    unsigned c = ssKernelCPUId();
    typeof(ssKernelIRQTests[0]) *test = &ssKernelIRQTests[c];
    if (!test->active) return;
    unsigned kind = test->active - 1;
    configASSERT(!test->stale.owner);
    configASSERT(!(ssKernelCPUs[c].object_repair[test->slot / 64] &
                   (UINT64_C(1) << (test->slot % 64))));
    test->active = 0;
    ss_kernel_test_irq_result(kind, fast, ssOwnerCounters[c].service_run - test->services);
}
#else
#define ssKernelIRQTestComplete(fast) ((void)0)
#endif
#endif
/* Ordinary post-EOI exit owns maintenance. API-masked callers defer it; a
 * fully admitted control preparation interval uses control_draining to reject
 * nested owner selection. Both paths preserve pending work for the next
 * ordinary entry. PMR still contains the interrupted caller's mask here. */
BaseType_t ssKernelOwnerIRQExit(void)
{
    if (ssKernelCPUs[ssKernelCPUId()].control_draining) return 2;
#if !defined(__aarch64__)
    /* The host fixture has no priority mask, so the exit work runs as it did before the mask guard existed. */
    return ssKernelOwnerIRQExitWork();
#else
    uint64_t pmr;
    __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(pmr));
    if (pmr <= portMAX_API_PRIORITY_MASK) { SS_OWNER_COUNT(exit_skipped); SS_OWNER_PENDING(); return 2; } /* tells the exit path not to switch tasks */
    SS_OWNER_COUNT(exit_work);
    const uint64_t begin = ssOwnerClock();
    SS_PH_BEGIN;
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    const bool trace_owner = ss_sched_trace_irq_begin(ssKernelCPUId(), ss_sched_trace_now());
#endif
#if !defined(SS_KERNEL_TEST_IRQ_SLOW)
    bool ready = ssKernelIRQSwitchReady(ssKernelCPUId(), &ssKernelCPUs[ssKernelCPUId()]);
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    if (trace_owner) ss_sched_trace_irq_guard(ssKernelCPUId());
#endif
    if (ready) {
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
        if (trace_owner) ss_sched_trace_irq_fast(ssKernelCPUId());
#endif
        SS_OWNER_COUNT(fast_switch);
        SS_OWNER_COUNT(exit_raised);
        ssKernelIRQTestComplete(true);
        ssOwnerSpan(begin, false);
        SS_PH_END(1);
        return pdTRUE;
    }
#elif defined(SUNSTONE_SCHED_LATENCY_TRACE)
    if (trace_owner) ss_sched_trace_irq_guard(ssKernelCPUId());
#endif
    /* Service masks local queue commits, then opens PMR and DAIF.I for each
     * bounded control batch's FIFO preparation and release. Nested API IRQs
     * publish incoming hints while that batch excludes owner reentry. */
    const UBaseType_t previous = (UBaseType_t)pmr;
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    if (trace_owner) ss_sched_trace_irq_stamp(ssKernelCPUId(), 1);
#endif
    const BaseType_t result = ssKernelOwnerIRQExitWork();
    ssKernelIRQTestComplete(false);
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    if (trace_owner) ss_sched_trace_irq_stamp(ssKernelCPUId(), 2);
#endif
    /* Set PSTATE.I before restoring the mask: portCLEAR_INTERRUPT_MASK_FROM_ISR clears it after the write, and an
     * ordinary interrupt taken then would be a second outermost exit inside this one. */
    __asm__ volatile("msr daifset, #2\n dsb sy\n isb\n msr icc_pmr_el1, %0\n dsb sy\n isb" :: "r"((uint64_t)previous) : "memory");
#ifdef SUNSTONE_LATENCY
    /* The mask was raised through portSET_INTERRUPT_MASK_FROM_ISR, which opens the latency build's masked span; this
     * restore bypasses portCLEAR_INTERRUPT_MASK_FROM_ISR, so close the span here, as that macro would. Otherwise it
     * stays open until the next instrumented clear on this core, up to a tick later on an idle core. */
    if (previous > portMAX_API_PRIORITY_MASK) ss_latency_mask_close(portGET_CORE_ID(), (uintptr_t)__builtin_return_address(0));
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    if (trace_owner) {
        ss_sched_trace_irq_stamp(ssKernelCPUId(), 3);
        ss_sched_trace_irq_end(ssKernelCPUId(), (unsigned)result);
    }
#endif
    ssOwnerSpan(begin, false);
    SS_PH_END(1);
    if (result == pdTRUE) SS_OWNER_COUNT(exit_raised);
    return result;
#endif
}
/* The port's tick handler brackets the tick with these instead of an ISR
 * critical section. The tick reads published deadlines and slice deadlines,
 * advances time and sends SGIs: none of that needs the global ISR lock, and it
 * leaves no publication or command record to flush. IRQ entry has masked
 * interrupts. The preemption count still holds the tick's reschedule request
 * for the interrupt exit, as the critical section did. */
UBaseType_t ssKernelTickEnter(void)
{
    if (!xSchedulerRunning) return 0;
    configASSERT(ssKernelIrqsDisabled());
    ++ssKernelCPUs[ssKernelCPUId()].preempt;
    return 1;
}
void ssKernelTickExit(UBaseType_t entered)
{
    if (!entered) return;
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    configASSERT(cpu->preempt);
    --cpu->preempt;
}
static BaseType_t ssKernelTick(void)
{
    /* CPU0 alone advances stage-B time. Remote CPUs receive only due-work or
     * slice requests; CPU0 never touches a remote timeout or ready link. */
    configASSERT(ssKernelCPUId() == 0);
    uint64_t now = atomic_fetch_add_explicit(&ssKernelTime, 1, memory_order_acq_rel) + 1;
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
#ifdef SUNSTONE_LATENCY_CAUSE
        ss_latency_cause_state_begin(c);
#endif
        uint64_t next;
        enum ss_deadline_state state = ss_deadline_snapshot(&ssKernelCPUs[c].deadlines.published, &next);
#ifdef SUNSTONE_LATENCY_CAUSE
        uint64_t cause_slice = atomic_load_explicit(&ssKernelCPUs[c].slice_deadline, memory_order_acquire);
        bool cause_due = state == SS_DEADLINE_UNKNOWN || (state == SS_DEADLINE_VALUE && next <= now) || cause_slice <= now;
        if (cause_due) {
#else
        if (state == SS_DEADLINE_UNKNOWN || (state == SS_DEADLINE_VALUE && next <= now) ||
            atomic_load_explicit(&ssKernelCPUs[c].slice_deadline, memory_order_acquire) <= now) {
#endif
            if (c) ssKernelForce(c);
            else ssKernelCPUs[0].reschedule = true;
        }
#ifdef SUNSTONE_LATENCY_CAUSE
        ss_latency_cause_state_end(c, (unsigned)state, state == SS_DEADLINE_VALUE ? next : 0, cause_slice, cause_due);
#endif
    }
    SS_OWNER_COUNT(tick_work);
    /* The ordinary post-EOI exit performs maintenance with API IRQs admitted.
     * CPU0 already owns that entry; a local SGI would repeat the same work. */
    return ssKernelCPUs[0].reschedule && !ssKernelCPUs[0].preempt ? pdTRUE : pdFALSE;
}
/* Test hook: the depth of the interrupt-lock sections when a waiter is inserted into an event list. */
extern void ss_kernel_test_event_insert(unsigned isr_depth) __attribute__((weak));
static void ssKernelEventBlock(List_t *list, TickType_t value, TickType_t ticks,
                               bool unordered, bool indefinite)
{
    struct ssKernelTask *s = ssKernelCurrent();
    TCB_t *t = s->ready.context;
    configASSERT(!s->event_reference.descriptor);
    configASSERT(ssKernelHandlePin(t, &s->event_reference));
    s->unordered = unordered;
    /* The caller holds its object's task-side lock (a suspension or a critical section). Insert under the interrupt-side
     * lock of the same slot as well, so that an owner timing out another waiter of this list cannot edit it at the same
     * moment, and record the slot for that owner and for priority repair. */
    struct ssKernelCPU *self = &ssKernelCPUs[ssKernelCPUId()];
    configASSERT(self->o_depth);
    s->event_slot = self->t_slot;
    UBaseType_t irq = ssObjectIsrEnterSlot(self->t_slot, list, (uintptr_t)__builtin_return_address(0));
    if (ss_kernel_test_event_insert) ss_kernel_test_event_insert(self->o_isr + self->o_critical);
    if (unordered) {
        listSET_LIST_ITEM_VALUE(&t->xEventListItem, value | taskEVENT_LIST_ITEM_VALUE_IN_USE);
        vListInsertEnd(list, &t->xEventListItem);
    } else vListInsert(list, &t->xEventListItem);
    ssObjectIsrLeave(irq);
    ssKernelBlock(ticks, indefinite, SS_K_EVENT);
}
static BaseType_t ssKernelEventWake(ListItem_t *item, TickType_t value, bool unordered)
{
    TCB_t *t = listGET_LIST_ITEM_OWNER(item);
    /* The object list owns this reference before reading any TCB field. */
    struct ssKernelTask *s = &ssKernelTasks[t->uxOwnerSlot];
    configASSERT(s->event_reference.descriptor);
    unsigned expected = SS_K_WAITING;
    bool won = atomic_compare_exchange_strong_explicit(&s->outcome, &expected, SS_K_EVENT_READY,
                                                      memory_order_acq_rel, memory_order_acquire);
    listREMOVE_ITEM(item);
    if (unordered) listSET_LIST_ITEM_VALUE(item, value | taskEVENT_LIST_ITEM_VALUE_IN_USE);
    if (won) tracePOST_MOVED_TASK_TO_READY_STATE(t);
    struct ss_kernel_pin pin = s->event_reference;
    s->event_reference = (struct ss_kernel_pin){0};
    unsigned owner = atomic_load_explicit(&s->owner, memory_order_acquire);
    unsigned priority = atomic_load_explicit(&s->effective, memory_order_acquire);
    bool yield = won && owner == ssKernelCPUId() &&
                 priority > ssKernelCPUs[owner].run.current->priority;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    if (yield && ((TCB_t *)ssKernelCPUs[owner].run.current->context)->xPreemptionDisable)
        yield = false;
#endif
    if (won) ssKernelDefer(pin); else ss_kernel_handle_put(&pin);
    return yield ? pdTRUE : pdFALSE;
}
static BaseType_t ssKernelNotify(TaskHandle_t task, UBaseType_t index,
                                 uint32_t value, eNotifyAction action,
                                 uint32_t *previous, BaseType_t *woken, bool from_isr)
{
    configASSERT(index < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    static const enum ss_notification_action actions[] = {
        SS_NOTIFY_NO_ACTION, SS_NOTIFY_SET_BITS, SS_NOTIFY_INCREMENT,
        SS_NOTIFY_OVERWRITE, SS_NOTIFY_NO_OVERWRITE
    };
    configASSERT((unsigned)action < sizeof(actions) / sizeof(actions[0]));
    SS_PH_BEGIN;
    /* An interrupt handler runs with PSTATE.I set, so no API-level interrupt can enter the section and raising the priority
     * mask would only cost two data synchronization barriers and two instruction barriers on each side. Interrupts above
     * the API mask never call the kernel. */
    const bool masked_by_exception = from_isr && ssKernelIrqsDisabled();
    UBaseType_t irq = masked_by_exception ? 0 : from_isr ? portSET_INTERRUPT_MASK_FROM_ISR() : portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    if (!ssKernelHandlePin(task, &pin)) {
        if (masked_by_exception) {} else if (from_isr) {
#ifdef SUNSTONE_LATENCY_CAUSE
            ss_latency_cause_mask_tail_begin();
#endif
            portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
#ifdef SUNSTONE_LATENCY_CAUSE
            ss_latency_cause_mask_tail_end();
#endif
        } else portCLEAR_INTERRUPT_MASK(irq);
        return pdFAIL;
    }
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    struct ss_notification_result result = ss_notification_apply(&s->notification[index],
                                             actions[action], value, &ssKernelBackend);
    if (previous) *previous = result.previous;
    if (result.completed_wait) {
        /* The notification payload transition uniquely won this wait. Record
         * the ready origin on the caller before route admission, SGI or owner
         * consumption, so remote activation time remains in the measurement. */
        tracePOST_MOVED_TASK_TO_READY_STATE(pin.descriptor->task);
        unsigned c = ssKernelCPUId();
        bool local_higher = atomic_load_explicit(&s->owner, memory_order_acquire) == c &&
            atomic_load_explicit(&s->effective, memory_order_acquire) > ssKernelCPUs[c].run.current->priority;
        ssKernelPublishReference(pin);
        if (woken && (local_higher || ssKernelCPUs[c].reschedule)) *woken = pdTRUE;
    } else ss_kernel_handle_put(&pin);
    if (masked_by_exception) {} else if (from_isr) {
#ifdef SUNSTONE_LATENCY_CAUSE
            ss_latency_cause_mask_tail_begin();
#endif
            portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
#ifdef SUNSTONE_LATENCY_CAUSE
            ss_latency_cause_mask_tail_end();
#endif
        } else portCLEAR_INTERRUPT_MASK(irq);
    SS_PH_END(0);
    return result.accepted ? pdPASS : pdFAIL;
}
static uint32_t ssKernelNotifyWait(UBaseType_t index, uint32_t clear_entry,
                                   uint32_t clear_exit, bool count, bool decrement,
                                   TickType_t ticks, bool *matched)
{
    configASSERT(index < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    UBaseType_t irq = ssKernelTaskMask();
    struct ssKernelTask *s = ssKernelCurrent();
    struct ss_notification_guard guard;
    ss_notification_gate_begin(&s->notification[index], &ssKernelBackend, &guard);
    configASSERT(s->generation != UINT64_MAX);
    uint64_t generation = s->generation + 1;
    bool block = ss_notification_wait_enter(&guard, &s->notify_wait, generation,
                      count ? SS_NOTIFY_WAIT_COUNT : SS_NOTIFY_WAIT_STATE, clear_entry, ticks != 0);
    if (block) {
        s->notify_index = (unsigned)index;
        ssKernelBlock(ticks, true, SS_K_NOTIFICATION);
        configASSERT(s->generation == generation);
    } else s->generation = generation;
    ss_notification_gate_end(&guard);
    portCLEAR_INTERRUPT_MASK(irq);
    if (block) portYIELD();
    irq = portSET_INTERRUPT_MASK();
    ss_notification_gate_begin(&s->notification[index], &ssKernelBackend, &guard);
    bool timeout = !ticks || atomic_load_explicit(&s->outcome, memory_order_acquire) != SS_K_EVENT_READY;
    struct ss_notification_finish result = ss_notification_wait_finish(&guard, &s->notify_wait,
                                      generation, clear_exit, decrement, timeout);
    ss_notification_gate_end(&guard);
    portCLEAR_INTERRUPT_MASK(irq);
    if (matched) *matched = result.matched && result.condition;
    return result.value;
}

/* Owner callbacks retain the per-task control FIFO. Object preparation uses a
 * nonblocking I-lock attempt and a short masked list commit before applying
 * local ready/deadline changes. The control reference spans repair and ack. */
static bool ssKernelObjectPrepared(struct ssKernelTask *s,
                                    const struct ss_control_state *state,
                                    unsigned priority)
{
    bool detach = state->dead || state->suspended || state->offcpu_sequence > s->offcpu_ack ||
        (state->abort_generation != s->abort_generation && state->abort_wait_generation == s->generation);
    if (s->object_sequence == state->sequence + 1 && s->object_priority == priority &&
        s->object_detach == detach && s->object_wait_generation == s->generation)
        return true;
    s->object_requested = state->sequence + 1;
    s->object_requested_priority = priority;
    s->object_detach = detach;
    s->object_wait_generation = s->generation;
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    uint64_t bit = UINT64_C(1) << (s->ready.slot % 64);
    TCB_t *t = s->ready.context;
    unsigned execution = atomic_load_explicit(&s->execution, memory_order_acquire);
    /* A running current task, or a task already off CPU outside an event
     * wait, cannot insert this item concurrently. Event-blocked producers
     * retain the object's I lock even while removing their list reference. */
    bool exclusive = (cpu->run.current == &s->ready && !s->off_cpu && execution == SS_K_RUNNING) ||
        (s->off_cpu && (execution != SS_K_BLOCKED || s->wait_kind != SS_K_EVENT));
    if (exclusive &&
        !s->event_reference.descriptor && !listLIST_ITEM_CONTAINER(&t->xEventListItem)) {
        if (!(s->object_detach && s->wait_kind == SS_K_EVENT) &&
            !s->unordered && !(listGET_LIST_ITEM_VALUE(&t->xEventListItem) & taskEVENT_LIST_ITEM_VALUE_IN_USE))
            listSET_LIST_ITEM_VALUE(&t->xEventListItem, configMAX_PRIORITIES - priority);
        s->object_sequence = s->object_requested;
        s->object_priority = s->object_requested_priority;
        /* An older deferred request must not edit a later wait by this task. */
        cpu->object_repair[s->ready.slot / 64] &= ~bit;
        return true;
    }
    cpu->object_repair[s->ready.slot / 64] |= bit;
    return false;
}
/* The FIFO protects this record while preparation runs with API IRQs open.
 * Only the object's short list commit is masked. A busy object parks the
 * retained control until that object's release signals its owner. */
static bool ssKernelControlPrepare(void *context, unsigned c, unsigned slot)
{
    (void)context;
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
#if defined(SS_KERNEL_TEST_ADMIN_ENTER) && defined(__aarch64__)
    if (xSchedulerRunning && ((ullPortGetTaskRegister() >> portTASK_REGISTER_EL1_BIT) & 1U)) {
        uint64_t pmr, daif;
        __asm__ volatile("mrs %0, icc_pmr_el1\n mrs %1, daif" : "=r"(pmr), "=r"(daif));
        configASSERT(pmr > portMAX_API_PRIORITY_MASK && !(daif & UINT64_C(0x80)));
        extern void ss_kernel_test_control_prepare(unsigned, unsigned) __attribute__((weak));
        if (ss_kernel_test_control_prepare) ss_kernel_test_control_prepare(c, slot);
    }
#endif
    uint64_t bit = UINT64_C(1) << (slot % 64);
    if (!(cpu->object_repair[slot / 64] & bit)) return true;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    TCB_t *t = s->ready.context;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    if (s->object_wait_generation != s->generation) {
        cpu->object_repair[slot / 64] &= ~bit;
        s->object_sequence = 0;
        portCLEAR_INTERRUPT_MASK(irq);
        return true;
    }
    const bool waiting = s->wait_kind == SS_K_EVENT;
    if (waiting && !ssObjectITryAcquire(cpu, c, s->event_slot)) {
        portCLEAR_INTERRUPT_MASK(irq);
        ssObjectRepairPark(s->event_slot, c, slot);
        return false;
    }
    if (waiting) ++cpu->o_isr;
    List_t *list = listLIST_ITEM_CONTAINER(&t->xEventListItem);
    if (s->object_detach && waiting) {
        if (list) {
            listREMOVE_ITEM(&t->xEventListItem);
            ss_kernel_handle_put(&s->event_reference);
        }
    } else if (!s->unordered && !(listGET_LIST_ITEM_VALUE(&t->xEventListItem) & taskEVENT_LIST_ITEM_VALUE_IN_USE)) {
        if (list) listREMOVE_ITEM(&t->xEventListItem);
        listSET_LIST_ITEM_VALUE(&t->xEventListItem, configMAX_PRIORITIES - s->object_requested_priority);
        if (list) vListInsert(list, &t->xEventListItem);
    }
    s->object_sequence = s->object_requested;
    s->object_priority = s->object_requested_priority;
    cpu->object_repair[slot / 64] &= ~bit;
    if (waiting) {
        configASSERT(cpu->o_isr == 1 && !cpu->o_critical);
        --cpu->o_isr;
        ssObjectIRelease(cpu, c, 0);
    }
    portCLEAR_INTERRUPT_MASK(irq);
    return true;
}
static bool ssKernelControlPause(void *context, unsigned c)
{
    (void)context;
    return ssKernelCPUs[c].reschedule || ss_owner_has_pending(&ssKernelCPUs[c].run);
}
static void ssKernelControlDrain(void)
{
    const bool exception = ((ullPortGetTaskRegister() >> portTASK_REGISTER_EL1_BIT) & 1U) != 0;
    const bool disabled = ssKernelIrqsDisabled();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(!cpu->critical_depth && !cpu->o_critical);
    configASSERT(!cpu->isr_depth && !cpu->o_isr);
    if (cpu->control_draining) { portCLEAR_INTERRUPT_MASK(irq); return; }
    unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
    if (xSchedulerRunning && (gate == SS_GATE_ACQUIRING || gate == SS_GATE_HELD)) {
        if (ssKernelNeedsFrame(cpu)) {
            cpu->reschedule = true;
            ssKernelForce(c);
        }
        else if (!cpu->preempt) (void)ss_control_checkpoint(&ssKernelControl, c);
        portCLEAR_INTERRUPT_MASK(irq);
        return;
    }
    UBaseType_t open = irq > portMAX_API_PRIORITY_MASK ? irq : cpu->control_open_mask;
    /* An explicit caller mask is not permission to admit interrupts. Ordinary
     * post-EOI or task-context maintenance will consume these requests. */
    if (xSchedulerRunning && open <= portMAX_API_PRIORITY_MASK) {
        portCLEAR_INTERRUPT_MASK(irq);
        return;
    }
    if (!cpu->service_depth) cpu->control_seen[0] = cpu->control_seen[1] = 0;
    cpu->control_draining = true;
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    ss_sched_trace_drain_begin(c);
#endif
    if (xSchedulerRunning) {
        if (exception) portCLEAR_INTERRUPT_MASK_FROM_ISR(open);
        else portCLEAR_INTERRUPT_MASK(open);
    }
    (void)ss_control_drain_excluding(&ssKernelControl, c, cpu->control_seen);
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t orphan = cpu->object_repair[w] &
            ~atomic_load_explicit(&ssKernelControl.parked[c].word[w], memory_order_acquire) &
            ~atomic_load_explicit(&ssKernelControl.pending[c].word[w], memory_order_acquire);
        configASSERT(!orphan);
    }
    extern void ss_kernel_test_after_control_batch(unsigned) __attribute__((weak));
    if (ss_kernel_test_after_control_batch) ss_kernel_test_after_control_batch(c);
#endif
    if (xSchedulerRunning) {
        if (exception) (void)portSET_INTERRUPT_MASK_FROM_ISR();
        else (void)portSET_INTERRUPT_MASK();
    }
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    ss_sched_trace_drain_end(c);
#endif
    cpu->control_draining = false;
    gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
    if (xSchedulerRunning && (gate == SS_GATE_ACQUIRING || gate == SS_GATE_HELD)) {
        if (ssKernelNeedsFrame(cpu)) {
            cpu->reschedule = true;
            ssKernelForce(c);
        } else if (!cpu->preempt) (void)ss_control_checkpoint(&ssKernelControl, c);
    }
#if defined(__aarch64__)
    if (disabled) __asm__ volatile("msr daifset, #2" ::: "memory");
#else
    (void)disabled;
#endif
    portCLEAR_INTERRUPT_MASK(irq);
}
static void ssKernelWait(unsigned slot, uint64_t sequence, uint64_t incarnation)
{
    while (!ss_control_applied(&ssKernelControl, slot, sequence) &&
           atomic_load_explicit(&ssKernelCleanupCompleted[slot], memory_order_acquire) < incarnation) {
        /* Re-enter this CPU's owner with API IRQs excluded, including after a
         * migrated task resumes its wait on a different CPU. Restore the
         * caller's mask before waiting for another owner's progress. */
        ssKernelControlDrain();
        bool applied = ss_control_applied(&ssKernelControl, slot, sequence) ||
            atomic_load_explicit(&ssKernelCleanupCompleted[slot], memory_order_acquire) >= incarnation;
        if (!applied) ss_owner_wait(ssKernelControl.backend);
    }
}
static bool ssKernelApplyMasked(void *context, unsigned slot, void *task,
                           const struct ss_control_state *state)
{
    (void)context;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    TCB_t *t = task;
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(atomic_load_explicit(&s->owner, memory_order_acquire) == c);
    unsigned suspension = ss_control_suspension(&ssKernelControl, slot);
    struct ss_control_state live = *state;
    live.suspended = (suspension & SS_CONTROL_SUSPENDED) != 0;
    state = &live;
    struct ss_control_state guarded;
    bool deferred = atomic_load_explicit(&s->cleanup_shield, memory_order_acquire) &&
        (state->dead || (state->suspended && !(suspension & SS_CONTROL_SELF)) ||
         ((suspension & SS_CONTROL_REMOTE_FRAME) && !(suspension & SS_CONTROL_SELF)) ||
         (state->offcpu_sequence > s->offcpu_ack && !(suspension & SS_CONTROL_SELF_FRAME)) ||
         state->abort_generation != s->abort_generation);
    if (deferred) {
        /* Resource destruction may block or migrate. Keep its caller alive
         * and pin-admissible, but still apply donation/base-priority repair. */
        s->cleanup_control_deferred = true;
        guarded = *state;
        guarded.dead = false;
        guarded.suspended = (suspension & SS_CONTROL_SELF) && state->suspended;
        if (!(suspension & SS_CONTROL_SELF_FRAME)) guarded.offcpu_sequence = s->offcpu_ack;
        guarded.abort_generation = s->abort_generation;
        state = &guarded;
    }
    if (state->dead) configASSERT(ss_kernel_handle_close(&ssKernelRegistry, slot));
    unsigned priority = state->priority;
    unsigned donation = atomic_load_explicit(&s->donation, memory_order_acquire);
    if (donation > priority) priority = donation;
    if (!ssKernelObjectPrepared(s, state, priority)) return false;
    bool eligible = (state->affinity & (UINT64_C(1) << c)) != 0;
    s->detach_pending = !eligible;
    if (!eligible && s->ready.queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));
    if (state->dead || state->offcpu_sequence > s->offcpu_ack || state->suspended) {
        if (s->ready.queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));
        ssKernelCancel(s);
        s->suspended = state->suspended;
        s->dead = state->dead;
        atomic_store_explicit(&s->execution, state->dead ? SS_K_DEAD : SS_K_SUSPENDED,
                               memory_order_release);
        if (!s->off_cpu) {
            s->offcpu_wait = true;
            ss_control_park_on(&ssKernelControl, c, slot, &s->off_cpu);
            cpu->reschedule = true;
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
            extern void ss_kernel_test_remote_suspend_mark(unsigned) __attribute__((weak));
            if (ss_kernel_test_remote_suspend_mark) ss_kernel_test_remote_suspend_mark(c);
#endif
            return false;
        }
        /* This frame and its required queue cancellation are complete even
         * when a separate destructive request waits for cleanup's shield.
         * A remote frame source survives Resume and retains its newer cookie;
         * completing only the older self frame must not acknowledge it. */
        bool pending_remote = deferred && (suspension & SS_CONTROL_REMOTE_FRAME) &&
            !(suspension & SS_CONTROL_SELF);
        if (!pending_remote && state->offcpu_sequence > s->offcpu_ack)
            s->offcpu_ack = state->offcpu_sequence;
        unsigned completed_frame = SS_CONTROL_SELF_FRAME |
            (pending_remote ? 0u : SS_CONTROL_OFFCPU | SS_CONTROL_REMOTE_FRAME);
        atomic_fetch_and_explicit(&ssKernelControl.slot[slot].suspension,
            ~completed_frame, memory_order_release);
        if (!pending_remote) ssKernelControl.slot[slot].desired.offcpu_sequence = 0;
    }
    if (s->ready.priority != priority) {
        bool queued = s->ready.queued;
        if (queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));
        s->ready.priority = priority;
        t->uxPriority = priority;
        if (queued && eligible) configASSERT(ss_owner_enqueue(&cpu->run, &s->ready, false));
        atomic_store_explicit(&s->effective, priority, memory_order_release);
        cpu->reschedule = true;
    }
    t->uxBasePriority = state->priority;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    bool enabling_preemption = t->xPreemptionDisable && !state->preemption_disabled;
    t->xPreemptionDisable = state->preemption_disabled ? pdTRUE : pdFALSE;
    if (enabling_preemption && cpu->run.current == &s->ready && cpu->run.ready_bitmap &&
        31U - (unsigned)__builtin_clz(cpu->run.ready_bitmap) > s->ready.priority)
        cpu->reschedule = true;
#endif
    t->uxCoreAffinityMask = (UBaseType_t)state->affinity;
    atomic_store_explicit(&s->base, state->priority, memory_order_release);
    if (state->dead || state->suspended) return !deferred;
    if (s->suspended || atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_SUSPENDED) {
        s->suspended = false;
        ssKernelMakeReady(s);
    }
    if (state->abort_generation != s->abort_generation) {
        if (s->generation == state->abort_wait_generation &&
            atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_BLOCKED) {
            bool trace_ready = atomic_load_explicit(&s->outcome, memory_order_acquire) == SS_K_WAITING;
            if (s->wait_kind == SS_K_NOTIFICATION) {
                struct ss_notification_guard guard;
                struct ss_notification *notification = &s->notification[s->notify_index];
                if (!ss_notification_gate_try_begin(notification, &ssKernelBackend, &guard)) {
                    ss_control_park_on(&ssKernelControl, c, slot, notification);
                    ss_notification_owner_wait(notification, c, &ssKernelBackend);
                    return false;
                }
                bool notified = ss_notification_wait_observe(&guard, &s->notify_wait, s->generation);
                trace_ready = trace_ready && !notified;
                if (!notified) {
                    guard.payload &= ~SS_NOTIFICATION_STATE;
                    s->notify_wait.blocked = false;
                }
                atomic_store_explicit(&s->outcome, SS_K_ABORTED, memory_order_release);
                if (trace_ready) tracePOST_MOVED_TASK_TO_READY_STATE(t);
                ss_notification_gate_end(&guard);
            } else {
                atomic_store_explicit(&s->outcome, SS_K_ABORTED, memory_order_release);
                if (trace_ready) tracePOST_MOVED_TASK_TO_READY_STATE(t);
            }
#if INCLUDE_xTaskAbortDelay == 1
            if (trace_ready && s->wait_kind == SS_K_EVENT) t->ucDelayAborted = pdTRUE;
#endif
            ssKernelMakeReady(s);
        }
        s->abort_generation = state->abort_generation;
    }
    s->resume_generation = state->resume_generation;
    ssKernelInspect(NULL, slot, priority);
    if (!eligible && !s->off_cpu) {
        s->offcpu_wait = true;
        ss_control_park_on(&ssKernelControl, c, slot, &s->off_cpu);
        cpu->reschedule = true;
        return false;
    }
    return !deferred;
}
static bool ssKernelApply(void *context, unsigned slot, void *task,
                          const struct ss_control_state *state)
{
    (void)state;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    ss_control_refresh_suspension(&ssKernelControl, slot);
    bool result = ssKernelApplyMasked(context, slot, task, &ssKernelControl.slot[slot].desired);
    portCLEAR_INTERRUPT_MASK(irq);
    return result;
}
static bool ssKernelControlDeferred(void *context, unsigned slot)
{
    (void)context;
    unsigned c = ssKernelCPUId();
    uint64_t bit = UINT64_C(1) << (slot % 64);
    if (atomic_load_explicit(&ssKernelControl.parked[c].word[slot / 64], memory_order_acquire) & bit)
        return true;
    if (ssKernelCPUs[c].object_repair[slot / 64] & bit) return false;
    return atomic_load_explicit(&ssKernelTasks[slot].cleanup_shield, memory_order_acquire) &&
        ssKernelTasks[slot].cleanup_control_deferred;
}
static bool ssKernelDetachMasked(void *context, unsigned slot, void *task,
                            struct ss_control_transfer *transfer)
{
    (void)context; (void)task;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (!s->off_cpu) {
        s->detach_pending = s->offcpu_wait = true;
        ss_control_park_on(&ssKernelControl, ssKernelCPUId(), slot, &s->off_cpu);
        cpu->reschedule = true;
        return false;
    }
    s->detach_pending = false;
    unsigned execution = atomic_load_explicit(&s->execution, memory_order_acquire);
    configASSERT(execution != SS_K_TRANSFERRING);
    atomic_store_explicit(&s->transfer_execution, execution, memory_order_relaxed);
    atomic_store_explicit(&s->execution, SS_K_TRANSFERRING, memory_order_release);
    if (s->ready.queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));
    transfer->deadline = s->deadline.owner ? s->deadline.deadline : UINT64_MAX;
    transfer->wait_generation = s->generation;
    transfer->wait_outcome = atomic_load_explicit(&s->outcome, memory_order_acquire);
    ssKernelCancel(s);
    /* Registration takes administration before this control FIFO. Migration
     * must never reverse that order merely to update placement estimates. */
    configASSERT(atomic_load_explicit(&ssKernelLoad[transfer->old_owner], memory_order_relaxed));
    atomic_fetch_sub_explicit(&ssKernelLoad[transfer->old_owner], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&ssKernelLoad[transfer->new_owner], 1, memory_order_relaxed);
    /* Keep source identity while neither owner may activate the task. The
     * destination publishes identity only after adopting its queue/deadline. */
    return true;
}
static bool ssKernelDetach(void *context, unsigned slot, void *task,
                           struct ss_control_transfer *transfer)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    bool result = ssKernelDetachMasked(context, slot, task, transfer);
    portCLEAR_INTERRUPT_MASK(irq);
    return result;
}
static void ssKernelAdoptMasked(void *context, unsigned slot, void *task,
                           const struct ss_control_transfer *transfer)
{
    (void)context; (void)task;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    unsigned c = ssKernelCPUId();
    configASSERT(s->off_cpu && c == transfer->new_owner && !s->ready.queued);
    configASSERT(atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_TRANSFERRING);
    if (transfer->deadline != UINT64_MAX)
        configASSERT(ss_deadline_insert(&ssKernelCPUs[c].deadlines, &s->deadline,
                                        s, transfer->wait_generation, transfer->deadline));
    atomic_store_explicit(&s->owner, c, memory_order_release);
    atomic_store_explicit(&s->execution,
        atomic_load_explicit(&s->transfer_execution, memory_order_relaxed), memory_order_release);
    ((TCB_t *)task)->uxCoreAffinityMask = (UBaseType_t)ssKernelControl.slot[slot].desired.affinity;
    ssKernelInspect(NULL, slot, atomic_load_explicit(&s->effective, memory_order_acquire));
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    extern void ss_kernel_test_ineligible_adopt(unsigned, bool) __attribute__((weak));
    if (!(atomic_load_explicit(&ssKernelControl.slot[slot].affinity, memory_order_acquire) & (UINT64_C(1) << c)) &&
        ss_kernel_test_ineligible_adopt) ss_kernel_test_ineligible_adopt(c, s->ready.queued);
#endif
}
static void ssKernelAdopt(void *context, unsigned slot, void *task,
                          const struct ss_control_transfer *transfer)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    ssKernelAdoptMasked(context, slot, task, transfer);
    portCLEAR_INTERRUPT_MASK(irq);
}
static bool ssKernelDrained(void *context, unsigned cpu, unsigned slot)
{
    (void)context;
    configASSERT(cpu == ssKernelCPUId());
    struct ss_owner *run = &ssKernelCPUs[cpu].run;
    uint64_t bit = UINT64_C(1) << (slot % 64);
    for (unsigned p = 0; p < SS_OWNER_PRIORITIES; ++p)
        if ((run->detached[p][slot / 64] & bit) ||
            (atomic_load_explicit(&run->incoming[p][slot / 64], memory_order_acquire) & bit))
            return false;
    return !ssKernelCPUs[cpu].reference[slot].descriptor;
}
static void ssKernelRetired(void *context, unsigned slot, uint64_t incarnation)
{
    (void)context;
    if (ssKernelCPUs[ssKernelCPUId()].selecting) {
        /* Registry retirement and heap cleanup use shared administration.
         * The owner's next exception-entry checkpoint completes them. */
        ss_control_repair(&ssKernelControl, slot);
        return;
    }
    uint64_t admin;
    if (!ssKernelAdminTryEnter(&admin)) {
        unsigned c = ssKernelCPUId();
        uint64_t bit = UINT64_C(1) << c;
        ssKernelWaitSetPark(&ssKernelAdminWait, c, slot);
        if (atomic_load_explicit(&ssKernelAdminNext, memory_order_acquire) ==
            atomic_load_explicit(&ssKernelAdminServing, memory_order_acquire) &&
            (atomic_fetch_and_explicit(&ssKernelAdminWait.cpus, ~bit, memory_order_acq_rel) & bit))
            ssKernelWaitSetWakeCPU(&ssKernelAdminWait, c);
        return;
    }
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ssKernelTask *s = &ssKernelTasks[slot];
    if (atomic_load_explicit(&ssKernelRegistry.descriptor[slot].incarnation,
                              memory_order_acquire) == incarnation &&
        ssKernelRegistry.registered[slot] && !s->cleanup_queued &&
        ss_kernel_handle_unmap(&ssKernelRegistry, slot, true)) {
        s->cleanup_queued = true;
        atomic_fetch_or_explicit(&ssKernelCleanupPending[slot / 64],
            UINT64_C(1) << (slot % 64), memory_order_release);
    }
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelAdminExit(admin);
}

static void ssKernelCleanupRelease(struct ssKernelTask *caller)
{
    if (!caller) return; /* Boot has no task that another CPU can delete. */
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned previous = atomic_fetch_sub_explicit(&caller->cleanup_shield, 1, memory_order_release);
    configASSERT(previous);
    if (previous == 1 && caller->cleanup_control_deferred) {
        caller->cleanup_control_deferred = false;
        ss_control_repair(&ssKernelControl, caller->ready.slot);
    }
    portCLEAR_INTERRUPT_MASK(irq);
}
/* Idle handles self-deletion; a nonself delete caller can complete cleanup
 * itself. Protect one claimed destructor at a time while allowing allocator
 * blocking and migration, with no administration or API mask held. */
static void ssKernelCleanup(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    configASSERT(!cpu->control_draining && !cpu->selecting && !cpu->preempt &&
                 !ssKernelHolds(cpu));
    for (unsigned w = 0; w < 2; ++w) for (;;) {
        UBaseType_t irq = portSET_INTERRUPT_MASK();
        uint64_t bits = atomic_load_explicit(&ssKernelCleanupPending[w], memory_order_acquire);
        if (!bits) { portCLEAR_INTERRUPT_MASK(irq); break; }
        TCB_t *current = ssKernelSelf();
        struct ssKernelTask *caller = current ? &ssKernelTasks[current->uxOwnerSlot] : NULL;
        if (caller) atomic_fetch_add_explicit(&caller->cleanup_shield, 1, memory_order_relaxed);
        unsigned b = (unsigned)__builtin_ctzll(bits), slot = w * 64 + b;
        uint64_t bit = UINT64_C(1) << b;
        bool claimed = (atomic_fetch_and_explicit(&ssKernelCleanupPending[w], ~bit,
                                                  memory_order_acquire) & bit) != 0;
        portCLEAR_INTERRUPT_MASK(irq);
        if (!claimed) { ssKernelCleanupRelease(caller); continue; }
        struct ssKernelTask *s = &ssKernelTasks[slot];
        TCB_t *task = s->ready.context;
        uint64_t incarnation = atomic_load_explicit(
            &ssKernelRegistry.descriptor[slot].incarnation, memory_order_acquire);
        if (!s->resources_destroyed) {
            prvDeleteTCB(task);
            s->resources_destroyed = true;
        }
        irq = portSET_INTERRUPT_MASK();
        uint64_t admin = ssKernelAdminEnter();
        if (ss_kernel_handle_retire(&ssKernelRegistry, slot, true)) {
            unsigned owner = atomic_load_explicit(&s->owner, memory_order_relaxed);
            configASSERT(atomic_load_explicit(&ssKernelLoad[owner], memory_order_relaxed) && uxCurrentNumberOfTasks);
            atomic_fetch_sub_explicit(&ssKernelLoad[owner], 1, memory_order_relaxed);
            --uxCurrentNumberOfTasks;
            atomic_store_explicit(&ssKernelCleanupCompleted[slot], incarnation, memory_order_release);
        } else {
            /* Closed stale lookups can briefly retain a rejected pin. Retry
             * registry retirement without repeating resource destruction. */
            atomic_fetch_or_explicit(&ssKernelCleanupPending[w], bit, memory_order_release);
        }
        ssKernelAdminExit(admin);
        portCLEAR_INTERRUPT_MASK(irq);
        ssKernelCleanupRelease(caller);
    }
}
static void ssKernelWaitCleanup(unsigned slot, uint64_t incarnation)
{
    while (atomic_load_explicit(&ssKernelCleanupCompleted[slot], memory_order_acquire) < incarnation) {
        ssKernelControlDrain();
        ssKernelCleanup();
        if (atomic_load_explicit(&ssKernelCleanupCompleted[slot], memory_order_acquire) >= incarnation) break;
        ss_owner_wait(ssKernelControl.backend);
    }
}

enum ssKernelCommand { SS_K_PRIORITY, SS_K_AFFINITY, SS_K_SUSPEND, SS_K_RESUME,
                       SS_K_ABORT, SS_K_DELETE, SS_K_PREEMPT, SS_K_MOVE };
struct ssKernelMutation { enum ssKernelCommand command; uint64_t argument; unsigned slot; bool changed; };
static bool ssKernelMutateState(void *opaque, struct ss_control_state *state)
{
    struct ssKernelMutation *m = opaque;
    switch (m->command) {
    case SS_K_PRIORITY:
        if (state->priority == m->argument) return false;
        /* Donation repair owns effective priority while inheritance exceeds
         * the previous base. Lowering a base must not discard that donation. */
        if (state->effective_priority == state->priority || state->effective_priority < m->argument)
            state->effective_priority = (unsigned)m->argument;
        state->priority = (unsigned)m->argument;
        return true;
    case SS_K_AFFINITY:
        if (state->affinity == m->argument) return false;
        state->affinity = m->argument;
        return true;
    case SS_K_SUSPEND:
        if (state->suspended) return false;
        state->suspended = true;
        return true;
    case SS_K_RESUME:
        if (!state->suspended) return false;
        state->suspended = false;
        ++state->resume_generation;
        tracePOST_MOVED_TASK_TO_READY_STATE(ssKernelTasks[m->slot].ready.context);
        return true;
    case SS_K_ABORT:
        if (atomic_load_explicit(&ssKernelTasks[m->slot].execution, memory_order_acquire) != SS_K_BLOCKED)
            return false;
        state->abort_wait_generation = atomic_load_explicit(
            &ssKernelTasks[m->slot].published_wait_generation, memory_order_acquire);
        ++state->abort_generation;
        return true;
    case SS_K_DELETE:
        state->dead = true;
        return true;
    case SS_K_MOVE:
        if (m->argument >= configNUMBER_OF_CORES || !(state->affinity & (UINT64_C(1) << m->argument))) return false;
        state->move_to_plus_one = (unsigned)m->argument + 1;
        return true;
    case SS_K_PREEMPT:
        if (state->preemption_disabled == (m->argument != 0)) return false;
        state->preemption_disabled = m->argument != 0;
        return true;
    }
    return false;
}
static bool ssKernelMutate(void *opaque, struct ss_control_state *state)
{
    struct ssKernelMutation *m = opaque;
    m->changed = ssKernelMutateState(opaque, state);
    return m->changed;
}
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
/* Private linked regressions use actual owner transitions and notification
 * gates, while placing the release at a deterministic scheduling boundary. */
static struct ss_notification_guard ssKernelTestGate;
static struct ss_kernel_pin ssKernelTestGatePin;
static unsigned ssKernelTestPausedSlot = SS_OWNER_SLOTS;
static unsigned char ssKernelTestPause;
void ss_kernel_test_yield_in_object(const void *object)
{
    ssKernelObjectEnterOn(object);
    vTaskYieldWithinAPI();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    configASSERT(irq <= portMAX_API_PRIORITY_MASK && cpu->o_critical == 1 && cpu->preempt == 1);
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelObjectLeave();
}
void ss_kernel_test_pending_slice(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    configASSERT(!cpu->explicit_yield && ssKernelSelf()->xPreemptionDisable);
    cpu->yield_tail = cpu->reschedule = true;
    ssKernelForce(ssKernelCPUId());
    portCLEAR_INTERRUPT_MASK(irq);
}
void ss_kernel_test_object_admission(const void *object, unsigned kind)
{
    if (kind == 0) { ssKernelObjectEnterOn(object); ssKernelObjectLeave(); }
    else if (kind == 1) { ssKernelObjectSuspendOn(object); (void)ssKernelObjectResumeOn(); }
    else if (kind == 2) { ssKernelObjectEnterFor(NULL, 0); ssKernelObjectExit(); }
    else { ssKernelObjectSuspend(); (void)ssKernelObjectResume(); }
}
void ss_kernel_test_freeze_mark(unsigned c)
{
    configASSERT(ssKernelNeedsFrame(&ssKernelCPUs[c]) && !ssKernelCPUs[c].preempt &&
        atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire) == SS_GATE_OPEN);
    atomic_store_explicit(&ssKernelControl.gate_ack, ssKernelControl.active_mask & ~(UINT64_C(1) << c), memory_order_relaxed);
    atomic_store_explicit(&ssKernelControl.gate_phase, SS_GATE_ACQUIRING, memory_order_release);
}
bool ss_kernel_test_current_running(void)
{ return atomic_load_explicit(&ssKernelCurrent()->execution, memory_order_acquire) == SS_K_RUNNING; }
bool ss_kernel_test_delay_aborted(void)
{ return ssKernelSelf()->ucDelayAborted != pdFALSE; }
unsigned ss_kernel_test_suspension(TaskHandle_t task)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    unsigned flags = ss_control_suspension(&ssKernelControl, pin.slot);
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
    return flags;
}
bool ss_kernel_test_frame_pending(TaskHandle_t task)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    configASSERT(atomic_load_explicit(&ssKernelTasks[pin.slot].owner, memory_order_acquire) == ssKernelCPUId());
    struct ss_control_state state;
    ss_control_snapshot(&ssKernelControl, pin.slot, &state);
    bool pending = state.offcpu_sequence > ssKernelTasks[pin.slot].offcpu_ack;
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
    return pending;
}
bool ss_kernel_test_freeze_unacked(unsigned c)
{ return !(atomic_load_explicit(&ssKernelControl.gate_ack, memory_order_acquire) & (UINT64_C(1) << c)); }
unsigned ss_kernel_test_drain_held(TaskHandle_t task)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    unsigned slot = pin.slot, priority = ssKernelTasks[slot].ready.priority;
    configASSERT(atomic_load_explicit(&ssKernelTasks[slot].owner, memory_order_acquire) == ssKernelCPUId() &&
        atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire) == SS_GATE_OPEN);
    atomic_store_explicit(&ssKernelControl.gate_phase, SS_GATE_HELD, memory_order_release);
    struct ssKernelMutation mutation = { SS_K_PRIORITY, priority + 1, slot, false };
    ss_control_publish(&ssKernelControl, slot, ssKernelMutate, &mutation);
    configASSERT(mutation.changed);
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelControlDrain();
    irq = portSET_INTERRUPT_MASK();
    configASSERT(ssKernelTasks[slot].ready.priority == priority);
    atomic_store_explicit(&ssKernelControl.gate_phase, SS_GATE_OPEN, memory_order_release);
    ssKernelForce(ssKernelCPUId());
    ss_owner_event(&ssKernelBackend);
    portCLEAR_INTERRUPT_MASK(irq);
    return priority;
}
void ss_kernel_test_freeze_release(TaskHandle_t outgoing)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    configASSERT(ssKernelSelf() != outgoing &&
        atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire) == SS_GATE_ACQUIRING &&
        atomic_load_explicit(&ssKernelControl.gate_ack, memory_order_acquire) == ssKernelControl.active_mask);
    atomic_store_explicit(&ssKernelControl.gate_phase, SS_GATE_OPEN, memory_order_release);
    ss_owner_event(&ssKernelBackend);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) ssKernelForce(c);
    portCLEAR_INTERRUPT_MASK(irq);
}
void ss_kernel_test_request(TaskHandle_t task, unsigned kind, unsigned argument)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    struct ssKernelMutation mutation = { kind == 0 ? SS_K_ABORT : kind == 1 ? SS_K_SUSPEND : kind == 2 ? SS_K_PRIORITY :
        kind == 3 ? SS_K_AFFINITY : SS_K_DELETE,
        argument, pin.slot, false };
    ss_control_publish(&ssKernelControl, pin.slot, ssKernelMutate, &mutation);
    configASSERT(mutation.changed);
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
}
void ss_kernel_test_resume_then_suspend(TaskHandle_t task, bool final_resume)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    configASSERT(ss_control_resume_isr(&ssKernelControl, pin.slot));
    struct ssKernelMutation mutation = { SS_K_SUSPEND, 0, pin.slot, false };
    ss_control_publish(&ssKernelControl, pin.slot, ssKernelMutate, &mutation);
    configASSERT(mutation.changed);
    if (final_resume) configASSERT(ss_control_resume_isr(&ssKernelControl, pin.slot));
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
}
bool ss_kernel_test_redirect_saved(unsigned slot)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelTask *s = &ssKernelTasks[slot];
    struct ss_control_slot *record = &ssKernelControl.slot[slot];
    if (!s->detach_pending) return false;
    configASSERT(s->off_cpu && record->phase == 1 && record->transfer.old_owner == c);
    ss_kernel_test_request(s->ready.context, 3, 1u << c);
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(s->ready.context, &pin));
    ssKernelPublishReference(pin); /* The closed route broadcasts incoming hints. */
    ss_owner_drain(&ssKernelCPUs[c].run, ssKernelInspect, NULL);
    return true;
}
bool ss_kernel_test_route_returned(TaskHandle_t task)
{
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    uint64_t route = atomic_load_explicit(&ssKernelControl.slot[pin.slot].route_word, memory_order_acquire);
    bool returned = (route >> 8) >= 2 && (route & 0xff) == ssKernelCPUId();
    ss_kernel_handle_put(&pin);
    return returned;
}
void ss_kernel_test_dump_control(TaskHandle_t task)
{
    extern void platform_puts(const char *);
    extern void platform_hex(uint64_t);
    struct ss_kernel_pin pin;
    if (!ssKernelHandlePin(task, &pin)) return;
    unsigned slot = pin.slot;
    platform_puts("task slot="); platform_hex(slot);
    platform_puts(" owner="); platform_hex(atomic_load(&ssKernelTasks[slot].owner));
    platform_puts(" execution="); platform_hex(atomic_load(&ssKernelTasks[slot].execution));
    platform_puts(" suspension="); platform_hex(atomic_load(&ssKernelControl.slot[slot].suspension));
    platform_puts(" applied="); platform_hex(atomic_load(&ssKernelControl.slot[slot].applied));
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        platform_puts(" pending/parked=");
        platform_hex((atomic_load(&ssKernelControl.pending[c].word[slot / 64]) >> (slot % 64)) & 1u);
        platform_hex((atomic_load(&ssKernelControl.parked[c].word[slot / 64]) >> (slot % 64)) & 1u);
    }
    platform_puts("\n");
    ss_kernel_handle_put(&pin);
}
unsigned ss_kernel_test_task_state(TaskHandle_t task)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    configASSERT(atomic_load_explicit(&s->owner, memory_order_acquire) == ssKernelCPUId());
    TCB_t *t = task;
    unsigned result = (atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_BLOCKED ? 1u : 0u) |
        (listLIST_ITEM_CONTAINER(&t->xEventListItem) ? 2u : 0u) |
        ((ssKernelCPUs[ssKernelCPUId()].object_repair[pin.slot / 64] >> (pin.slot % 64)) & 1u ? 4u : 0u) |
        (s->cleanup_control_deferred ? 8u : 0u) |
        ((atomic_load_explicit(&ssKernelControl.parked[ssKernelCPUId()].word[pin.slot / 64], memory_order_acquire) >> (pin.slot % 64)) & 1u ? 16u : 0u) |
        (s->off_cpu && atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_SUSPENDED &&
            (ss_control_suspension(&ssKernelControl, pin.slot) &
             (SS_CONTROL_SUSPENDED | SS_CONTROL_OFFCPU)) == SS_CONTROL_SUSPENDED ? 32u : 0u) |
        (listGET_LIST_ITEM_VALUE(&t->xEventListItem) == configMAX_PRIORITIES - s->ready.priority ? 64u : 0u) |
        (s->abort_generation ? 128u : 0u) |
        (s->ready.queued && atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_READY ? 256u : 0u);
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
    return result;
}
void ss_kernel_test_cleanup_shield(TaskHandle_t task, bool hold)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    configASSERT(ssKernelHandlePin(task, &pin));
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    if (hold) atomic_fetch_add_explicit(&s->cleanup_shield, 1, memory_order_relaxed);
    else ssKernelCleanupRelease(s);
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
}
void ss_kernel_test_gate_hold(TaskHandle_t task)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    configASSERT(!ssKernelTestGatePin.descriptor &&
        ssKernelHandlePin(task, &ssKernelTestGatePin));
    struct ssKernelTask *s = &ssKernelTasks[ssKernelTestGatePin.slot];
    configASSERT(s->wait_kind == SS_K_NOTIFICATION && s->off_cpu);
    ss_notification_gate_begin(&s->notification[s->notify_index], &ssKernelBackend, &ssKernelTestGate);
    configASSERT(ss_notification_payload_state(ssKernelTestGate.payload) == SS_NOTIFY_WAITING);
    portCLEAR_INTERRUPT_MASK(irq);
}
bool ss_kernel_test_gate_complete(void)
{
    if (!ssKernelTestGatePin.descriptor) return false;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned slot = ssKernelTestGatePin.slot, c = ssKernelCPUId();
    uint64_t bit = UINT64_C(1) << (slot % 64);
    if (!(atomic_load_explicit(&ssKernelControl.parked[c].word[slot / 64], memory_order_acquire) & bit)) {
        portCLEAR_INTERRUPT_MASK(irq); return false;
    }
    /* A real registered producer commits a received payload, completion
     * latch and owner hint before releasing this held gate. */
    ssKernelTestGate.payload = (ssKernelTestGate.payload & ~(SS_NOTIFICATION_STATE | SS_NOTIFICATION_VALUE)) |
        ((uint64_t)SS_NOTIFY_RECEIVED << 32) | SS_NOTIFICATION_COMPLETED | 1u;
    ssKernelPublishReference(ssKernelTestGatePin);
    ssKernelTestGatePin = (struct ss_kernel_pin){0};
    ss_notification_gate_end(&ssKernelTestGate);
    /* Delay the released control's delivery until the real task enters its
     * next wait, a permitted ordering between separate release and IRQ CPUs. */
    ssKernelTestPausedSlot = slot;
    ss_control_park_on(&ssKernelControl, c, slot, &ssKernelTestPause);
    portCLEAR_INTERRUPT_MASK(irq);
    return true;
}
void ss_kernel_test_release_after_wait(unsigned slot)
{
    if (ssKernelTestPausedSlot != slot) return;
    ssKernelTestPausedSlot = SS_OWNER_SLOTS;
    ss_control_wake_on(&ssKernelControl, ssKernelCPUId(), slot, &ssKernelTestPause);
}
#endif
/* Idle pull (docs/designs/scheduler-partitioned.md, series D). Each owner publishes the CPUs
 * that its queued migratable tasks may move to. An owner serves the idle ones among them, and
 * an idle CPU that sees a victim with such a task asks for it with one coalesced bit and one
 * SGI. The owner moves the task through the acknowledged transfer; no CPU edits a foreign
 * queue. The scan is bounded to eight entries per priority. */
static uint64_t ssKernelPullTargets(struct ssKernelCPU *cpu, unsigned c)
{
    uint64_t targets = 0;
    uint32_t bitmap = cpu->run.ready_bitmap;
    while (bitmap) {
        const unsigned p = (unsigned)__builtin_ctz(bitmap);
        bitmap &= bitmap - 1;
        unsigned seen = 0;
        for (struct ss_owner_task *t = ss_owner_ready(&cpu->run, p)->head; t && seen < 8; t = t->next, ++seen)
            targets |= (uint64_t)((TCB_t *)t->context)->uxCoreAffinityMask;
    }
    return targets & ((UINT64_C(1) << configNUMBER_OF_CORES) - 1) & ~(UINT64_C(1) << c);
}
static struct ss_owner_task *ssKernelPullCandidate(struct ssKernelCPU *cpu, unsigned target)
{
    /* A task that would preempt the running one runs here at once; pull only waiting ones. */
    const unsigned running = cpu->run.current ? cpu->run.current->priority : 0;
    uint32_t bitmap = cpu->run.ready_bitmap & (running >= 31 ? ~UINT32_C(0) : ((UINT32_C(2) << running) - 1));
    while (bitmap) {
        const unsigned p = 31u - (unsigned)__builtin_clz(bitmap);
        bitmap &= ~(UINT32_C(1) << p);
        unsigned seen = 0;
        for (struct ss_owner_task *t = ss_owner_ready(&cpu->run, p)->head; t && seen < 8; t = t->next, ++seen)
            if (((uint64_t)((TCB_t *)t->context)->uxCoreAffinityMask >> target) & 1u) return t;
    }
    return NULL;
}
/* Moves started by idle pull since the start, and the times a CPU entered idle while an owner
 * held a ready task that CPU could run; diagnostics read them. */
static _Atomic unsigned long ssKernelIdlePulls;
static unsigned long ssKernelIdleWaiting[configNUMBER_OF_CORES];
unsigned long ulSunstoneIdleWaiting(unsigned core) { return core < configNUMBER_OF_CORES ? ssKernelIdleWaiting[core] : 0; }
unsigned long ulSunstoneIdlePulls(void) { return atomic_load_explicit(&ssKernelIdlePulls, memory_order_relaxed); }
unsigned long ss_kernel_idle_pulls(void) { return atomic_load_explicit(&ssKernelIdlePulls, memory_order_relaxed); }
static void ssKernelPullServe(unsigned c)
{
#ifdef SS_KERNEL_TEST_NO_IDLE_PULL
    (void)c; /* linked fixtures of ownership transitions check placement-independent behavior */
    return;
#endif
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    /* Only a core running a task can leave work waiting; a core entering idle selects at once. */
    /* A running task that is suspending, blocking or deleted leaves this core to the waiting one. */
    const struct ss_owner_task *current = cpu->run.current;
    const bool keeps_core = current && !ssKernelTasks[current->slot].suspended && !ssKernelTasks[current->slot].dead &&
        atomic_load_explicit(&ssKernelTasks[current->slot].execution, memory_order_acquire) == SS_K_RUNNING;
    const uint64_t targets = cpu->run.ready_bitmap && keeps_core ? ssKernelPullTargets(cpu, c) : 0;
    if (atomic_load_explicit(&cpu->pull_targets, memory_order_relaxed) != targets)
        atomic_store_explicit(&cpu->pull_targets, targets, memory_order_release);
    uint64_t requests = 0;
    if (atomic_load_explicit(&cpu->pull_requests, memory_order_relaxed))
        requests = atomic_exchange_explicit(&cpu->pull_requests, 0, memory_order_acq_rel);
    uint64_t serve = (requests | atomic_load_explicit(&ssKernelIdleMask, memory_order_acquire)) & targets;
#if SS_PULL_MOVES_PER_PASS
    unsigned moved = 0;
#endif
    while (serve) {
        const unsigned target = (unsigned)__builtin_ctzll(serve);
        serve &= serve - 1;
        struct ss_owner_task *candidate = ssKernelPullCandidate(cpu, target);
        if (!candidate) continue;
        struct ss_kernel_pin pin;
        if (!ssKernelHandlePin(candidate->context, &pin)) continue;
        struct ssKernelMutation move = { .command = SS_K_MOVE, .argument = target, .slot = pin.slot };
        (void)ss_control_publish(&ssKernelControl, pin.slot, ssKernelMutate, &move);
        ss_kernel_handle_put(&pin);
        /* Publication requests the next maintenance entry. The queued task
         * remains owned here until that entry starts its acknowledged move. */
        if (move.changed) {
            atomic_fetch_add_explicit(&ssKernelIdlePulls, 1, memory_order_relaxed);
#if SS_PULL_MOVES_PER_PASS
            /* Bound the transfer work of one pass; an idle core stays in the idle mask and the next pass serves it. */
            if (++moved >= SS_PULL_MOVES_PER_PASS) break;
#endif
        }
        if (cpu->run.ready_bitmap == 0) break;
    }
}
static void ssKernelPullIdle(unsigned c, bool idle)
{
#ifdef SS_KERNEL_TEST_NO_IDLE_PULL
    (void)c; (void)idle;
    return;
#endif
    const uint64_t bit = UINT64_C(1) << c;
    const bool marked = (atomic_load_explicit(&ssKernelIdleMask, memory_order_relaxed) & bit) != 0;
    if (!idle) {
        if (marked) atomic_fetch_and_explicit(&ssKernelIdleMask, ~bit, memory_order_acq_rel);
        return;
    }
    if (!marked) atomic_fetch_or_explicit(&ssKernelIdleMask, bit, memory_order_acq_rel);
    if (ssKernelCPUs[c].direct_suspend) {
        ssKernelCPUs[c].idle_pull_deferred = true;
        return;
    }
    for (unsigned v = 0; v < configNUMBER_OF_CORES; ++v) {
        if (v == c || !(atomic_load_explicit(&ssKernelCPUs[v].pull_targets, memory_order_acquire) & bit)) continue;
        ++ssKernelIdleWaiting[c];
        atomic_fetch_or_explicit(&ssKernelCPUs[v].pull_requests, bit, memory_order_acq_rel);
        ssKernelForce(v);
    }
}
/* The executing task already owns its TCB lifetime until its frame is saved.
 * Borrow that reference before raising the API mask. Logical publication uses
 * one atomic state/fence word and cannot block behind an interrupted FIFO. */
static bool ssKernelSuspendCurrent(TCB_t *current)
{
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    struct ss_sched_trace_command trace = { .command = SS_K_SUSPEND,
        .flags = 1u | 4u | 16u | 64u, .stamp = {ss_sched_trace_now()} };
#endif
    unsigned slot = current->uxOwnerSlot;
    struct ssKernelTask *s = &ssKernelTasks[slot];
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    trace.slot = slot;
    trace.stamp[1] = ss_sched_trace_now();
#endif
    bool changed = ss_control_suspend_current(&ssKernelControl, slot, ssKernelCPUId());
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    trace.stamp[2] = ss_sched_trace_now();
#endif
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
    extern void ss_kernel_test_self_suspend_published(void) __attribute__((weak));
    if (ss_kernel_test_self_suspend_published) ss_kernel_test_self_suspend_published();
#endif
    for (;;) {
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
        uint64_t mask_begin = ss_sched_trace_now();
#endif
        UBaseType_t irq = portSET_INTERRUPT_MASK();
        unsigned c = ssKernelCPUId();
        struct ssKernelCPU *cpu = &ssKernelCPUs[c];
        configASSERT(cpu->run.current == &s->ready && !s->off_cpu);
        if (cpu->preempt || ssKernelHolds(cpu) || irq <= portMAX_API_PRIORITY_MASK) {
            unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
            configASSERT(gate != SS_GATE_HELD && (gate != SS_GATE_ACQUIRING ||
                !(atomic_load_explicit(&ssKernelControl.gate_ack, memory_order_acquire) & (UINT64_C(1) << c))));
            unsigned flags = ss_control_suspension(&ssKernelControl, slot);
            ssKernelCancel(s);
            s->suspended = (flags & SS_CONTROL_SUSPENDED) &&
                (!atomic_load_explicit(&s->cleanup_shield, memory_order_acquire) || (flags & SS_CONTROL_SELF));
            atomic_store_explicit(&s->execution, SS_K_SUSPENDED, memory_order_release);
            cpu->direct_suspend = cpu->reschedule = true;
            ssKernelForce(c);
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
            trace.flags |= 8u;
            trace.stamp[3] = mask_begin;
            trace.stamp[4] = trace.stamp[5] = trace.stamp[6] = ss_sched_trace_now();
            ss_sched_trace_command(c, &trace);
#endif
            portCLEAR_INTERRUPT_MASK(irq);
            return changed;
        }
        unsigned phase = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
        if (phase == SS_GATE_ACQUIRING || phase == SS_GATE_HELD) {
            (void)ss_control_checkpoint(&ssKernelControl, c);
            portCLEAR_INTERRUPT_MASK(irq);
            ss_owner_wait(&ssKernelBackend);
            continue;
        }
        unsigned suspension = ss_control_suspension(&ssKernelControl, slot);
        if (!(suspension & (SS_CONTROL_SELF | SS_CONTROL_SELF_FRAME))) {
            /* An interrupt may have fulfilled this suspend and its later
             * resume before the caller reaches its local transition. */
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
            trace.stamp[3] = mask_begin;
            trace.stamp[4] = trace.stamp[5] = trace.stamp[6] = ss_sched_trace_now();
            ss_sched_trace_command(c, &trace);
#endif
            portCLEAR_INTERRUPT_MASK(irq);
            return changed;
        }
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
        trace.stamp[3] = mask_begin;
        ss_sched_trace_direct_suspend_begin(c, &trace);
#endif
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
        extern void ss_kernel_test_before_self_suspend(uint64_t *, uint64_t) __attribute__((weak));
        if (ss_kernel_test_before_self_suspend)
            ss_kernel_test_before_self_suspend(&cpu->object_repair[slot / 64], UINT64_C(1) << (slot % 64));
#endif
        ssKernelCancel(s);
        s->suspended = (suspension & SS_CONTROL_SUSPENDED) &&
            (!atomic_load_explicit(&s->cleanup_shield, memory_order_acquire) || (suspension & SS_CONTROL_SELF));
        atomic_store_explicit(&s->execution, SS_K_SUSPENDED, memory_order_release);
        cpu->direct_suspend = cpu->reschedule = true;
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
        ss_sched_trace_direct_suspend_mark(c);
#endif
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
        extern void ss_kernel_test_self_suspend(unsigned, bool, bool, bool, bool) __attribute__((weak));
        if (ss_kernel_test_self_suspend)
            ss_kernel_test_self_suspend(0,
                (cpu->object_repair[slot / 64] & (UINT64_C(1) << (slot % 64))) != 0,
                true, s->off_cpu, !(ss_control_suspension(&ssKernelControl, slot) & SS_CONTROL_OFFCPU));
        extern void ss_kernel_test_direct_suspend_mark(unsigned) __attribute__((weak));
        if (ss_kernel_test_direct_suspend_mark) ss_kernel_test_direct_suspend_mark(c);
#endif
        /* The saved-frame selector restores the replacement task's mask. Do
         * not open this span for a general owner pass before that selection. */
        portYIELD();
        configASSERT(atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_RUNNING);
        return changed;
    }
}
static bool ssKernelCommand(TaskHandle_t handle, enum ssKernelCommand command,
                             uint64_t argument, bool from_isr)
{
    TCB_t *current = ssKernelSelf();
    if (!handle) handle = current;
    if (xSchedulerRunning && handle == current && command == SS_K_SUSPEND && !from_isr)
        return ssKernelSuspendCurrent(current);
    UBaseType_t irq = from_isr ? portSET_INTERRUPT_MASK_FROM_ISR() : ssKernelTaskMask();
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    struct ss_sched_trace_command trace_command = { .command = command, .slot = UINT64_MAX,
        .flags = (handle == current ? 1u : 0u) | (from_isr ? 2u : 0u) | (xSchedulerRunning ? 4u : 0u),
        .stamp = {ss_sched_trace_now()} };
#define SS_COMMAND_STAMP(n) (trace_command.stamp[n] = ss_sched_trace_now())
#define SS_COMMAND_FINISH() do { \
    for (unsigned ss_i = 1; ss_i < 6; ++ss_i) \
        if (!trace_command.stamp[ss_i]) trace_command.stamp[ss_i] = trace_command.stamp[ss_i - 1]; \
    trace_command.stamp[6] = ss_sched_trace_now(); \
    ss_sched_trace_command(ssKernelCPUId(), &trace_command); \
} while (0)
#else
#define SS_COMMAND_STAMP(n) ((void)0)
#define SS_COMMAND_FINISH() ((void)0)
#endif
    struct ss_kernel_pin pin;
    if (!ssKernelHandlePin(handle, &pin)) {
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
        trace_command.flags |= 32u;
#endif
        SS_COMMAND_STAMP(1);
        SS_COMMAND_FINISH();
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        return false;
    }
    SS_COMMAND_STAMP(1);
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    trace_command.slot = pin.slot;
#endif
    bool resume_cleanup_self = command == SS_K_RESUME &&
        atomic_load_explicit(&ssKernelTasks[pin.slot].cleanup_shield, memory_order_acquire) &&
        (ss_control_suspension(&ssKernelControl, pin.slot) & SS_CONTROL_SELF);
    if (command == SS_K_RESUME && (from_isr || resume_cleanup_self)) {
        bool resumed = ss_control_resume_isr(&ssKernelControl, pin.slot);
        struct ssKernelTask *s = &ssKernelTasks[pin.slot];
        unsigned c = ssKernelCPUId();
        struct ssKernelCPU *cpu = &ssKernelCPUs[c];
        if (resumed && atomic_load_explicit(&s->owner, memory_order_acquire) == c &&
            atomic_load_explicit(&s->effective, memory_order_acquire) > cpu->run.current->priority)
            cpu->reschedule = true;
        SS_COMMAND_STAMP(2);
        ss_kernel_handle_put(&pin);
        SS_COMMAND_FINISH();
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        return resumed;
    }
    struct ssKernelMutation mutation = { .command = command, .argument = argument, .slot = pin.slot };
    uint64_t sequence = ss_control_publish(&ssKernelControl, pin.slot, ssKernelMutate, &mutation);
    SS_COMMAND_STAMP(2);
    bool changed = mutation.changed;
    if (!xSchedulerRunning) {
        /* Boot has no running owner on another CPU and no task stack to save.
         * The boot CPU initializes every owner's queue until startup. Remove
         * changed tasks until owner maintenance applies their controls and
         * makes them ready; a bounded drain can leave other controls pending. */
        struct ssKernelTask *task = &ssKernelTasks[pin.slot];
        unsigned owner = atomic_load_explicit(&task->owner, memory_order_relaxed);
        configASSERT(task->off_cpu);
        if (changed && task->ready.queued)
            configASSERT(ss_owner_remove(&ssKernelCPUs[owner].run, &task->ready));
        ss_kernel_handle_put(&pin);
        SS_COMMAND_FINISH();
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        return changed;
    }
    bool self = handle == current;
    unsigned slot = pin.slot;
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (ssKernelHolds(cpu) || cpu->preempt) {
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
        trace_command.flags |= 8u;
#endif
        /* Logical desired state is committed now. Owner consumption and task
         * acknowledgments wait until the caller releases compatibility I. */
        if (cpu->command_reference[slot].descriptor) ss_kernel_handle_put(&pin);
        else cpu->command_reference[slot] = pin;
        cpu->command_pending[slot / 64] |= UINT64_C(1) << (slot % 64);
        cpu->command_sequence[slot] = sequence;
        cpu->command_no_wait[slot] = from_isr || self;
        cpu->command_delete[slot] |= command == SS_K_DELETE;
        if (self && changed) cpu->reschedule = true;
        SS_COMMAND_FINISH();
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        return changed;
    }
    if (command == SS_K_DELETE && self) {
        struct ssKernelTask *s = &ssKernelTasks[slot];
        unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);
        /* A destructor must return to release its own cleanup shield. */
        configASSERT(!atomic_load_explicit(&s->cleanup_shield, memory_order_acquire));
        configASSERT(gate != SS_GATE_HELD &&
            (gate != SS_GATE_ACQUIRING || !(atomic_load_explicit(&ssKernelControl.gate_ack, memory_order_acquire) &
                                           (UINT64_C(1) << ssKernelCPUId()))));
        ssKernelCancel(s);
        atomic_store_explicit(&s->execution, SS_K_DEAD, memory_order_release);
        cpu->reschedule = true;
#ifdef SS_KERNEL_TEST_ADMIN_ENTER
        extern void ss_kernel_test_self_delete_mark(unsigned) __attribute__((weak));
        if (ss_kernel_test_self_delete_mark) ss_kernel_test_self_delete_mark(ssKernelCPUId());
#endif
        ss_kernel_handle_put(&pin);
        SS_COMMAND_FINISH();
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        portYIELD();
        configASSERT(false); /* Deleted tasks cannot regain execution. */
        return changed;
    }
    if (atomic_load_explicit(&ssKernelTasks[slot].owner, memory_order_acquire) == ssKernelCPUId()) {
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
        trace_command.flags |= 16u;
#endif
        SS_COMMAND_STAMP(3);
        /* Only an unmasked maintenance point consumes general controls. */
        if (!xSchedulerRunning) ssKernelControlDrain();
        SS_COMMAND_STAMP(4);
        ssKernelSliceRefresh(ssKernelCPUId(), false);
        SS_COMMAND_STAMP(5);
    }
    if (!from_isr && changed && self && command == SS_K_SUSPEND) {
        /* No reference is kept on a stack that can remain suspended forever.
         * Returning here requires that this same task was resumed alive. */
        ss_kernel_handle_put(&pin);
        SS_COMMAND_FINISH();
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        portYIELD();
        return changed;
    }
    uint64_t incarnation = pin.incarnation;
    /* The caller's stack can disappear at the first unmask. Retain only a
     * permanent slot/incarnation cookie across acknowledgment waits; cleanup
     * completion also satisfies a command whose target has since retired. */
    ss_kernel_handle_put(&pin);
    SS_COMMAND_FINISH();
    if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
    if (!from_isr && changed) {
        if (self && command == SS_K_AFFINITY) portYIELD();
        /* Permanent cookies remain valid after target retirement and reuse. */
        ssKernelWait(slot, sequence, incarnation);
    }
    if (!from_isr && changed && command == SS_K_DELETE)
        ssKernelWaitCleanup(slot, incarnation);
    if (!from_isr && ssKernelCPUs[ssKernelCPUId()].reschedule && ssKernelCanPreempt()) portYIELD();
    return changed;
}
#undef SS_COMMAND_STAMP
#undef SS_COMMAND_FINISH
static void ssKernelCommandsDetach(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (cpu->control_draining || cpu->preempt || ssKernelHolds(cpu)) return;
    TCB_t *caller = ssKernelSelf();
    if (!caller) return;
    struct ssKernelTaskCommands *batch = &ssKernelTaskCommands[caller->uxOwnerSlot];
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t bits = cpu->command_pending[w];
        while (bits) {
            unsigned b = (unsigned)__builtin_ctzll(bits), slot = w * 64 + b;
            uint64_t bit = UINT64_C(1) << b;
            bits &= bits - 1;
            struct ss_kernel_pin pin = cpu->command_reference[slot];
            if (!cpu->command_no_wait[slot]) {
                batch->pending[w] |= bit;
                batch->sequence[slot] = cpu->command_sequence[slot];
                batch->incarnation[slot] = pin.incarnation;
                if (cpu->command_delete[slot]) batch->deletion[w] |= bit;
                else batch->deletion[w] &= ~bit;
            }
            cpu->command_pending[w] &= ~bit;
            cpu->command_reference[slot] = (struct ss_kernel_pin){0};
            cpu->command_delete[slot] = false;
            ss_kernel_handle_put(&pin);
        }
    }
}
static void ssKernelCommandsFinish(bool can_wait)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (cpu->control_draining || cpu->preempt || ssKernelHolds(cpu)) {
        portCLEAR_INTERRUPT_MASK(irq);
        return;
    }
    if (!can_wait) {
        /* IRQ flush may release ISR/self records, but cannot consume task
         * obligations or wait for runtime resource destruction. */
        /* Most flushes have no pending obligation; visit set bits only. */
        for (unsigned w = 0; w < 2; ++w) {
            uint64_t bits = cpu->command_pending[w];
            while (bits) {
                unsigned b = (unsigned)__builtin_ctzll(bits), slot = w * 64 + b;
                uint64_t bit = UINT64_C(1) << b;
                bits &= bits - 1;
                if (!cpu->command_no_wait[slot]) continue;
                struct ss_kernel_pin pin = cpu->command_reference[slot];
                cpu->command_pending[w] &= ~bit;
                cpu->command_reference[slot] = (struct ss_kernel_pin){0};
                cpu->command_delete[slot] = false;
                ss_kernel_handle_put(&pin);
            }
        }
        portCLEAR_INTERRUPT_MASK(irq);
        return;
    }
    ssKernelCommandsDetach();
    TCB_t *caller = ssKernelSelf();
    struct ssKernelTaskCommands *batch = caller ? &ssKernelTaskCommands[caller->uxOwnerSlot] : NULL;
    portCLEAR_INTERRUPT_MASK(irq);
    if (!batch) return;
    for (unsigned w = 0; w < 2; ++w) while (batch->pending[w]) {
        unsigned b = (unsigned)__builtin_ctzll(batch->pending[w]), slot = w * 64 + b;
        uint64_t bit = UINT64_C(1) << b;
        batch->pending[w] &= ~bit;
        uint64_t sequence = batch->sequence[slot], incarnation = batch->incarnation[slot];
        bool deletion = (batch->deletion[w] & bit) != 0;
        batch->deletion[w] &= ~bit;
        ssKernelWait(slot, sequence, incarnation);
        if (deletion) ssKernelWaitCleanup(slot, incarnation);
    }
}
static unsigned ssKernelPriority(TaskHandle_t handle, bool base)
{
    if (!handle) handle = ssKernelSelf();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    if (!ssKernelHandlePin(handle, &pin)) { portCLEAR_INTERRUPT_MASK(irq); return 0; }
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    unsigned priority = atomic_load_explicit(base ? &s->base : &s->effective, memory_order_acquire);
    if (!xSchedulerRunning) {
        struct ss_control_state desired;
        ss_control_snapshot(&ssKernelControl, pin.slot, &desired);
        priority = base ? desired.priority : desired.effective_priority;
    }
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
    return priority;
}
static bool ssKernelPin(TaskHandle_t handle, struct ss_kernel_pin *pin)
{
    if (!handle) handle = ssKernelSelf();
    return ssKernelHandlePin(handle, pin);
}
static unsigned ssKernelLogicalExecution(struct ssKernelTask *s)
{
    unsigned state = atomic_load_explicit(&s->execution, memory_order_acquire);
    return state == SS_K_TRANSFERRING ? atomic_load_explicit(&s->transfer_execution, memory_order_acquire) : state;
}
static eTaskState ssKernelState(TaskHandle_t handle)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    if (!ssKernelHandlePin(handle, &pin)) { portCLEAR_INTERRUPT_MASK(irq); return eDeleted; }
    unsigned state = ssKernelLogicalExecution(&ssKernelTasks[pin.slot]);
    /* A suspend, resume or delete the owner has not applied yet is already the task's state,
     * as in upstream FreeRTOS, where the call completes the change. */
    struct ss_control_state desired;
    ss_control_snapshot(&ssKernelControl, pin.slot, &desired);
    if (desired.dead) state = SS_K_DEAD;
    else if (desired.suspended) state = SS_K_SUSPENDED;
    else if (state == SS_K_SUSPENDED) state = SS_K_READY;
    static const eTaskState states[] = { eReady, eRunning, eBlocked, eSuspended, eDeleted };
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
    return states[state];
}

static void ssKernelSuspendAll(void)
{
    ssKernelInitialize();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    ++ssKernelCPUs[c].preempt;
    uintptr_t holder = (uintptr_t)pxCurrentTCBs[c];
    uint64_t incarnation = xSchedulerRunning ? atomic_load_explicit(
        &ssKernelRegistry.descriptor[ssKernelCurrent()->ready.slot].incarnation, memory_order_acquire) : 0;
    portCLEAR_INTERRUPT_MASK(irq);
    ss_control_suspend_all(&ssKernelControl, c, holder, incarnation, xSchedulerRunning != pdFALSE);
}
static BaseType_t ssKernelResumeAll(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    uintptr_t holder = (uintptr_t)pxCurrentTCBs[c];
    uint64_t incarnation = xSchedulerRunning ? atomic_load_explicit(
        &ssKernelRegistry.descriptor[ssKernelCurrent()->ready.slot].incarnation, memory_order_acquire) : 0;
    bool yield = ss_control_resume_all(&ssKernelControl, c, holder, incarnation,
                                      xSchedulerRunning != pdFALSE);
    configASSERT(ssKernelCPUs[c].preempt);
    --ssKernelCPUs[c].preempt;
    yield |= ssKernelCPUs[c].reschedule;
    yield &= xSchedulerRunning && ssKernelCPUs[c].preempt == 0;
    ssKernelCommandsDetach();
    portCLEAR_INTERRUPT_MASK(irq);
    ssKernelCommandsFinish(true);
    if (yield && ssKernelCanPreempt()) portYIELD();
    return yield ? pdTRUE : pdFALSE;
}
static uint32_t ssKernelNotifyClear(TaskHandle_t handle, UBaseType_t index,
                                    uint32_t bits, bool clear_state)
{
    configASSERT(index < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    if (!handle) handle = ssKernelSelf();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    if (!ssKernelHandlePin(handle, &pin)) {
        portCLEAR_INTERRUPT_MASK(irq);
        return 0;
    }
    struct ss_notification_result result = ss_notification_apply(
        &ssKernelTasks[pin.slot].notification[index],
        clear_state ? SS_NOTIFY_CLEAR_STATE : SS_NOTIFY_CLEAR_BITS, bits, &ssKernelBackend);
    ss_kernel_handle_put(&pin);
    portCLEAR_INTERRUPT_MASK(irq);
    return clear_state ? result.previous_state == SS_NOTIFY_RECEIVED : result.previous;
}

/* Called with compatibility object exclusion. The donor commits metadata and
 * retains lifetime before publishing a control hint after the outer unlock. */
static void ssKernelDonationDefer(struct ss_kernel_pin pin)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    cpu->repair[pin.slot / 64] |= UINT64_C(1) << (pin.slot % 64);
    ssKernelDefer(pin);
}
static BaseType_t ssKernelInherit(TaskHandle_t holder)
{
    if (!holder) return pdFALSE;
    UBaseType_t irq = ssDonationEnter();
    struct ss_kernel_pin pin;
    if (!ssKernelPin(holder, &pin)) {
        ssDonationLeave(irq);
        return pdFALSE;
    }
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    unsigned priority = atomic_load_explicit(&ssKernelCurrent()->effective, memory_order_acquire);
    unsigned base = atomic_load_explicit(&s->base, memory_order_acquire);
    unsigned donation = atomic_load_explicit(&s->donation, memory_order_relaxed);
    if (priority > donation && priority > base) {
        atomic_store_explicit(&s->donation, priority, memory_order_release);
        ssKernelDonationDefer(pin);
    } else ss_kernel_handle_put(&pin);
    ssDonationLeave(irq);
    return base < priority ? pdTRUE : pdFALSE;
}
static BaseType_t ssKernelDisinherit(TaskHandle_t holder, bool timeout, unsigned waiting)
{
    if (!holder) return pdFALSE;
    UBaseType_t irq = ssDonationEnter();
    struct ss_kernel_pin pin;
    if (!ssKernelPin(holder, &pin)) {
        ssDonationLeave(irq);
        return pdFALSE;
    }
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    unsigned count = atomic_load_explicit(&s->mutexes, memory_order_relaxed);
    configASSERT(count);
    if (!timeout) {
        configASSERT(holder == ssKernelSelf());
        atomic_store_explicit(&s->mutexes, --count, memory_order_release);
    }
    bool change = (!timeout && !count) || (timeout && count == 1);
    unsigned donation = atomic_load_explicit(&s->donation, memory_order_relaxed);
    unsigned target = timeout ? waiting : 0;
    bool lowered = change && donation > target &&
        donation > atomic_load_explicit(&s->base, memory_order_acquire);
    if (change && donation != target) {
        atomic_store_explicit(&s->donation, target, memory_order_release);
        ssKernelDonationDefer(pin);
    } else ss_kernel_handle_put(&pin);
    ssDonationLeave(irq);
    return lowered ? pdTRUE : pdFALSE;
}
static TaskHandle_t ssKernelMutexTaken(void)
{
    struct ssKernelTask *s = ssKernelCurrent();
    unsigned count = atomic_fetch_add_explicit(&s->mutexes, 1, memory_order_relaxed);
    configASSERT(count != UINT32_MAX);
    return s->ready.context;
}

#if ( configUSE_TRACE_FACILITY == 1 )
#if ( configGENERATE_RUN_TIME_STATS == 1 )
static configRUN_TIME_COUNTER_TYPE ssKernelRuntime(TaskHandle_t);
#endif
static void ssKernelInfo(TaskHandle_t handle, TaskStatus_t *status, bool stack)
{
    configASSERT(status);
    ssKernelSuspendAll();
    struct ss_kernel_pin pin;
    if (!ssKernelPin(handle, &pin)) {
        *status = (TaskStatus_t){ .eCurrentState = eDeleted };
        (void)ssKernelResumeAll();
        return;
    }
    TCB_t *t = pin.descriptor->task;
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    UBaseType_t irq = ssKernelObjectEnterFromISROn(t);
    *status = (TaskStatus_t){0};
    status->xHandle = t;
    status->pcTaskName = t->pcTaskName;
    status->uxCurrentPriority = atomic_load_explicit(&s->effective, memory_order_acquire);
    status->pxStackBase = t->pxStack;
#if ( ( portSTACK_GROWTH > 0 ) || ( configRECORD_STACK_HIGH_ADDRESS == 1 ) )
    status->pxTopOfStack = (StackType_t *)t->pxTopOfStack;
    status->pxEndOfStack = t->pxEndOfStack;
#endif
    status->xTaskNumber = t->uxTCBNumber;
    status->uxCoreAffinityMask = t->uxCoreAffinityMask;
#if ( configUSE_MUTEXES == 1 )
    status->uxBasePriority = atomic_load_explicit(&s->base, memory_order_acquire);
#endif
#if ( configGENERATE_RUN_TIME_STATS == 1 )
    status->ulRunTimeCounter = ssKernelRuntime(t);
#endif
    static const eTaskState states[] = { eReady, eRunning, eBlocked, eSuspended, eDeleted };
    status->eCurrentState = states[ssKernelLogicalExecution(s)];
    ssKernelObjectLeaveFromISR(irq);
    if (stack) {
#if ( portSTACK_GROWTH > 0 )
        status->usStackHighWaterMark = prvTaskCheckFreeStackSpace((uint8_t *)t->pxEndOfStack);
#else
        status->usStackHighWaterMark = prvTaskCheckFreeStackSpace((uint8_t *)t->pxStack);
#endif
    }
    ss_kernel_handle_put(&pin);
    (void)ssKernelResumeAll();
}
static UBaseType_t ssKernelForEach(TaskStatusCallbackFunction_t callback, void *context)
{
    UBaseType_t count = 0;
    /* Caller holds the global compatibility gate. Registry buckets are
     * permanent; each callback also has a task lifetime pin. */
    for (unsigned b = 0; b < SS_KERNEL_HANDLE_BUCKETS; ++b) {
        uintptr_t handle = atomic_load_explicit(&ssKernelRegistry.bucket[b].handle, memory_order_acquire);
        if (!handle || handle == UINTPTR_MAX) continue;
        struct ss_kernel_pin pin;
        if (!ssKernelPin((TaskHandle_t)handle, &pin)) continue;
        unsigned execution = ssKernelLogicalExecution(&ssKernelTasks[pin.slot]);
        static const eTaskState states[] = { eReady, eRunning, eBlocked, eSuspended, eDeleted };
        callback((TaskHandle_t)handle, states[execution], context);
        ++count;
        ss_kernel_handle_put(&pin);
    }
    return count;
}
#endif

static bool ssKernelIdleShouldYield(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    bool ready = ss_owner_ready(&ssKernelCPUs[ssKernelCPUId()].run, tskIDLE_PRIORITY)->head != NULL;
    portCLEAR_INTERRUPT_MASK(irq);
    return ready;
}

#if ( configGENERATE_RUN_TIME_STATS == 1 )
static configRUN_TIME_COUNTER_TYPE ssKernelRuntimeNow(void)
{
    configRUN_TIME_COUNTER_TYPE now;
#ifdef portALT_GET_RUN_TIME_COUNTER_VALUE
    portALT_GET_RUN_TIME_COUNTER_VALUE(now);
#else
    now = portGET_RUN_TIME_COUNTER_VALUE();
#endif
    return now;
}
static configRUN_TIME_COUNTER_TYPE ssKernelRuntime(TaskHandle_t handle)
{
    ssKernelSuspendAll();
    struct ss_kernel_pin pin;
    configRUN_TIME_COUNTER_TYPE result = 0;
    if (ssKernelPin(handle, &pin)) {
        struct ssKernelTask *s = &ssKernelTasks[pin.slot];
        TCB_t *t = pin.descriptor->task;
        result = t->ulRunTimeCounter;
        if (ssKernelLogicalExecution(s) == SS_K_RUNNING) {
            unsigned owner = atomic_load_explicit(&s->owner, memory_order_acquire);
            result += ssKernelRuntimeNow() - ulTaskSwitchedInTime[owner];
        }
        ss_kernel_handle_put(&pin);
    }
    (void)ssKernelResumeAll();
    return result;
}
#endif
