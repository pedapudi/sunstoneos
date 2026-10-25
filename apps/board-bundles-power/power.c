/* SPDX-License-Identifier: Apache-2.0 */
/* PMIC provider, power-domain owner and service task of bundles 6 and 7;
 * see power.h. */
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "a733_power_authority.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "a733_task_context.h"

struct bb_power bb_power;
struct bb_cost_next bb_power_next;
static atomic_uint poll_armed;
static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];

static int clock_now(void *unused, uint64_t *out)
{ (void)unused; return a733_counter_clock_now(&bb_power.counter, out); }
static int pin_enter(void *unused, uint64_t end)
{ (void)unused; return a733_platform_lock_enter(&bb_power.locks, A733_PLATFORM_REGISTER_LOCK, end); }
static void pin_leave(void *unused)
{ (void)unused; a733_platform_lock_leave(&bb_power.locks, A733_PLATFORM_REGISTER_LOCK); }
static uint64_t cntpct(void)
{ return counter(); }

/* The provider's callbacks are the native adapter's, with the TWI observer
 * on every completed S_TWI0 access; the cookie is the adapter itself. Each
 * write is timed in place. Once armed, the first byte-phase CNTR write is
 * followed by back-to-back CNTR reads until INT_FLAG sets: the wire time of
 * one byte. */
static int pmic_read(void *c, uintptr_t a, uint32_t *v)
{
    const int r = bb_power.native_pmic_ops.read(c, a, v);
    if (r == 1)
        bb_twi_observe(&bb_power.twi, 0, a, *v);
    return r;
}
/* The poll's task check: the first callback of every a733_pmic_poll(). The
 * stamp starts the poll's span; one counter read per poll. */
static int pmic_current(void *c)
{
    struct bb_power *p = &bb_power;
    p->poll_at = counter();
    p->poll_events = p->twi.conditions + p->twi.bytes + p->twi.stops;
    p->poll_stops = p->twi.stops;
    p->poll_open = 1;
    p->poll_data = 0;
    return p->native_pmic_ops.current(c);
}
static int pmic_write(void *c, uintptr_t a, uint32_t v)
{
    struct bb_power *p = &bb_power;
    /* The wire-time spin runs with IRQs masked from before the write, so no
     * interrupt lands between the write and the reads that bracket INT_FLAG. */
    const unsigned spin = a == BB_TWI_CNTR && v == 0x48 &&
        atomic_exchange_explicit(&p->wire_armed, 0, memory_order_acquire);
    uint64_t daif = 0;
    if (spin)
        __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(daif) :: "memory");
    const uint64_t before = counter();
    const int r = p->native_pmic_ops.write(c, a, v);
    const uint64_t after = counter();
    if (r == 1 && spin) {
        struct bb_status_trace t;
        bb_status_spin(p->native_pmic_ops.read, c, cntpct, BB_TWI_CNTR, 8, 8, after, BB_POWER_STATUS_BOUND, &t);
        p->cost.wire_seen = t.on;
        p->cost.wire_clear = t.on ? t.on_before : 0;
        p->cost.wire_set = t.on ? t.on_at : 0;
        p->cost.wire_reads = t.reads;
        p->wire_in_step = 1;
    }
    if (spin)
        __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    if (r != 1)
        return r;
    const uint32_t ticks = (uint32_t)(after - before);
    ++p->cost.writes;
    p->cost.write_ticks += ticks;
    if (ticks < p->cost.write_min) p->cost.write_min = ticks;
    if (ticks > p->cost.write_max) p->cost.write_max = ticks;
    bb_twi_observe(&p->twi, 1, a, v);
    /* A poll that completed a bus phase ends its span at its first
     * phase-start CNTR write; the wire-time spin's write is left out. A byte
     * phase writes DATA before CNTR; a receive writes CNTR alone. */
    if (a == BB_TWI_DATA)
        p->poll_data = 1;
    if (a == BB_TWI_CNTR && p->poll_open) {
        p->poll_open = 0;
        if (p->in_step && !spin && p->twi.conditions + p->twi.bytes + p->twi.stops != p->poll_events)
            bb_span_add(&p->step_complete[v == 0x48 ? (p->poll_data ? BB_COMPLETE_BYTE : BB_COMPLETE_RECEIVE) :
                v == 0x58 ? BB_COMPLETE_STOP : p->twi.stops != p->poll_stops ? BB_COMPLETE_START :
                BB_COMPLETE_RESTART], (uint32_t)(after - p->poll_at));
    }
    return r;
}
/* Caller buffers must also stay clear of the records this layer writes. */
static int pmic_outside(void *c, const void *p, size_t n)
{
    return bb_power.native_pmic_ops.outside(c, p, n) == 1 && a733_ranges_disjoint(p, n, &bb_power.twi,
        (size_t)((const char *)&bb_power.stop - (const char *)&bb_power.twi));
}

