#include "npu_runtime_queue.h"
#include "a733_mmio.h"
#include "a733_npu_idle.h"
#include "sunstone_crumb.h"
#include "sunstone_deadline.h"
#ifdef SUNSTONE_NPU_OP_TIMING
#include "aarch64_device_memory.h"
#include "nbg_command.h"
#include <string.h>
#endif

/* This file is the A733 resident-queue driver, so the submission interface of
 * the VIPLite 2 command processor (vip_drv_hardware.c:882-893) is fixed here:
 * the command fetch address, the fetch trigger and its value, and the
 * read-to-acknowledge event word. Only the idle and clock-gate protocol is
 * part-specific data (struct npu_idle_part); the status word and the full-idle
 * value come from the same descriptor. */
#define NPU_REG_COMMAND_ADDRESS UINT32_C(0x654)
#define NPU_REG_COMMAND_FETCH UINT32_C(0x3a4)
#define NPU_REG_EVENTS UINT32_C(0x010)
#ifdef SUNSTONE_NPU_TRAFFIC_COUNTERS
static const uint32_t traffic_reg[4] = {0x040, 0x044, 0x078, 0x07c};
#endif
static void power_owner(void *);
static struct npu_runtime_queue *creating_power;
BaseType_t npu_runtime_queue_task_privileged(TaskFunction_t entry, void *argument)
{
#if configNUMBER_OF_CORES > 1
    return creating_power && argument == creating_power && entry == power_owner ? pdTRUE : pdFALSE;
#else
    (void)entry; (void)argument;
    return pdFALSE; /* The single-core SRE port already starts every task at EL1. */
#endif
}

uint64_t npu_runtime_queue_lock(void)
{
    uint64_t saved;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(saved) :: "memory");
    return saved;
}
void npu_runtime_queue_unlock(uint64_t saved)
{ __asm__ volatile("msr daif, %0" :: "r"(saved) : "memory"); }
static uint64_t ticks(void)
{
    uint64_t value;
    __asm__ volatile("isb\n mrs %0,cntpct_el0" : "=r"(value) :: "memory");
    return value;
}
static int owner(struct npu_runtime_queue *q)
{
    uint64_t el;
    __asm__ volatile("mrs %0,CurrentEL" : "=r"(el));
    return el == 4 && q && q->claimed && xTaskGetCurrentTaskHandle() == q->task;
}
static int validate(void *cookie, const struct a733_npu_command *c)
{
    struct npu_runtime_queue *q = cookie;
    struct npu_runtime *rt = q->runtime;
    /* Slow registration checks the exact pinned command reservation, its
     * address-bearing bytes and its complete mapped fetch tail once. */
    for (unsigned i = 0; i < rt->slots; ++i) {
        const struct a733_npu_command *own = &rt->commands[i + 1];
        if (c->device == own->device && c->readable_bytes == own->readable_bytes && c->token == own->token)
            return atomic_load_explicit(&rt->slot[i].state, memory_order_acquire) == NPU_RUNTIME_READY &&
                npu_runtime_stream_intact(rt, &rt->slot[i]);
    }
    return 0;
}
static int window(struct npu_runtime_queue *q)
{
    unsigned failed = atomic_load_explicit(&q->runtime->service.signals->runtime_failed, memory_order_acquire);
    unsigned open = a733_npu_startup_window_open(q->runtime->startup);
    unsigned owns = q->claimed && q->runtime->service.direct == &q->direct;
    if (!failed && open && owns) return 1;
    sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU), failed | open << 1 | owns << 2, SUNSTONE_CRUMB_HERE());
    return 0;
}
#ifdef SUNSTONE_NPU_OP_TIMING
/* Run the first operations of the job's graph only: the assembler's footer (flush, semaphore and stall, event, END) replaces
 * the commands after the last operation asked for, and complete() puts them back. The NPU is idle here, and a footer is
 * written only at an operation end the assembler recorded, so the job is a prefix of the admitted stream. */
