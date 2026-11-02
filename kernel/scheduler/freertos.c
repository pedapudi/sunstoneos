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

enum ssKernelExecution { SS_K_READY, SS_K_RUNNING, SS_K_BLOCKED, SS_K_SUSPENDED, SS_K_DEAD, SS_K_TRANSFERRING };
enum ssKernelWait { SS_K_NONE, SS_K_DELAY, SS_K_EVENT, SS_K_NOTIFICATION };
enum ssKernelOutcome { SS_K_WAITING, SS_K_EVENT_READY, SS_K_TIMED_OUT, SS_K_ABORTED };
struct ssKernelTask {
    _Alignas(SS_CACHE_LINE) struct ss_owner_task ready;
    struct ss_deadline_entry deadline;
    struct ss_notification notification[configTASK_NOTIFICATION_ARRAY_ENTRIES];
    struct ss_notification_wait notify_wait;
    struct ss_kernel_pin event_reference;
    _Atomic unsigned owner, execution, outcome, effective, base;
    _Atomic unsigned transfer_execution;
    /* Compatibility object exclusion serializes donation writers. Owners read
     * committed donation without acquiring an object lock before their FIFO. */
    _Atomic unsigned donation, mutexes;
    uint64_t generation;
    unsigned wait_kind, notify_index;
    bool indefinite, unordered, off_cpu;
    bool suspended, dead, cleanup_queued, resources_destroyed, cleanup_control_deferred;
    _Atomic unsigned cleanup_shield;
    uint64_t resume_generation, abort_generation, offcpu_ack;
    uint64_t object_sequence, object_requested;
    unsigned object_priority, object_requested_priority;
    bool object_detach;
};
struct ssKernelCPU {
    struct ss_owner run;
    struct ss_deadline_heap deadlines;
    unsigned preempt, object_depth, critical_depth, suspend_depth, isr_depth;
    UBaseType_t object_mask;
    bool yield_tail, reschedule, irq_drained, command_finishing, control_draining, selecting;
    bool command_no_wait[SS_OWNER_SLOTS], command_delete[SS_OWNER_SLOTS];
    uint64_t publication[2];
    uint64_t repair[2], object_repair[2];
    uint64_t command_pending[2], command_sequence[SS_OWNER_SLOTS];
    struct ss_kernel_pin command_reference[SS_OWNER_SLOTS];
    struct ss_kernel_pin reference[SS_OWNER_SLOTS];
    /* CPU0's tick samples this deadline of every owner; idle peers read the pull set. */
    _Alignas(SS_CACHE_LINE) _Atomic uint64_t slice_deadline;
    /* Idle pull. pull_targets: CPUs that a ready migratable task here may move to.
     * pull_requests: idle CPUs that asked this owner for work. */
    _Atomic uint64_t pull_targets, pull_requests;
};
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
static struct ssKernelTaskCommands ssKernelTaskCommands[SS_OWNER_SLOTS];
static struct ssKernelTask ssKernelTasks[SS_OWNER_SLOTS];
static struct ssKernelCPU ssKernelCPUs[configNUMBER_OF_CORES];
static struct ss_kernel_registry ssKernelRegistry;
static struct ss_control ssKernelControl;
static _Atomic uint64_t ssKernelTime;
static _Atomic uint64_t ssKernelAdminNext, ssKernelAdminServing;
static _Atomic uint64_t ssKernelCleanupPending[2];
static _Atomic uint64_t ssKernelCleanupCompleted[SS_OWNER_SLOTS];
static bool ssKernelInitialized;
/* Registered live task count. Registration is administration-serialized;
 * migration updates estimates atomically without reversing the control order.
 * The series-B placement input includes running and blocked home tasks. */
static _Atomic unsigned ssKernelLoad[configNUMBER_OF_CORES];
static unsigned ssKernelIdleNext;