/* The owner's callbacks: the native adapter's, with each completed access
 * stored with its CNTPCT stamp. */
#define DOMAIN_OPS bb_power.domains_native.operations
#define DOMAIN_NATIVE (&bb_power.domains_native)
static void record(uintptr_t a, uint32_t v, unsigned write)
{
    if (bb_power.accessed < BB_POWER_ACCESSES)
        bb_power.accesses[bb_power.accessed] = (struct bb_power_access){(uint32_t)a, v, write, counter()};
    ++bb_power.accessed;
}
static int domain_read(void *c, uintptr_t a, uint32_t *v)
{
    (void)c;
    const int r = DOMAIN_OPS.read(DOMAIN_NATIVE, a, v);
    if (r == 1)
        record(a, *v, 0);
    return r;
}
static int domain_write(void *c, uintptr_t a, uint32_t v)
{
    (void)c;
    const int r = DOMAIN_OPS.write(DOMAIN_NATIVE, a, v);
    if (r != 1)
        return r;
    record(a, v, 1);
    ++bb_power.writes;
    /* The write accessor ends with DSB SY, so the stamps count from the
     * command reaching the PPU. */
    if (bb_power.spin_after_on && a == A733_POWER_PCK_DOMAIN(1) && (v & 15) == 8)
        bb_status_spin(DOMAIN_OPS.read, DOMAIN_NATIVE, cntpct, A733_POWER_PCK_DOMAIN(1) + 8, 15, 8, counter(),
                       BB_POWER_STATUS_BOUND, &bb_power.status);
    return r;
}
static void domain_notify(void *c) { (void)c; DOMAIN_OPS.notify(DOMAIN_NATIVE); }

/* Bundle 6's access-cost windows, on the service task with the provider
 * idle and the I2C mutex entered through the adapter, so that the adapter's
 * read operation runs as the provider runs it. CNTR reads have no side
 * effect. */
/* One cost window: BB_POWER_COST_READS iterations of body in
 * BB_COST_SUBWINDOWS equal sub-windows, each timed by CNTPCT after `end`
 * (a DSB SY for the windows whose loads may still be outstanding). */
#define WINDOW(w, body, end)                                                    \
    do {                                                                        \
        (w).sub_min = UINT32_MAX;                                               \
        (w).sub_max = 0;                                                        \
        (w).ticks = 0;                                                          \
        for (unsigned s_ = 0; s_ < BB_COST_SUBWINDOWS; ++s_) {                  \
            const uint64_t t0_ = counter();                                     \
            for (unsigned i = 0; i < BB_POWER_COST_READS / BB_COST_SUBWINDOWS; ++i) \
                body;                                                           \
            end;                                                                \
            const uint32_t d_ = (uint32_t)(counter() - t0_);                    \
            (w).ticks += d_;                                                    \
            if (d_ < (w).sub_min) (w).sub_min = d_;                             \
            if (d_ > (w).sub_max) (w).sub_max = d_;                             \
        }                                                                       \
    } while (0)
_Static_assert(BB_POWER_COST_READS % BB_COST_SUBWINDOWS == 0, "whole sub-windows");

