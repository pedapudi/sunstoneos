/* Host port for the unmodified FreeRTOS kernel; see host_rtos.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include "host_rtos.h"
#include "sunstone_deadline.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HOST_ASAN 1
#endif
#endif
#ifdef HOST_ASAN
static const void *main_bottom; static size_t main_size;
void __sanitizer_start_switch_fiber(void **fake, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake, const void **bottom, size_t *size);
#endif

#define HOST_STACK_BYTES (512u * 1024u)
#define HOST_STATIC_STACK_WORDS 16
#define HOST_TASKS 64
#define HOST_EVENTS 32

unsigned host_core_id;
struct host_context {
    ucontext_t uc;
    void *stack;
    void *fake;
    void (*entry)(void *);
    void *argument;
    struct host_context *next;
    int dead;
    unsigned critical, mask;
};
static struct host_context main_context, *contexts, *current;
static unsigned critical, in_isr, mask_level, switch_pending;
static uint64_t now_us, switches;
static uint64_t next_tick_us = 1000;
static uint64_t tick_debt;
static unsigned running, finished, stuck;
static uint64_t idle_limit_us, idle_start_us;
static uint64_t idle_seen_switches;
static void (*idle_callback)(void *);
static void *idle_argument;
static void (*test_entry)(void *);
static void *test_argument;
static void (*timer_handler)(void);
static uint64_t compare_us;
static unsigned defer_kicks, kick_pending;
static uint64_t timer_hold_us;
#define COMPARE_DUE() (compare_us && (compare_us > timer_hold_us ? compare_us : timer_hold_us) <= now_us)
#define COMPARE_AT() (compare_us > timer_hold_us ? compare_us : timer_hold_us)

struct task_slot {
    StaticTask_t tcb;
    StackType_t stack[HOST_STATIC_STACK_WORDS];
};
static struct task_slot slots[HOST_TASKS];
static unsigned used_slots;

void host_assert_failed(const char *file, unsigned line)
{
    fprintf(stderr, "FAIL configASSERT %s:%u\n", file, line);
    fflush(stderr);
    abort();
}
static void fail(const char *what)
{
    fprintf(stderr, "FAIL host runtime: %s\n", what);
    fflush(stderr);
    abort();
}

/* ---- context switching ------------------------------------------------- */
static struct host_context *context_of(void *tcb)
{
    /* pxTopOfStack is the first member of the kernel's task control block. */
    return **(struct host_context ***)tcb;
}
static void transfer(struct host_context *from, struct host_context *to)
{
    if (from == to) return;
#ifdef HOST_ASAN
    __sanitizer_start_switch_fiber(&from->fake, to == &main_context ? main_bottom : to->stack,
                                   to == &main_context ? main_size : HOST_STACK_BYTES);
#endif
    /* Each task keeps its own critical nesting and interrupt mask across a switch,
     * as the board's port saves them with the task. */
    from->critical = critical; from->mask = mask_level;
    critical = to->critical; mask_level = to->mask;
    current = to;
    ++switches;
    swapcontext(&from->uc, &to->uc);
#ifdef HOST_ASAN
    const void *old_bottom; size_t old_size;
    __sanitizer_finish_switch_fiber(from->fake, &old_bottom, &old_size);
#endif
}
static void trampoline(void)
{
    struct host_context *self = current;
#ifdef HOST_ASAN
    const void *bottom; size_t size;
    __sanitizer_finish_switch_fiber(NULL, &bottom, &size);
    if (!main_bottom) { main_bottom = bottom; main_size = size; }
#endif
    self->entry(self->argument);
    fail("a task function returned");
}
StackType_t *pxPortInitialiseStack(StackType_t *top, TaskFunction_t code, void *parameters)
{
    struct host_context *c = calloc(1, sizeof *c);
    if (!c) fail("context allocation");
    c->stack = malloc(HOST_STACK_BYTES);
    if (!c->stack) fail("stack allocation");
    c->entry = (void (*)(void *))code;
    c->argument = parameters;
    c->next = contexts;
    contexts = c;
    getcontext(&c->uc);
    c->uc.uc_stack.ss_sp = c->stack;
    c->uc.uc_stack.ss_size = HOST_STACK_BYTES;
    c->uc.uc_link = NULL;
    makecontext(&c->uc, trampoline, 0);
    /* The kernel keeps the returned pointer in the task and checks that it lies in the
     * task's stack; the context pointer is stored there. */
    StackType_t *sp = top - 2;
    *(struct host_context **)sp = c;
    return sp;
}
extern void *volatile pxCurrentTCB;
void vPortYield(void)
{
    if (in_isr) { switch_pending = 1; return; }
    struct host_context *from = current;
    vTaskSwitchContext();
    transfer(from, context_of(pxCurrentTCB));
}
void host_isr_request_switch(void) { switch_pending = 1; }
void host_task_dying(void *tcb)
{
    struct host_context *c = context_of(tcb);
    c->dead = 1;
}
BaseType_t xPortStartScheduler(void)
{
    running = 1;
    current = &main_context;
    transfer(&main_context, context_of(pxCurrentTCB));
    running = 0;
    return 0;
}
void vPortEndScheduler(void) { fail("vPortEndScheduler"); }

