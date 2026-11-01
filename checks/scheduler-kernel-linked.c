/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included after the generated private tasks translation. The host does not
 * execute task stacks; each checkpoint explicitly saves a frame and enters its
 * owner, so a passing run is linked transition coverage, not an SMP soak. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned fixture_cpu, fixture_sgi[configNUMBER_OF_CORES], fixture_locks;
volatile uint64_t ullCriticalNestings[configNUMBER_OF_CORES];
uint64_t ullPortYieldRequired[configNUMBER_OF_CORES];
void platform_panic(const char *file, unsigned line)
{ fprintf(stderr, "%s:%u: kernel assertion\n", file, line); abort(); }
uint64_t ssFixtureTaskRegister(void) { return (uintptr_t)pxCurrentTCBs[fixture_cpu]; }
uint8_t ucPortGetCoreID(void) { return (uint8_t)fixture_cpu; }
uint8_t ucPortGetCoreIDFromIsr(void) { return (uint8_t)fixture_cpu; }
UBaseType_t uxPortSetInterruptMask(void) { return 0; }
UBaseType_t uxPortSetInterruptMaskFromISR(void) { return 0; }
void vPortClearInterruptMask(UBaseType_t mask) { (void)mask; }
void vPortClearInterruptMaskFromISR(UBaseType_t mask) { (void)mask; }
void vPortRecursiveLock(uint8_t cpu, ePortRTOSLock lock, BaseType_t acquire)
{ (void)cpu; (void)lock; if (acquire) ++fixture_locks; }
void vInterruptCore(uint32_t interrupt, uint8_t cpu)
{
    (void)interrupt; ++fixture_sgi[cpu];
    /* Explicit delivery of gate checkpoints; no concurrent task stacks. */
    if (atomic_load(&ssKernelControl.gate_phase) == SS_GATE_ACQUIRING)
        (void)ss_control_checkpoint(&ssKernelControl, cpu);
}
StackType_t *pxPortInitialiseStack(StackType_t *stack, TaskFunction_t entry, void *arg)
{ (void)entry; (void)arg; *stack = 0; return stack; }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; (void)name; abort(); }
static BaseType_t fixture_hook(void *arg) { return *(BaseType_t *)arg; }
static void fixture_entry(void *arg) { (void)arg; }
static StaticTask_t storage[configNUMBER_OF_CORES + 4];
static StackType_t stacks[configNUMBER_OF_CORES + 4][configMINIMAL_STACK_SIZE];
static TaskHandle_t create(unsigned index, unsigned priority)
{
    return xTaskCreateStatic(fixture_entry, "linked", configMINIMAL_STACK_SIZE,
                            NULL, priority, stacks[index], &storage[index]);
}
static struct ssKernelTask *record(TaskHandle_t task)
{ return &ssKernelTasks[((TCB_t *)task)->uxOwnerSlot]; }
static void enter(unsigned cpu)
{ fixture_cpu = cpu; ssKernelSwitch(cpu); }
static void invariant(void)
{
    unsigned running[SS_OWNER_SLOTS] = {0};
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        struct ss_owner *run = &ssKernelCPUs[c].run;
        if (run->current) {
            unsigned slot = run->current->slot;
            assert(++running[slot] == 1);
            assert(atomic_load(&ssKernelTasks[slot].owner) == c);
            assert(!run->current->queued);
        }
        for (unsigned p = 0; p < SS_OWNER_PRIORITIES; ++p)
            for (struct ss_owner_task *t = run->ready[p].head; t; t = t->next) {
                assert(atomic_load(&ssKernelTasks[t->slot].owner) == c);
                assert(t->queued && t->priority == p);
            }
    }
}
#include "scheduler-kernel-objects.c"
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) return fixture_startup();
    /* Real static creation enters the registry and least-loaded placement. */
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        TaskHandle_t idle = create(c, 0);
        struct ssKernelTask *s = record(idle);
        assert(atomic_load(&s->owner) == c);
        xIdleTaskHandles[c] = idle;
        ((TCB_t *)idle)->uxTaskAttributes |= taskATTRIBUTE_IS_IDLE;
        ((TCB_t *)idle)->uxCoreAffinityMask = (UBaseType_t)1 << c;
        pxCurrentTCBs[c] = idle;
    }
    ssKernelBootstrap();
    xSchedulerRunning = pdTRUE;
    TaskHandle_t task = create(configNUMBER_OF_CORES, 5);
    TaskHandle_t second = create(configNUMBER_OF_CORES + 1, 5);
    assert(atomic_load(&record(task)->owner) == 0);
    assert(atomic_load(&record(second)->owner) == 1);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    fixture_cpu = 0;
    assert(!strcmp(pcTaskGetName(task), "linked"));
    assert(vTaskCoreAffinityGet(task) == tskNO_AFFINITY);
    StackType_t *stack = NULL;
    StaticTask_t *tcb = NULL;
    assert(xTaskGetStaticBuffers(task, &stack, &tcb));
    assert(stack == stacks[configNUMBER_OF_CORES] && tcb == &storage[configNUMBER_OF_CORES]);
    vTaskSetTaskNumber(task, 42);
    assert(uxTaskGetTaskNumber(task) == 42);
    vTaskSetThreadLocalStoragePointer(task, 1, stack);
    assert(pvTaskGetThreadLocalStoragePointer(task, 1) == stack);
    assert(!pvTaskGetThreadLocalStoragePointer(task, -1));
    assert(!pvTaskGetThreadLocalStoragePointer(task, 2));
    vTaskSetApplicationTaskTag(task, fixture_hook);
    assert(xTaskGetApplicationTaskTag(task) == fixture_hook);
    assert(xTaskGetApplicationTaskTagFromISR(task) == fixture_hook);
    BaseType_t hook_value = 73;
    assert(xTaskCallApplicationTaskHook(task, &hook_value) == 73);
    assert(uxTaskGetStackHighWaterMark(task) == uxTaskGetStackHighWaterMark2(task));
    assert(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    vTaskSuspendAll();
    vTaskSuspendAll();
    assert(xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED);
    assert(ssKernelCPUs[0].preempt == 2);
    (void)xTaskResumeAll();
    assert(ssKernelCPUs[0].preempt == 1);
    (void)xTaskResumeAll();
    assert(!ssKernelCPUs[0].preempt);
    assert(atomic_load(&ssKernelControl.gate_phase) == SS_GATE_OPEN);
    TickType_t previous = xTaskGetTickCount();
    assert(xTaskPeriodicDelay(&previous, 2) == 0);
    enter(0);
    (void)xTaskIncrementTick();
    (void)xTaskIncrementTick();
    enter(0);
    assert(pxCurrentTCBs[0] == task);
    unsigned baseline_locks = fixture_locks;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        struct ssKernelTask *s = record(task);
        fixture_cpu = 0;
        assert(pxCurrentTCBs[0] == task);
        /* Block commit and remote notify use real notification compound state
         * and the public FreeRTOS notification entry point. */
        struct ss_notification_guard guard;
        ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
        uint64_t generation = s->generation + 1;
        assert(ss_notification_wait_enter(&guard, &s->notify_wait, generation,
                                          SS_NOTIFY_WAIT_COUNT, 0, true));
        s->notify_index = 0;
        ssKernelBlock(3, true, SS_K_NOTIFICATION);
        ss_notification_gate_end(&guard);
        if (iteration & 1) enter(0); /* Also wake before saved-frame eviction. */
        fixture_cpu = 1;
        BaseType_t woken = pdFALSE;
        assert(xTaskGenericNotifyFromISR(task, 0, 0, eIncrement, NULL, &woken) == pdPASS);
        enter(0);
        assert(pxCurrentTCBs[0] == task && !s->deadline.owner);
        ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
        struct ss_notification_finish finish = ss_notification_wait_finish(
            &guard, &s->notify_wait, generation, 0, false, false);
        ss_notification_gate_end(&guard);
        assert(finish.matched && finish.condition && finish.value == 1);
        assert(ulTaskGenericNotifyTake(0, pdTRUE, 0) == 0);
        /* Owner-local delay with CPU0 tick only posting remote owners. */
        vTaskDelay(2);
        enter(0);
        fixture_cpu = 0;
        (void)xTaskIncrementTick();
        assert(atomic_load(&s->execution) == SS_K_BLOCKED);
        (void)xTaskIncrementTick();
        enter(0);
        assert(pxCurrentTCBs[0] == task && !s->deadline.owner);
        invariant();
    }
    assert(fixture_locks == baseline_locks);
    fixture_cpu = 1;
    vTaskDelay(2);
    enter(1);
    fixture_cpu = 0;
    (void)xTaskIncrementTick();
    (void)xTaskIncrementTick();
    /* Remote CPU0 expiration delivery must leave CPU1's queue and heap alone. */
    assert(atomic_load(&record(second)->execution) == SS_K_BLOCKED);
    assert(record(second)->deadline.owner == &ssKernelCPUs[1].deadlines);
    enter(1);
    assert(pxCurrentTCBs[1] == second && !record(second)->deadline.owner);
    fixture_cpu = 0;
    List_t event;
    vListInitialise(&event);
    ssKernelObjectEnter();
    vTaskPlaceOnEventList(&event, 4);
    ssKernelObjectExit();
    enter(0);
    fixture_cpu = 1;
    UBaseType_t irq = ssKernelObjectEnterFromISR();
    (void)xTaskRemoveFromEventList(&event);
    ssKernelObjectExitFromISR(irq);
    enter(0);
    assert(pxCurrentTCBs[0] == task && listLIST_IS_EMPTY(&event));
    assert(!record(task)->event_reference.descriptor);
    assert(!record(task)->deadline.owner);
    /* Timeout removes an event registration before making the task ready. */
    ssKernelObjectEnter();
    vTaskPlaceOnEventList(&event, 1);
    ssKernelObjectExit();
    enter(0);
    (void)xTaskIncrementTick();
    enter(0);
    assert(listLIST_IS_EMPTY(&event) && pxCurrentTCBs[0] == task);
    fixture_objects();
    fixture_inheritance(task, second);
    fixture_diagnostics();
    fixture_critical();
    fixture_migration(task);
    fixture_migration_wake(task);
    fixture_delete_reuse(task, second);
    invariant();
    printf("linked FreeRTOS owner transitions: 10000 remote wakes, block-commit races, zero local scheduler locks, delay/event timeout\n");
    return 0;
}
