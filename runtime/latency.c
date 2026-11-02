#include "sunstone_latency.h"

#ifdef SUNSTONE_LATENCY
#include "FreeRTOS.h"
#include "task.h"
struct ss_latency_core ss_latency_results[8];
uint64_t ss_latency_frequency;
const uint64_t ss_latency_budget_us[SS_LAT_METRICS] = {
    50, 25, 25, 25, 25, 25, 100, 250, 250, 1000, 1000
};
static uint64_t limits[SS_LAT_METRICS], pending[SS_LAT_METRICS];
static uint64_t gpu_receipt[2];
static unsigned gpu_active;
static unsigned current_core(void);
#define HIST_BINS 1025U
struct histogram {
    uint64_t bins[HIST_BINS];
    uint64_t count, maximum, start, site;
};
static struct histogram histogram[SS_LAT_PHASES][8][SS_LAT_METRICS];
static uint64_t phase_start[SS_LAT_PHASES], phase_end[SS_LAT_PHASES];
static unsigned active_phase = SS_LAT_INIT;
static uint64_t acquisitions[8][2], phase_acquisitions[SS_LAT_PHASES][8][2];

/* Bin zero contains zero. Positive bin endpoints subdivide [2^e, 2^(e+1))
 * into sixteen integer intervals. Exponents below four use singleton bins. */