/* ---- critical sections and interrupt masks ------------------------------ */
static void deliver(void);
void vPortEnterCritical(void)
{
    if (in_isr) fail("task critical section entered from an interrupt");
    ++critical;
}
void vPortExitCritical(void)
{
    if (!critical) fail("critical section exited without entry");
    if (!--critical) deliver();
}
void vPortDisableInterrupts(void) { ++mask_level; }
void vPortEnableInterrupts(void) { if (mask_level) --mask_level; if (!mask_level) deliver(); }
UBaseType_t xPortSetInterruptMask(void) { UBaseType_t old = mask_level; mask_level = 1; return old; }
void vPortClearInterruptMask(UBaseType_t old) { mask_level = (unsigned)old; if (!mask_level) deliver(); }
unsigned host_critical_depth(void) { return critical; }
unsigned host_in_isr(void) { return in_isr; }

/* ---- interrupts --------------------------------------------------------- */
static struct { uint64_t at; void (*handler)(void *); void *argument; unsigned used; } events[HOST_EVENTS];
static unsigned gives, max_gives;
static uint64_t total_gives;
void host_trace_give(void) { ++gives; ++total_gives; }
uint64_t host_gives_total(void) { return total_gives; }
uint64_t host_max_gives_per_isr(void) { return max_gives; }
void host_irq_at(uint64_t us, void (*handler)(void *), void *argument)
{
    for (unsigned i = 0; i < HOST_EVENTS; ++i)
        if (!events[i].used) { events[i].at = us; events[i].handler = handler; events[i].argument = argument; events[i].used = 1; return; }
    fail("too many scheduled interrupts");
}
static void finish_isr(void)
{
    if (switch_pending && !in_isr && !critical && !mask_level) {
        switch_pending = 0;
        vPortYield();
    }
}
static void run_isr(void (*handler)(void *), void *argument)
{
    if (in_isr) fail("nested interrupt");
    in_isr = 1;
    mask_level = 1;
    gives = 0;
    handler(argument);
    if (gives > max_gives) max_gives = gives;
    mask_level = 0;
    in_isr = 0;
}
void host_isr(void (*handler)(void *), void *argument)
{
    if (critical || mask_level) fail("interrupt raised while interrupts are masked");
    run_isr(handler, argument);
    finish_isr();
}
static void timer_isr(void *unused)
{
    (void)unused;
    compare_us = 0;
    if (timer_handler) timer_handler();
}
static void tick_isr(void *unused)
{
    (void)unused;
    while (tick_debt) {
        --tick_debt;
        if (xTaskIncrementTick() != pdFALSE) switch_pending = 1;
    }
}
/* Delivers every interrupt that is due, in the order tick, scheduled events by
 * time, compare timer, then switches once, so interrupts due at the same time
 * all run before the woken task does. Masked interrupts stay pending. */
static void deliver(void)
{
    while (running && !in_isr && !critical && !mask_level) {
        unsigned best = HOST_EVENTS;
        for (unsigned i = 0; i < HOST_EVENTS; ++i)
            if (events[i].used && events[i].at <= now_us && (best == HOST_EVENTS || events[i].at < events[best].at)) best = i;
        if (tick_debt) run_isr(tick_isr, NULL);
        else if (best != HOST_EVENTS) {
            void (*h)(void *) = events[best].handler; void *a = events[best].argument;
            events[best].used = 0;
            run_isr(h, a);
        } else if (COMPARE_DUE()) run_isr(timer_isr, NULL);
        else break;
    }
    finish_isr();
}

/* ---- virtual time ------------------------------------------------------- */
uint64_t host_time_us(void) { return now_us; }
uint64_t host_counter(void) { return now_us * 24u; }
uint64_t a733_smp_counter(void) { return host_counter(); }
void host_set_time_us(uint64_t us)
{
    if (us < now_us) fail("time moved backward");
    if (running && us != now_us) fail("host_set_time_us after the scheduler started; use host_advance_us");
    now_us = us;
    next_tick_us = (us / 1000u + 1u) * 1000u;
}
void host_advance_us(uint64_t us)
{
    uint64_t target = now_us + us;
    deliver();
    while (now_us < target) {
        uint64_t step = target;
        if (next_tick_us < step) step = next_tick_us;
        if (compare_us && COMPARE_AT() > now_us && COMPARE_AT() < step) step = COMPARE_AT();
        for (unsigned i = 0; i < HOST_EVENTS; ++i)
            if (events[i].used && events[i].at > now_us && events[i].at < step) step = events[i].at;
        now_us = step;
        if (now_us >= next_tick_us) { ++tick_debt; next_tick_us += 1000u; }
        deliver();
    }
    deliver();
}

