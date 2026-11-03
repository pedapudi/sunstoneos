/* Two parts, so that no verdict depends on how loaded the host is.
 *
 * Real threads: eight host threads contend for a ticket lock, with and without the unmasked wait of
 * runtime/lock_prewait_wait.h in front of the ticket. The verdict is what holds at any scheduling: every
 * thread completes its acquisitions and no update is lost. Latencies are printed and not judged, because a
 * host thread can be descheduled for milliseconds.
 *
 * Model: the same workload runs as eight cooperative fibers on a controlled clock. A fiber advances time only
 * by sleeping, so each behaves as a core of its own that polls every POLL_NS, and every run is identical on
 * every host. The latency bounds are judged there, with no slack for descheduling: a thread's wait has two
 * parts, the unmasked wait (interrupts would run), which ends within the limit plus one poll, and the queued
 * wait after the ticket is taken (interrupts would be masked), which stays within the ticket queue's bound.
 * A bound in real microseconds on the board's cores is a board measurement, not a host check. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/resource.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ucontext.h>

#define THREADS 8
#define ROUNDS 20000
#define LIMIT_NS 100000
#define DEPTH 1

static uint64_t hold_ns = 2000; /* set per phase before the threads start */
static unsigned rounds = 500; /* real threads: enough contention for the atomics, short when the host is oversubscribed */
static _Alignas(64) _Atomic uint32_t lock_word; /* owner low, next high */
static _Atomic uint64_t protected_counter;
static _Atomic int start_flag;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static int model_active; /* nonzero while the fiber model runs on the controlled clock */
static uint64_t model_now;
static void model_sleep(uint64_t ns);
#define POLL_NS 1000u
#define SS_PW_NOW() (model_active ? model_now : now_ns())
#define SS_PW_ARM(word) atomic_load_explicit((const _Atomic uint32_t *)(word), memory_order_acquire)
#define SS_PW_SLEEP() (model_active ? model_sleep(POLL_NS) : (void)sched_yield())
#define SS_PW_DISARM() ((void)0)
#include "../runtime/lock_prewait_wait.h"