static void measure(struct bb_power *p)
{
    struct bb_cost *c = &p->cost;
    void *n = &p->pmic_native;
    const uintptr_t cntr = BB_TWI_CNTR;
    uint64_t daif, mpidr, now;
    uint32_t v = 0;
    unsigned failed = 0;
    if (p->native_pmic_ops.enter(n, p->deadline) != 1)
        bb_power_fail("board bundle 6: cost windows could not enter the I2C mutex");
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(daif) :: "memory");
    WINDOW(c->dsb, (void)a733_mmio_read32_dsb_sy_both(cntr), (void)0);
    WINDOW(c->plain, (void)a733_mmio_read32(cntr), a733_mmio_dsb_sy());
    uintptr_t address = cntr;
    WINDOW(c->dependent, ({
        uint64_t zero;
        v = a733_mmio_read32(address);
        __asm__ volatile("eor %0, %1, %1" : "=r"(zero) : "r"((uint64_t)v));
        address = cntr + zero;
    }), a733_mmio_dsb_sy());
    WINDOW(c->time_read, failed |= p->native_pmic_ops.now(n, &now) != 1, (void)0);
    WINDOW(c->adapter_read, failed |= p->native_pmic_ops.read(n, cntr, &v) != 1, (void)0);
    /* As a flag poll after a phase-start write that reads the flag clear. */
    struct bb_twi scratch = {.mode = 1};
    WINDOW(c->observer, bb_twi_observe(&scratch, 0, cntr, v & ~UINT32_C(8)), (void)0);
    struct bb_cost_next *x = &bb_power_next;
    /* The counter overhead of a window whose sub-windows end in DSB SY, as
     * the write window's do. */
    WINDOW(x->empty, __asm__ volatile("" ::: "memory"), a733_mmio_dsb_sy());
    /* Plain writes of XADDR's own value, admitted by the idle reads of
     * trace.h; the barrier after each sub-window waits for the writes. */
    x->addr = a733_mmio_read32_dsb_sy_both(BB_TWI_ADDR);
    x->xaddr = a733_mmio_read32_dsb_sy_both(BB_TWI_XADDR);
    x->cntr = a733_mmio_read32_dsb_sy_both(BB_TWI_CNTR);
    x->stat = a733_mmio_read32_dsb_sy_both(BB_TWI_STAT);
    x->lcr = a733_mmio_read32_dsb_sy_both(BB_TWI_LCR);
    x->write_run = bb_xaddr_admitted(x->addr, x->cntr, x->stat, x->lcr);
    if (x->write_run) {
        const uint32_t saved = x->xaddr;
        WINDOW(x->xaddr_write, a733_mmio_write32(BB_TWI_XADDR, saved), a733_mmio_dsb_sy());
        x->xaddr_after = a733_mmio_read32_dsb_sy_both(BB_TWI_XADDR);
        x->cntr_after = a733_mmio_read32_dsb_sy_both(BB_TWI_CNTR);
        x->stat_after = a733_mmio_read32_dsb_sy_both(BB_TWI_STAT);
        x->lcr_after = a733_mmio_read32_dsb_sy_both(BB_TWI_LCR);
        /* Every register read before the window must read the same after it. */
        x->write_changed = x->xaddr_after != saved || a733_mmio_read32_dsb_sy_both(BB_TWI_ADDR) != x->addr ||
            x->cntr_after != x->cntr || x->stat_after != x->stat || x->lcr_after != x->lcr;
    }
    /* The service loop's work between two polls (drivers/power/a733_power_domains.c,
     * pmic()): the work query, the time read and the work query with the
     * spin time. */
    uint64_t loop_now = 0, loop_last = 0, until = 0;
    unsigned work = 0;
    WINDOW(x->loop, ({
        work |= a733_pmic_work(&p->pmic, 0, NULL);
        failed |= a733_pmic_now(&p->pmic, &loop_now) != 1 || !a733_monotonic_advance(loop_now, &loop_last);
        work |= a733_pmic_work(&p->pmic, loop_now, &until);
    }), (void)0);
    /* The overhead of one stamped poll sample. */
    bb_samples_start(&x->stamp);
    for (unsigned i = 0; i < BB_COST_SAMPLES; ++i) {
        const uint64_t t0 = counter();
        const uint64_t t1 = counter();
        (void)bb_samples_add(&x->stamp, (uint32_t)(t1 - t0));
    }
    x->loop_work = work;
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    const TickType_t first = xTaskGetTickCount();
    WINDOW(c->task_check, failed |= a733_task_context_owner_el1(p->service) != 1, (void)0);
    const TickType_t last = xTaskGetTickCount();
    p->native_pmic_ops.leave(n);
    if (failed)
        bb_power_fail("board bundle 6: cost window operation refused");
    c->reads = BB_POWER_COST_READS;
    c->core = (uint32_t)((mpidr >> 8) & 0xff);
    c->task_ticks = (uint32_t)(last - first);
}