static unsigned histogram_bin(uint64_t value)
{
    if (!value) return 0;
    unsigned exponent = 63U - (unsigned)__builtin_clzll(value);
    unsigned part = exponent < 4 ? (unsigned)(value - (UINT64_C(1) << exponent)) : (unsigned)((value >> (exponent - 4)) & 15U);
    return 1U + exponent * 16U + part;
}
static uint64_t histogram_upper(unsigned bin)
{
    if (!bin) return 0;
    unsigned exponent = (bin - 1U) / 16U, part = (bin - 1U) % 16U;
    if (exponent < 4) return (UINT64_C(1) << exponent) + part;
    /* Avoid shifting 32 into bit 64 for the last endpoint. */
    if (exponent == 63 && part == 15) return UINT64_MAX;
    return ((uint64_t)(17U + part) << (exponent - 4)) - 1U;
}
void ss_latency_phase_set(unsigned phase)
{
    if (phase >= SS_LAT_PHASES && phase != SS_LAT_PHASE_NONE) return;
    /* Never rewrite a published phase window. */
    if (phase == SS_LAT_INIT || (phase < SS_LAT_PHASES && phase_start[phase])) return;
    if (active_phase < SS_LAT_PHASES) {
        /* Close admission before selecting the cutoff. DSB completes the
         * closing publication to the inner-shareable observers before the
         * ISB/counter read. A reader that still observed OPEN therefore ended
         * before this cutoff; readers seeing CLOSING exclude boundary samples. */
        __atomic_store_n(&phase_end[active_phase], UINT64_MAX, __ATOMIC_RELEASE);
        __asm__ volatile("dsb ish" ::: "memory");
    }
    uint64_t now = ss_latency_now();
    if (active_phase < SS_LAT_PHASES)
        __atomic_store_n(&phase_end[active_phase], now, __ATOMIC_RELEASE);
    if (phase < SS_LAT_PHASES)
        __atomic_store_n(&phase_start[phase], now, __ATOMIC_RELEASE);
    active_phase = phase;
}
void ss_latency_phase_lock_counts(unsigned phase, unsigned core, uint64_t out[2])
{
    for (unsigned lock = 0; lock < 2; ++lock)
        out[lock] = phase < SS_LAT_PHASES && core < 8 ? phase_acquisitions[phase][core][lock] : 0;
}
void ss_latency_phase_bounds(uint64_t out[SS_LAT_PHASES][2])
{
    for (unsigned p = 0; p < SS_LAT_PHASES; ++p) {
        out[p][0] = phase_start[p]; out[p][1] = phase_end[p];
    }
}
void ss_latency_lock_counts(unsigned core, uint64_t out[2])
{
    for (unsigned lock = 0; lock < 2; ++lock)
        out[lock] = core < 8 ? acquisitions[core][lock] : 0;
}
void ss_latency_phase_snapshot(struct ss_latency_phase_core out[SS_LAT_PHASES][8])
{
    for (unsigned p = 0; p < SS_LAT_PHASES; ++p)
        for (unsigned c = 0; c < 8; ++c)
            for (unsigned r = 0; r < SS_LAT_METRICS; ++r) {
                const struct histogram *h = &histogram[p][c][r];
                struct ss_latency_distribution *d = &out[p][c].metric[r];
                *d = (struct ss_latency_distribution){.count=h->count,
                    .maximum=h->maximum, .start=h->start, .site=h->site,
                    .overflow_bin_count=h->bins[HIST_BINS - 1]};
                if (!h->count) continue;
                uint64_t seen = 0;
                unsigned have_half = 0;
                uint64_t half = h->count / 2 + h->count % 2;
                uint64_t tail = h->count - h->count / 100;
                for (unsigned b = 0; b < HIST_BINS; ++b) {
                    seen += h->bins[b];
                    if (seen >= half && !have_half) {
                        d->p50_upper = histogram_upper(b);
                        have_half = 1;
                    }
                    if (seen >= tail) {
                        d->p99_upper = histogram_upper(b);
                        break;
                    }
                }
            }
}
static unsigned interval_phase(uint64_t start, uint64_t elapsed)
{
    /* Finish the caller's end-counter read before observing phase bounds. */
    __asm__ volatile("isb" ::: "memory");
    for (unsigned p = 0; p < SS_LAT_PHASES; ++p) {
        uint64_t begin = __atomic_load_n(&phase_start[p], __ATOMIC_ACQUIRE);
        uint64_t end = __atomic_load_n(&phase_end[p], __ATOMIC_ACQUIRE);
        if (end == UINT64_MAX) return SS_LAT_PHASE_NONE;
        if ((p != SS_LAT_INIT && !begin) || start < begin ||
            (end && (start >= end || elapsed > end - start))) continue;
        return p;
    }
    return SS_LAT_PHASE_NONE;
}
static void histogram_record(unsigned core, unsigned region, uint64_t start,
                             uint64_t elapsed, uintptr_t site)
{
    unsigned p = interval_phase(start, elapsed);
    if (p == SS_LAT_PHASE_NONE) return;
    struct histogram *h = &histogram[p][core][region];
    ++h->bins[histogram_bin(elapsed)];
    ++h->count;
    if (h->count == 1 || elapsed > h->maximum) {
        h->maximum = elapsed;
        h->start = start;
        h->site = site;
    }
}
struct local_state {
    uint64_t mask_start, mask_site, lock_start[2], lock_site[2];
    unsigned mask_active, lock_depth[2], frozen;
} __attribute__((aligned(64)));
static struct local_state local[8];
struct site_table {
    struct ss_latency_site sample[SS_LAT_SITE_SLOTS];
    unsigned minimum;
} __attribute__((aligned(64)));
static struct site_table sites[SS_LAT_PHASES + 1][8][SS_LAT_SITE_KINDS];