static int cut_maintain(const uint8_t *at)
{
    const uintptr_t first = (uintptr_t)at & ~(uintptr_t)63, end = ((uintptr_t)at + sizeof(footer) + 63) & ~(uintptr_t)63;
    return aarch64_device_cache(AARCH64_CACHE_CLEAN, first, end - first);
}
static int cut_apply(struct npu_runtime_queue *q, const struct a733_npu_command *c)
{
    volatile uint32_t *r = q->cut_request;
    struct npu_runtime *rt = q->runtime;
    if (!r || q->cut_at) return 1;
    for (unsigned i = 0; i < rt->slots; ++i) {
        if (rt->commands[i + 1].device != c->device) continue;
        struct npu_runtime_slot *s = &rt->slot[i];
        const uint32_t want = r[0], ops = r[1];
        if (want != i + 1) return 1;
        r[2] = s->op_count; r[3] = 0;
        if (!ops || ops >= s->op_count) return 1;
        const uint32_t at = s->op_end[ops - 1];
        if ((at & 7u) || at + sizeof(footer) > s->stream_bytes) return 1;
        uint8_t *const p = (uint8_t *)s->command.cpu + at;
        memcpy(q->cut_saved, p, sizeof(footer)); memcpy(p, footer, sizeof(footer));
        q->cut_at = p;
        r[3] = at + (uint32_t)sizeof(footer);
        return cut_maintain(p);
    }
    return 1;
}
static int cut_restore(struct npu_runtime_queue *q)
{
    if (!q->cut_at) return 1;
    uint8_t *const p = q->cut_at;
    memcpy(p, q->cut_saved, sizeof(footer)); q->cut_at = NULL;
    return cut_maintain(p);
}
_Static_assert(sizeof(((struct npu_runtime_queue *)0)->cut_saved) >= sizeof(footer), "the saved commands hold one footer");
#endif
static int start(void *cookie, const struct a733_npu_command *c, uint64_t timeout)
{
    struct npu_runtime_queue *q = cookie;
    if (q->power.initialized && (q->power.phase != NPU_IDLE_ON || q->power_stop)) return 0;
    if (timeout > UINT64_MAX / q->frequency) {
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU), (uint32_t)timeout, SUNSTONE_CRUMB_HERE());
        return 0;
    }
    q->timeout_ticks = timeout * q->frequency / 1000000;
    if (!q->timeout_ticks) return 0;
    q->started = ticks();
    q->power_resume_irq = 0;
    const uintptr_t base = q->runtime->native.registers;
    /* VIPLite vip_drv_hardware.c:882-893: address, full barrier, fetch word.
     * Mapping, power and command validation belong to claim/registration. */
    if (!window(q)) return 0;
#ifdef SUNSTONE_NPU_OP_TIMING
    if (!cut_apply(q, c)) return 0;
#endif
    a733_mmio_write32_dsb_sy_after(base + NPU_REG_COMMAND_ADDRESS, c->device);
    if (!window(q)) return 0;
#ifdef SUNSTONE_NPU_TRAFFIC_COUNTERS
    for (unsigned k = 0; k < 4; ++k) q->traffic_start[k] = *(volatile uint32_t *)(base + traffic_reg[k]);
    q->traffic_armed = 1;
#endif
    a733_mmio_write32_dsb_sy_after(base + NPU_REG_COMMAND_FETCH, a733_npu_fetch_word(c->readable_bytes));
#ifdef SUNSTONE_NPU_QUEUE_TIMING
    q->runtime->doorbell_ticks = ticks();