/* Bundle 6's poll window: one queued snapshot polled back to back, without
 * the service's spin, by the provider task. Each a733_pmic_poll() is stamped;
 * one that read only CNTR, wrote nothing and completed no phase, in a flag
 * phase, and returned PENDING with the transfer waiting, is a whole poll
 * with the flag clear. A sample with a scheduler tick inside is left out and
 * counted. The transfer's phases complete outside a service step, so the
 * snapshot leaves the step records; the observer still counts its STOPs. */
static void poll_window(struct bb_power *p)
{
    struct bb_cost_next *x = &bb_power_next;
    if (x->poll_snapshots++ == 0)
        bb_samples_start(&x->poll);
    for (unsigned n = 0; n < BB_POLL_WINDOW_POLLS; ++n) {
        const struct bb_twi before = p->twi;
        const uint32_t writes = p->cost.writes;
        const TickType_t first = xTaskGetTickCount();
        const uint64_t t0 = counter();
        const enum a733_pmic_status r = a733_pmic_poll(&p->pmic);
        const uint64_t t1 = counter();
        const TickType_t last = xTaskGetTickCount();
        ++x->polls;
        const struct bb_twi *t = &p->twi;
        if (r == A733_PMIC_PENDING && p->pmic.waiting && t->mode == 1 && t->reads == before.reads + 1 &&
            p->cost.writes == writes && t->conditions == before.conditions && t->bytes == before.bytes &&
            t->stops == before.stops) {
            if (last != first)
                ++x->poll_ticked;
            else
                (void)bb_samples_add(&x->poll, (uint32_t)(t1 - t0));
        }
        if (r != A733_PMIC_PENDING && r != A733_PMIC_BUSY)
            return;
        if (!(a733_pmic_work(&p->pmic, 0, NULL) & (A733_PMIC_WORK_QUEUED | A733_PMIC_WORK_ACTIVE)))
            return;
    }
}

void bb_power_arm_poll_window(void)
{ atomic_store_explicit(&poll_armed, 1, memory_order_release); }

/* The one service task: each step that completes a bus phase is recorded
 * with its CNTPCT span and the scheduler ticks inside it, except a step that
 * held the wire-time spin. */
static void service(void *unused)
{
    (void)unused;
    struct bb_power *p = &bb_power;
    for (;;) {
        /* The wire-time spin comes first: it is armed before the first
         * snapshot and must not fall inside a poll window. */
        if (atomic_load_explicit(&poll_armed, memory_order_acquire) &&
            !atomic_load_explicit(&p->wire_armed, memory_order_acquire) &&
            (a733_pmic_work(&p->pmic, 0, NULL) & A733_PMIC_WORK_QUEUED)) {
            poll_window(p);
            if (bb_power_next.poll.count >= BB_COST_SAMPLES || bb_power_next.poll_snapshots >= BB_POLL_WINDOW_SNAPSHOTS)
                atomic_store_explicit(&poll_armed, 0, memory_order_relaxed);
        }
        const struct bb_twi before = p->twi;
        p->twi.polls_max = 0;
        const TickType_t first = xTaskGetTickCount();
        for (unsigned k = 0; k < BB_COMPLETE_KINDS; ++k)
            p->step_complete[k] = (struct bb_span){0};
        p->in_step = 1;
        const uint64_t start = counter();
        const uint32_t ticks = a733_power_service_step(&p->domains);
        const uint64_t end = counter();
        p->in_step = 0;
        const TickType_t last = xTaskGetTickCount();
        const struct bb_twi *t = &p->twi;
        if (p->wire_in_step) {
            p->wire_in_step = 0;
            p->wire_excluded = 1;
        } else if (t->conditions != before.conditions || t->bytes != before.bytes || t->stops != before.stops) {
            for (unsigned k = 0; k < BB_COMPLETE_KINDS; ++k)
                bb_span_merge(&bb_power_next.complete[k], &p->step_complete[k]);
            if (p->stepped < BB_POWER_STEPS)
                p->steps[p->stepped++] = (struct bb_step){end - start, last - first,
                    (uint16_t)(t->conditions - before.conditions), (uint16_t)(t->bytes - before.bytes),
                    (uint16_t)(t->stops - before.stops), (uint16_t)(t->first_set - before.first_set),
                    (uint16_t)(t->first_clear - before.first_clear), (uint16_t)t->polls_max,
                    (uint16_t)(t->respins - before.respins)};
            else
                ++p->steps_dropped;
        }
        if (atomic_load_explicit(&p->stop, memory_order_acquire) && a733_power_service_idle(&p->domains)) {
            if (p->cost_at_park)
                measure(p);
            atomic_store_explicit(&p->parked, 1, memory_order_release);
            for (;;)
                vTaskSuspend(NULL);
        }
        (void)ulTaskNotifyTake(pdTRUE, ticks == UINT32_MAX ? portMAX_DELAY : (TickType_t)ticks);
    }
}