static void ssKernelSGI(void *context, unsigned cpu)
{
    (void)context;
    if (xSchedulerRunning) portYIELD_CORE((BaseType_t)cpu);
}
static const struct ss_owner_backend ssKernelBackend = { .sgi = ssKernelSGI };
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
static BaseType_t ssKernelSchedulerState(void)
{
    if (!xSchedulerRunning) return taskSCHEDULER_NOT_STARTED;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
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
         * Only a task that can continue running suppresses automatic yields. */
        if (state == SS_K_RUNNING || state == SS_K_READY) return false;
    }
#endif
    return true;
}
static void ssKernelForce(unsigned cpu)
{
    /* Controls and retirement are urgent regardless of running priority. */
    ss_owner_barrier(&ssKernelBackend);
    ssKernelSGI(NULL, cpu);
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
}
static bool ssKernelApply(void *, unsigned, void *, const struct ss_control_state *);
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
    for (unsigned n = 0; n < configTASK_NOTIFICATION_ARRAY_ENTRIES; ++n)
        ss_notification_init(&s->notification[n], 0);
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
            configASSERT(ss_kernel_handle_pin(&ssKernelRegistry, t, &pin));
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
static void ssKernelPullIdle(unsigned, bool);
static void ssKernelCommandsFinish(bool);
static void ssKernelWait(unsigned, uint64_t, uint64_t);
static void ssKernelCommandsDetach(void);
/* Object publications retain waiter references until the outer metadata lock
 * is released. No producer writes a foreign owner node or timeout entry. */