/* ---- compare timer and deadline backend --------------------------------- */
uint64_t sunstone_deadline_now_us(void) { return now_us; }
void sunstone_deadline_program(uint64_t us) { compare_us = us; }
void host_timer_handler(void (*handler)(void)) { timer_handler = handler; }
void host_timer_hold_until(uint64_t us) { timer_hold_us = us; }
uint64_t host_timer_compare_us(void) { return compare_us; }
void host_deadline_defer_kicks(unsigned defer) { defer_kicks = defer; }
unsigned host_deadline_kick_pending(void) { return kick_pending; }
void sunstone_deadline_kick(void)
{
    if (defer_kicks) { kick_pending = 1; return; }
    sunstone_deadline_rearm();
}
void host_deadline_deliver_kick(void)
{
    if (!kick_pending) fail("no deferred kick to deliver");
    kick_pending = 0;
    sunstone_deadline_rearm();
}

/* ---- script task -------------------------------------------------------- */
static struct { uint64_t at; void (*fn)(void *); void *argument; unsigned used; uint64_t order; } script[HOST_EVENTS];
static uint64_t script_order;
static TaskHandle_t script_task;
static void script_wake(void *unused)
{
    BaseType_t woken = pdFALSE;
    (void)unused;
    vTaskNotifyGiveFromISR(script_task, &woken);
    portYIELD_FROM_ISR(woken);
}
static void script_main(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            unsigned best = HOST_EVENTS;
            for (unsigned i = 0; i < HOST_EVENTS; ++i)
                if (script[i].used && script[i].at <= now_us &&
                    (best == HOST_EVENTS || script[i].at < script[best].at ||
                     (script[i].at == script[best].at && script[i].order < script[best].order))) best = i;
            if (best == HOST_EVENTS) break;
            void (*fn)(void *) = script[best].fn; void *argument = script[best].argument;
            script[best].used = 0;
            fn(argument);
        }
    }
}
void host_run_at(uint64_t us, void (*fn)(void *), void *argument)
{
    for (unsigned i = 0; i < HOST_EVENTS; ++i)
        if (!script[i].used) {
            script[i].at = us; script[i].fn = fn; script[i].argument = argument; script[i].used = 1; script[i].order = script_order++;
            host_irq_at(us, script_wake, NULL);
            return;
        }
    fail("too many scheduled script steps");
}
void host_run_now(void (*fn)(void *), void *argument)
{
    host_run_at(now_us, fn, argument);
    host_advance_us(0);
}

/* ---- tasks, idle and the run loop -------------------------------------- */
uint64_t host_switches(void) { return switches; }
void host_on_idle(void (*callback)(void *), void *argument) { idle_callback = callback; idle_argument = argument; }
TaskHandle_t host_task_create(const char *name, void (*entry)(void *), void *argument, unsigned priority)
{
    if (used_slots == HOST_TASKS) fail("out of static task slots");
    struct task_slot *s = &slots[used_slots++];
    TaskHandle_t t = xTaskCreateStatic(entry, name, HOST_STATIC_STACK_WORDS, argument, priority, s->stack, &s->tcb);
    if (!t) fail("xTaskCreateStatic");
    return t;
}
void vApplicationIdleHook(void)
{
    if (finished) return;
    /* Idle time restarts whenever another task has run since the last visit. */
    if (switches != idle_seen_switches) { idle_start_us = now_us; }
    idle_seen_switches = switches;
    if (idle_callback) idle_callback(idle_argument);
    /* Nothing is runnable: time steps to the next tick. */
    host_advance_us(next_tick_us - now_us);
    if (now_us - idle_start_us > idle_limit_us) {
        stuck = 1;
        fprintf(stderr, "FAIL host runtime: every task blocked for %llu us of virtual time\n",
                (unsigned long long)(now_us - idle_start_us));
        fflush(stderr);
        abort();
    }
}
static void test_task(void *unused)
{
    (void)unused;
    test_entry(test_argument);
    finished = 1;
    transfer(current, &main_context);
}
void host_rtos_run(void (*test)(void *), void *argument, unsigned priority, uint64_t max_idle_us)
{
    if (running || finished) fail("one scheduler session per process");
    test_entry = test;
    test_argument = argument;
    idle_limit_us = max_idle_us;
    script_task = host_task_create("script", script_main, NULL, configMAX_PRIORITIES - 1);
    host_task_create("test", test_task, NULL, priority);
    vTaskStartScheduler();
    if (!finished) fail("scheduler returned before the test finished");
    (void)stuck;
    while (contexts) {
        struct host_context *c = contexts;
        contexts = c->next;
        free(c->stack);
        free(c);
    }
}