static int power_init(TaskFunction_t client, const char *name, unsigned domain_row)
{
    struct bb_power *p = &bb_power;
    p->deadline = RESULT->start_counter / 24 + BB_POWER_SESSION_US;
    atomic_init(&p->stop, 0);
    atomic_init(&p->parked, 0);
    atomic_init(&p->wire_armed, 0);
    atomic_init(&poll_armed, 0);
    p->cost.write_min = UINT32_MAX;
    if (!a733_counter_clock_init(&p->counter, 24000000) ||
        !a733_platform_locks_init(&p->locks, a733_counter_clock_now, &p->counter) ||
        !a733_clock_leases_init(&p->clocks, a733_counter_clock_now, &p->counter))
        return 0;
    const struct a733_pins_locks pin_locks = {pin_enter, pin_leave, clock_now, NULL};
    p->handoff.authorized[A733_PIN_L] = 3;
    if (!a733_pins_native_init(&p->pin_native, &pin_locks) ||
        !a733_pins_init_banks(&p->pins, &p->pin_native.operations, &p->pin_native, &p->handoff) ||
        !a733_gpu_resources_native_init(&p->gpu_native, &p->locks.callbacks))
        return 0;
    p->gpu_native.operations.now = clock_now;
    if (!a733_gpu_resources_init(&p->gpu, &p->gpu_native.operations, &p->gpu_native))
        return 0;
    const struct a733_rail_binding binding = {&p->clocks, &p->gpu, 1, a733_counter_clock_now, &p->counter};
    if (!a733_rail_leases_init(&p->rails, &binding))
        return 0;
    p->service = xTaskCreateStatic(service, "power-service", 4096, NULL, 2, stacks[0], &tasks[0]);
    p->client = xTaskCreateStatic(client, name, 4096, NULL, 1, stacks[1], &tasks[1]);
    if (!p->service || !p->client || !a733_pmic_native_init(&p->pmic_native, &p->locks, p->service))
        return 0;
    p->native_pmic_ops = p->pmic_native.pmic_bus_ops;
    struct a733_pmic_ops ops = p->native_pmic_ops;
    ops.current = pmic_current;
    ops.read = pmic_read;
    ops.write = pmic_write;
    ops.outside = pmic_outside;
    const struct a733_pmic_config config = {.owner = BB_POWER_PMIC_OWNER, .identity = 1,
        .startup_deadline = p->deadline, .pins = {{2, 1, 1}, {2, 1, 1}}};
    if (!a733_pmic_init(&p->pmic, &config, &ops, &p->pmic_native, &p->clocks, &p->rails, &p->pins) ||
        !a733_gpu_resources_native_use_pmic(&p->gpu_native, &p->gpu, &p->pmic))
        return 0;
    static const struct a733_power_domain_ops domain_ops = {domain_read, domain_write, domain_notify};
    const struct a733_power_service_config service_config = {BB_POWER_PMIC_OWNER, p->deadline,
                                                             1000000u / configTICK_RATE_HZ, 0};
    if (!a733_power_domains_native_init(&p->domains_native, p->service) ||
        !a733_power_domains_init(&p->domains, &p->pmic, &domain_ops, p, &service_config))
        return 0;
    /* The row makes the service verify DCDC2 and DCDC4 once; bundle 6 never
     * requests it. */
    return !domain_row || a733_power_domain_row_init(&p->row, &p->domains, 1, BB_POWER_CLIENT_OWNER, &p->token);
}

int bb_power_init(TaskFunction_t client, const char *name)
{ return power_init(client, name, 1); }

int bb_power_snapshot_init(TaskFunction_t client, const char *name)
{ return power_init(client, name, 0); }

