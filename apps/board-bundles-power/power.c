/* SPDX-License-Identifier: Apache-2.0 */
/* PMIC provider, power-domain owner and service task of bundles 6 and 7;
 * see power.h. */
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "a733_power_authority.h"
#include "a733_range.h"

struct bb_power bb_power;
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
 * on every completed S_TWI0 access; the cookie is the adapter itself. */
static int pmic_read(void *c, uintptr_t a, uint32_t *v)
{
    const int r = bb_power.native_pmic_ops.read(c, a, v);
    if (r == 1)
        bb_twi_observe(&bb_power.twi, 0, a, *v);
    return r;
}
static int pmic_write(void *c, uintptr_t a, uint32_t v)
{
    const int r = bb_power.native_pmic_ops.write(c, a, v);
    if (r == 1)
        bb_twi_observe(&bb_power.twi, 1, a, v);
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
        bb_status_spin(DOMAIN_OPS.read, DOMAIN_NATIVE, cntpct, A733_POWER_PCK_DOMAIN(1) + 8, counter(),
                       BB_POWER_STATUS_BOUND, &bb_power.status);
    return r;
}
static void domain_notify(void *c) { (void)c; DOMAIN_OPS.notify(DOMAIN_NATIVE); }

/* The one service task: each step that completes a bus phase is recorded
 * with its CNTPCT span and the scheduler ticks inside it. */
static void service(void *unused)
{
    (void)unused;
    struct bb_power *p = &bb_power;
    for (;;) {
        const struct bb_twi before = p->twi;
        p->twi.polls_max = 0;
        const TickType_t first = xTaskGetTickCount();
        const uint64_t start = counter();
        const uint32_t ticks = a733_power_service_step(&p->domains);
        const uint64_t end = counter();
        const TickType_t last = xTaskGetTickCount();
        const struct bb_twi *t = &p->twi;
        if (t->conditions != before.conditions || t->bytes != before.bytes || t->stops != before.stops) {
            if (p->stepped < BB_POWER_STEPS)
                p->steps[p->stepped++] = (struct bb_step){end - start, last - first,
                    (uint16_t)(t->conditions - before.conditions), (uint16_t)(t->bytes - before.bytes),
                    (uint16_t)(t->stops - before.stops), (uint16_t)(t->first_set - before.first_set),
                    (uint16_t)(t->first_clear - before.first_clear), (uint16_t)t->polls_max};
            else
                ++p->steps_dropped;
        }
        if (atomic_load_explicit(&p->stop, memory_order_acquire) && a733_power_service_idle(&p->domains)) {
            atomic_store_explicit(&p->parked, 1, memory_order_release);
            for (;;)
                vTaskSuspend(NULL);
        }
        (void)ulTaskNotifyTake(pdTRUE, ticks == UINT32_MAX ? portMAX_DELAY : (TickType_t)ticks);
    }
}

int bb_power_init(TaskFunction_t client, const char *name)
{
    struct bb_power *p = &bb_power;
    p->deadline = RESULT->start_counter / 24 + BB_POWER_SESSION_US;
    atomic_init(&p->stop, 0);
    atomic_init(&p->parked, 0);
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
    p->native_pmic_ops = p->pmic_native.operations;
    struct a733_pmic_ops ops = p->native_pmic_ops;
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
    return a733_power_domain_row_init(&p->row, &p->domains, 1, BB_POWER_CLIENT_OWNER, &p->token);
}

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