#endif
    return 1;
}
/* The slot of the job at the head of the queue, from its command address; 0xff when none matches. */
static unsigned slot_of(const struct npu_runtime_queue *q)
{
    const struct npu_queue_job *job = q->queue.head;
    if (!job || !job->command) return 0xffu;
    for (unsigned k = 1; k <= q->runtime->slots && k < 0xffu; ++k)
        if (q->runtime->commands[k].device == job->command->hardware.device) return k - 1;
    return 0xffu;
}
static int complete(void *cookie)
{
    struct npu_runtime_queue *q = cookie;
    const uintptr_t base = q->runtime->native.registers;
    /* +0x010 is read-to-acknowledge. Read it once, as the vendor ISR does.
     * Completion requires event zero AND full idle before CPU acquisition. */
    if (!window(q)) return 0;
    const uint32_t events = *(volatile uint32_t *)(base + NPU_REG_EVENTS);
    if (!window(q)) return 0;
    const uint32_t idle = *(volatile uint32_t *)(base + a733_npu_idle_part.status);
    __asm__ volatile("dsb sy" ::: "memory");
    q->last_events = events; q->last_idle = idle; q->observed = ticks();
    if (events != 1) {
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU) | SUNSTONE_CRUMB_NPU_REGISTER | (4u << 16),
                            events, SUNSTONE_CRUMB_HERE());
        return 0;
    }
    const uint32_t full = a733_npu_idle_part.full_idle;
    q->idle_wait_ticks = 0; q->idle_busy_bits = 0;
    if ((idle & full) != full) {
        /* The event can come while units still show busy. VIPLite polls +0x004 after every completion until all units are
         * idle (vip_drv_task_common.c:1078 vipdrv_hw_idle, vip_drv_pm.c:814-826 vipdrv_hw_wait_idle with
         * EXPECT_IDLE_VALUE_ALL_MODULE). Poll for at most NPU_IDLE_WAIT_US and record the wait; a unit still busy then is a
         * fault, whose value names the busy units (bits 23:0 of the complement) and the slot (bits 31:24). */
        uint32_t now = idle, busy = ~idle & full;
        uint64_t bound = (uint64_t)NPU_IDLE_WAIT_US * q->frequency / 1000000u;
#ifdef SUNSTONE_NPU_OP_TIMING
        /* A cut job's footer event does not wait for the neural-network and shader units: run 1794509651684078706-d1eaad0b
         * saw them (idle bits 18 and 3, with the memory controller, 14) busy 100 us after the event of a subsampling prefix.
         * Wait for them up to the job's timeout, so that a prefix's time ends when the NPU is idle. */
        if (q->cut_at) bound = q->timeout_ticks;
#endif
        uint64_t waited = 0;
        while ((now & full) != full && (waited = ticks() - q->observed) < bound) {
            now = *(volatile uint32_t *)(base + a733_npu_idle_part.status);
            busy |= ~now & full;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        q->last_idle = now;
        if ((now & full) != full) {
            sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU) | SUNSTONE_CRUMB_NPU_REGISTER | (1u << 16),
                                (~now & 0x00ffffffu) | (uint32_t)slot_of(q) << 24, SUNSTONE_CRUMB_HERE());
            return 0;
        }
        q->idle_wait_ticks = waited ? waited : 1; q->idle_busy_bits = busy;
    }
#ifdef SUNSTONE_NPU_OP_TIMING
    if (!cut_restore(q)) return 0;
#endif
#ifdef SUNSTONE_NPU_TRAFFIC_COUNTERS
    if (q->traffic_armed) {
        for (unsigned k = 0; k < 4; ++k) q->traffic[k] = *(volatile uint32_t *)(base + traffic_reg[k]) - q->traffic_start[k];
        q->traffic_armed = 0; q->traffic_ready = 1;
    }
#endif
    if (q->observed - q->started >= q->timeout_ticks) {
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),
                            (uint32_t)(q->observed - q->started), SUNSTONE_CRUMB_HERE());
        return 0;
    }
    return 1;
}
static void notify(void *cookie)
{
    struct npu_runtime_queue *q = cookie;
    if (!q->progress_from_isr) vTaskNotifyGiveFromISR(q->task, q->woken);
}
/* A power-state fault never reaches the ordinary read-to-acknowledge path: the
 * interrupt is retained with its origin and nothing touches the device. */