struct result {
    long last_switches;
    uint64_t done, max_unmasked, max_queued, max_total, timed_out, max_hold, late_unmasked, late_queued, descheduled;
    uint64_t queued_sample[ROUNDS];
};
static int compare(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

static _Atomic uint64_t host_switches; /* involuntary context switches of all threads, as they were observed */
static long involuntary_switches(void)
{
    struct rusage usage;
    getrusage(RUSAGE_THREAD, &usage);
    return usage.ru_nivcsw;
}

static uint64_t note_switches(struct result *r)
{
    const long now = involuntary_switches();
    if (now != r->last_switches) { atomic_fetch_add(&host_switches, (uint64_t)(now - r->last_switches)); r->last_switches = now; }
    return atomic_load(&host_switches);
}

static void spin_ns(uint64_t ns)
{
    const uint64_t end = now_ns() + ns;
    while (now_ns() < end) {}
}

static void *worker(void *argument)
{
    struct result *r = argument;
    uint64_t state = (uint64_t)(uintptr_t)r * 2654435761u + 1;
    while (!atomic_load(&start_flag)) {}
    for (unsigned i = 0; i < rounds; ++i) {
        const uint64_t switches_before = note_switches(r);
        const uint64_t begin = now_ns();
        uint64_t end = begin;
        if (r->max_hold != UINT64_MAX) { /* UINT64_MAX in max_hold selects the control without the unmasked wait */
            const uint32_t seen = atomic_load(&lock_word);
            if (ss_prewait_depth(seen) > DEPTH)
                r->timed_out += ss_prewait_run((const volatile uint32_t *)&lock_word, DEPTH, begin, LIMIT_NS, &end);
        }
        const uint64_t queued_from = now_ns();
        const uint32_t old = atomic_fetch_add(&lock_word, 1u << 16);
        const uint32_t ticket = old >> 16;
        while ((uint16_t)atomic_load_explicit(&lock_word, memory_order_acquire) != (uint16_t)ticket) {}
        const uint64_t got = now_ns();
        spin_ns(hold_ns);
        const uint64_t switches_after = note_switches(r); /* before the release, so a waiter sees a descheduled holder */
        atomic_fetch_add(&protected_counter, 1);
        const uint64_t held = now_ns() - got;
        if (held != 0 && r->max_hold != UINT64_MAX && held > r->max_hold) r->max_hold = held;
        /* The port releases with a halfword store (STLRH), so the owner wraps without carrying into next. */
        _Atomic uint16_t *owner = (_Atomic uint16_t *)&lock_word;
        atomic_store_explicit(owner, (uint16_t)(atomic_load_explicit(owner, memory_order_relaxed) + 1), memory_order_release);
        const uint64_t unmasked = end - begin, queued = got - queued_from, total = got - begin;
        if (unmasked > r->max_unmasked) r->max_unmasked = unmasked;
        if (queued > r->max_queued) r->max_queued = queued;
        if (total > r->max_total) r->max_total = total;
        (void)switches_before; (void)switches_after;
        r->queued_sample[r->done] = queued;
        ++r->done;
        state = state * 6364136223846793005u + 1442695040888963407u;
        spin_ns(1000 + (state >> 33) % 6000); /* think time, so requests arrive in bursts */
    }
    return NULL;
}

static int run(int prewait, const char *label)
{
    pthread_t thread[THREADS];
    static struct result result[THREADS];
    for (int i = 0; i < THREADS; ++i) result[i] = (struct result){0};
    for (int i = 0; i < THREADS; ++i) result[i].last_switches = 0;
    atomic_store(&lock_word, 0);
    atomic_store(&protected_counter, 0);
    atomic_store(&host_switches, 0);
    atomic_store(&start_flag, 0);
    for (int i = 0; i < THREADS; ++i) {
        if (!prewait) result[i].max_hold = UINT64_MAX;
        pthread_create(&thread[i], NULL, worker, &result[i]);
    }
    atomic_store(&start_flag, 1);
    for (int i = 0; i < THREADS; ++i) pthread_join(thread[i], NULL);
    int failed = 0;
    uint64_t worst_queued = 0, worst_unmasked = 0, worst_total = 0;
    printf("%s\n thread  done  max_unmasked_us  max_queued_us  max_total_us  timed_out\n", label);
    for (int i = 0; i < THREADS; ++i) {
        printf(" %5d %5llu %15.1f %14.1f %13.1f %10llu\n", i, (unsigned long long)result[i].done,
               result[i].max_unmasked / 1e3, result[i].max_queued / 1e3, result[i].max_total / 1e3,
               (unsigned long long)result[i].timed_out);
        if (result[i].done != rounds) { printf("FAIL thread %d completed %llu of %d\n", i, (unsigned long long)result[i].done, rounds); failed = 1; }
        qsort(result[i].queued_sample, ROUNDS, sizeof(uint64_t), compare);
        if (result[i].max_queued > worst_queued) worst_queued = result[i].max_queued;
        if (result[i].max_unmasked > worst_unmasked) worst_unmasked = result[i].max_unmasked;
        if (result[i].max_total > worst_total) worst_total = result[i].max_total;
    }
    if (atomic_load(&protected_counter) != (uint64_t)THREADS * rounds) { printf("FAIL lost update\n"); failed = 1; }
    static uint64_t all[THREADS * ROUNDS];
    const unsigned total_waits = THREADS * rounds;
    for (int i = 0; i < THREADS; ++i) for (unsigned j = 0; j < rounds; ++j) all[i * rounds + j] = result[i].queued_sample[j];
    qsort(all, total_waits, sizeof(uint64_t), compare);
    const uint64_t p50 = all[total_waits / 2], p99 = all[total_waits * 99 / 100], p999 = all[total_waits * 999 / 1000];
    printf(" queued wait over all threads: p50 %.1f us, p99 %.1f us, p99.9 %.1f us\n", p50 / 1e3, p99 / 1e3, p999 / 1e3);
    printf(" worst unmasked %.1f us, worst queued %.1f us, worst total %.1f us\n", worst_unmasked / 1e3, worst_queued / 1e3, worst_total / 1e3);
    printf(" latencies above are reported, not judged\n");
    return failed;
}

/* ---- model on the controlled clock ---- */
#define FIBER_STACK 65536
struct fiber { ucontext_t context; uint64_t wake; int done; unsigned char *stack; };
static struct fiber fiber[THREADS];
static ucontext_t scheduler;
static int current_fiber;
static void model_sleep(uint64_t ns)
{
    fiber[current_fiber].wake = model_now + ns;
    swapcontext(&fiber[current_fiber].context, &scheduler);
}
struct model_result { uint64_t done, timed_out, max_unmasked, max_queued, late_unmasked, late_queued; };
static struct model_result model_result[THREADS];
static unsigned model_rounds;
static void model_worker(void)
{
    const int id = current_fiber;
    struct model_result *r = &model_result[id];
    uint64_t state = (uint64_t)(id + 1) * 2654435761u + 1;
    for (unsigned i = 0; i < model_rounds; ++i) {
        const uint64_t begin = model_now;
        uint64_t end = begin;
        const uint32_t seen = atomic_load(&lock_word);
        if (ss_prewait_depth(seen) > DEPTH)
            r->timed_out += ss_prewait_run((const volatile uint32_t *)&lock_word, DEPTH, begin, LIMIT_NS, &end);
        const uint64_t queued_from = model_now;
        const uint32_t ticket = atomic_fetch_add(&lock_word, 1u << 16) >> 16;
        while ((uint16_t)atomic_load(&lock_word) != (uint16_t)ticket) model_sleep(POLL_NS);
        const uint64_t got = model_now;
        model_sleep(hold_ns);
        _Atomic uint16_t *owner = (_Atomic uint16_t *)&lock_word;
        atomic_store(owner, (uint16_t)(atomic_load(owner) + 1));
        const uint64_t unmasked = end - begin, queued = got - queued_from;
        if (unmasked > r->max_unmasked) r->max_unmasked = unmasked;
        if (queued > r->max_queued) r->max_queued = queued;
        if (unmasked > LIMIT_NS + POLL_NS) ++r->late_unmasked;
        if (queued > (THREADS - 1) * (uint64_t)(hold_ns + POLL_NS) + POLL_NS) ++r->late_queued;
        ++r->done;
        state = state * 6364136223846793005u + 1442695040888963407u;
        model_sleep(1000 + (state >> 33) % 6000);
    }
    fiber[id].done = 1;
}
static int run_model(const char *label)
{
    for (int i = 0; i < THREADS; ++i) model_result[i] = (struct model_result){0};
    atomic_store(&lock_word, 0);
    model_now = 0;
    model_active = 1;
    for (int i = 0; i < THREADS; ++i) {
        fiber[i].done = 0; fiber[i].wake = 0;
        if (!fiber[i].stack) fiber[i].stack = malloc(FIBER_STACK);
        getcontext(&fiber[i].context);
        fiber[i].context.uc_stack.ss_sp = fiber[i].stack;
        fiber[i].context.uc_stack.ss_size = FIBER_STACK;
        fiber[i].context.uc_link = &scheduler;
        makecontext(&fiber[i].context, model_worker, 0);
    }
    for (;;) { /* resume the fiber with the earliest wake time; ties go to the lowest index */
        int pick = -1;
        for (int i = 0; i < THREADS; ++i) if (!fiber[i].done && (pick < 0 || fiber[i].wake < fiber[pick].wake)) pick = i;
        if (pick < 0) break;
        if (fiber[pick].wake > model_now) model_now = fiber[pick].wake;
        current_fiber = pick;
        swapcontext(&scheduler, &fiber[pick].context);
    }
    model_active = 0;
    int failed = 0;
    uint64_t late_u = 0, late_q = 0, timed_out = 0, worst_u = 0, worst_q = 0;
    for (int i = 0; i < THREADS; ++i) {
        const struct model_result *r = &model_result[i];
        if (r->done != model_rounds) { printf("FAIL model thread %d completed %llu of %u\n", i, (unsigned long long)r->done, model_rounds); failed = 1; }
        late_u += r->late_unmasked; late_q += r->late_queued; timed_out += r->timed_out;
        if (r->max_unmasked > worst_u) worst_u = r->max_unmasked;
        if (r->max_queued > worst_q) worst_q = r->max_queued;
    }
    printf("model, %s: worst unmasked %.1f us, worst queued %.1f us, timed out %llu, late unmasked %llu, late queued %llu\n",
           label, worst_u / 1e3, worst_q / 1e3, (unsigned long long)timed_out, (unsigned long long)late_u, (unsigned long long)late_q);
    if (late_u) { printf("FAIL model: unmasked waits past the limit\n"); failed = 1; }
    if (late_q) { printf("FAIL model: queued waits past the ticket queue bound\n"); failed = 1; }
    return failed;
}

int main(void)
{
    int failed = run(0, "control: ticket lock without the unmasked wait");
    failed |= run(1, "unmasked wait first, depth 1, limit 100 us");
    /* Long holds keep the queue deep, so the limit ends every wait: it must hold at 100 us however long the lock is busy. */
    hold_ns = 40000;
    rounds = 100;
    failed |= run(1, "long holds (40 us): the limit ends the unmasked wait");
    /* The same two workloads on the controlled clock, where the bounds are judged. */
    hold_ns = 2000; model_rounds = ROUNDS;
    failed |= run_model("short holds (2 us)");
    hold_ns = 40000; model_rounds = 1000;
    failed |= run_model("long holds (40 us): the limit ends the unmasked wait");
    if (!failed && !model_result[0].timed_out && !model_result[1].timed_out) { puts("FAIL model: long holds never reached the limit"); failed = 1; }
    puts(failed ? "FAILED" : "PASSED");
    return failed;
}
