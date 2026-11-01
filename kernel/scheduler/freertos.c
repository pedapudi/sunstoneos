/* SPDX-License-Identifier: GPL-2.0-or-later */
/* This adaptation is included by the generated tasks.c after its private TCB,
 * variables and prototypes. Stack construction, task ABI and port frame remain
 * upstream implementations. It is never compiled as a separate translation unit. */
#ifndef SS_FREERTOS_TASKS_TRANSLATION
#error "Include this adapter only from the patched FreeRTOS tasks.c"
#endif
#include "freertos.h"
#include "freertos_registry.h"
#include "freertos_control.h"
#include "notification.h"
#include "deadline.h"

_Static_assert(configNUMBER_OF_CORES > 1 && configNUMBER_OF_CORES <= 8,
               "the admitted owner fork supports two through eight CPUs");
_Static_assert(configMAX_PRIORITIES == SS_OWNER_PRIORITIES,
               "owner priorities must match FreeRTOS priorities");

enum ssKernelExecution { SS_K_READY, SS_K_RUNNING, SS_K_BLOCKED, SS_K_SUSPENDED, SS_K_DEAD };
enum ssKernelWait { SS_K_NONE, SS_K_DELAY, SS_K_EVENT, SS_K_NOTIFICATION };
enum ssKernelOutcome { SS_K_WAITING, SS_K_EVENT_READY, SS_K_TIMED_OUT, SS_K_ABORTED };
struct ssKernelTask {
    struct ss_owner_task ready;
    struct ss_deadline_entry deadline;
    struct ss_notification notification[configTASK_NOTIFICATION_ARRAY_ENTRIES];
    struct ss_notification_wait notify_wait;
    struct ss_kernel_pin event_reference;
    _Atomic unsigned owner, execution, outcome, effective, base;
    uint64_t generation;
    unsigned wait_kind, notify_index;
    bool indefinite, unordered, off_cpu;
};
struct ssKernelCPU {
    struct ss_owner run;
    struct ss_deadline_heap deadlines;
    unsigned preempt, object_depth, critical_depth, suspend_depth, isr_depth;
    UBaseType_t object_mask;
    bool yield_tail, reschedule, irq_drained;
    uint64_t publication[2];
    struct ss_kernel_pin reference[SS_OWNER_SLOTS];
};
static struct ssKernelTask ssKernelTasks[SS_OWNER_SLOTS];
static struct ssKernelCPU ssKernelCPUs[configNUMBER_OF_CORES];
static struct ss_kernel_registry ssKernelRegistry;
static struct ss_control ssKernelControl;
static _Atomic uint64_t ssKernelTime;
static _Atomic uint64_t ssKernelAdminNext, ssKernelAdminServing;
static bool ssKernelInitialized;

static void ssKernelSGI(void *context, unsigned cpu)
{
    (void)context;
    if (xSchedulerRunning) portYIELD_CORE((BaseType_t)cpu);
}
static const struct ss_owner_backend ssKernelBackend = { .sgi = ssKernelSGI };
static unsigned ssKernelCPUId(void) { return (unsigned)portGET_CORE_ID(); }
static uint64_t ssKernelNow(void)
{ return atomic_load_explicit(&ssKernelTime, memory_order_acquire); }
static struct ssKernelTask *ssKernelCurrent(void)
{
    TCB_t *t = pxCurrentTCBs[ssKernelCPUId()];
    configASSERT(t && t->uxOwnerSlot < SS_OWNER_SLOTS);
    return &ssKernelTasks[t->uxOwnerSlot];
}
static void ssKernelForce(unsigned cpu)
{
    /* Controls and retirement are urgent regardless of running priority. */
    ss_owner_barrier(&ssKernelBackend);
    ssKernelSGI(NULL, cpu);
}
static uint64_t ssKernelAdminEnter(void)
{
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
static void ssKernelAdopt(void *, unsigned, void *, const struct ss_control_transfer *);
static bool ssKernelDrained(void *, unsigned, unsigned);
static void ssKernelRetired(void *, unsigned);
static bool ssKernelDispatch(void *, unsigned);
static void ssKernelInspect(void *, unsigned, unsigned);
static void ssKernelExpired(void *, const struct ss_deadline_result *);

static void ssKernelInitialize(void)
{
    if (ssKernelInitialized) return; /* Boot CPU only, before scheduler start. */
    ss_kernel_registry_init(&ssKernelRegistry, &ssKernelBackend);
    struct ss_control_ops ops = {
        .apply = ssKernelApply, .detach = ssKernelDetach, .adopt = ssKernelAdopt,
        .drained = ssKernelDrained, .retired = ssKernelRetired,
        .dispatch = ssKernelDispatch
    };
    ss_control_init(&ssKernelControl, (UINT64_C(1) << configNUMBER_OF_CORES) - 1,
                    &ssKernelBackend, &ops);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        ss_owner_init(&ssKernelCPUs[c].run, c, &ssKernelBackend);
        ss_deadline_init(&ssKernelCPUs[c].deadlines);
    }
    atomic_init(&ssKernelTime, configINITIAL_TICK_COUNT);
    atomic_init(&ssKernelAdminNext, 0);
    atomic_init(&ssKernelAdminServing, 0);
    ssKernelInitialized = true;
}