static void irq_fault(struct npu_runtime_queue *q, BaseType_t *woken, unsigned site)
{
    npu_idle_retain(&q->power, site, 0, q->power.phase, NPU_IDLE_ON);
    atomic_store_explicit(&q->queue.failed, 1, memory_order_release);
    atomic_store_explicit(&q->runtime->service.signals->runtime_failed, 1, memory_order_release);
    atomic_fetch_or_explicit(&q->runtime->service.signals->irq_pending, 2u, memory_order_release);
    if (q->progress_from_isr) q->progress_from_isr(q->progress_cookie, woken);
    q->woken = NULL;
    vTaskNotifyGiveFromISR(q->task, woken);
    if (q->power_task) vTaskNotifyGiveFromISR(q->power_task, woken);
}
static void irq(void *cookie, BaseType_t *woken)
{
    struct npu_runtime_queue *q = cookie;
#ifdef SUNSTONE_NPU_QUEUE_TIMING
    uint64_t since = ticks();
#endif
    q->woken = woken;
    if (q->power.initialized && q->power.phase != NPU_IDLE_ON) {
        irq_fault(q, woken, NPU_IDLE_UNEXPECTED_IRQ); return;
    }
    if (q->power.initialized && !q->queue.head) {
        /* No job is active: an interrupt here follows a resume that left an
         * interrupt pending, or is spurious. */
        irq_fault(q, woken, q->power_resume_irq ? NPU_IDLE_RESUME_IRQ : NPU_IDLE_UNEXPECTED_IRQ); return;
    }
    npu_queue_irq(&q->queue);
    q->woken = NULL;
    if (atomic_load_explicit(&q->queue.failed, memory_order_acquire)) {
        /* The first IRQ's status/deadline inputs remain in the queue adapter;
         * no controller operation or release follows this retained failure. */
        atomic_store_explicit(&q->runtime->service.signals->runtime_failed, 1, memory_order_release);
        atomic_fetch_or_explicit(&q->runtime->service.signals->irq_pending, 2u, memory_order_release);
    }
    if (q->progress_from_isr) q->progress_from_isr(q->progress_cookie, woken);
    /* Completion refreshes the idle time and wakes no task; the owner's alarm
     * observes it. */
    if (q->power.initialized && !q->queue.head &&
        atomic_load_explicit(&q->queue.completed, memory_order_acquire) == q->queue.submitted &&
        !atomic_load_explicit(&q->queue.failed, memory_order_acquire))
        npu_idle_completed(&q->power, q->observed);
#ifdef SUNSTONE_NPU_QUEUE_TIMING
    q->irq_cpu_ticks += ticks() - since;
#endif
}
int npu_runtime_queue_claim(struct npu_runtime_queue *q, struct npu_runtime *rt)
{
    if (!q || q->claimed || !rt || !rt->bound || !rt->service.runtime ||
        atomic_load_explicit(&rt->service.signals->runtime_failed, memory_order_acquire) || !rt->service.irq_bound ||
        rt->service.runtime->hooks.idle_hint || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() != rt->slot[0].client || !rt->slot[0].opened ||
        rt->slot[0].closed || rt->slot[0].pending || rt->service.direct || rt->service.direct_request ||
        rt->service.controller.state != A733_NPU_READY ||
        rt->service.running != NPU_SERVICE_NONE || rt->service.has_deferred ||
        !a733_npu_startup_window_open(rt->startup) ||
        !npu_runtime_tables_intact(rt)) return 0;
    uint64_t el;
    __asm__ volatile("mrs %0,CurrentEL" : "=r"(el));
    uint64_t mpidr;
    __asm__ volatile("mrs %0,mpidr_el1" : "=r"(mpidr));
    unsigned core = (unsigned)(mpidr >> 8) & 0xff;
    if (el != 4 || (mpidr & UINT64_C(0xff00ff00ff)) || core >= 32 ||
        rt->service.affinity != (UINT32_C(1) << core)) return 0;
    for (unsigned i = 0; i < rt->slots; ++i)
        if (rt->slot[i].pending || atomic_load_explicit(&rt->slot[i].state, memory_order_acquire) != NPU_RUNTIME_READY)
            return 0;
    q->runtime = rt; q->task = xTaskGetCurrentTaskHandle();
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(q->frequency));
    if (!q->frequency || q->frequency > 1000000000) return 0;
    const struct npu_queue_ops ops = {validate, start, complete, notify};
    if (!npu_queue_init(&q->queue, &ops, q)) return 0;
    q->direct = (struct npu_service_direct){irq, q};
    const uint64_t saved = npu_runtime_queue_lock();
    if (atomic_load_explicit(&rt->service.signals->irq_pending, memory_order_acquire)) {
        npu_runtime_queue_unlock(saved); return 0;
    }
    rt->service.direct_owner = q->task;
    atomic_store_explicit(&rt->service.direct_ready, 0, memory_order_relaxed);
    rt->service.direct_request = &q->direct;
    npu_runtime_queue_unlock(saved);
    xTaskNotifyGive(rt->service.worker);
    unsigned ready;
    while (!(ready = atomic_load_explicit(&rt->service.direct_ready, memory_order_acquire)))
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (ready != 1) return 0;
    const uint64_t armed_saved = npu_runtime_queue_lock();
    if (rt->service.direct != &q->direct ||
        atomic_load_explicit(&rt->service.signals->runtime_failed, memory_order_acquire) ||
        atomic_load_explicit(&q->queue.failed, memory_order_acquire) ||
        !a733_npu_startup_window_open(rt->startup) ||
        atomic_load_explicit(&rt->service.signals->irq_pending, memory_order_acquire)) {
        atomic_store_explicit(&q->queue.failed, 1, memory_order_release);
        atomic_store_explicit(&rt->service.signals->runtime_failed, 1, memory_order_release);
        npu_runtime_queue_unlock(armed_saved); return -1;
    }
    const int armed = rt->service.irq_ops.rearm(rt->service.irq_cookie);
    if (armed == 1) { q->claimed = 1; rt->service.irq_masked = 0; }
    else {
        atomic_store_explicit(&q->queue.failed, 1, memory_order_release);
        atomic_store_explicit(&rt->service.signals->runtime_failed, 1, memory_order_release);
    }
    npu_runtime_queue_unlock(armed_saved);
    return armed == 1 ? 1 : -1;
}
int npu_runtime_queue_wait(struct npu_runtime_queue *q, struct npu_queue_fence fence)
{
    if (!owner(q)) return -1;
    for (;;) {
        int result = npu_queue_fence_status(&q->queue, fence);
        if (result) return result;
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
static int power_health(void *cookie)
{
    struct npu_runtime_queue *q = cookie;
    return window(q) && !q->queue.retired &&
        !atomic_load_explicit(&q->queue.failed, memory_order_acquire) &&
        !atomic_load_explicit(&q->runtime->service.signals->irq_pending, memory_order_acquire);
}
/* Only the three registers of the part descriptor are reachable. */
static int power_read(void *cookie, uint32_t offset, uint32_t *value)
{
    struct npu_runtime_queue *q = cookie;
    const struct npu_idle_part *part = q->power.part;
    if (offset != part->clock && offset != part->status && offset != part->shader) return 0;
    if (!power_health(q)) return 0;
    *value = *(volatile uint32_t *)(q->runtime->native.registers + offset);
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int power_write(void *cookie, uint32_t offset, uint32_t value)
{
    struct npu_runtime_queue *q = cookie;
    const struct npu_idle_part *part = q->power.part;
    if ((offset != part->clock && offset != part->shader) || !power_health(q)) return 0;
    a733_mmio_write32_dsb_sy_after(q->runtime->native.registers + offset, value);
    return 1;
}
static uint64_t power_now(void *cookie) { (void)cookie; return ticks(); }
static int power_idle(struct npu_runtime_queue *q)
{
    return !q->queue.head &&
        atomic_load_explicit(&q->queue.completed, memory_order_acquire) == q->queue.submitted &&
        (q->last_idle & q->power.part->full_idle) == q->power.part->full_idle;
}
static uint64_t ticks_to_us(struct npu_runtime_queue *q, uint64_t value)
{
    return value / q->frequency * UINT64_C(1000000) +
        (value % q->frequency * UINT64_C(1000000) + q->frequency - 1) / q->frequency;
}
static void power_failed(struct npu_runtime_queue *q)
{
    atomic_store_explicit(&q->queue.failed, 1, memory_order_release);
    atomic_store_explicit(&q->runtime->service.signals->runtime_failed, 1, memory_order_release);
    atomic_fetch_or_explicit(&q->runtime->service.signals->irq_pending, 2u, memory_order_release);
    sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),
        q->power.failure.site | q->power.failure.phase << 16, SUNSTONE_CRUMB_HERE());
}
int npu_runtime_queue_admit(struct npu_runtime_queue *q)
{
    if (!q || !q->claimed || atomic_load_explicit(&q->queue.failed, memory_order_acquire) ||
        q->power_stop || (q->power.initialized && q->power.phase == NPU_IDLE_RETAINED)) return -1;
    if (!q->power.initialized || q->power.phase == NPU_IDLE_ON) return 1;
    if (!q->power_resume) {
        q->power_resume = 1; q->power_requested_at = ticks();
        ++q->power_stats.resume_requests;
    }
    return 0;
}
void npu_runtime_queue_power_kick(struct npu_runtime_queue *q)
{
    if (q && q->power_task && q->power_resume) xTaskNotifyGive(q->power_task);
}
int npu_runtime_queue_wait_ready(struct npu_runtime_queue *q)
{
    if (!owner(q)) return -1;
    for (;;) {
        const uint64_t saved = npu_runtime_queue_lock();
        const int ready = npu_runtime_queue_admit(q);
        npu_runtime_queue_unlock(saved);
        if (ready) return ready;
        npu_runtime_queue_power_kick(q);
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
int npu_runtime_queue_submit(struct npu_runtime_queue *q, struct npu_queue_job *job,
                             struct npu_queue_command *command, struct npu_queue_fence after,
                             uint64_t timeout, struct npu_queue_fence *out)
{
    if (!owner(q)) return -1;
    const uint64_t saved = npu_runtime_queue_lock();
    int result = npu_runtime_queue_admit(q);
    if (result == 1) {
        result = npu_queue_submit(&q->queue, job, command, after, timeout, out);
        if (result == 0) result = -1;
    }
    npu_runtime_queue_unlock(saved);
    return result;
}
/* A busy device, an idle device before its delay and a transiently non-quiet
 * interrupt controller each leave the owner an alarm; none polls the device. */
#define QUIET_RETRY_US UINT64_C(1000)
static void power_owner(void *argument)
{
    struct npu_runtime_queue *q = argument;
    q->power_task = xTaskGetCurrentTaskHandle();
    const int registered = sunstone_deadline_register();
    if (registered != 1) {
        npu_idle_retain(&q->power, NPU_IDLE_OWNER, 0, (uint32_t)registered, 1);
        power_failed(q);
    }
    atomic_store_explicit(&q->power_started, registered == 1 ? 1u : 2u, memory_order_release);
    xTaskNotifyGive(q->task);
    const uint64_t delay_us = q->power.part->delay_us;
    for (;;) {
        const uint64_t began = ticks(), saved = npu_runtime_queue_lock();
        ++q->power.counters.owner_wakes;
        unsigned notify_ready = 0, stopped = 0;
        uint64_t deadline = 0;
        if (q->power.phase != NPU_IDLE_RETAINED && !power_health(q))
            npu_idle_retain(&q->power, NPU_IDLE_HEALTH, 0, 0, 1);
        if (q->power.phase != NPU_IDLE_RETAINED && (q->power_stop || q->power_resume)) {
            const uint64_t resumed = q->power.counters.resumes;
            const int ready = npu_idle_resume(&q->power);
            if (ready == 1 && q->power.counters.resumes != resumed) {
                /* An interrupt pending after the ungate is attributed to the
                 * resume; an invalid binding is a fault of its own. */
                const int after = q->power_config.quiet(q->runtime->service.irq_cookie);
                q->power_resume_irq = after != 1;
                if (after < 0) npu_idle_retain(&q->power, NPU_IDLE_RESUME_IRQ, 0, (uint32_t)after, 1);
            }
            if (ready == 1 && q->power.phase == NPU_IDLE_ON) {
                if (q->power_resume) {
                    const uint64_t us = ticks_to_us(q, ticks() - q->power_requested_at);
                    q->power_stats.last_request_to_ready_us = us;
                    if (us > q->power_stats.max_request_to_ready_us)
                        q->power_stats.max_request_to_ready_us = us;
                    q->power_resume = 0; notify_ready = 1;
                }
                if (q->power_stop) stopped = 1;
            }
        }
        if (q->power.phase == NPU_IDLE_RETAINED) {
            power_failed(q); notify_ready = 1;
            if (q->power_stop) stopped = 1;
        } else if (!stopped && q->power.phase == NPU_IDLE_ON) {
            const uint64_t now = ticks();
            if (!power_idle(q)) {
                deadline = ticks_to_us(q, now) + delay_us;
            } else if (!npu_idle_eligible(&q->power, now)) {
                deadline = ticks_to_us(q, npu_idle_deadline_ticks(&q->power));
            } else {
                /* The first suspend write follows this decision in the same
                 * raw IRQ lock. No RTOS wrapper can unmask it. */
                const int quiet = q->power_config.quiet(q->runtime->service.irq_cookie);
                if (quiet == 1) {
                    (void)npu_idle_gate(&q->power);
                    if (!q->power.enabled) deadline = ticks_to_us(q, now) + delay_us;
                } else if (quiet == 0) {
                    ++q->power_stats.quiet_retries;
                    deadline = ticks_to_us(q, now) + QUIET_RETRY_US;
                } else {
                    npu_idle_retain(&q->power, NPU_IDLE_OWNER, 0, (uint32_t)quiet, 1);
                }
            }
        }
        if (q->power.phase == NPU_IDLE_RETAINED) { power_failed(q); notify_ready = 1; deadline = 0; }
        npu_runtime_queue_unlock(saved);
        if (notify_ready) xTaskNotifyGive(q->task);
        q->power.counters.owner_cpu_ticks += ticks() - began;
        if (stopped) {
            /* Any wait_until returned and canceled its alarm before this ack.
             * The registered static owner is parked permanently, never deleted. */
            atomic_store_explicit(&q->power_stopped, 1, memory_order_release);
            xTaskNotifyGive(q->task);
            for (;;) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        if (deadline) {
            if (sunstone_deadline_wait_until(deadline) == SUNSTONE_DEADLINE_ERROR) {
                const uint64_t failed_saved = npu_runtime_queue_lock();
                npu_idle_retain(&q->power, NPU_IDLE_OWNER, 0, 0, 1);
                npu_runtime_queue_unlock(failed_saved);
            }
        } else (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
int npu_runtime_queue_power_start(struct npu_runtime_queue *q,
                                 const struct npu_runtime_queue_power_config *config)
{
    /* The owner must outrank the producer on the route core; the caller
     * guarantees that, because the kernel build does not query priorities. */
    if (!owner(q) || !config || !config->part || config->enabled > 1 || config->strict > 1 ||
        !config->quiet || !config->priority || config->priority >= configMAX_PRIORITIES ||
        q->power.initialized || q->power_task || q->queue.head ||
        q->queue.submitted != atomic_load_explicit(&q->queue.completed, memory_order_acquire) ||
        !power_health(q)) return 0;
    q->power_config = *config;
    const struct npu_idle_ops ops = {power_read, power_write, power_now, power_health};
    if (npu_idle_init(&q->power, config->part, &ops, q, q->frequency, config->enabled, config->strict) != 1) return -1;
    const uint64_t saved = npu_runtime_queue_lock();
    const uint32_t full_idle = config->part->full_idle;
    if (!power_read(q, config->part->status, &q->last_idle) || (q->last_idle & full_idle) != full_idle ||
        config->quiet(q->runtime->service.irq_cookie) != 1) {
        npu_idle_retain(&q->power, NPU_IDLE_FULL_IDLE, config->part->status, q->last_idle, full_idle);
        power_failed(q); npu_runtime_queue_unlock(saved); return -1;
    }
    npu_runtime_queue_unlock(saved);
    creating_power = q;
#if configNUMBER_OF_CORES > 1
    q->power_task = xTaskCreateStaticAffinitySet(power_owner, "npu-power", 1024, q,
        config->priority, q->power_stack, &q->power_task_state, q->runtime->service.affinity);
#else
    q->power_task = xTaskCreateStatic(power_owner, "npu-power", 1024, q,
        config->priority, q->power_stack, &q->power_task_state);
#endif
    creating_power = NULL;
    if (!q->power_task) {
        npu_idle_retain(&q->power, NPU_IDLE_OWNER, 0, 0, 1); power_failed(q); return -1;
    }
    unsigned started;
    while (!(started = atomic_load_explicit(&q->power_started, memory_order_acquire)))
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return started == 1 ? 1 : -1;
}
int npu_runtime_queue_return(struct npu_runtime_queue *q)
{
    if (!owner(q) || !npu_queue_idle(&q->queue)) return -1;
    if (q->power.initialized) {
        const uint64_t stop_saved = npu_runtime_queue_lock();
        q->power_stop = 1;
        npu_runtime_queue_unlock(stop_saved);
        xTaskNotifyGive(q->power_task);
        while (!atomic_load_explicit(&q->power_stopped, memory_order_acquire))
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (q->power.phase != NPU_IDLE_ON) return -1;
    }
    struct npu_service *s = &q->runtime->service;
    /* The provider may enter the RTOS critical section. In the single-core
     * SRE port that can clear DAIF on exit, so invoke it before our raw lock.
     * A late IRQ during masking is retained and caught by the locked recheck. */
    int result = s->irq_ops.mask(s->irq_cookie);
    const uint64_t saved = npu_runtime_queue_lock();
    const uint64_t count = atomic_load_explicit(&q->queue.completed, memory_order_acquire);
    if (!npu_queue_idle(&q->queue) || atomic_load_explicit(&q->runtime->service.signals->runtime_failed, memory_order_acquire) ||
        atomic_load_explicit(&s->signals->irq_pending, memory_order_acquire) ||
        count > UINT32_MAX - npu_service_irq_completions(s) ||
        q->queue.submitted > UINT64_MAX - q->runtime->counters.jobs) result = -1;
    if (result == 1 && !npu_queue_retire(&q->queue)) result = -1;
    if (result == 1) {
        s->irq_masked = 1;
        atomic_fetch_add_explicit(&s->irq_completions, (unsigned)count, memory_order_release);
        q->runtime->counters.jobs += q->queue.submitted;
        s->direct = NULL; s->direct_owner = NULL;
        atomic_store_explicit(&s->direct_ready, 0, memory_order_release);
        q->claimed = 0;
    } else if (result < 0) {
        atomic_store_explicit(&q->queue.failed, 1, memory_order_release);
        atomic_store_explicit(&q->runtime->service.signals->runtime_failed, 1, memory_order_release);
    }
    npu_runtime_queue_unlock(saved);
    if (result == 1) xTaskNotifyGive(s->worker);
    return result;
}