uint64_t bb_power_now(void)
{
    uint64_t now;
    if (a733_counter_clock_now(&bb_power.counter, &now) != 1)
        bb_power_fail("PMIC clock");
    return now;
}
void bb_power_pause(const char *what)
{
    if (bb_power_now() >= bb_power.deadline)
        bb_power_fail(what);
    vTaskDelay(1);
}

void bb_power_wait_ready(void)
{
    for (;;) {
        const int ready = a733_power_service_ready(&bb_power.domains);
        if (ready < 0)
            bb_power_fail("provider fault");
        if (ready > 0 && (a733_power_authority_load(&bb_power.rails.authority) & A733_POWER_SUPPLY_DONE))
            return;
        bb_power_pause("provider startup and supply verification deadline");
    }
}

void bb_power_acquire(uint64_t rails)
{
    enum a733_rail_result r;
    while ((r = a733_rail_acquire(&bb_power.rails, BB_POWER_CLIENT_OWNER, rails, 0, bb_power.deadline,
                                  &bb_power.token)) == A733_RAIL_BUSY)
        bb_power_pause("rail acquisition deadline");
    if (r != A733_RAIL_PENDING && r != A733_RAIL_READY)
        bb_power_fail("rail acquisition");
    while ((r = a733_rail_poll(&bb_power.rails, &bb_power.token)) != A733_RAIL_READY) {
        if (r != A733_RAIL_PENDING && r != A733_RAIL_BUSY)
            bb_power_fail("rail poll");
        bb_power_pause("rail poll deadline");
    }
}

void bb_power_park(void)
{
    atomic_store_explicit(&bb_power.stop, 1, memory_order_release);
    xTaskNotifyGive(bb_power.service);
    while (!atomic_load_explicit(&bb_power.parked, memory_order_acquire))
        bb_power_pause("service park deadline");
    const struct a733_pmic *pmic = &bb_power.pmic;
    if (pmic->held || pmic->retained || pmic->failed || pmic->active != A733_PMIC_REQUESTS ||
        xSemaphoreGetMutexHolder(bb_power.locks.mutex[0]) || xSemaphoreGetMutexHolder(bb_power.locks.mutex[1]))
        bb_power_fail("provider not quiescent at park");
}

int bb_power_read(uintptr_t address, uint32_t *value)
{ return domain_read(NULL, address, value) == 1; }

_Noreturn void bb_power_fail(const char *text)
{
    platform_puts("FAIL ");
    platform_puts(text);
    platform_puts("\n");
    struct bb_line l;
    bb_line_start(&l, "PMIC");
    bb_line_dec(&l, "stage", bb_power.pmic.stage);
    bb_line_dec(&l, "held", bb_power.pmic.held);
    bb_line_dec(&l, "retained", bb_power.pmic.retained);
    bb_line_dec(&l, "failed", bb_power.pmic.failed);
    bb_line_hex(&l, "state_word", a733_power_authority_load(&bb_power.rails.authority));
    bb_line_end(&l);
    platform_puts(l.text);
    platform_exit(1);
}

static unsigned log_free(void)
{
    const uint64_t used = RESULT->log_length, room = RESULT_SIZE - LOG_OFFSET - 1;
    return used < room ? (unsigned)(room - used) : 0;
}
void bb_power_print(const struct bb_line *l)
{ platform_puts(l->text); }
void bb_power_detail(const struct bb_line *l)
{
    if (log_free() >= l->n + BB_POWER_RESERVE)
        platform_puts(l->text);
    else
        ++bb_power.omitted;
}
void bb_power_details_end(void)
{
    struct bb_line l;
    bb_line_start(&l, "DETAIL");
    bb_line_dec(&l, "lines_omitted", bb_power.omitted);
    bb_line_dec(&l, "accesses_dropped", bb_power.accessed > BB_POWER_ACCESSES ? bb_power.accessed - BB_POWER_ACCESSES : 0);
    bb_line_end(&l);
    platform_puts(l.text);
}
void bb_power_detail_accesses(void)
{
    const unsigned n = bb_power.accessed < BB_POWER_ACCESSES ? bb_power.accessed : BB_POWER_ACCESSES;
    for (unsigned i = 0; i < n; ++i) {
        const struct bb_power_access *a = &bb_power.accesses[i];
        struct bb_line l;
        bb_access_line(a->address, a->value, a->write, a->at - bb_power.accesses[0].at, &l);
        bb_power_detail(&l);
    }
}
