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
struct local_state {
    uint64_t mask_start, mask_site, lock_start[2], lock_site[2];
    unsigned mask_active, lock_depth[2], frozen;
} __attribute__((aligned(64)));
static struct local_state local[8];
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
void ss_latency_mask_close(unsigned core)
{
    if (core < 8 && local[core].mask_active) {
        ss_latency_record(core, SS_LAT_MASKED, local[core].mask_start, local[core].mask_site);
        local[core].mask_active = 0;
    }
}
void ss_latency_lock_acquired(unsigned core, unsigned lock, uint64_t start, uintptr_t site)
{
    if (!enabled(core) || lock >= 2) return;
    struct local_state *l = &local[core];
    if (l->lock_depth[lock]++ == 0) {
        const uint64_t now = ss_latency_now();
        ss_latency_record_at(core, SS_LAT_LOCK_WAIT0 + 2 * lock, start, now, site);
        l->lock_start[lock] = now;
        l->lock_site[lock] = site;
    }
}
void ss_latency_lock_releasing(unsigned core, unsigned lock)
{
    if (core >= 8 || lock >= 2 || !local[core].lock_depth[lock]) return;
    struct local_state *l = &local[core];
    if (--l->lock_depth[lock] == 0)
        ss_latency_record(core, SS_LAT_LOCK_HOLD0 + 2 * lock,
                          l->lock_start[lock], l->lock_site[lock]);
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