struct ssKernelCreation { TCB_t *task; unsigned owner; };
static void ssKernelSlotInitialize(void *opaque, unsigned slot, uint64_t incarnation)
{
    struct ssKernelCreation *creation = opaque;
    TCB_t *t = creation->task;
    struct ssKernelTask *s = &ssKernelTasks[slot];
    *s = (struct ssKernelTask){0};
    s->ready = (struct ss_owner_task){ .context = t, .slot = slot,
                                      .priority = (unsigned)t->uxPriority };
    ss_deadline_entry_init(&s->deadline);
    for (unsigned n = 0; n < configTASK_NOTIFICATION_ARRAY_ENTRIES; ++n)
        ss_notification_init(&s->notification[n], 0);
    atomic_init(&s->owner, creation->owner);
    atomic_init(&s->execution, SS_K_READY);
    atomic_init(&s->outcome, SS_K_WAITING);
    atomic_init(&s->effective, (unsigned)t->uxPriority);
    atomic_init(&s->base, (unsigned)t->uxPriority);
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
    unsigned owner = (unsigned)__builtin_ctzll(mask);
    /* Fixed partition placement initially preserves explicit affinities. A
     * caller on an eligible CPU keeps local work; D adds capacity placement. */
    if (xSchedulerRunning && (mask & (UINT64_C(1) << ssKernelCPUId()))) owner = ssKernelCPUId();
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    uint64_t admin = ssKernelAdminEnter();
    struct ssKernelCreation creation = { t, owner };
    unsigned slot = ss_kernel_handle_register(&ssKernelRegistry, t,
                                              ssKernelSlotInitialize, &creation);
    if (slot < SS_OWNER_SLOTS) {
        ++uxCurrentNumberOfTasks;
        ++uxTaskNumber;
#if configUSE_TRACE_FACILITY == 1
        t->uxTCBNumber = uxTaskNumber;
#endif
        portSETUP_TCB(t);
        traceTASK_CREATE(t);
    }
    ssKernelAdminExit(admin);
    if (slot < SS_OWNER_SLOTS) {
        if (!xSchedulerRunning || owner == ssKernelCPUId()) {
            configASSERT(ss_owner_enqueue(&ssKernelCPUs[owner].run, &ssKernelTasks[slot].ready, false));
        } else {
            ss_owner_publish(&ssKernelCPUs[owner].run, slot, (unsigned)t->uxPriority);
        }
    }
    portCLEAR_INTERRUPT_MASK(irq);
    if (slot < SS_OWNER_SLOTS && xSchedulerRunning) {
        struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
        if (owner == ssKernelCPUId() && t->uxPriority > pxCurrentTCBs[owner]->uxPriority) {
            cpu->reschedule = true;
            portYIELD();
        }
    }
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

/* Object publications retain waiter references until the outer metadata lock
 * is released. No producer writes a foreign owner node or timeout entry. */
static void ssKernelPublishReference(struct ss_kernel_pin pin)
{
    struct ssKernelTask *s = &ssKernelTasks[pin.slot];
    unsigned owner = atomic_load_explicit(&s->owner, memory_order_acquire);
    unsigned priority = atomic_load_explicit(&s->effective, memory_order_acquire);
    if (owner == ssKernelCPUId()) ssKernelInspect(NULL, pin.slot, priority);
    else ss_owner_publish(&ssKernelCPUs[owner].run, pin.slot, priority);
    ss_kernel_handle_put(&pin);
}
static void ssKernelFlush(void)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    if (cpu->object_depth || cpu->isr_depth) return;
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t bits = cpu->publication[w];
        cpu->publication[w] = 0;
        while (bits) {
            unsigned b = (unsigned)__builtin_ctzll(bits), slot = w * 64 + b;
            bits &= bits - 1;
            struct ss_kernel_pin pin = cpu->reference[slot];
            cpu->reference[slot] = (struct ss_kernel_pin){0};
            ssKernelPublishReference(pin);
        }
    }
}
static void ssKernelDefer(struct ss_kernel_pin pin)
{
    struct ssKernelCPU *cpu = &ssKernelCPUs[ssKernelCPUId()];
    unsigned slot = pin.slot;
    configASSERT(pin.descriptor && !cpu->reference[slot].descriptor);
    cpu->reference[slot] = pin;
    cpu->publication[slot / 64] |= UINT64_C(1) << (slot % 64);
}
void ssKernelObjectEnter(void)
{
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    UBaseType_t irq = portSET_INTERRUPT_MASK();
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
    portCLEAR_INTERRUPT_MASK(irq);
    if (yield) portYIELD();
}
UBaseType_t ssKernelObjectEnterFromISR(void)
{
    unsigned c = ssKernelCPUId();
    UBaseType_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
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
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
}
void ssKernelObjectSuspend(void)
{
    /* Queue retry bookkeeping retains T with local preemption disabled. API
     * IRQs remain enabled outside its separate inner I-protected sections. */
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    UBaseType_t irq = portSET_INTERRUPT_MASK();
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
    unsigned c = ssKernelCPUId();
    struct ssKernelCPU *cpu = &ssKernelCPUs[c];
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    configASSERT(cpu->suspend_depth && cpu->object_depth && !cpu->critical_depth);
    --cpu->suspend_depth;
    if (!--cpu->object_depth) {
        portRELEASE_TASK_LOCK((BaseType_t)c);
        ssKernelFlush();
        configASSERT(cpu->preempt);
        --cpu->preempt;
    }
    bool yield = !cpu->preempt && cpu->reschedule;
    portCLEAR_INTERRUPT_MASK(irq);
    if (yield) portYIELD();
    return yield ? pdTRUE : pdFALSE;
}
