/* SPDX-License-Identifier: Apache-2.0 */
/* PMIC provider, power-domain owner and service task of bundles 6 and 7;
 * see power.h. */
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "a733_power_authority.h"
#include "a733_range.h"

#define DOMAIN1 UINT32_C(0x07061000)
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

/* The provider's callbacks: the native adapter's, with the TWI observer on
 * every completed S_TWI0 access. */
#define PMIC_OPS bb_power.pmic_native.operations
#define PMIC_NATIVE (&bb_power.pmic_native)
static int pmic_now(void *c, uint64_t *out) { (void)c; return PMIC_OPS.now(PMIC_NATIVE, out); }
static int pmic_current(void *c) { (void)c; return PMIC_OPS.current(PMIC_NATIVE); }
static int pmic_enter(void *c, uint64_t deadline) { (void)c; return PMIC_OPS.enter(PMIC_NATIVE, deadline); }
static void pmic_leave(void *c) { (void)c; PMIC_OPS.leave(PMIC_NATIVE); }
static void pmic_retain(void *c) { (void)c; PMIC_OPS.retain(PMIC_NATIVE); }
static int pmic_read(void *c, uintptr_t a, uint32_t *v)
{
    (void)c;
    const int r = PMIC_OPS.read(PMIC_NATIVE, a, v);
    if (r == 1)
        bb_twi_observe(&bb_power.twi, 0, a, *v);
    return r;
}
static int pmic_write(void *c, uintptr_t a, uint32_t v)
{
    (void)c;
    const int r = PMIC_OPS.write(PMIC_NATIVE, a, v);
    if (r == 1)
        bb_twi_observe(&bb_power.twi, 1, a, v);
    return r;
}
/* Caller buffers must also stay clear of the records this layer writes. */
static int pmic_outside(void *c, const void *p, size_t n)
{
    (void)c;
    return PMIC_OPS.outside(PMIC_NATIVE, p, n) == 1 && a733_ranges_disjoint(p, n, &bb_power.twi,
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
    if (bb_power.spin_after_on && a == DOMAIN1 && (v & 15) == 8) {
        bb_power.status_start = counter();
        bb_status_spin(DOMAIN_OPS.read, DOMAIN_NATIVE, cntpct, DOMAIN1 + 8, bb_power.status_start,
                       BB_POWER_STATUS_BOUND, &bb_power.status);
    }
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
                p->steps[p->stepped++] = (struct bb_power_step){end - start, last - first,
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

int bb_power_init(uint64_t deadline_us, TaskFunction_t client, const char *name)
{
    struct bb_power *p = &bb_power;
    p->deadline = deadline_us;
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
    if (!p->service || !p->client)
        return 0;
    static const struct a733_pmic_ops pmic_ops = {pmic_now, pmic_current, pmic_enter, pmic_leave, pmic_retain,
                                                  pmic_read, pmic_write, pmic_outside};
    const struct a733_pmic_config config = {.owner = BB_POWER_PMIC_OWNER, .identity = 1,
        .startup_deadline = deadline_us, .pins = {{2, 1, 1}, {2, 1, 1}}};
    if (!a733_pmic_native_init(&p->pmic_native, &p->locks, p->service) ||
        !a733_pmic_init(&p->pmic, &config, &pmic_ops, p, &p->clocks, &p->rails, &p->pins) ||
        !a733_gpu_resources_native_use_pmic(&p->gpu_native, &p->gpu, &p->pmic))
        return 0;
    static const struct a733_power_domain_ops domain_ops = {domain_read, domain_write, domain_notify};
    const struct a733_power_service_config service_config = {BB_POWER_PMIC_OWNER, deadline_us,
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
static void pause(const char *what)
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
        pause("provider startup and supply verification deadline");
    }
}

void bb_power_acquire(uint64_t rails)
{
    enum a733_rail_result r;
    while ((r = a733_rail_acquire(&bb_power.rails, BB_POWER_CLIENT_OWNER, rails, 0, bb_power.deadline,
                                  &bb_power.token)) == A733_RAIL_BUSY)
        pause("rail acquisition deadline");
    if (r != A733_RAIL_PENDING && r != A733_RAIL_READY)
        bb_power_fail("rail acquisition");
    while ((r = a733_rail_poll(&bb_power.rails, &bb_power.token)) != A733_RAIL_READY) {
        if (r != A733_RAIL_PENDING && r != A733_RAIL_BUSY)
            bb_power_fail("rail poll");
        pause("rail poll deadline");
    }
}

void bb_power_park(void)
{
    atomic_store_explicit(&bb_power.stop, 1, memory_order_release);
    xTaskNotifyGive(bb_power.service);
    while (!atomic_load_explicit(&bb_power.parked, memory_order_acquire))
        pause("service park deadline");
    const struct a733_pmic *pmic = &bb_power.pmic;
    if (pmic->held || pmic->retained || pmic->failed || pmic->active != A733_PMIC_REQUESTS ||
        xSemaphoreGetMutexHolder(bb_power.locks.mutex[0]) || xSemaphoreGetMutexHolder(bb_power.locks.mutex[1]))
        bb_power_fail("provider not quiescent at park");
}

int bb_power_read(uintptr_t address, uint32_t *value)
{ return domain_read(NULL, address, value) == 1; }

static void put_decimal(uint64_t value)
{
    char text[21];
    unsigned n = sizeof text - 1;
    text[n] = 0;
    do {
        text[--n] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n);
    platform_puts(text + n);
}
static void field(const char *name, uint64_t value)
{ platform_puts(" "); platform_puts(name); platform_puts("="); put_decimal(value); }
void bb_power_put(const char *name, uint64_t value)
{ platform_puts(name); put_decimal(value); platform_puts("\n"); }
void bb_power_put_hex(const char *name, uint64_t value)
{ platform_puts(name); platform_hex(value); platform_puts("\n"); }

_Noreturn void bb_power_fail(const char *text)
{
    platform_puts("FAIL ");
    platform_puts(text);
    platform_puts("\n");
    bb_power_put("PMIC_STAGE=", bb_power.pmic.stage);
    bb_power_put("PMIC_HELD=", bb_power.pmic.held);
    bb_power_put("PMIC_RETAINED=", bb_power.pmic.retained);
    bb_power_put("PMIC_FAILED=", bb_power.pmic.failed);
    bb_power_put_hex("POWER_STATE_WORD=", a733_power_authority_load(&bb_power.rails.authority));
    platform_exit(1);
}

/* Each recorded step: its span, the ticks inside it, the phases completed,
 * the first flag read of each byte phase, and its span against the wire time
 * of those phases at the admitted bit time, in thousandths. */
void bb_power_report_steps(void)
{
    const uint64_t bit_ns = bb_power.pmic.bit_ns;
    unsigned bins[4] = {0}, over_ms = 0;
    uint64_t worst = 0;
    for (unsigned i = 0; i < bb_power.stepped; ++i) {
        const struct bb_power_step *s = &bb_power.steps[i];
        const uint64_t ns = s->ticks * 125 / 3;
        const uint64_t wire = bb_twi_wire_bits(s->conditions, s->bytes, s->stops) * bit_ns;
        const uint64_t ratio = wire ? ns * 1000 / wire : 0;
        platform_puts("STEP");
        field("ns", ns);
        field("tick_interrupts", s->interrupts);
        field("conditions", s->conditions);
        field("bytes", s->bytes);
        field("stops", s->stops);
        field("flag_first_set", s->first_set);
        field("flag_first_clear", s->first_clear);
        field("polls_max", s->polls_max);
        field("wire_ns", wire);
        field("permille", ratio);
        platform_puts("\n");
        bins[ratio <= 1000 ? 0 : ratio <= 1050 ? 1 : ratio <= 1100 ? 2 : 3]++;
        over_ms += ns > 1100000;
        if (ratio > worst)
            worst = ratio;
    }
    bb_power_put("STEPS recorded=", bb_power.stepped);
    bb_power_put("STEPS dropped=", bb_power.steps_dropped);
    bb_power_put("STEPS permille max=", worst);
    bb_power_put("STEPS at most 100%=", bins[0]);
    bb_power_put("STEPS 100% to 105%=", bins[1]);
    bb_power_put("STEPS 105% to 110%=", bins[2]);
    bb_power_put("STEPS above 110%=", bins[3]);
    bb_power_put("STEPS above 1.1 ms=", over_ms);
    bb_power_put("TWI reads=", bb_power.twi.reads);
    bb_power_put_hex("TWI interrupt enable seen=", bb_power.twi.enable_seen);
}

void bb_power_report_accesses(void)
{
    const unsigned n = bb_power.accessed < BB_POWER_ACCESSES ? bb_power.accessed : BB_POWER_ACCESSES;
    for (unsigned i = 0; i < n; ++i) {
        const struct bb_power_access *a = &bb_power.accesses[i];
        bb_power_put_hex(a->write ? "OWNER write=" : "OWNER read=", a->address);
        bb_power_put_hex("  value=", a->value);
        bb_power_put("  cntpct=", a->at - bb_power.accesses[0].at);
    }
    bb_power_put("OWNER accesses=", bb_power.accessed);
    bb_power_put("OWNER writes=", bb_power.writes);
}