static void site_insert(struct site_table *table, struct ss_latency_site sample)
{
    if (sample.ticks <= table->sample[table->minimum].ticks) return;
    table->sample[table->minimum] = sample;
    /* Fixed three-entry scan only when a new duration enters the table. */
    for (unsigned i = 0; i < SS_LAT_SITE_SLOTS; ++i)
        if (table->sample[i].ticks < table->sample[table->minimum].ticks)
            table->minimum = i;
}
static void site_record(unsigned core, unsigned kind, uint64_t start,
                        uint64_t end, uintptr_t enter_site, uintptr_t exit_site)
{
    if (end <= start || !enter_site || !exit_site) return;
    struct ss_latency_site sample = {end - start, start, enter_site, exit_site};
    site_insert(&sites[SS_LAT_PHASES][core][kind], sample);
    unsigned phase = interval_phase(start, end - start);
    if (phase < SS_LAT_PHASES) site_insert(&sites[phase][core][kind], sample);
}
void ss_latency_sites_snapshot(struct ss_latency_site out[SS_LAT_PHASES + 1][8][SS_LAT_SITE_KINDS][SS_LAT_SITE_SLOTS])
{
    for (unsigned p = 0; p <= SS_LAT_PHASES; ++p)
        for (unsigned c = 0; c < 8; ++c)
            for (unsigned k = 0; k < SS_LAT_SITE_KINDS; ++k) {
                for (unsigned i = 0; i < SS_LAT_SITE_SLOTS; ++i)
                    out[p][c][k][i] = sites[p][c][k].sample[i];
                /* Bounded insertion sort runs only on the frozen snapshot. */
                for (unsigned i = 1; i < SS_LAT_SITE_SLOTS; ++i) {
                    struct ss_latency_site sample = out[p][c][k][i];
                    unsigned j = i;
                    while (j && out[p][c][k][j - 1].ticks < sample.ticks) {
                        out[p][c][k][j] = out[p][c][k][j - 1];
                        --j;
                    }
                    out[p][c][k][j] = sample;
                }
            }
}
static int enabled(unsigned core)
{
    return core < 8 && __atomic_load_n(&ss_latency_frequency, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&local[core].frozen, __ATOMIC_RELAXED);
}

void ss_latency_init(uint64_t frequency)
{
    /* Both supported counters exceed 1 MHz. Divide first to avoid overflow. */
    for (unsigned r = 0; r < SS_LAT_METRICS; ++r)
        limits[r] = (frequency / 1000000) * ss_latency_budget_us[r] +
            (frequency % 1000000) * ss_latency_budget_us[r] / 1000000;
    __atomic_store_n(&ss_latency_frequency, frequency, __ATOMIC_RELEASE);
}
void ss_latency_freeze(unsigned core)
{
    if (core < 8) __atomic_store_n(&local[core].frozen, 1, __ATOMIC_RELEASE);
}
uint64_t ss_latency_counter_frequency(void)
{ return __atomic_load_n(&ss_latency_frequency, __ATOMIC_ACQUIRE); }
void ss_latency_snapshot(struct ss_latency_core out[8])
{
    for (unsigned c = 0; c < 8; ++c) out[c] = ss_latency_results[c];
}
void ss_latency_record_at(unsigned core, unsigned region, uint64_t start,
                          uint64_t end, uintptr_t site)
{
    if (core >= 8 || region >= SS_LAT_METRICS ||
        !__atomic_load_n(&ss_latency_frequency, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&local[core].frozen, __ATOMIC_RELAXED) || end < start) return;
    struct ss_latency_metric *m = &ss_latency_results[core].metric[region];
    const uint64_t elapsed = end - start;
    histogram_record(core, region, start, elapsed, site);
    ++m->count;
    if (elapsed > m->maximum) {
        m->maximum = elapsed;
        m->start = start;
        m->site = site;
    }
    if (elapsed > limits[region] && !m->first_over_ticks) {
        m->first_over_start = start;
        m->first_over_ticks = elapsed;
        m->first_over_site = site;
    }
}
void ss_latency_record(unsigned core, unsigned region, uint64_t start, uintptr_t site)
{ ss_latency_record_at(core, region, start, ss_latency_now(), site); }