static void ssKernelPublishReference(struct ss_kernel_pin pin)
{
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    unsigned priority = atomic_load_explicit(&s->effective, memory_order_acquire);
    struct ss_control_route route;
    if (ss_control_route_pin(&ssKernelControl, pin.slot, &route)) {
        if (route.owner == ssKernelCPUId()) ssKernelInspect(NULL, pin.slot, priority);
        else ss_owner_publish(&ssKernelCPUs[route.owner].run, pin.slot, priority);
        ss_control_route_put(&route);
    } else {
        /* One failed admission never waits for a moving route. Ordinary hints
         * let each owner inspect authoritative completion after adoption. */
        for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu)
            ss_owner_publish(&ssKernelCPUs[cpu].run, pin.slot, priority);
    }
    ss_kernel_handle_put(&pin);
}
static void ssKernelFlush(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (cpu->object_depth || cpu->isr_depth) return;
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
void ssKernelObjectEnter(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    if (!cpu->object_depth) {
        ++cpu->preempt;
        portGET_TASK_LOCK((BaseType_t)c);
    }
    if (!cpu->critical_depth) {
        cpu->object_mask = irq;
        portGET_ISR_LOCK((BaseType_t)c);
    }
    ++cpu->object_depth;
    ++cpu->critical_depth;
}
void ssKernelObjectExit(void)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->object_depth && cpu->critical_depth && cpu->preempt);
    --cpu->object_depth;
    if (--cpu->critical_depth) return;
    portRELEASE_ISR_LOCK((BaseType_t)c);
    if (!cpu->object_depth) {
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
UBaseType_t ssKernelObjectEnterFromISR(void)
{
    /* Also used for metadata repair from task APIs. The port's execution tag
     * selects direct privileged masking in IRQ/SVC and SVC masking at EL0. */
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    if (!cpu->isr_depth++) portGET_ISR_LOCK((BaseType_t)c);
    return irq;
}
void ssKernelObjectExitFromISR(UBaseType_t irq)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->isr_depth);
    if (!--cpu->isr_depth) {
        portRELEASE_ISR_LOCK((BaseType_t)c);
        ssKernelFlush();
    }
    portCLEAR_INTERRUPT_MASK(irq);
}
void ssKernelObjectSuspend(void)
{
    /* Queue retry bookkeeping retains T with local preemption disabled. API
     * IRQs remain enabled outside its separate inner I-protected sections. */
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    if (!cpu->object_depth) {
        ++cpu->preempt;
        portGET_TASK_LOCK((BaseType_t)c);
    }
    ++cpu->object_depth;
    ++cpu->suspend_depth;
    portCLEAR_INTERRUPT_MASK(irq);
}
BaseType_t ssKernelObjectResume(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(cpu->suspend_depth && cpu->object_depth && !cpu->critical_depth);
    --cpu->suspend_depth;
    if (!--cpu->object_depth) {
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
            cpu->run.ready[current->priority].head != NULL;
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
    if (&s->ready != cpu->run.current && !s->ready.queued)
        configASSERT(ss_owner_enqueue(&cpu->run, &s->ready, false));
    if (!cpu->run.current || s->ready.priority > cpu->run.current->priority)
        cpu->reschedule = true;
    ssKernelSliceRefresh(c, false);
}
static void ssKernelBlock(TickType_t ticks, bool indefinite, unsigned kind)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelTask *s = ssKernelCurrent();
    configASSERT(!s->deadline.owner && !s->ready.queued);
    configASSERT(atomic_load_explicit(&s->owner, memory_order_relaxed) == c);
    configASSERT(s->generation != UINT64_MAX);
    ++s->generation;
    s->wait_kind = kind;
    s->indefinite = indefinite && ticks == portMAX_DELAY;
    atomic_store_explicit(&s->outcome, SS_K_WAITING, memory_order_release);
    atomic_store_explicit(&s->execution, SS_K_BLOCKED, memory_order_release);
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
        ss_notification_gate_begin(&s->notification[s->notify_index], &ssKernelBackend, &guard);
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
        UBaseType_t irq = ssKernelObjectEnterFromISR();
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
        ssKernelObjectExitFromISR(irq);
        ssKernelMakeReady(s);
    } else {
        atomic_store_explicit(&s->outcome, SS_K_TIMED_OUT, memory_order_release);
        tracePOST_MOVED_TASK_TO_READY_STATE(s->ready.context);
        ssKernelMakeReady(s);
    }
}
static void ssKernelService(void)
{
    /* IRQ return and SVC selection share this owner. Keep API interrupts
     * masked across control maintenance, activation and deadline updates. */
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    ssKernelControlDrain();
    ss_owner_drain(&cpu->run, ssKernelInspect, NULL);
    if (!cpu->selecting) ssKernelPullServe(c);
    /* Event timeout completion edits object metadata. Exception entry does
     * that maintenance before selection; newly due work gets another entry. */
    if (!cpu->selecting)
        ss_deadline_expire(&cpu->deadlines, ssKernelNow(), SS_DEADLINE_BATCH,
                           ssKernelExpired, NULL);
    ssKernelSliceRefresh(c, false);
    if (atomic_load_explicit(&cpu->slice_deadline, memory_order_relaxed) <= ssKernelNow()) {
        cpu->yield_tail = true;
        cpu->reschedule = true;
        atomic_store_explicit(&cpu->slice_deadline, UINT64_MAX, memory_order_release);
    }
    uint64_t next;
    if ((ss_deadline_next(&cpu->deadlines, &next) && next <= ssKernelNow()) ||
        ss_owner_has_pending(&cpu->run) || cpu->object_repair[0] ||
        cpu->object_repair[1]) ssKernelForce(c);
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
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    if (cpu->preempt || ss_control_checkpoint(&ssKernelControl, c)) {
        cpu->reschedule = true;
        return;
    }
    struct ss_owner_task *previous = cpu->run.current;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    struct ss_owner_descriptor *previous_reference = previous ?
        &ssKernelRegistry.descriptor[previous->slot] : NULL;
    /* Current-owner exclusion proves this task cannot retire yet, including
     * after close. Retain that existing execution lifetime through service. */
    if (previous_reference)
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
            configASSERT(ss_owner_enqueue(&cpu->run, previous, !cpu->yield_tail));
        }
    }
    bool voluntary = cpu->yield_tail;
    cpu->yield_tail = false;
    cpu->selecting = true;
    ssKernelService();
    cpu->selecting = false;
    struct ss_owner_task *selected = NULL;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    if (previous && !voluntary) {
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
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    if (previous_reference) ss_owner_put(previous_reference);
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
}
void ssKernelOwnerYieldTail(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    /* The port calls this at SVC entry with the outgoing frame saved, before
     * vTaskSwitchContext. Match IRQ entry's object/timeout maintenance phase. */
    if (!cpu->preempt && !ss_control_checkpoint(&ssKernelControl, ssKernelCPUId()))
        ssKernelService();
    struct ssKernelTask *s = ssKernelCurrent();
    unsigned state = atomic_load_explicit(&s->execution, memory_order_acquire);
    if (state == SS_K_RUNNING || state == SS_K_READY) cpu->yield_tail = true;
    cpu->reschedule = true;
    portCLEAR_INTERRUPT_MASK(irq);
}
void ssKernelOwnerYield(void)
{
    /* SVC saves the frame and calls OwnerYieldTail exactly once. */
    portYIELD();
}
BaseType_t ssKernelOwnerIRQExit(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (!cpu->preempt && !ss_control_checkpoint(&ssKernelControl, ssKernelCPUId()))
        ssKernelService();
    return cpu->reschedule && !cpu->preempt ? pdTRUE : pdFALSE;
}
static BaseType_t ssKernelTick(void)
{
    /* CPU0 alone advances stage-B time. Remote CPUs receive only due-work or
     * slice requests; CPU0 never touches a remote timeout or ready link. */
    configASSERT(ssKernelCPUId() == 0);
    uint64_t now = atomic_fetch_add_explicit(&ssKernelTime, 1, memory_order_acq_rel) + 1;
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        uint64_t next;
        enum ss_deadline_state state = ss_deadline_snapshot(&ssKernelCPUs[c].deadlines.published, &next);
        if (state == SS_DEADLINE_UNKNOWN || (state == SS_DEADLINE_VALUE && next <= now))
            ssKernelForce(c);
        else if (atomic_load_explicit(&ssKernelCPUs[c].slice_deadline, memory_order_acquire) <= now)
            ssKernelForce(c);
    }
    return ssKernelOwnerIRQExit();
}
static void ssKernelEventBlock(List_t *list, TickType_t value, TickType_t ticks,
                               bool unordered, bool indefinite)
{
    struct ssKernelTask *s = ssKernelCurrent();
    TCB_t *t = s->ready.context;
    configASSERT(!s->event_reference.descriptor);
    configASSERT(ss_kernel_handle_pin(&ssKernelRegistry, t, &s->event_reference));
    s->unordered = unordered;
    if (unordered) {
        listSET_LIST_ITEM_VALUE(&t->xEventListItem, value | taskEVENT_LIST_ITEM_VALUE_IN_USE);
        vListInsertEnd(list, &t->xEventListItem);
    } else vListInsert(list, &t->xEventListItem);
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
    UBaseType_t irq = from_isr ? portSET_INTERRUPT_MASK_FROM_ISR() : portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    if (!ss_kernel_handle_pin(&ssKernelRegistry, task, &pin)) {
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
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
        ssKernelPublishReference(pin);
        if (woken && ssKernelCPUs[ssKernelCPUId()].reschedule) *woken = pdTRUE;
    } else ss_kernel_handle_put(&pin);
    if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
    return result.accepted ? pdPASS : pdFAIL;
}
static uint32_t ssKernelNotifyWait(UBaseType_t index, uint32_t clear_entry,
                                   uint32_t clear_exit, bool count, bool decrement,
                                   TickType_t ticks, bool *matched)
{
    configASSERT(index < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    UBaseType_t irq = portSET_INTERRUPT_MASK();
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

/* Owner callbacks run under the per-task control FIFO. They request object
 * maintenance here; its ISR lock is acquired only after that FIFO is released.
 * The retained control reference protects the TCB through repair and ack. */
static bool ssKernelObjectPrepared(struct ssKernelTask *s,
                                    const struct ss_control_state *state,
                                    unsigned priority)
{
    if (s->object_sequence == state->sequence + 1 && s->object_priority == priority)
        return true;
    s->object_requested = state->sequence + 1;
    s->object_requested_priority = priority;
    s->object_detach = state->dead || state->suspended ||
        state->offcpu_sequence > s->offcpu_ack || state->abort_generation != s->abort_generation;
    ssKernelCPUs[ssKernelCPUId()].object_repair[s->ready.slot / 64] |=
        UINT64_C(1) << (s->ready.slot % 64);
    return false;
}
static void ssKernelControlDrain(void)
{
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(!cpu->critical_depth);
    configASSERT(!cpu->isr_depth);
    configASSERT(!cpu->control_draining);
    /* Object unlock flushes publications. Keep deferred command consumption
     * outside this whole maintenance interval so its final ack cannot retire
     * a descriptor still referenced by the unlocked repair batch. */
    cpu->control_draining = true;
    ss_control_drain(&ssKernelControl, c);
    /* Controls can arrive after exception-entry preparation. Leave their
     * object repair pending until the next entry, outside task selection. */
    if (cpu->selecting) {
        cpu->control_draining = false;
        portCLEAR_INTERRUPT_MASK(irq);
        return;
    }
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t bits = cpu->object_repair[w];
        cpu->object_repair[w] = 0;
        while (bits) {
            unsigned b = (unsigned)__builtin_ctzll(bits);
            bits &= bits - 1;
            unsigned slot = w * 64 + b;
            struct ssKernelTask *s = &ssKernelTasks[slot];
            TCB_t *t = s->ready.context;
            UBaseType_t irq = ssKernelObjectEnterFromISR();
            List_t *list = listLIST_ITEM_CONTAINER(&t->xEventListItem);
            if (s->object_detach && s->wait_kind == SS_K_EVENT) {
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
            ssKernelObjectExitFromISR(irq);
            ss_control_repair(&ssKernelControl, slot);
        }
    }
    /* Repair completion can now be acknowledged without acquiring I in FIFO. */
    ss_control_drain(&ssKernelControl, c);
    cpu->control_draining = false;
    portCLEAR_INTERRUPT_MASK(irq);
}
static void ssKernelWait(unsigned slot, uint64_t sequence, uint64_t incarnation)
{
    while (!ss_control_applied(&ssKernelControl, slot, sequence) &&
           atomic_load_explicit(&ssKernelCleanupCompleted[slot], memory_order_acquire) < incarnation) {
        /* Re-enter this CPU's owner with API IRQs excluded, including after a
         * migrated task resumes its wait on a different CPU. Restore the
         * caller's mask before waiting for another owner's progress. */
        UBaseType_t irq = portSET_INTERRUPT_MASK();
        ssKernelControlDrain();
        bool applied = ss_control_applied(&ssKernelControl, slot, sequence) ||
            atomic_load_explicit(&ssKernelCleanupCompleted[slot], memory_order_acquire) >= incarnation;
        portCLEAR_INTERRUPT_MASK(irq);
        if (!applied) ss_owner_wait(ssKernelControl.backend);
    }
}
static bool ssKernelApply(void *context, unsigned slot, void *task,
                           const struct ss_control_state *state)
{
    (void)context;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    TCB_t *t = task;
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    configASSERT(atomic_load_explicit(&s->owner, memory_order_acquire) == c);
    struct ss_control_state guarded;
    bool deferred = atomic_load_explicit(&s->cleanup_shield, memory_order_acquire) &&
        (state->dead || state->suspended || state->offcpu_sequence > s->offcpu_ack ||
         state->abort_generation != s->abort_generation);
    if (deferred) {
        /* Resource destruction may block or migrate. Keep its caller alive
         * and pin-admissible, but still apply donation/base-priority repair. */
        s->cleanup_control_deferred = true;
        guarded = *state;
        guarded.dead = guarded.suspended = false;
        guarded.offcpu_sequence = s->offcpu_ack;
        guarded.abort_generation = s->abort_generation;
        state = &guarded;
    }
    if (state->dead) configASSERT(ss_kernel_handle_close(&ssKernelRegistry, slot));
    unsigned priority = state->priority;
    unsigned donation = atomic_load_explicit(&s->donation, memory_order_acquire);
    if (donation > priority) priority = donation;
    if (!ssKernelObjectPrepared(s, state, priority)) return false;
    if (state->dead || state->offcpu_sequence > s->offcpu_ack || state->suspended) {
        if (s->ready.queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));
        ssKernelCancel(s);
        s->suspended = state->suspended;
        s->dead = state->dead;
        atomic_store_explicit(&s->execution, state->dead ? SS_K_DEAD : SS_K_SUSPENDED,
                               memory_order_release);
        if (!s->off_cpu) { cpu->reschedule = true; return false; }
        s->offcpu_ack = state->offcpu_sequence;
    }
    if (s->ready.priority != priority) {
        bool queued = s->ready.queued;
        if (queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));
        s->ready.priority = priority;
        t->uxPriority = priority;
        if (queued) configASSERT(ss_owner_enqueue(&cpu->run, &s->ready, false));
        atomic_store_explicit(&s->effective, priority, memory_order_release);
        cpu->reschedule = true;
    }
    t->uxBasePriority = state->priority;
#if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )
    t->xPreemptionDisable = state->preemption_disabled ? pdTRUE : pdFALSE;
#endif
    t->uxCoreAffinityMask = (UBaseType_t)state->affinity;
    atomic_store_explicit(&s->base, state->priority, memory_order_release);
    if (state->dead || state->suspended) return true;
    if (s->suspended || atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_SUSPENDED) {
        s->suspended = false;
        ssKernelMakeReady(s);
    }
    if (state->abort_generation != s->abort_generation) {
        s->abort_generation = state->abort_generation;
        if (atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_BLOCKED) {
            bool trace_ready = atomic_load_explicit(&s->outcome, memory_order_acquire) == SS_K_WAITING;
            if (s->wait_kind == SS_K_NOTIFICATION) {
                struct ss_notification_guard guard;
                ss_notification_gate_begin(&s->notification[s->notify_index], &ssKernelBackend, &guard);
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
            t->ucDelayAborted = pdTRUE;
#endif
            ssKernelMakeReady(s);
        }
    }
    s->resume_generation = state->resume_generation;
    ssKernelInspect(NULL, slot, priority);
    return !deferred;
}
static bool ssKernelControlDeferred(void *context, unsigned slot)
{
    (void)context;
    return atomic_load_explicit(&ssKernelTasks[slot].cleanup_shield, memory_order_acquire) &&
           ssKernelTasks[slot].cleanup_control_deferred;
}
static bool ssKernelDetach(void *context, unsigned slot, void *task,
                            struct ss_control_transfer *transfer)
{
    (void)context; (void)task;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (!s->off_cpu) { cpu->reschedule = true; return false; }
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
static void ssKernelAdopt(void *context, unsigned slot, void *task,
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
    ssKernelInspect(NULL, slot, atomic_load_explicit(&s->effective, memory_order_acquire));
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
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    uint64_t admin = ssKernelAdminEnter();
    struct ssKernelTask *s = &ssKernelTasks[slot];
    if (atomic_load_explicit(&ssKernelRegistry.descriptor[slot].incarnation,
                              memory_order_acquire) == incarnation &&
        ssKernelRegistry.registered[slot] && !s->cleanup_queued &&
        ss_kernel_handle_unmap(&ssKernelRegistry, slot, true)) {
        s->cleanup_queued = true;
        atomic_fetch_or_explicit(&ssKernelCleanupPending[slot / 64],
            UINT64_C(1) << (slot % 64), memory_order_release);
    }
    ssKernelAdminExit(admin);
    portCLEAR_INTERRUPT_MASK(irq);
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
                 !cpu->object_depth && !cpu->isr_depth);
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
        ++state->abort_generation;
        return true;
    case SS_K_DELETE:
        state->dead = true;
        return true;
    case SS_K_MOVE:
        if (m->argument >= configNUMBER_OF_CORES || !(state->affinity & (UINT64_C(1) << m->argument)) ||
            state->move_to_plus_one == m->argument + 1) return false;
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
        for (struct ss_owner_task *t = cpu->run.ready[p].head; t && seen < 8; t = t->next, ++seen)
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
        for (struct ss_owner_task *t = cpu->run.ready[p].head; t && seen < 8; t = t->next, ++seen)
            if (((uint64_t)((TCB_t *)t->context)->uxCoreAffinityMask >> target) & 1u) return t;
    }
    return NULL;
}
/* Moves started by idle pull since the start; diagnostics read it. */
static _Atomic unsigned long ssKernelIdlePulls;
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
    while (serve) {
        const unsigned target = (unsigned)__builtin_ctzll(serve);
        serve &= serve - 1;
        struct ss_owner_task *candidate = ssKernelPullCandidate(cpu, target);
        if (!candidate) continue;
        struct ss_kernel_pin pin;
        if (!ss_kernel_handle_pin(&ssKernelRegistry, candidate->context, &pin)) continue;
        struct ssKernelMutation move = { .command = SS_K_MOVE, .argument = target, .slot = pin.slot };
        (void)ss_control_publish(&ssKernelControl, pin.slot, ssKernelMutate, &move);
        ss_kernel_handle_put(&pin);
        /* Start the transfer while the task is still queued here. */
        if (move.changed) {
            atomic_fetch_add_explicit(&ssKernelIdlePulls, 1, memory_order_relaxed);
            ssKernelControlDrain();
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
    for (unsigned v = 0; v < configNUMBER_OF_CORES; ++v) {
        if (v == c || !(atomic_load_explicit(&ssKernelCPUs[v].pull_targets, memory_order_acquire) & bit)) continue;
        atomic_fetch_or_explicit(&ssKernelCPUs[v].pull_requests, bit, memory_order_acq_rel);
        ssKernelForce(v);
    }
}
static bool ssKernelCommand(TaskHandle_t handle, enum ssKernelCommand command,
                             uint64_t argument, bool from_isr)
{
    TCB_t *current = ssKernelSelf();
    if (!handle) handle = current;
    UBaseType_t irq = from_isr ? portSET_INTERRUPT_MASK_FROM_ISR() : portSET_INTERRUPT_MASK();
    struct ss_kernel_pin pin;
    if (!ss_kernel_handle_pin(&ssKernelRegistry, handle, &pin)) {
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        return false;
    }
    struct ssKernelMutation mutation = { .command = command, .argument = argument, .slot = pin.slot };
    uint64_t sequence = ss_control_publish(&ssKernelControl, pin.slot, ssKernelMutate, &mutation);
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
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        return changed;
    }
    bool self = handle == current;
    unsigned slot = pin.slot;
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (cpu->object_depth || cpu->isr_depth) {
        /* Logical desired state is committed now. Owner consumption and task
         * acknowledgments wait until the caller releases compatibility I. */
        if (cpu->command_reference[slot].descriptor) ss_kernel_handle_put(&pin);
        else cpu->command_reference[slot] = pin;
        cpu->command_pending[slot / 64] |= UINT64_C(1) << (slot % 64);
        cpu->command_sequence[slot] = sequence;
        cpu->command_no_wait[slot] = from_isr || self;
        cpu->command_delete[slot] |= command == SS_K_DELETE;
        if (self && changed) cpu->reschedule = true;
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        return changed;
    }
    if (command == SS_K_DELETE && self) {
        ss_kernel_handle_put(&pin);
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        portYIELD();
        configASSERT(false); /* Deleted tasks cannot regain execution. */
        return changed;
    }
    if (atomic_load_explicit(&ssKernelTasks[slot].owner, memory_order_acquire) == ssKernelCPUId()) {
        ssKernelControlDrain();
        ssKernelSliceRefresh(ssKernelCPUId(), false);
    }
    if (!from_isr && changed && self && command == SS_K_SUSPEND) {
        /* No reference is kept on a stack that can remain suspended forever.
         * Returning here requires that this same task was resumed alive. */
        ss_kernel_handle_put(&pin);
        if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
        portYIELD();
        return changed;
    }
    uint64_t incarnation = pin.incarnation;
    /* The caller's stack can disappear at the first unmask. Retain only a
     * permanent slot/incarnation cookie across acknowledgment waits; cleanup
     * completion also satisfies a command whose target has since retired. */
    ss_kernel_handle_put(&pin);
    if (from_isr) portCLEAR_INTERRUPT_MASK_FROM_ISR(irq); else portCLEAR_INTERRUPT_MASK(irq);
    if (!from_isr && changed) {
        if (self && command == SS_K_AFFINITY) portYIELD();
        /* Permanent cookies remain valid after target retirement and reuse. */
        ssKernelWait(slot, sequence, incarnation);
    }
    ss_kernel_handle_put(&pin);
    if (!from_isr && changed && command == SS_K_DELETE)
        ssKernelWaitCleanup(slot, incarnation);
    if (!from_isr && ssKernelCPUs[ssKernelCPUId()].reschedule && ssKernelCanPreempt()) portYIELD();
    return changed;
}
static void ssKernelCommandsDetach(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (cpu->control_draining || cpu->preempt || cpu->object_depth || cpu->isr_depth) return;
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
    if (cpu->control_draining || cpu->preempt || cpu->object_depth || cpu->isr_depth) {
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
    if (!ss_kernel_handle_pin(&ssKernelRegistry, handle, &pin)) { portCLEAR_INTERRUPT_MASK(irq); return 0; }
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
    return ss_kernel_handle_pin(&ssKernelRegistry, handle, pin);
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
    if (!ss_kernel_handle_pin(&ssKernelRegistry, handle, &pin)) { portCLEAR_INTERRUPT_MASK(irq); return eDeleted; }
    unsigned state = ssKernelLogicalExecution(&ssKernelTasks[pin.slot]);
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
    portCLEAR_INTERRUPT_MASK(irq);
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
    if (!ss_kernel_handle_pin(&ssKernelRegistry, handle, &pin)) {
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
    UBaseType_t irq = ssKernelObjectEnterFromISR();
    struct ss_kernel_pin pin;
    if (!ssKernelPin(holder, &pin)) {
        ssKernelObjectExitFromISR(irq);
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
    ssKernelObjectExitFromISR(irq);
    return base < priority ? pdTRUE : pdFALSE;
}
static BaseType_t ssKernelDisinherit(TaskHandle_t holder, bool timeout, unsigned waiting)
{
    if (!holder) return pdFALSE;
    UBaseType_t irq = ssKernelObjectEnterFromISR();
    struct ss_kernel_pin pin;
    if (!ssKernelPin(holder, &pin)) {
        ssKernelObjectExitFromISR(irq);
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
    ssKernelObjectExitFromISR(irq);
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
    UBaseType_t irq = ssKernelObjectEnterFromISR();
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
    ssKernelObjectExitFromISR(irq);
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
    bool ready = ssKernelCPUs[ssKernelCPUId()].run.ready[tskIDLE_PRIORITY].head != NULL;
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