void ss_latency_mask_open(unsigned core, uint64_t start, uintptr_t site)
{
    if (enabled(core) && !local[core].mask_active) {
        local[core].mask_start = start;
        local[core].mask_site = site;
        local[core].mask_active = 1;
    }
}
void ss_latency_mask_close(unsigned core, uintptr_t site)
{
    if (core < 8 && local[core].mask_active) {
        const uint64_t end = ss_latency_now();
        if (enabled(core)) {
            ss_latency_record_at(core, SS_LAT_MASKED, local[core].mask_start, end, local[core].mask_site);
            site_record(core, 0, local[core].mask_start, end, local[core].mask_site, site);
        }
        local[core].mask_active = 0;
    }
}
void ss_latency_lock_acquired(unsigned core, unsigned lock, uint64_t start, uintptr_t site)
{
    if (!enabled(core) || lock >= 2) return;
    ++acquisitions[core][lock];
    unsigned p = interval_phase(start, 0);
    if (p < SS_LAT_PHASES) ++phase_acquisitions[p][core][lock];
    struct local_state *l = &local[core];
    /* Ownership already belongs to this core. Do not charge subsequent
     * acquisition counters or phase lookup to contention wait. */
    const uint64_t now = l->lock_depth[lock] ? 0 : ss_latency_now();
    ++acquisitions[core][lock];
    unsigned p = interval_phase(start, 0);
    if (p < SS_LAT_PHASES) ++phase_acquisitions[p][core][lock];
    if (l->lock_depth[lock]++ == 0) {
        ss_latency_record_at(core, SS_LAT_LOCK_WAIT0 + 2 * lock, start, now, site);
        l->lock_start[lock] = now;
        l->lock_site[lock] = site;
    }
}
void ss_latency_lock_releasing(unsigned core, unsigned lock, uintptr_t site)
{
    if (core >= 8 || lock >= 2 || !local[core].lock_depth[lock]) return;
    struct local_state *l = &local[core];
    if (--l->lock_depth[lock] == 0 && enabled(core)) {
        const uint64_t end = ss_latency_now();
        ss_latency_record_at(core, SS_LAT_LOCK_HOLD0 + 2 * lock,
                             l->lock_start[lock], end, l->lock_site[lock]);
        if (lock == 1) site_record(core, 1, l->lock_start[lock], end, l->lock_site[lock], site);
    }
}
static unsigned current_core(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return (unsigned)((value >> 48) & 255u);
}
void ss_latency_event_begin(unsigned region)
{
    if (region >= SS_LAT_METRICS || !enabled(current_core()) ||
        (region == SS_LAT_GPU && !__atomic_load_n(&gpu_active, __ATOMIC_ACQUIRE))) return;
    uint64_t empty = 0;
    (void)__atomic_compare_exchange_n(&pending[region], &empty, ss_latency_now(), 0,
                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
void ss_latency_event_end(unsigned region)
{
    ss_latency_event_finish(region, ss_latency_event_take(region));
}
uint64_t ss_latency_event_take(unsigned region)
{
    if (region >= SS_LAT_METRICS) return 0;
    return __atomic_exchange_n(&pending[region], 0, __ATOMIC_ACQUIRE);
}
void ss_latency_event_finish(unsigned region, uint64_t start)
{
    if (start) ss_latency_record(current_core(), region, start,
                                 (uintptr_t)__builtin_return_address(0));
}
void ss_latency_gpu_arm(void)
{
    (void)ss_latency_event_take(SS_LAT_GPU);
    __atomic_store_n(&gpu_active, 1, __ATOMIC_RELEASE);
}
void ss_latency_gpu_post(unsigned client)
{
    __atomic_store_n(&gpu_active, 0, __ATOMIC_RELEASE);
    uint64_t start = ss_latency_event_take(SS_LAT_GPU);
    if (client < 2) __atomic_store_n(&gpu_receipt[client], start, __ATOMIC_RELEASE);
}
void ss_latency_gpu_received(unsigned client)
{
    if (client >= 2) return;
    uint64_t end = ss_latency_now();
    uint64_t start = __atomic_exchange_n(&gpu_receipt[client], 0, __ATOMIC_ACQUIRE);
    if (!start) return; /* A polled or refused job has no IRQ latency sample. */
    /* Clients can share a core. Serialize only their metric update; the end
     * timestamp precedes this instrumentation's additional critical section. */
    taskENTER_CRITICAL();
    ss_latency_record_at(current_core(), SS_LAT_GPU, start, end,
                          (uintptr_t)__builtin_return_address(0));
    taskEXIT_CRITICAL();
}
__attribute__((weak)) void ss_latency_tick_hook(void) {}
__attribute__((weak)) void ss_latency_task_ready(void *task) { (void)task; }
#endif
